// ============================================================
// Smart Plug / Smart Switch — Developer Main File
//
// This is the ONLY file you need to modify.
// The SmartPlugSDK handles all cloud, WiFi, BLE, and protocol
// communication automatically.
//
// Steps:
//   1. Configure your pins in config.h
//   2. Register relays & sensors below
//   3. Implement the onRelay callback
//   4. Flash and go!
// ============================================================

#include <Arduino.h>
#include "SmartPlugSDK.h"  
#include "config.h"

SmartPlugSDK plug;

// Store relay pins for easy access in callback
const uint8_t relayPins[] = { RELAY_PIN_1 };
// For multi-gang: const uint8_t relayPins[] = { RELAY_PIN_1, RELAY_PIN_2, RELAY_PIN_3, RELAY_PIN_4 };

// ── Called when cloud sends relay ON/OFF command ─────────────
// You MUST control your hardware GPIO here.
void onRelay(uint8_t nodeIndex, bool on) {
    if (nodeIndex < sizeof(relayPins)) {
        if (RELAY_ACTIVE_HIGH) {
            digitalWrite(relayPins[nodeIndex], on ? HIGH : LOW);
        } else {
            digitalWrite(relayPins[nodeIndex], on ? LOW : HIGH);
        }
    }
}

// ── Optional: called before factory reset ────────────────────
// void onFactoryReset() {
//     // Clean up your own NVS data here if needed
// }

// ── Optional: called before reboot ───────────────────────────
// void onReboot() {
//     // Save any custom state here if needed
// }

void setup() {
    Serial.begin(115200);

    // Register relay(s) with rated power specs.
    // The SDK uses these values for power reporting when no PZEM sensor is connected.
    // Current is auto-calculated from: watts / (voltage * power_factor)
    plug.addRelay(RELAY_PIN_1, RELAY_ACTIVE_HIGH,
                  { RATED_WATTS_1, RATED_VOLTAGE_1, RATED_FREQUENCY_1, RATED_PF_1 });

    // Uncomment for multi-gang:
    // plug.addRelay(RELAY_PIN_2, RELAY_ACTIVE_HIGH,
    //               { RATED_WATTS_2, RATED_VOLTAGE_2, RATED_FREQUENCY_2, RATED_PF_2 });
    // plug.addRelay(RELAY_PIN_3, RELAY_ACTIVE_HIGH,
    //               { RATED_WATTS_3, RATED_VOLTAGE_3, RATED_FREQUENCY_3, RATED_PF_3 });
    // plug.addRelay(RELAY_PIN_4, RELAY_ACTIVE_HIGH,
    //               { RATED_WATTS_4, RATED_VOLTAGE_4, RATED_FREQUENCY_4, RATED_PF_4 });

    // Register power sensor (optional — omit if no PZEM connected)
    // When a PZEM is connected, real readings override the rated values above.
    if (PZEM_RX_PIN > 0 && PZEM_TX_PIN > 0) {
        plug.addPowerSensor(PZEM_RX_PIN, PZEM_TX_PIN);
    }
 
    // Set callbacks
    plug.onRelayCommand(onRelay);
    // plug.onFactoryReset(onFactoryReset);
    // plug.onReboot(onReboot);

    // Start SDK — connects WiFi, cloud, and BLE provisioning
    plug.begin(LED_STATUS_PIN);
}

void loop() {
    plug.loop();
}
