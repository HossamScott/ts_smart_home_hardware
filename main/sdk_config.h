#ifndef SMARTPLUG_SDK_CONFIG_H
#define SMARTPLUG_SDK_CONFIG_H

// ============================================================
// SmartPlug SDK Internal Configuration
// DO NOT MODIFY — these are managed by the SDK
// ============================================================

// --- Device Identity ---
#define SDK_DEVICE_MODEL          "SP-1000"
#define SDK_FIRMWARE_VERSION      "1.1.0"
#define SDK_HARDWARE_VERSION      "1.0"

// --- Cloud Server ---
#define SDK_CLOUD_WS_HOST         "techs-solutions.com"
#define SDK_CLOUD_WS_PORT         443
#define SDK_CLOUD_WS_PATH         "/smartplug/ws/device"
#define SDK_CLOUD_USE_SSL         true
#define SDK_CLOUD_API_BASE        "https://techs-solutions.com/smartplug/api"

// --- WiFi Provisioning ---
#define SDK_AP_PREFIX             "SmartPlug_"
#define SDK_AP_PASSWORD           ""
#define SDK_SETUP_TIMEOUT_MS      300000  // 5 minutes

// --- Timing ---
#define SDK_POWER_READ_INTERVAL   2000    // Read every 2 seconds
#define SDK_POWER_REPORT_INTERVAL 10000   // Report to cloud every 10 seconds
#define SDK_HEARTBEAT_INTERVAL    30000   // Ping cloud every 30 seconds
#define SDK_WIFI_RETRY_INTERVAL   5000
#define SDK_WS_RETRY_INTERVAL     5000
#define SDK_MAX_WIFI_RETRIES      60      // 5 min of retries before recovery
#define SDK_RECOVERY_AP_TIMEOUT   600000  // 10 min in recovery AP mode
#define SDK_WIFI_RECOVERY_RETRY_INTERVAL 10000  // 10s between WiFi retries during BLE recovery
#define SDK_CLOUD_FAIL_BLE_TIMEOUT    90000UL  // 90s cloud unreachable (WiFi up) → start BLE
#define SDK_CLOUD_FAIL_RETRY_INTERVAL 60000UL  // 60s BLE advertises, then pause to attempt cloud

// --- NVS Keys ---
#define SDK_NVS_NAMESPACE         "smartplug"
#define SDK_NVS_KEY_SERIAL        "serial"
#define SDK_NVS_KEY_SECRET        "secret"
#define SDK_NVS_KEY_CLAIMED       "claimed"

// --- Offline Energy Tracking ---
// NVS keys used to survive reboots while WiFi is down
#define SDK_OFFLINE_NVS_SAVE_INTERVAL  900000UL  // Refresh NVS delta every 15 min while offline
#define SDK_NVS_KEY_OFFLINE_FLAG       "off_flag" // bool — true while offline data pending
#define SDK_NVS_KEY_OFFLINE_MS         "off_ms"   // uint32 — millis() snapshot at offline start
#define SDK_NVS_KEY_OFFLINE_DL_PREFIX  "off_dl_"  // float prefix per node (off_dl_0 … off_dl_7)

// --- BLE Provisioning ---
#define SDK_BLE_SERVICE_UUID      "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define SDK_BLE_CHAR_SSID_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define SDK_BLE_CHAR_PASS_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define SDK_BLE_CHAR_STATUS_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26aa"

#endif // SMARTPLUG_SDK_CONFIG_H
