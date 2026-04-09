# Fog Machine Remote

WiFi-controlled fog machine trigger built on ESP32. Control your fog machine from any phone or laptop on the same network — no app install required.

## Features

- **Burst / Hold / Loop** — tap for a timed burst, hold for continuous fog, or set it to auto-fire on a loop
- **Tap Tempo** — tap a rhythm to set loop interval
- **Pattern Recorder** — record tap patterns and play them back in a loop, with 3 preset slots (like a looper pedal for fog)
- **WiFi Provisioning** — scan for networks and connect via the web UI. No hardcoded credentials — everything managed from your phone
- **AP Fallback** — if no saved network is found, the ESP32 creates a "FogControl" hotspot with a captive portal
- **Multi-Client Sync** — multiple devices stay in sync via WebSocket
- **Safety** — max hold timer (30s default), 10s cooldown after hold, auto-off on client disconnect
- **Night Mode** — dim red UI for dark stages
- **OTA Updates** — flash new firmware over WiFi (ArduinoOTA)
- **mDNS** — access at `http://fog.local` (when on the same network)

## Hardware

### Parts

| Part | Notes |
|------|-------|
| **ESP32 NodeMCU-32S** (30-pin) | Any ESP32 dev board works — adjust pins if needed |
| **5V Relay Module** | Single-channel, active-HIGH. Must handle your fog machine's trigger voltage |
| **Fog Machine** | Any machine with a wired remote / momentary trigger port |
| USB cable + 5V power supply | For powering the ESP32 |

### Pinout

| ESP32 Pin | Connection | Notes |
|-----------|------------|-------|
| **GPIO 4** | Relay IN (signal) | Active-HIGH — relay energizes when GPIO 4 goes HIGH |
| **GPIO 2** | Onboard LED | Built-in on most ESP32 boards. Status indicator |
| **5V** | Relay VCC | Powers the relay module |
| **GND** | Relay GND | Common ground |

### Wiring Diagram

```
ESP32                  Relay Module              Fog Machine Remote Port
┌──────────┐          ┌──────────────┐          ┌─────────────────────┐
│          │          │              │          │                     │
│  GPIO 4  ├─────────►│ IN (Signal)  │          │  Most fog machines  │
│          │          │              │          │  have a wired remote│
│  5V      ├─────────►│ VCC          │          │  with a momentary   │
│          │          │              │          │  switch. The relay   │
│  GND     ├─────────►│ GND          │          │  replaces that      │
│          │          │              │          │  switch.             │
│          │          │  COM  ───────┼──────────┤  Wire 1             │
│          │          │  NO   ───────┼──────────┤  Wire 2             │
│          │          │              │          │                     │
└──────────┘          └──────────────┘          └─────────────────────┘
```

### Relay Wiring to Fog Machine

Most fog machines have a wired remote that's just a momentary push button completing a circuit. To wire the relay:

1. **Open the fog machine's remote** (or cut the remote cable)
2. **Identify the two wires** going to the momentary switch
3. **Connect one wire to COM** (Common) on the relay
4. **Connect the other wire to NO** (Normally Open) on the relay

When the ESP32 triggers GPIO 4 HIGH, the relay closes the NO-COM circuit, which is the same as pressing the fog button.

> **Important:** Use the **NO** (Normally Open) terminal, not NC. This way the fog machine is OFF when the relay is unpowered — safe default.

### Relay Polarity

This firmware uses **active-HIGH** logic (`RELAY_ON = HIGH`). If your relay module is active-LOW (relay LED is ON at idle, OFF during trigger), swap the values in `main.cpp`:

```cpp
const int RELAY_ON  = LOW;   // for active-LOW relay
const int RELAY_OFF = HIGH;
```

## Building & Flashing

### Requirements

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)

### Build & Upload

```bash
# Clone
git clone https://github.com/CHARL3X/FogController.git
cd FogController

# Build
pio run

# Flash over USB (hold BOOT button if auto-reset doesn't work)
pio run -t upload

# Monitor serial output
pio device monitor -b 115200
```

### OTA Update (after first flash)

Once connected to the same WiFi network:

```bash
pio run -t upload --upload-port fog.local
```

## Usage

### First Boot

1. Power on the ESP32
2. No saved networks yet — it creates a **"FogControl"** WiFi hotspot
3. Connect your phone/laptop to "FogControl"
4. A captive portal opens (or go to `http://192.168.4.1`)
5. Tap **SCAN FOR NETWORKS** in the WiFi Setup section
6. Select your network, enter the password, tap **CONNECT**
7. The ESP32 saves the credentials, reboots, and connects to your WiFi
8. Access the controller at **http://fog.local**

### Controls

| Control | What it does |
|---------|-------------|
| **BURST** | Single fog burst (adjustable 0.1–10s duration) |
| **HOLD** | Continuous fog while held (max 30s, then 10s cooldown) |
| **LOOP** | Auto-fires a burst every X seconds (0.15–60s interval) |
| **TAP** | Tap repeatedly to set loop tempo from your rhythm |
| **REC** | Start recording a tap pattern |
| **PLAY** | Play back the recorded pattern in a loop |
| **P1 / P2 / P3** | Save/load pattern presets |
| **NIGHT** | Toggle night mode (dim red UI) |

### WiFi Management

- **AP Mode**: WiFi setup section is visible — scan, connect, save networks
- **STA Mode**: Saved networks shown with forget (X) buttons — manage from the UI
- Saves up to 8 networks in flash (NVS). On boot, connects to the strongest known network in range

## REST API

Every endpoint returns the full state JSON and works with CORS. Trigger fog from anything — curl, Shortcuts, Home Assistant, Stream Deck, whatever.

```bash
# Fire a burst
curl http://fog.local/api/burst

# Start looping every 3 seconds
curl "http://fog.local/api/loop?val=1"
curl "http://fog.local/api/li?val=3.0"

# Stop everything
curl http://fog.local/api/stop

# Check current state
curl http://fog.local/api/status
```

| Endpoint | Action |
|----------|--------|
| `GET /api/status` | Current state (no action) |
| `GET /api/burst` | Fire a single burst |
| `GET /api/stop` | Stop everything |
| `GET /api/hold?val=1\|0` | Hold on/off (default: on) |
| `GET /api/loop?val=1\|0` | Loop on/off (default: on) |
| `GET /api/play` | Play current pattern |
| `GET /api/dur?val=X` | Set burst duration (0.1–10s) |
| `GET /api/li?val=X` | Set loop interval (0.15–60s) |
| `GET /api/night?val=1\|0` | Night mode on/off |
| `GET /api/help` | List all endpoints |

## Configuration

Key defaults in `src/main.cpp`:

| Setting | Default | Range |
|---------|---------|-------|
| Burst duration | 2.0s | 0.1–10s |
| Loop interval | 5.0s | 0.15–60s |
| Max hold time | 30s | 10–120s |
| Cooldown | 10s | 1–60s (or disabled via Settings) |
| AP SSID | "FogControl" | Change `AP_SSID` |

## License

MIT
