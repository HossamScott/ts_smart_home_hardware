#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// ============================================================
// Hardware Pin Configuration
// Modify these to match YOUR hardware wiring
// ============================================================

// --- Relay Outputs ---
// Add as many relay pins as your device has (max 8)
#define RELAY_PIN_1       4    // GPIO for relay 1 (change to your relay GPIO)
// #define RELAY_PIN_2    27    // Uncomment for 2nd relay
// #define RELAY_PIN_3    32    // Uncomment for 3rd relay
// #define RELAY_PIN_4    33    // Uncomment for 4th relay

// Set to true if relay turns ON when pin is HIGH
#define RELAY_ACTIVE_HIGH true

// --- Device Power Ratings ---
// Set these to match your actual device specs.
// Used for power reporting when no PZEM sensor is connected.
#define RATED_WATTS_1       60.0    // Relay 1: rated power in watts  (e.g. 60W lamp)
#define RATED_VOLTAGE_1    220.0    // Relay 1: rated voltage (V)
#define RATED_FREQUENCY_1   50.0    // Relay 1: grid frequency (Hz)
#define RATED_PF_1           1.0    // Relay 1: power factor (0.0 - 1.0)

// Uncomment for multi-gang (match your RELAY_PIN defines)
// #define RATED_WATTS_2     100.0
// #define RATED_VOLTAGE_2   220.0
// #define RATED_FREQUENCY_2  50.0
// #define RATED_PF_2          0.95

// #define RATED_WATTS_3      40.0
// #define RATED_VOLTAGE_3   220.0
// #define RATED_FREQUENCY_3  50.0
// #define RATED_PF_3          1.0

// #define RATED_WATTS_4      75.0
// #define RATED_VOLTAGE_4   220.0
// #define RATED_FREQUENCY_4  50.0
// #define RATED_PF_4          0.95

// --- Power Monitoring (PZEM-004T) ---
// Set to 0 if no power sensor is connected
#define PZEM_RX_PIN       16   // UART RX for PZEM sensor
#define PZEM_TX_PIN       17   // UART TX for PZEM sensor

// --- Status LED ---
#define LED_STATUS_PIN    2    // Built-in LED (GPIO 2 = onboard blue LED)

// ============================================================
// Manufacturing Info
// Set these values before flashing each production batch.
// They are sent to the cloud on first connect for analytics.
// ============================================================
#define MFG_MANUFACTURER    "Techs Solutions"   // Company / factory name
#define MFG_BATCH_ID        "2026-Q1-B001"      // Production batch / lot number
#define MFG_DATE            "2026-02-28"        // Date of manufacture (YYYY-MM-DD)

// ============================================================
// Developer Token
// Register at techs-solutions.com/developer to get your token.
// This links devices you ship to your developer account so you 
// can track registrations and usage in your dashboard.
// Leave as-is for development; replace before production.
// ============================================================
#define SDK_DEVELOPER_TOKEN    "paste-your-token-here"

#endif // USER_CONFIG_H
