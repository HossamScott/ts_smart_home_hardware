#ifndef SMARTPLUG_SDK_H
#define SMARTPLUG_SDK_H

#include <Arduino.h>
#include "device_state.h"
#include <WebSocketsClient.h>

// ============================================================
// SmartPlug SDK — Public API
//
// This library handles all cloud communication, WiFi/BLE
// provisioning, and device lifecycle management.
// You only need to register your hardware and handle callbacks.
// ============================================================

class SmartPlugSDK {
public:
    SmartPlugSDK();
    ~SmartPlugSDK();

    // ── Node Registration (call before begin()) ──────────────

    // Register a relay output. Returns the node index (0-7).
    // Max 8 relays per device.
    // Optionally pass a PowerRating for estimated power when no PZEM sensor.
    uint8_t addRelay(uint8_t pin, bool activeHigh = true);
    uint8_t addRelay(uint8_t pin, bool activeHigh, PowerRating rating);

    // Register a PZEM-004T power sensor on UART.
    // Associates the sensor with the last added relay's node.
    // Returns the sensor index. Max 8 sensors.
    uint8_t addPowerSensor(uint8_t rxPin, uint8_t txPin, uint8_t modbusAddr = 0x01);

    // ── Callbacks ────────────────────────────────────────────

    // Called when cloud sends relay on/off command.
    // You MUST control your GPIO here.
    void onRelayCommand(void (*cb)(uint8_t nodeIndex, bool on));

    // Called before factory reset. Clean up custom NVS data here.
    void onFactoryReset(void (*cb)());

    // Called before reboot. Save state if needed.
    void onReboot(void (*cb)());

    // ── Lifecycle ────────────────────────────────────────────

    // Initialize SDK. Call after all addRelay/addPowerSensor/callbacks.
    // Starts WiFi, cloud connection, and BLE provisioning if needed.
    void begin(uint8_t ledPin = 2);

    // Must be called every loop iteration.
    // Handles WiFi, WebSocket, BLE, heartbeat, power reporting.
    void loop();

    // ── State ────────────────────────────────────────────────

    // Report relay state change to SDK (for cloud sync).
    // Called inside your onRelayCommand callback or from your own logic.
    void setRelay(uint8_t nodeIndex, bool on);

    // Get current relay state for a node.
    bool getRelay(uint8_t nodeIndex);

    // Push a power reading to SDK (for custom / non-PZEM sensors).
    // SDK auto-reads PZEM sensors, so only use this for custom sensors.
    void updatePower(uint8_t nodeIndex, PowerReading reading);

    // Returns true when it's time to read power (based on interval).
    // Use in your custom sensor loop.
    bool shouldReadPower(uint8_t nodeIndex);

    // ── Info ─────────────────────────────────────────────────

    // Number of registered relays.
    uint8_t getRelayCount();

    // Number of registered power sensors.
    uint8_t getSensorCount();

    // Device serial number (auto-generated, e.g., "SP-A1B2C").
    String getSerial();

    // True if connected to cloud server.
    bool isConnected();

    // True if a user has claimed this device in the mobile app.
    bool isClaimed();

    // Get current device state.
    DeviceState getState();

    // Internal use only — do not call directly
    void _onWsEvent(WStype_t type, uint8_t* payload, size_t length);

private:
    // Internal state
    DeviceStatus _status;
    uint8_t _ledPin;

    // Callbacks
    void (*_relayCb)(uint8_t, bool);
    void (*_factoryResetCb)();
    void (*_rebootCb)();

    // Internal methods
    void _generateSerial();
    void _initBLE();
    void _startBLEProvisioning();
    void _stopBLE();
    void _startSoftAP();
    void _connectToWiFi();
    void _connectToCloud();
    void _handleCloudMessage(uint8_t* payload, size_t length);
    void _sendStatusToCloud();
    void _sendPowerToCloud(uint8_t nodeIndex);
    void _readPower(uint8_t nodeIndex);
    void _updateLED();
    void _checkRecovery();
    void _saveOfflineSnapshot();
    void _sendOfflineEnergyReport();

    // Timers
    unsigned long _lastPowerRead;
    unsigned long _lastPowerReport;
    unsigned long _lastHeartbeat;
    unsigned long _lastWiFiRetry;
    unsigned long _setupModeStart;
    unsigned long _reconnectStart;
    int _wifiRetryCount;

    // Offline energy tracking
    bool          _hasOfflineData;
    unsigned long _offlineStartMs;
    unsigned long _lastOfflineNvsSave;
    float         _offlineStartEnergy[SMARTPLUG_MAX_NODES];    // RAM energy baseline at start of THIS boot's offline period
    float         _offlinePreRebootAccum[SMARTPLUG_MAX_NODES]; // Energy delta accumulated in previous boots while offline

    // Cloud-fail BLE recovery (WiFi connected, cloud unreachable)
    unsigned long _cloudFailStart;     // When cloud first went down while WiFi was up (0 = not tracking)
    bool          _cloudFailBleActive; // True when BLE started due to cloud being unreachable
    unsigned long _lastWsRetry;        // Last time _webSocket.loop() was called during cloud-fail BLE recovery

    // BLE state
    bool _bleInitialized;
    bool _bleClientConnected;
    bool _blePendingStart;        // Flag: loop() should start BLE safely
    bool _blePhaseActive;         // True when in BLE advertising phase (cloud disconnected)
    bool _waitingForClaim;        // True after BLE provisioned WiFi — stay on cloud for serial claim
    bool _wifiRecoveryBleActive;  // True when BLE active during WiFi recovery (claimed device)
    unsigned long _blePhaseStart; // When BLE phase began
    String _bleReceivedSSID;
    String _bleReceivedPass;
    bool _bleCredentialsReceived;

    // Forward-declared BLE pointers (void* to avoid exposing BLE headers)
    void* _bleServer;
    void* _bleCharSSID;
    void* _bleCharPass;
    void* _bleCharStatus;

    friend class _SDKBLEServerCB;
    friend class _SDKSSIDCharCB;
    friend class _SDKPassCharCB;
};

#endif // SMARTPLUG_SDK_H
