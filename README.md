# TS Smart Home — ESP32 SDK

Turn any ESP32 into a cloud-connected smart plug in **under 5 minutes**.

> **How it works:**
> You flash this SDK on your ESP32 hardware. The device auto-connects to
> [techs-solutions.com](https://techs-solutions.com). Your customers download
> the **TS Smart Home** app, scan or enter the device serial, and claim it.
> You host nothing — the cloud and the app are provided for you.

---

## Step 0 — Register as a Developer

Before flashing, you need a **free developer token** so we can link your
devices to your account and show you usage insights.

1. Visit **[techs-solutions.com/en/developer](https://techs-solutions.com/en/developer)**
2. Fill in your name, company, email, and country
3. Verify your email address
4. Copy your **SDK_DEVELOPER_TOKEN**

Then open `main/config.h` and paste it:

```cpp
#define SDK_DEVELOPER_TOKEN    "paste-your-token-here"
```

That's it. Every device you ship will be linked to your account.

---

You only ever touch **two files**: `config.h` and `main.ino`.

---

## Quick Start

### 1. Wire Your Hardware

| Component | ESP32 GPIO | Notes |
|-----------|-----------|-------|
| Relay signal | Any free GPIO (e.g. **4**) | Avoid GPIO 2 (built-in LED) |
| Built-in LED | **GPIO 2** | Used by SDK for status blink |
| PZEM-004T RX | **GPIO 16** | Optional — for real power readings |
| PZEM-004T TX | **GPIO 17** | Optional — for real power readings |

---

### 2. Edit `config.h`

This is the **only config file** you need. Open it and set your values:

```cpp
// ── Which GPIO is your relay on? ─────────────────────────
#define RELAY_PIN_1       4       // Change to your relay GPIO

// ── Does HIGH = ON for your relay? ───────────────────────
#define RELAY_ACTIVE_HIGH true    // false for active-low relays

// ── What device is plugged in? ───────────────────────────
#define RATED_WATTS_1     60.0   // e.g. 60W lamp, 1200W kettle
#define RATED_VOLTAGE_1   220.0  // 220V (Egypt/EU) or 110V (US)
#define RATED_FREQUENCY_1  50.0  // 50Hz (Egypt/EU) or 60Hz (US)
#define RATED_PF_1          1.0  // 1.0 for resistive (lamps, heaters)
                                 // 0.8-0.95 for motors/compressors

// ── PZEM power sensor (leave as-is if not connected) ─────
#define PZEM_RX_PIN       16
#define PZEM_TX_PIN       17
```

**Common device ratings:**

| Device | Watts | Voltage | PF |
|--------|-------|---------|-----|
| 60W Lamp | 60 | 220 | 1.0 |
| LED Bulb 9W | 9 | 220 | 0.9 |
| Kettle 1500W | 1500 | 220 | 1.0 |
| Fan 80W | 80 | 220 | 0.85 |
| AC 1.5HP | 1100 | 220 | 0.85 |
| PC Desktop | 300 | 220 | 0.9 |

---

### 3. `main.ino` — No Changes Needed for Single Relay

The default `main.ino` works out of the box for a single relay.
Just flash it and it reads everything from `config.h` automatically:

```cpp
#include "SmartPlugSDK.h"
#include "config.h"

SmartPlugSDK plug;
const uint8_t relayPins[] = { RELAY_PIN_1 };

void onRelay(uint8_t nodeIndex, bool on) {
    digitalWrite(relayPins[nodeIndex],
                 RELAY_ACTIVE_HIGH ? (on ? HIGH : LOW)
                                   : (on ? LOW  : HIGH));
}

void setup() {
    Serial.begin(115200);

    plug.addRelay(RELAY_PIN_1, RELAY_ACTIVE_HIGH,
                  { RATED_WATTS_1, RATED_VOLTAGE_1,
                    RATED_FREQUENCY_1, RATED_PF_1 });

    plug.onRelayCommand(onRelay);
    plug.begin(LED_STATUS_PIN);
}

void loop() {
    plug.loop();
}
```

---

## Multi-Gang Smart Plug (2–4 Relays)

### Step 1 — Uncomment extra pins in `config.h`

```cpp
#define RELAY_PIN_1       4
#define RELAY_PIN_2      27
#define RELAY_PIN_3      32
#define RELAY_PIN_4      33

#define RELAY_ACTIVE_HIGH true

// Relay 1 — 60W lamp
#define RATED_WATTS_1     60.0
#define RATED_VOLTAGE_1  220.0
#define RATED_FREQUENCY_1 50.0
#define RATED_PF_1         1.0

// Relay 2 — 1500W kettle
#define RATED_WATTS_2    1500.0
#define RATED_VOLTAGE_2  220.0
#define RATED_FREQUENCY_2 50.0
#define RATED_PF_2         1.0

// Relay 3 — 80W fan
#define RATED_WATTS_3     80.0
#define RATED_VOLTAGE_3  220.0
#define RATED_FREQUENCY_3 50.0
#define RATED_PF_3         0.85

// Relay 4 — 300W PC
#define RATED_WATTS_4    300.0
#define RATED_VOLTAGE_4  220.0
#define RATED_FREQUENCY_4 50.0
#define RATED_PF_4         0.9
```

### Step 2 — Uncomment the extra `addRelay` calls in `main.ino`

```cpp
const uint8_t relayPins[] = { RELAY_PIN_1, RELAY_PIN_2, RELAY_PIN_3, RELAY_PIN_4 };

void setup() {
    Serial.begin(115200);

    plug.addRelay(RELAY_PIN_1, RELAY_ACTIVE_HIGH,
                  { RATED_WATTS_1, RELAY_VOLTAGE_1, RATED_FREQUENCY_1, RATED_PF_1 });
    plug.addRelay(RELAY_PIN_2, RELAY_ACTIVE_HIGH,
                  { RATED_WATTS_2, RELAY_VOLTAGE_2, RATED_FREQUENCY_2, RATED_PF_2 });
    plug.addRelay(RELAY_PIN_3, RELAY_ACTIVE_HIGH,
                  { RATED_WATTS_3, RELAY_VOLTAGE_3, RATED_FREQUENCY_3, RATED_PF_3 });
    plug.addRelay(RELAY_PIN_4, RELAY_ACTIVE_HIGH,
                  { RATED_WATTS_4, RELAY_VOLTAGE_4, RATED_FREQUENCY_4, RATED_PF_4 });

    plug.onRelayCommand(onRelay);
    plug.begin(LED_STATUS_PIN);
}
```

The `onRelay` callback works for any number of relays — `nodeIndex` tells you which one fired (0 = relay 1, 1 = relay 2, etc.).

---

## With Real Power Sensor (PZEM-004T)

When a PZEM is wired, the SDK reads **live watts, voltage, current, and energy**.
The rated values in `config.h` become the fallback if the sensor disconnects.

```cpp
void setup() {
    plug.addRelay(RELAY_PIN_1, RELAY_ACTIVE_HIGH,
                  { RATED_WATTS_1, RATED_VOLTAGE_1, RATED_FREQUENCY_1, RATED_PF_1 });

    // Add real sensor — automatically overrides rated values
    plug.addPowerSensor(PZEM_RX_PIN, PZEM_TX_PIN);

    plug.onRelayCommand(onRelay);
    plug.begin(LED_STATUS_PIN);
}
```

> If `PZEM_RX_PIN` and `PZEM_TX_PIN` are both `> 0` in `config.h`,
> the default `main.ino` already adds the sensor automatically.

---

## `addRelay()` — Full Reference

```cpp
// Without power rating (power shows as 0 in app)
plug.addRelay(uint8_t pin, bool activeHigh = true);

// With power rating (recommended — shows estimated power in app)
plug.addRelay(uint8_t pin, bool activeHigh, PowerRating rating);
```

**PowerRating fields:**

```cpp
struct PowerRating {
    float watts;         // Device rated power in watts
    float voltage;       // Mains voltage (220.0 or 110.0)
    float frequency;     // Grid frequency (50.0 or 60.0)
    float power_factor;  // 1.0 for resistive, 0.8–0.95 for motors
};
```

The SDK **auto-calculates current** from:
`current (A) = watts / (voltage × power_factor)`

So you never need to set amperes manually.

---

## LED Status Blink Pattern

The onboard LED (GPIO 2) tells you what the device is doing:

| Pattern | Meaning |
|---------|---------|
| Fast blink (200ms) | Setup mode — waiting for WiFi via BLE/app |
| Slow blink (1000ms) | Connecting to WiFi or cloud |
| Solid ON | Connected to cloud, device claimed, fully active |
| 3 quick flashes | Factory reset triggered |
| Off | Device unclaimed or error |

---

## Device States

The SDK goes through these states automatically:

```
FACTORY_NEW → SETUP_MODE → CONNECTING → REGISTERING → UNCLAIMED → ACTIVE
                                                                      ↕
                                                               RECONNECTING
```

| State | What it means |
|-------|--------------|
| `STATE_FACTORY_NEW` | First boot ever — no WiFi saved |
| `STATE_SETUP_MODE` | BLE provisioning active — open the app to connect |
| `STATE_CONNECTING` | Joining saved WiFi network |
| `STATE_REGISTERING` | WiFi OK — connecting to cloud server |
| `STATE_UNCLAIMED` | On cloud — waiting for someone to claim it in the app |
| `STATE_ACTIVE` | Fully operational — commands work, power reported |
| `STATE_RECONNECTING` | Lost connection — SDK will auto-reconnect |
| `STATE_RECOVERY` | Too many failures — AP mode for reconfiguration |

---

## Optional Callbacks

```cpp
// Called before factory reset — clean up your own stored data
void onFactoryReset() {
    // e.g. clear your custom NVS keys
}

// Called before reboot — save any state if needed
void onReboot() {
    // e.g. flush buffers
}

void setup() {
    // ...
    plug.onFactoryReset(onFactoryReset);
    plug.onReboot(onReboot);
    // ...
}
```

---

## Useful SDK Methods

```cpp
plug.isConnected()      // true if WebSocket to cloud is open
plug.isClaimed()        // true if a user has claimed this device
plug.getSerial()        // returns "SP-A1B2C3D4E5F6" (auto-generated from MAC)
plug.getRelay(index)    // current ON/OFF state of relay at index
plug.getRelayCount()    // how many relays are registered
plug.getState()         // current DeviceState enum value
```

---

## Factory Reset

Hold the BOOT button (GPIO 0) for **5 seconds** while the device is running.
This clears WiFi credentials and claimed status.
The device returns to `SETUP_MODE` and is ready to be configured again.

---

## Common Issues

| Problem | Fix |
|---------|-----|
| Relay not switching | Check `RELAY_ACTIVE_HIGH` — try `false` if relay is inverted |
| LED always off | Make sure `LED_STATUS_PIN 2` matches your board's built-in LED |
| Power shows 0 in app | Add `{ RATED_WATTS_1, ... }` to `addRelay()` |
| Device not found in app | Hold BOOT 5s to reset, then re-provision via BLE |
| GPIO conflict | Never use GPIO 2 for a relay — it is reserved for the status LED |

---

## File Overview

```
main/
├── main.ino          ← You edit this (add relays, callbacks)
├── config.h          ← You edit this (pins, watts, voltage, developer token)
├── SmartPlugSDK.h    ← SDK public API — read-only reference
├── SmartPlugSDK.cpp  ← SDK implementation — do not modify
├── sdk_config.h      ← Server URL — do not modify
└── device_state.h    ← Structs (PowerRating, NodeConfig) — read-only
```

---

## How Your Customers Use the Device

Once you flash and ship the hardware, your customers:

1. **Download the TS Smart Home app**
   - Android: [Google Play Store](https://play.google.com/store)
   - iOS: [App Store](https://apps.apple.com)

2. **Power on the device** — the LED blinks fast (setup mode)

3. **Open the app → Add Device** → scan the QR code or enter the serial number
   printed on the device (format: `SP-XXXXX`)

4. **Follow the in-app steps** to connect the device to their WiFi network

5. **The device is now live** — they can control it, monitor power usage,
   set schedules, and share access with family

---

## Security Notes

- Device **serial numbers** are auto-generated from the ESP32 MAC address —
  unique per chip, no two devices are the same
- Device **secret keys** are randomly generated from hardware entropy on first
  boot, stored in NVS — never in the firmware code
- Your `SDK_DEVELOPER_TOKEN` is not a secret — it is a usage tracking ID,
  not an authentication credential

---

## Contact & Support

- **Developer support:** support@techs-solutions.com
- **Register / developer dashboard:** [techs-solutions.com/en/developer](https://techs-solutions.com/en/developer)
- **Website:** [techs-solutions.com](https://techs-solutions.com)
