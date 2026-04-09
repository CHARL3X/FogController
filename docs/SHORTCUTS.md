# iOS Shortcuts Setup

Control the fog machine from your lock screen, home screen, or Siri using Apple Shortcuts and the REST API.

> **Prerequisite:** Your phone must be on the same WiFi network as the fog controller.

## Quick Start — Fog Burst Shortcut

1. Open the **Shortcuts** app
2. Tap **+** to create a new shortcut
3. Tap **Add Action**
4. Search for **Get Contents of URL**
5. Tap the URL field and type: `http://fog.local/api/burst`
6. Tap the name at the top and rename it to **Fog Burst**
7. (Optional) Tap the icon to change it — pick a cloud icon, orange color
8. Tap **Done**

That's it. Run it and the fog fires.

## Adding to Home Screen

After creating a shortcut:

1. Tap the **three dots (...)** on the shortcut
2. Tap the **share icon** (bottom of screen)
3. Tap **Add to Home Screen**
4. Choose a name and icon
5. Tap **Add**

Now it's a one-tap button on your phone.

## Useful Shortcuts

Each shortcut is the same steps — just change the URL.

### Fog Burst

```
URL: http://fog.local/api/burst
```

### Fog Stop

```
URL: http://fog.local/api/stop
```

### Start Loop

```
URL: http://fog.local/api/loop?val=1
```

### Stop Loop

```
URL: http://fog.local/api/loop?val=0
```

### Night Mode On

```
URL: http://fog.local/api/night?val=1
```

### Night Mode Off

```
URL: http://fog.local/api/night?val=0
```

## Fog Control Menu (All-in-One Shortcut)

A single shortcut with a menu to pick your action:

1. Create a new shortcut, name it **Fog Control**
2. Add a **Choose from Menu** action
3. Set the options to: `Burst`, `Loop On`, `Loop Off`, `Stop`, `Night`
4. Under each menu option, add a **Get Contents of URL** action:
   - **Burst** → `http://fog.local/api/burst`
   - **Loop On** → `http://fog.local/api/loop?val=1`
   - **Loop Off** → `http://fog.local/api/loop?val=0`
   - **Stop** → `http://fog.local/api/stop`
   - **Night** → `http://fog.local/api/night?val=1`

## Siri

Every shortcut is automatically available via Siri. Just say:

> *"Hey Siri, Fog Burst"*

Or whatever you named the shortcut.

## Set Burst Duration

To change the burst length before firing:

```
URL: http://fog.local/api/dur?val=3.0
```

Replace `3.0` with your desired duration in seconds (0.1–10).

You could add an **Ask for Input** action to make it dynamic:

1. Add **Ask for Input** → Number → prompt: "Duration (seconds)"
2. Add **Get Contents of URL** → `http://fog.local/api/dur?val=` and insert the input variable
3. Add another **Get Contents of URL** → `http://fog.local/api/burst`

## Tips

- If `fog.local` doesn't resolve on your phone, use the ESP32's IP address instead (check your router or serial monitor for the IP)
- Shortcuts work from the widget screen, home screen, or Siri — no need to open an app
- You can add shortcuts to your Apple Watch for wrist-tap fog control
- The API returns JSON state after every call — use **Get Dictionary Value** actions if you want to show status in the shortcut
