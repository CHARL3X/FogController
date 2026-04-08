/*
 * Fog Machine Remote V3 — ESP32 WiFi Controller
 *
 * NodeMCU ESP32S drives a relay on GPIO 4 (active-high)
 * to trigger a fog machine's momentary port.
 *
 * V3: Pattern recorder — record a tap pattern, play it back in a loop.
 *     3 preset slots (A/B/C) to save and recall patterns.
 *
 * Also: WebSocket sync, loop/repeat, tap-tempo, max hold + cooldown,
 *       disconnect safety, night mode, mDNS, OTA, AP fallback.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>

#define FW_VERSION "3.0.0"

// ── Hardware ────────────────────────────────────────────────────────────────
const int RELAY_PIN = 4;
const int RELAY_ON  = HIGH;
const int RELAY_OFF = LOW;
const int LED_PIN   = 2;

// ── Network ─────────────────────────────────────────────────────────────────
const char* STA_SSID = "Intergalactic Networking Hub";
const char* STA_PASS = "charlestobin";
const unsigned long STA_TIMEOUT_MS = 10000;

const char* AP_SSID = "FogControl";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

bool apMode = false;

DNSServer dnsServer;
AsyncWebServer webServer(80);
AsyncWebSocket ws("/ws");

// ── State: Burst ────────────────────────────────────────────────────────────
float         burstDuration  = 2.0;
bool          burstActive    = false;
unsigned long burstStartMs   = 0;

// ── State: Hold ─────────────────────────────────────────────────────────────
bool          holdActive     = false;
unsigned long holdStartMs    = 0;
float         maxHoldTime    = 30.0;

// ── State: Cooldown ─────────────────────────────────────────────────────────
bool          cooldownActive  = false;
unsigned long cooldownStartMs = 0;
const float   COOLDOWN_TIME   = 10.0;

// ── State: Loop/repeat ──────────────────────────────────────────────────────
bool          loopActive     = false;
float         loopInterval   = 5.0;
unsigned long loopLastFireMs = 0;

// ── State: Pattern recorder ─────────────────────────────────────────────────
struct PatternEvent {
  unsigned long offsetMs;
  unsigned long durationMs;
};

const int MAX_EVENTS  = 32;
const int NUM_PRESETS = 3;

struct Pattern {
  PatternEvent events[MAX_EVENTS];
  int count;
  unsigned long totalLengthMs;
};

Pattern currentPattern = {{}, 0, 0};
Pattern presets[NUM_PRESETS] = {{{}, 0, 0}, {{}, 0, 0}, {{}, 0, 0}};
int activeSlot = -1; // -1 = no slot loaded

bool          recording   = false;
unsigned long recStartMs  = 0;

bool          playing     = false;
unsigned long playStartMs = 0;
bool          playRelayOn = false; // tracks current relay state during playback

// ── State: Night mode ───────────────────────────────────────────────────────
bool nightMode = false;

// ── State: Disconnect safety ────────────────────────────────────────────────
unsigned long lastClientActivityMs = 0;
const unsigned long DISCONNECT_TIMEOUT_MS = 15000;

// ── State: Periodic broadcast ───────────────────────────────────────────────
unsigned long lastBroadcastMs = 0;

// ── Relay / LED helpers ─────────────────────────────────────────────────────
void relayOn()  { digitalWrite(RELAY_PIN, RELAY_ON);  }
void relayOff() { digitalWrite(RELAY_PIN, RELAY_OFF); }
void ledOn()    { digitalWrite(LED_PIN, HIGH); }
void ledOff()   { digitalWrite(LED_PIN, LOW);  }

// ── State JSON builder ──────────────────────────────────────────────────────
String buildStateJson() {
  float holdLeft = 0;
  if (holdActive) {
    float elapsed = (millis() - holdStartMs) / 1000.0;
    holdLeft = maxHoldTime - elapsed;
    if (holdLeft < 0) holdLeft = 0;
  }
  float cdLeft = 0;
  if (cooldownActive) {
    float elapsed = (millis() - cooldownStartMs) / 1000.0;
    cdLeft = COOLDOWN_TIME - elapsed;
    if (cdLeft < 0) cdLeft = 0;
  }

  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"relay\":%d,\"burst\":%d,\"hold\":%d,\"dur\":%.1f,"
    "\"loop\":%d,\"li\":%.1f,\"mh\":%.0f,"
    "\"holdLeft\":%.1f,\"cd\":%d,\"cdLeft\":%.1f,"
    "\"rec\":%d,\"play\":%d,\"patLen\":%d,\"patDur\":%lu,\"slot\":%d,"
    "\"slots\":[%d,%d,%d],"
    "\"clients\":%u,\"night\":%d,\"ap\":%d,\"ver\":\"%s\"}",
    digitalRead(RELAY_PIN) == RELAY_ON ? 1 : 0,
    burstActive ? 1 : 0,
    holdActive ? 1 : 0,
    burstDuration,
    loopActive ? 1 : 0,
    loopInterval,
    maxHoldTime,
    holdLeft,
    cooldownActive ? 1 : 0,
    cdLeft,
    recording ? 1 : 0,
    playing ? 1 : 0,
    currentPattern.count,
    currentPattern.totalLengthMs,
    activeSlot,
    presets[0].count > 0 ? 1 : 0,
    presets[1].count > 0 ? 1 : 0,
    presets[2].count > 0 ? 1 : 0,
    (unsigned int)ws.count(),
    nightMode ? 1 : 0,
    apMode ? 1 : 0,
    FW_VERSION
  );
  return String(buf);
}

void broadcastState() {
  ws.textAll(buildStateJson());
}

// ── Command parser ──────────────────────────────────────────────────────────
void handleWsCommand(uint8_t *data, size_t len) {
  String msg;
  msg.reserve(len);
  for (size_t i = 0; i < len; i++) msg += (char)data[i];

  // Extract cmd
  int ci = msg.indexOf("\"cmd\"");
  if (ci < 0) return;
  int cs = msg.indexOf('"', ci + 5);
  if (cs < 0) return;
  int ce = msg.indexOf('"', cs + 1);
  if (ce < 0) return;
  String cmd = msg.substring(cs + 1, ce);

  // Extract val (optional)
  float val = 0;
  int vi = msg.indexOf("\"val\"");
  if (vi >= 0) {
    int vs = vi + 5;
    while (vs < (int)msg.length() && msg[vs] != ':') vs++;
    vs++;
    val = msg.substring(vs).toFloat();
  }

  // ── Handle commands ─────────────────────────────────────────────────────
  if (cmd == "burst") {
    if (holdActive || cooldownActive || playing) return;

    if (recording) {
      // Record the tap
      if (currentPattern.count < MAX_EVENTS) {
        unsigned long offset = millis() - recStartMs;
        currentPattern.events[currentPattern.count].offsetMs = offset;
        currentPattern.events[currentPattern.count].durationMs = (unsigned long)(burstDuration * 1000);
        currentPattern.count++;
      }
    }

    burstActive  = true;
    burstStartMs = millis();
    if (loopActive) loopLastFireMs = millis();
    relayOn();
  }
  else if (cmd == "hold") {
    if (cooldownActive) return;
    holdActive = (val > 0);
    if (holdActive) {
      loopActive  = false;
      burstActive = false;
      playing     = false;
      recording   = false;
      holdStartMs = millis();
      relayOn();
    } else {
      relayOff();
    }
  }
  else if (cmd == "dur") {
    if (val >= 0.1 && val <= 10.0) burstDuration = val;
  }
  else if (cmd == "loop") {
    loopActive = (val > 0);
    if (loopActive) {
      holdActive = false;
      playing    = false;
      recording  = false;
      burstActive    = true;
      burstStartMs   = millis();
      loopLastFireMs = millis();
      relayOn();
    }
  }
  else if (cmd == "li") {
    if (val >= 0.5 && val <= 60.0) loopInterval = val;
  }
  else if (cmd == "mh") {
    if (val >= 10 && val <= 120) maxHoldTime = val;
  }
  else if (cmd == "night") {
    nightMode = (val > 0);
  }
  // ── Pattern commands ────────────────────────────────────────────────────
  else if (cmd == "rec") {
    if (holdActive || cooldownActive || playing) return;
    loopActive = false;
    burstActive = false;
    relayOff();
    recording = true;
    recStartMs = millis();
    currentPattern.count = 0;
    currentPattern.totalLengthMs = 0;
    activeSlot = -1;
  }
  else if (cmd == "stop") {
    if (recording) {
      recording = false;
      currentPattern.totalLengthMs = millis() - recStartMs;
      // Ensure minimum length if events were recorded
      if (currentPattern.count == 0) {
        currentPattern.totalLengthMs = 0;
      }
    }
    if (playing) {
      playing = false;
      playRelayOn = false;
      relayOff();
    }
    // Also stop burst/loop for a clean halt
    burstActive = false;
    loopActive = false;
    relayOff();
  }
  else if (cmd == "play") {
    if (currentPattern.count == 0 || currentPattern.totalLengthMs == 0) return;
    if (recording || cooldownActive) return;
    holdActive  = false;
    loopActive  = false;
    burstActive = false;
    playing     = true;
    playStartMs = millis();
    playRelayOn = false;
  }
  else if (cmd == "save") {
    int slot = (int)val;
    if (slot >= 0 && slot < NUM_PRESETS && currentPattern.count > 0) {
      presets[slot] = currentPattern;
      activeSlot = slot;
    }
  }
  else if (cmd == "load") {
    int slot = (int)val;
    if (slot >= 0 && slot < NUM_PRESETS && presets[slot].count > 0) {
      // Stop current activity
      playing = false;
      recording = false;
      burstActive = false;
      relayOff();
      currentPattern = presets[slot];
      activeSlot = slot;
    }
  }

  broadcastState();
}

// ── WebSocket event handler ─────────────────────────────────────────────────
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    lastClientActivityMs = millis();
    client->text(buildStateJson());
  }
  else if (type == WS_EVT_DISCONNECT) {
    broadcastState();
  }
  else if (type == WS_EVT_DATA) {
    lastClientActivityMs = millis();
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      handleWsCommand(data, len);
    }
  }
}

// ── HTML ────────────────────────────────────────────────────────────────────
const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Fog Control</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Chakra+Petch:wght@400;600;700&family=IBM+Plex+Mono:wght@400;500;600&display=swap" rel="stylesheet">
<style>
:root{
  --bg:#08080a;--surface:#111214;--surface2:#161819;
  --border:#1e2023;--border2:#2a2d31;
  --amber:#e8860c;--amber-dim:#7a4a0a;--amber-glow:rgba(232,134,12,.25);
  --red:#cc2936;--red-dim:#661418;--red-glow:rgba(204,41,54,.3);
  --green:#22a867;--green-dim:#0f5432;
  --text:#d4d4d8;--text2:#71717a;--text3:#3f3f46;
  --mono:'IBM Plex Mono','SF Mono','Fira Code',monospace;
  --sans:'Chakra Petch','SF Pro Display',system-ui,sans-serif;
  --radius:10px;
}
*{box-sizing:border-box;margin:0;padding:0}
body{
  font-family:var(--sans);
  background:var(--bg);color:var(--text);
  display:flex;flex-direction:column;align-items:center;
  min-height:100vh;min-height:100dvh;padding:20px 16px 32px;
  -webkit-user-select:none;user-select:none;
  transition:background .5s,color .5s;
}
/* Subtle noise texture */
body::before{
  content:"";position:fixed;inset:0;z-index:-1;
  background:radial-gradient(ellipse at 50% 0%,#14151a 0%,var(--bg) 70%);
}

/* ═══ Night/Red mode ════════════════════════════════════════ */
body.night{--bg:#080000;--surface:#120000;--surface2:#1a0000;
  --border:#220000;--border2:#330000;
  --amber:#cc2200;--amber-dim:#551100;--amber-glow:rgba(200,30,0,.3);
  --text:#bb3333;--text2:#662222;--text3:#331111;
  --green:#aa2200;--green-dim:#440000;--red:#aa0000;--red-dim:#440000}
body.night::before{background:radial-gradient(ellipse at 50% 0%,#1a0500 0%,#080000 70%)}
body.night .burst-ring{border-color:var(--red-dim)}
body.night #burst{
  background:radial-gradient(circle at 40% 35%,#2a0000,#100000);
  color:#662222;box-shadow:0 8px 32px rgba(0,0,0,.8);
  border-color:#330000;
}
body.night #burst.firing{
  background:radial-gradient(circle at 40% 35%,#aa0000,#550000);
  color:#ff4444;border-color:#cc0000;
  box-shadow:0 0 60px rgba(200,0,0,.5),0 0 120px rgba(200,0,0,.2);
}
body.night .burst-ring.active{border-color:var(--red);box-shadow:0 0 30px var(--red-glow)}
body.night input[type=range]::-webkit-slider-thumb{background:var(--red)}
body.night .cd-overlay{background:rgba(8,0,0,.95)}
body.night .cd-time{color:#ff2200}
body.night .night-toggle{color:var(--red)}

/* ═══ Header ═══════════════════════════════════════════════ */
.header{
  width:100%;max-width:360px;
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:28px;
}
h1{
  font-size:1.05rem;font-weight:700;color:var(--text2);
  letter-spacing:.14em;text-transform:uppercase;
}
.badge{
  display:flex;align-items:center;gap:6px;
  background:var(--surface);border:1px solid var(--border);border-radius:20px;
  padding:5px 12px;font-family:var(--mono);font-size:.6rem;
  font-weight:500;color:var(--text3);letter-spacing:.03em;
}
.badge .dot{
  width:5px;height:5px;border-radius:50%;
  background:var(--green);
  box-shadow:0 0 6px rgba(34,168,103,.5);
}

/* ═══ Burst Button ═════════════════════════════════════════ */
.burst-wrap{position:relative;margin-bottom:12px}
.burst-ring{
  width:188px;height:188px;border-radius:50%;
  border:2px solid var(--border);
  display:flex;align-items:center;justify-content:center;
  transition:all .3s;
}
.burst-ring.active{
  border-color:var(--amber);
  box-shadow:0 0 40px var(--amber-glow);
}
.burst-ring.rec-active{
  border-color:var(--red);
  box-shadow:0 0 30px var(--red-glow);
  animation:ring-pulse 1.2s ease-in-out infinite;
}
@keyframes ring-pulse{0%,100%{opacity:1}50%{opacity:.5}}

#burst{
  width:164px;height:164px;border-radius:50%;
  border:1px solid var(--border2);
  background:radial-gradient(circle at 38% 32%,#2a2c30,#151618);
  color:var(--text2);
  font-family:var(--sans);font-size:1.3rem;font-weight:700;
  letter-spacing:.12em;text-transform:uppercase;
  cursor:pointer;
  transition:all .2s cubic-bezier(.22,1,.36,1);
  box-shadow:0 8px 32px rgba(0,0,0,.6),inset 0 1px 0 rgba(255,255,255,.04);
}
#burst:active:not(:disabled){transform:scale(.93)}
#burst:disabled{opacity:.2;cursor:not-allowed}
#burst.firing{
  background:radial-gradient(circle at 38% 32%,#d67200,#8a3a00);
  color:#fff;border-color:var(--amber);
  box-shadow:0 0 60px var(--amber-glow),0 0 120px rgba(232,134,12,.12);
}

/* ═══ Sections ═════════════════════════════════════════════ */
.section{
  width:100%;max-width:360px;
  background:var(--surface);
  border:1px solid var(--border);
  border-radius:var(--radius);
  padding:14px 16px;
  margin-bottom:8px;
}
.sec-head{
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:10px;
}
.sec-title{
  font-size:.6rem;font-weight:700;color:var(--text3);
  letter-spacing:.16em;text-transform:uppercase;
}
.row{display:flex;align-items:center;gap:10px}
.row+.row{margin-top:8px}
.lbl{font-family:var(--mono);font-size:.75rem;color:var(--text3);min-width:36px}
.val{
  font-family:var(--mono);font-size:.85rem;font-weight:600;
  color:var(--amber);min-width:3.2em;text-align:right;
  letter-spacing:.02em;
}

/* ═══ Range slider ═════════════════════════════════════════ */
input[type=range]{
  -webkit-appearance:none;flex:1;height:4px;border-radius:2px;
  background:var(--border);outline:none;
}
input[type=range]::-webkit-slider-thumb{
  -webkit-appearance:none;width:18px;height:18px;border-radius:50%;
  background:var(--text);cursor:pointer;
  box-shadow:0 0 0 3px var(--bg),0 0 0 4px var(--border2);
  transition:background .15s;
}
input[type=range]:active::-webkit-slider-thumb{background:var(--amber)}

/* ═══ Toggle switch ════════════════════════════════════════ */
.switch{position:relative;width:44px;height:24px;flex-shrink:0}
.switch input{opacity:0;width:0;height:0}
.switch .sl{
  position:absolute;inset:0;background:var(--border);border-radius:12px;
  cursor:pointer;transition:all .3s;
}
.switch .sl::before{
  content:"";position:absolute;left:2px;top:2px;
  width:20px;height:20px;border-radius:50%;
  background:var(--text3);transition:all .3s;
}
.switch input:checked+.sl{background:var(--amber-dim)}
.switch input:checked+.sl::before{
  transform:translateX(20px);
  background:var(--amber);
  box-shadow:0 0 8px var(--amber-glow);
}

/* ═══ Hold countdown bar ═══════════════════════════════════ */
.hold-bar-wrap{
  width:100%;height:3px;background:var(--border);border-radius:2px;
  margin-top:10px;overflow:hidden;display:none;
}
.hold-bar-wrap.active{display:block}
.hold-bar{
  height:100%;border-radius:2px;
  background:linear-gradient(90deg,var(--amber),var(--red));
  transition:width .15s linear;
}

/* ═══ Tap-tempo button ═════════════════════════════════════ */
.tap-btn{
  padding:5px 16px;border-radius:6px;border:1px solid var(--border2);
  background:var(--surface2);color:var(--text2);
  font-family:var(--mono);font-size:.65rem;font-weight:600;
  cursor:pointer;letter-spacing:.08em;text-transform:uppercase;
  transition:all .15s;flex-shrink:0;
}
.tap-btn:active{background:var(--border);color:var(--text)}

/* ═══ Pattern controls ═════════════════════════════════════ */
.pat-row{display:flex;gap:6px;margin-bottom:10px}
.pat-btn{
  flex:1;padding:10px 0;border-radius:8px;border:1px solid var(--border2);
  background:var(--surface2);color:var(--text3);
  font-family:var(--sans);font-size:.7rem;font-weight:700;
  cursor:pointer;text-align:center;letter-spacing:.06em;
  text-transform:uppercase;transition:all .2s;
}
.pat-btn:active:not(:disabled){transform:scale(.95)}
.pat-btn:disabled{opacity:.15;cursor:not-allowed}

.pat-btn.rec-btn.recording{
  background:var(--red);color:#fff;border-color:var(--red);
  box-shadow:0 0 20px var(--red-glow);
  animation:rec-flash 1s ease-in-out infinite;
}
@keyframes rec-flash{0%,100%{opacity:1}50%{opacity:.65}}

.pat-btn.play-btn.playing{
  background:var(--green);color:#fff;border-color:var(--green);
  box-shadow:0 0 16px rgba(34,168,103,.3);
}
.pat-btn.stop-btn:active:not(:disabled){background:var(--border2);color:var(--text)}

.preset-row{display:flex;gap:5px}
.preset-row+.preset-row{margin-top:5px}
.preset-lbl{
  font-family:var(--mono);font-size:.55rem;font-weight:600;
  color:var(--text3);letter-spacing:.1em;width:32px;
  display:flex;align-items:center;flex-shrink:0;
}
.preset-btn{
  flex:1;padding:7px 0;border-radius:6px;border:1px solid var(--border);
  background:transparent;color:var(--text3);
  font-family:var(--mono);font-size:.7rem;font-weight:600;
  cursor:pointer;text-align:center;transition:all .15s;
  letter-spacing:.04em;
}
.preset-btn:disabled{opacity:.15;cursor:not-allowed}
.preset-btn.has-data{color:var(--text2);border-color:var(--border2)}
.preset-btn.active{
  background:var(--amber-dim);color:var(--amber);border-color:var(--amber-dim);
}
.preset-btn:active:not(:disabled){transform:scale(.95)}

.pat-info{
  font-family:var(--mono);font-size:.65rem;font-weight:500;
  color:var(--text3);text-align:center;margin-top:8px;
  letter-spacing:.03em;min-height:1em;
}
.pat-info.recording{color:var(--red)}

/* ═══ Cooldown overlay ═════════════════════════════════════ */
.cd-overlay{
  position:fixed;inset:0;z-index:100;
  background:rgba(8,8,10,.95);
  backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);
  display:none;flex-direction:column;
  align-items:center;justify-content:center;
}
.cd-overlay.active{display:flex}
.cd-label{
  font-size:.7rem;font-weight:700;color:var(--text3);
  letter-spacing:.2em;text-transform:uppercase;margin-bottom:12px;
}
.cd-time{
  font-family:var(--mono);font-size:4rem;font-weight:600;
  color:var(--amber);letter-spacing:-.02em;
}

/* ═══ Footer ═══════════════════════════════════════════════ */
.footer{
  margin-top:auto;padding-top:24px;
  display:flex;align-items:center;justify-content:space-between;
  width:100%;max-width:360px;
}
.night-toggle{
  background:none;border:none;color:var(--text3);
  font-size:1.2rem;cursor:pointer;padding:4px;
  transition:color .2s;
}
.night-toggle:active{color:var(--amber)}
.fw-ver{
  font-family:var(--mono);font-size:.55rem;font-weight:500;
  color:var(--text3);letter-spacing:.06em;opacity:.5;
}

/* ═══ Entrance animations ══════════════════════════════════ */
@keyframes fadeUp{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:none}}
.header{animation:fadeUp .5s ease both}
.burst-wrap{animation:fadeUp .5s ease .05s both}
.section:nth-child(3){animation:fadeUp .4s ease .1s both}
.section:nth-child(4){animation:fadeUp .4s ease .15s both}
.section:nth-child(5){animation:fadeUp .4s ease .2s both}
.section:nth-child(6){animation:fadeUp .4s ease .25s both}
.footer{animation:fadeUp .4s ease .3s both}
</style>
</head>
<body>

<div class="header">
  <h1>Fog Control</h1>
  <div class="badge">
    <span class="dot"></span>
    <span id="wifiMode">--</span>
    <span>&middot;</span>
    <span><span id="clients">0</span></span>
  </div>
</div>

<div class="burst-wrap">
  <div class="burst-ring" id="burstRing">
    <button id="burst" onclick="fireBurst()">Burst</button>
  </div>
</div>

<div class="section">
  <div class="sec-head">
    <span class="sec-title">Duration</span>
    <span class="val" id="durVal">2.0s</span>
  </div>
  <div class="row">
    <input type="range" id="dur" min="0.1" max="10" step="0.1" value="2.0"
           oninput="send({cmd:'dur',val:parseFloat(this.value)});
                    document.getElementById('durVal').textContent=parseFloat(this.value).toFixed(1)+'s'">
  </div>
</div>

<div class="section">
  <div class="sec-head">
    <span class="sec-title">Hold</span>
    <span class="lbl" id="holdLbl" style="min-width:0">OFF</span>
  </div>
  <div class="row">
    <label class="switch">
      <input type="checkbox" id="holdCb" onchange="send({cmd:'hold',val:this.checked?1:0})">
      <span class="sl"></span>
    </label>
    <span style="flex:1"></span>
    <span class="lbl" style="text-align:right;min-width:0">MAX</span>
    <input type="range" id="mh" min="10" max="120" step="5" value="30" style="width:72px;flex:none"
           oninput="send({cmd:'mh',val:parseFloat(this.value)});
                    document.getElementById('mhVal').textContent=this.value+'s'">
    <span class="val" id="mhVal" style="min-width:2.5em">30s</span>
  </div>
  <div class="hold-bar-wrap" id="holdBarWrap">
    <div class="hold-bar" id="holdBar"></div>
  </div>
</div>

<div class="section">
  <div class="sec-head">
    <span class="sec-title">Loop</span>
    <span class="lbl" id="loopLbl" style="min-width:0">OFF</span>
  </div>
  <div class="row">
    <label class="switch">
      <input type="checkbox" id="loopCb" onchange="send({cmd:'loop',val:this.checked?1:0})">
      <span class="sl"></span>
    </label>
    <span style="flex:1"></span>
    <button class="tap-btn" onclick="handleTap()">TAP</button>
  </div>
  <div class="row">
    <span class="lbl">Every</span>
    <input type="range" id="li" min="0.5" max="60" step="0.5" value="5.0"
           oninput="send({cmd:'li',val:parseFloat(this.value)});
                    document.getElementById('liVal').textContent=parseFloat(this.value).toFixed(1)+'s'">
    <span class="val" id="liVal">5.0s</span>
  </div>
</div>

<div class="section">
  <div class="sec-head">
    <span class="sec-title">Pattern</span>
    <span class="pat-info" id="patInfo"></span>
  </div>
  <div class="pat-row">
    <button class="pat-btn rec-btn" id="recBtn" onclick="send({cmd:'rec'})">&#9679; Rec</button>
    <button class="pat-btn stop-btn" id="stopBtn" onclick="send({cmd:'stop'})">&#9632; Stop</button>
    <button class="pat-btn play-btn" id="playBtn" onclick="send({cmd:'play'})">&#9654; Play</button>
  </div>
  <div class="preset-row">
    <span class="preset-lbl">SAVE</span>
    <button class="preset-btn" id="saveA" onclick="send({cmd:'save',val:0})">A</button>
    <button class="preset-btn" id="saveB" onclick="send({cmd:'save',val:1})">B</button>
    <button class="preset-btn" id="saveC" onclick="send({cmd:'save',val:2})">C</button>
  </div>
  <div class="preset-row">
    <span class="preset-lbl">LOAD</span>
    <button class="preset-btn" id="loadA" onclick="send({cmd:'load',val:0})">A</button>
    <button class="preset-btn" id="loadB" onclick="send({cmd:'load',val:1})">B</button>
    <button class="preset-btn" id="loadC" onclick="send({cmd:'load',val:2})">C</button>
  </div>
</div>

<div class="cd-overlay" id="cdOverlay">
  <div class="cd-label">Cooldown</div>
  <div class="cd-time" id="cdTime">0.0</div>
</div>

<div class="footer">
  <button class="night-toggle" id="nightBtn" onclick="send({cmd:'night',val:document.body.classList.contains('night')?0:1})">&#9789;</button>
  <span class="fw-ver" id="fwVer">v--</span>
</div>

<script>
var ws,state={},reconnTimer=null;

function connect(){
  ws=new WebSocket("ws://"+location.hostname+"/ws");
  ws.onopen=function(){clearTimeout(reconnTimer)};
  ws.onclose=function(){reconnTimer=setTimeout(connect,2000)};
  ws.onmessage=function(e){
    state=JSON.parse(e.data);
    renderUI();
  };
}
function send(obj){
  if(ws&&ws.readyState===WebSocket.OPEN)ws.send(JSON.stringify(obj));
}
connect();

function fireBurst(){
  var btn=document.getElementById("burst");
  if(btn.disabled)return;
  send({cmd:"burst"});
}

var tapTimes=[],tapReset=null;
function handleTap(){
  var now=Date.now();
  clearTimeout(tapReset);
  tapReset=setTimeout(function(){tapTimes=[]},3000);
  tapTimes.push(now);
  if(tapTimes.length>8)tapTimes.shift();
  if(tapTimes.length>=2){
    var sum=0;
    for(var i=1;i<tapTimes.length;i++)sum+=tapTimes[i]-tapTimes[i-1];
    var avg=sum/(tapTimes.length-1);
    var sec=Math.round(avg/100)/10;
    sec=Math.max(0.5,Math.min(60,sec));
    document.getElementById("li").value=sec;
    document.getElementById("liVal").textContent=sec.toFixed(1)+"s";
    send({cmd:"li",val:sec});
  }
}

setInterval(function(){
  if(state.hold&&state.holdLeft>0){
    state.holdLeft=Math.max(0,state.holdLeft-0.1);
    renderCountdowns();
  }
  if(state.cd&&state.cdLeft>0){
    state.cdLeft=Math.max(0,state.cdLeft-0.1);
    renderCountdowns();
  }
},100);

function renderCountdowns(){
  var wrap=document.getElementById("holdBarWrap");
  var bar=document.getElementById("holdBar");
  if(state.hold&&state.mh>0){
    wrap.classList.add("active");
    bar.style.width=Math.max(0,(state.holdLeft/state.mh)*100)+"%";
  }else{
    wrap.classList.remove("active");
  }
  var cd=document.getElementById("cdOverlay");
  var cdt=document.getElementById("cdTime");
  if(state.cd){
    cd.classList.add("active");
    cdt.textContent=state.cdLeft.toFixed(1);
  }else{
    cd.classList.remove("active");
  }
}

function renderUI(){
  var btn=document.getElementById("burst");
  var ring=document.getElementById("burstRing");
  var isFiring=state.burst||state.hold||state.relay;

  if(isFiring){btn.classList.add("firing")}else{btn.classList.remove("firing")}
  if(isFiring||state.play){ring.classList.add("active")}else{ring.classList.remove("active")}
  if(state.rec){ring.classList.add("rec-active")}else{ring.classList.remove("rec-active")}
  btn.disabled=state.hold||state.cd||state.play;

  document.getElementById("dur").value=state.dur;
  document.getElementById("durVal").textContent=state.dur.toFixed(1)+"s";

  document.getElementById("holdCb").checked=state.hold;
  document.getElementById("holdLbl").textContent=state.hold?"ON":"OFF";
  document.getElementById("mh").value=state.mh;
  document.getElementById("mhVal").textContent=state.mh+"s";
  document.getElementById("holdCb").disabled=state.cd||state.rec||state.play;

  document.getElementById("loopCb").checked=state.loop;
  document.getElementById("loopLbl").textContent=state.loop?"ON":"OFF";
  document.getElementById("li").value=state.li;
  document.getElementById("liVal").textContent=state.li.toFixed(1)+"s";
  document.getElementById("loopCb").disabled=state.cd||state.rec||state.play;

  var recBtn=document.getElementById("recBtn");
  var stopBtn=document.getElementById("stopBtn");
  var playBtn=document.getElementById("playBtn");

  recBtn.disabled=state.hold||state.cd||state.play;
  if(state.rec){recBtn.classList.add("recording")}else{recBtn.classList.remove("recording")}

  playBtn.disabled=state.rec||state.cd||(state.patLen==0);
  if(state.play){playBtn.classList.add("playing")}else{playBtn.classList.remove("playing")}

  stopBtn.disabled=!state.rec&&!state.play&&!state.burst&&!state.loop;

  var hasPat=state.patLen>0;
  document.getElementById("saveA").disabled=!hasPat||state.rec;
  document.getElementById("saveB").disabled=!hasPat||state.rec;
  document.getElementById("saveC").disabled=!hasPat||state.rec;

  var loadBtns=["loadA","loadB","loadC"];
  var saveBtns=["saveA","saveB","saveC"];
  for(var i=0;i<3;i++){
    var lb=document.getElementById(loadBtns[i]);
    lb.disabled=!state.slots[i]||state.rec;
    if(state.slots[i]){lb.classList.add("has-data")}else{lb.classList.remove("has-data")}
    if(state.slot==i){lb.classList.add("active")}else{lb.classList.remove("active")}
    var sb=document.getElementById(saveBtns[i]);
    if(state.slots[i]){sb.classList.add("has-data")}else{sb.classList.remove("has-data")}
    if(state.slot==i){sb.classList.add("active")}else{sb.classList.remove("active")}
  }

  var info=document.getElementById("patInfo");
  if(state.rec){
    info.textContent=state.patLen+" taps";
    info.classList.add("recording");
  }else if(state.patLen>0){
    var secs=(state.patDur/1000).toFixed(1);
    info.textContent=state.patLen+" tap"+(state.patLen!=1?"s":"")+" \u00b7 "+secs+"s";
    info.classList.remove("recording");
  }else{
    info.textContent="";
    info.classList.remove("recording");
  }

  document.getElementById("wifiMode").textContent=state.ap?"AP":"HOME";
  document.getElementById("clients").textContent=state.clients;

  if(state.night){document.body.classList.add("night")}
  else{document.body.classList.remove("night")}

  document.getElementById("fwVer").textContent="v"+state.ver;
  renderCountdowns();
}
</script>
</body>
</html>
)rawliteral";

// ── Captive portal handler ──────────────────────────────────────────────────
void servePage(AsyncWebServerRequest *request) {
  request->send(200, "text/html", PAGE_HTML);
}

void handleNotFound(AsyncWebServerRequest *request) {
  if (apMode) {
    request->redirect("http://192.168.4.1/");
  } else {
    request->send(404, "text/plain", "not found");
  }
}

// ── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  relayOff();

  pinMode(LED_PIN, OUTPUT);
  ledOff();

  Serial.begin(115200);
  Serial.println("\n[FogControl V3] Starting...");

  // ── Try home WiFi ─────────────────────────────────────────────────────
  Serial.print("[FogControl] Connecting to home WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < STA_TIMEOUT_MS) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(100);
    Serial.print(".");
  }
  ledOff();
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.print("[FogControl] Connected! IP: ");
    Serial.println(WiFi.localIP());
    MDNS.begin("fog");

    ArduinoOTA.setHostname("fog");
    ArduinoOTA.onStart([]() { relayOff(); ledOff(); playing = false; recording = false; });
    ArduinoOTA.onEnd([]()   { ledOn(); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    });
    ArduinoOTA.begin();
  } else {
    apMode = true;
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID);
    Serial.print("[FogControl] AP mode — IP: ");
    Serial.println(WiFi.softAPIP());
    dnsServer.start(53, "*", AP_IP);
  }

  ledOn();

  ws.onEvent(onWsEvent);
  webServer.addHandler(&ws);

  webServer.on("/", HTTP_GET, servePage);

  if (apMode) {
    webServer.on("/generate_204",        HTTP_GET, servePage);
    webServer.on("/gen_204",             HTTP_GET, servePage);
    webServer.on("/hotspot-detect.html", HTTP_GET, servePage);
    webServer.on("/canonical.html",      HTTP_GET, servePage);
    webServer.on("/success.txt",         HTTP_GET, servePage);
    webServer.on("/connecttest.txt",     HTTP_GET, servePage);
    webServer.on("/redirect",            HTTP_GET, servePage);
  }

  webServer.onNotFound(handleNotFound);
  webServer.begin();

  lastClientActivityMs = millis();
  Serial.println("[FogControl V3] Ready.");
}

// ── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  if (apMode) dnsServer.processNextRequest();
  if (!apMode) ArduinoOTA.handle();

  unsigned long now = millis();

  // ── Burst timeout ─────────────────────────────────────────────────────
  if (burstActive && !holdActive && !playing) {
    if (now - burstStartMs >= (unsigned long)(burstDuration * 1000.0)) {
      relayOff();
      burstActive = false;
      broadcastState();
    }
  }

  // ── Max hold timer ────────────────────────────────────────────────────
  if (holdActive) {
    if (now - holdStartMs >= (unsigned long)(maxHoldTime * 1000.0)) {
      holdActive  = false;
      burstActive = false;
      relayOff();
      cooldownActive  = true;
      cooldownStartMs = now;
      broadcastState();
    }
  }

  // ── Cooldown ──────────────────────────────────────────────────────────
  if (cooldownActive) {
    if (now - cooldownStartMs >= (unsigned long)(COOLDOWN_TIME * 1000.0)) {
      cooldownActive = false;
      broadcastState();
    }
  }

  // ── Loop/repeat mode ──────────────────────────────────────────────────
  if (loopActive && !holdActive && !cooldownActive && !burstActive && !playing) {
    if (now - loopLastFireMs >= (unsigned long)(loopInterval * 1000.0)) {
      burstActive    = true;
      burstStartMs   = now;
      loopLastFireMs = now;
      relayOn();
      broadcastState();
    }
  }

  // ── Pattern playback ──────────────────────────────────────────────────
  if (playing && !cooldownActive && currentPattern.count > 0 && currentPattern.totalLengthMs > 0) {
    unsigned long cyclePos = (now - playStartMs) % currentPattern.totalLengthMs;

    // Check if any event covers the current position
    bool shouldBeOn = false;
    for (int i = 0; i < currentPattern.count; i++) {
      unsigned long start = currentPattern.events[i].offsetMs;
      unsigned long end   = start + currentPattern.events[i].durationMs;
      if (cyclePos >= start && cyclePos < end) {
        shouldBeOn = true;
        break;
      }
    }

    if (shouldBeOn && !playRelayOn) {
      relayOn();
      playRelayOn = true;
      broadcastState();
    } else if (!shouldBeOn && playRelayOn) {
      relayOff();
      playRelayOn = false;
      broadcastState();
    }
  }

  // Pause playback relay during cooldown
  if (playing && cooldownActive && playRelayOn) {
    relayOff();
    playRelayOn = false;
  }

  // ── Disconnect safety ─────────────────────────────────────────────────
  if (ws.count() == 0 && (holdActive || burstActive || loopActive || playing)) {
    if (now - lastClientActivityMs >= DISCONNECT_TIMEOUT_MS) {
      holdActive     = false;
      burstActive    = false;
      loopActive     = false;
      playing        = false;
      recording      = false;
      cooldownActive = false;
      playRelayOn    = false;
      relayOff();
    }
  }

  // ── Periodic state broadcast ──────────────────────────────────────────
  if ((holdActive || cooldownActive || loopActive || burstActive || playing || recording) &&
      now - lastBroadcastMs >= 2000) {
    lastBroadcastMs = now;
    broadcastState();
  }

  // ── WebSocket cleanup ─────────────────────────────────────────────────
  ws.cleanupClients();
}
