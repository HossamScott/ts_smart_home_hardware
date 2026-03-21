// ============================================================
// SmartPlug SDK — Internal Implementation
// All cloud communication, provisioning, and protocol logic
// ============================================================

#include "SmartPlugSDK.h"
#include "sdk_config.h"
#include "config.h"

#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <HardwareSerial.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// ============================================================
// Globals (internal to SDK)
// ============================================================

static Preferences _prefs;
static WebSocketsClient _webSocket;
static HardwareSerial _pzemSerial(2);
static SmartPlugSDK* _instance = nullptr;

// ============================================================
// BLE Callbacks
// ============================================================

class _SDKBLEServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer* s) override {
        if (_instance) _instance->_bleClientConnected = true;
        Serial.println("[BLE] Client connected");
    }
    void onDisconnect(BLEServer* s) override {
        if (_instance) {
            _instance->_bleClientConnected = false;
            Serial.println("[BLE] Client disconnected");
            if (_instance->_status.state == STATE_SETUP_MODE ||
                _instance->_status.state == STATE_UNCLAIMED ||
                _instance->_status.state == STATE_RECOVERY) {
                s->getAdvertising()->start();
                Serial.println("[BLE] Advertising restarted");
            }
        }
    }
};

class _SDKSSIDCharCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        if (_instance) {
            _instance->_bleReceivedSSID = String(c->getValue().c_str());
            Serial.printf("[BLE] Received SSID: %s\n", _instance->_bleReceivedSSID.c_str());
        }
    }
};

class _SDKPassCharCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        if (_instance) {
            _instance->_bleReceivedPass = String(c->getValue().c_str());
            _instance->_bleCredentialsReceived = true;
            Serial.printf("[BLE] Received password (length: %d)\n", _instance->_bleReceivedPass.length());
            BLECharacteristic* statusChar = (BLECharacteristic*)_instance->_bleCharStatus;
            if (statusChar) {
                statusChar->setValue("credentials_received");
                statusChar->notify();
            }
        }
    }
};

// ============================================================
// WebSocket Event Handler (static callback)
// ============================================================

static void _wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    if (_instance) _instance->_onWsEvent(type, payload, length);
}

void SmartPlugSDK::_onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            _status.cloud_connected = false;
            // During BLE phase the disconnect is intentional — stay in UNCLAIMED
            // so loop() continues servicing the BLE advertising cycle.
            if (_blePhaseActive) {
                Serial.printf("[%lus] [WS] Disconnected (BLE phase — intentional)\n", millis() / 1000);
            } else if (_cloudFailRetrying) {
                // During cloud-fail retry, disconnect events are expected (stale from previous
                // connection). Do NOT change state or reset timers — the retry loop handles everything.
                Serial.printf("[%lus] [WS] Disconnected (cloud-fail retry — expected, suppressed)\n", millis() / 1000);
            } else {
                Serial.printf("[%lus] [WS] Disconnected from cloud\n", millis() / 1000);
                if (_status.state == STATE_ACTIVE || _status.state == STATE_UNCLAIMED ||
                    _status.state == STATE_REGISTERING) {
                    _status.state = STATE_RECONNECTING;
                    _reconnectStart = millis();
                }
                // Track when cloud first went down while WiFi is still up
                if (_status.wifi_connected && _cloudFailStart == 0) {
                    _cloudFailStart = millis();
                }
            }
            break;

        case WStype_CONNECTED:
            Serial.printf("[%lus] [WS] Connected to cloud\n", millis() / 1000);
            _status.cloud_connected = true;
            _status.last_cloud_contact = millis();
            // Cloud reconnected — clear cloud-fail tracking and stop cloud-fail BLE if active
            _cloudFailStart = 0;
            _cloudFailCycleCount = 0;
            if (_cloudFailBleActive) {
                _stopBLE();
                _cloudFailBleActive = false;
                Serial.printf("[%lus] [Cloud] Reconnected — stopped cloud-fail BLE\n", millis() / 1000);
            }
            {
                JsonDocument doc;
                doc["type"] = "auth";
                doc["serial"] = _status.serial;
                doc["secret"] = _status.secret_key;
                doc["firmware"] = SDK_FIRMWARE_VERSION;
                doc["hardware"] = SDK_HARDWARE_VERSION;
                doc["model"] = SDK_DEVICE_MODEL;
                doc["manufacturer"] = MFG_MANUFACTURER;
                doc["batch_id"] = MFG_BATCH_ID;
                doc["mfg_date"] = MFG_DATE;
                doc["mac"] = WiFi.macAddress();
                doc["ip"] = WiFi.localIP().toString();
                doc["relay_count"] = _status.relayCount;
                doc["sensor_count"] = _status.sensorCount;
                doc["developer_token"] = SDK_DEVELOPER_TOKEN;
                String json;
                serializeJson(doc, json);
                _webSocket.sendTXT(json);
            }
            break;

        case WStype_TEXT:
            Serial.printf("[WS] Received: %s\n", payload);
            _handleCloudMessage(payload, length);
            break;

        case WStype_PING:
        case WStype_PONG:
            _status.last_cloud_contact = millis();
            break;

        default:
            break;
    }
}

// ============================================================
// Constructor / Destructor
// ============================================================

SmartPlugSDK::SmartPlugSDK() {
    _instance = this;
    memset(&_status, 0, sizeof(_status));
    _status.state = STATE_FACTORY_NEW;
    _status.relayCount = 0;
    _status.sensorCount = 0;
    _ledPin = 2;
    _relayCb = nullptr;
    _factoryResetCb = nullptr;
    _rebootCb = nullptr;
    _lastPowerRead = 0;
    _lastPowerReport = 0;
    _lastHeartbeat = 0;
    _lastWiFiRetry = 0;
    _setupModeStart = 0;
    _reconnectStart = 0;
    _wifiRetryCount = 0;
    _cloudFailStart = 0;
    _cloudFailBleActive = false;
    _cloudFailRetrying = false;
    _cloudFailCycleCount = 0;
    _lastWsRetry = 0;
    _bleInitialized = false;
    _bleClientConnected = false;
    _blePendingStart = false;
    _blePhaseActive = false;
    _waitingForClaim = false;
    _wifiRecoveryBleActive = false;
    _blePhaseStart = 0;
    _bleCredentialsReceived = false;
    _bleServer = nullptr;
    _bleCharSSID = nullptr;
    _bleCharPass = nullptr;
    _bleCharStatus = nullptr;

    // Offline energy tracking
    _hasOfflineData = false;
    _offlineStartMs = 0;
    _lastOfflineNvsSave = 0;
    for (int i = 0; i < SMARTPLUG_MAX_NODES; i++) {
        _offlineStartEnergy[i] = 0.0f;
        _offlinePreRebootAccum[i] = 0.0f;
    }

    // Initialize all nodes
    for (int i = 0; i < SMARTPLUG_MAX_NODES; i++) {
        _status.nodes[i].relayPin = 0;
        _status.nodes[i].relayActiveHigh = true;
        _status.nodes[i].sensorRxPin = 0;
        _status.nodes[i].sensorTxPin = 0;
        _status.nodes[i].modbusAddr = 0x01;
        _status.nodes[i].hasSensor = false;
        _status.nodes[i].hasRating = false;
        _status.nodes[i].relayOn = false;
        memset(&_status.nodes[i].rating, 0, sizeof(PowerRating));
        memset(&_status.nodes[i].power, 0, sizeof(PowerReading));
    }
}

SmartPlugSDK::~SmartPlugSDK() {
    if (_instance == this) _instance = nullptr;
}

// ============================================================
// Public API — Node Registration
// ============================================================

uint8_t SmartPlugSDK::addRelay(uint8_t pin, bool activeHigh) {
    if (_status.relayCount >= SMARTPLUG_MAX_NODES) {
        Serial.println("[SDK] ERROR: Max 8 relays reached");
        return 255;
    }
    uint8_t idx = _status.relayCount;
    _status.nodes[idx].relayPin = pin;
    _status.nodes[idx].relayActiveHigh = activeHigh;
    _status.nodes[idx].hasRating = false;
    _status.relayCount++;
    Serial.printf("[SDK] Relay %d registered on GPIO %d (activeHigh=%d)\n", idx, pin, activeHigh);
    return idx;
}

uint8_t SmartPlugSDK::addRelay(uint8_t pin, bool activeHigh, PowerRating rating) {
    if (_status.relayCount >= SMARTPLUG_MAX_NODES) {
        Serial.println("[SDK] ERROR: Max 8 relays reached");
        return 255;
    }
    uint8_t idx = _status.relayCount;
    _status.nodes[idx].relayPin = pin;
    _status.nodes[idx].relayActiveHigh = activeHigh;
    _status.nodes[idx].hasRating = true;
    _status.nodes[idx].rating = rating;
    _status.relayCount++;
    Serial.printf("[SDK] Relay %d registered on GPIO %d (activeHigh=%d, rated=%.1fW/%.0fV)\n",
                  idx, pin, activeHigh, rating.watts, rating.voltage);
    return idx;
}

uint8_t SmartPlugSDK::addPowerSensor(uint8_t rxPin, uint8_t txPin, uint8_t modbusAddr) {
    if (_status.sensorCount >= SMARTPLUG_MAX_NODES) {
        Serial.println("[SDK] ERROR: Max 8 sensors reached");
        return 255;
    }
    // Associate sensor with the node at sensorCount index
    uint8_t idx = _status.sensorCount;
    _status.nodes[idx].sensorRxPin = rxPin;
    _status.nodes[idx].sensorTxPin = txPin;
    _status.nodes[idx].modbusAddr = modbusAddr;
    _status.nodes[idx].hasSensor = true;
    _status.sensorCount++;
    Serial.printf("[SDK] Power sensor %d registered on RX=%d TX=%d addr=0x%02X\n", idx, rxPin, txPin, modbusAddr);
    return idx;
}

// ============================================================
// Public API — Callbacks
// ============================================================

void SmartPlugSDK::onRelayCommand(void (*cb)(uint8_t, bool)) { _relayCb = cb; }
void SmartPlugSDK::onFactoryReset(void (*cb)()) { _factoryResetCb = cb; }
void SmartPlugSDK::onReboot(void (*cb)()) { _rebootCb = cb; }

// ============================================================
// Public API — State
// ============================================================

void SmartPlugSDK::setRelay(uint8_t nodeIndex, bool on) {
    if (nodeIndex >= _status.relayCount) return;
    _status.nodes[nodeIndex].relayOn = on;

    // Persist per-node relay state
    _prefs.begin(SDK_NVS_NAMESPACE, false);
    char key[12];
    snprintf(key, sizeof(key), "relay_%d", nodeIndex);
    _prefs.putBool(key, on);
    _prefs.end();

    Serial.printf("[Relay %d] Set to %s\n", nodeIndex, on ? "ON" : "OFF");
}

bool SmartPlugSDK::getRelay(uint8_t nodeIndex) {
    if (nodeIndex >= _status.relayCount) return false;
    return _status.nodes[nodeIndex].relayOn;
}

void SmartPlugSDK::updatePower(uint8_t nodeIndex, PowerReading reading) {
    if (nodeIndex >= SMARTPLUG_MAX_NODES) return;
    _status.nodes[nodeIndex].power = reading;
}

bool SmartPlugSDK::shouldReadPower(uint8_t nodeIndex) {
    (void)nodeIndex;
    return (millis() - _lastPowerRead >= SDK_POWER_READ_INTERVAL);
}

// ============================================================
// Public API — Info
// ============================================================

uint8_t SmartPlugSDK::getRelayCount() { return _status.relayCount; }
uint8_t SmartPlugSDK::getSensorCount() { return _status.sensorCount; }
String SmartPlugSDK::getSerial() { return _status.serial; }
bool SmartPlugSDK::isConnected() { return _status.cloud_connected; }
bool SmartPlugSDK::isClaimed() { return _status.claimed; }
DeviceState SmartPlugSDK::getState() { return _status.state; }

// ============================================================
// Lifecycle — begin()
// ============================================================

void SmartPlugSDK::begin(uint8_t ledPin) {
    _ledPin = ledPin;

    Serial.println("\n==========================================");
    Serial.println("  Smart Plug ESP32 Firmware v" SDK_FIRMWARE_VERSION);
    Serial.println("  SmartPlug SDK");
    Serial.println("==========================================\n");

    // ── Validate developer token ──────────────────────────────
    // A valid developer token is REQUIRED for production devices.
    // Register at techs-solutions.com/developer to get your token.
    {
        const char* token = SDK_DEVELOPER_TOKEN;
        bool tokenInvalid = (!token || strlen(token) == 0 ||
                             strcmp(token, "paste-your-token-here") == 0 ||
                             strcmp(token, "your-developer-token-here") == 0);
        if (tokenInvalid) {
            Serial.println("\n!!! FATAL: Developer token not configured !!!");
            Serial.println("Open config.h and set SDK_DEVELOPER_TOKEN to your token.");
            Serial.println("Register at techs-solutions.com/developer to get one.");
            Serial.println("Device will NOT start without a valid developer token.\n");
            // Blink LED rapidly to indicate configuration error
            pinMode(ledPin, OUTPUT);
            while (true) {
                digitalWrite(ledPin, HIGH); delay(100);
                digitalWrite(ledPin, LOW);  delay(100);
            }
        }
        Serial.printf("[Init] Developer Token: %.4s****\n", token);
    }

    // Initialize LED
    pinMode(_ledPin, OUTPUT);

    // Initialize relay pins
    for (uint8_t i = 0; i < _status.relayCount; i++) {
        pinMode(_status.nodes[i].relayPin, OUTPUT);
        // Start with relay OFF
        if (_status.nodes[i].relayActiveHigh) {
            digitalWrite(_status.nodes[i].relayPin, LOW);
        } else {
            digitalWrite(_status.nodes[i].relayPin, HIGH);
        }
    }

    // Initialize power monitor serial (first sensor's pins)
    if (_status.sensorCount > 0) {
        _pzemSerial.begin(9600, SERIAL_8N1,
            _status.nodes[0].sensorRxPin, _status.nodes[0].sensorTxPin);
    }

    // Generate or load device serial
    _generateSerial();
    Serial.printf("[Init] Device Serial: %s\n", _status.serial.c_str());
    Serial.printf("[Init] Claimed: %s\n", _status.claimed ? "Yes" : "No");
    Serial.printf("[Init] Relays: %d, Sensors: %d\n", _status.relayCount, _status.sensorCount);

    // Restore per-node relay state + offline energy tracking state
    _prefs.begin(SDK_NVS_NAMESPACE, true);
    String savedSSID = _prefs.getString("wifi_ssid", "");
    for (uint8_t i = 0; i < _status.relayCount; i++) {
        char key[12];
        snprintf(key, sizeof(key), "relay_%d", i);
        bool savedRelay = _prefs.getBool(key, false);
        if (savedRelay) {
            _status.nodes[i].relayOn = true;
            // Call the user's relay callback to set the hardware
            if (_relayCb) {
                _relayCb(i, true);
            }
        }
    }

    // Restore offline energy state if device rebooted while offline
    bool offFlag = _prefs.getBool(SDK_NVS_KEY_OFFLINE_FLAG, false);
    if (offFlag) {
        _hasOfflineData = true;
        _offlineStartMs = millis();  // duration continues from now (RAM energy starts at 0 after reboot)
        char key[12];
        for (uint8_t i = 0; i < _status.relayCount; i++) {
            _offlineStartEnergy[i] = 0.0f;  // RAM reset to 0 after reboot → new baseline is 0
            snprintf(key, sizeof(key), "%s%d", SDK_NVS_KEY_OFFLINE_DL_PREFIX, i);
            _offlinePreRebootAccum[i] = _prefs.getFloat(key, 0.0f);  // carry forward what was tracked before reboot
        }
        _lastOfflineNvsSave = millis();
        Serial.println("[Offline] Restored offline state from NVS (was offline before reboot)");
    }
    _prefs.end();

    // Check if we have WiFi credentials
    if (savedSSID.length() == 0) {
        _status.state = STATE_SETUP_MODE;
        Serial.println("[Init] No WiFi configured - entering setup mode");
        _startBLEProvisioning();
        _startSoftAP();
        _setupModeStart = millis();
    } else {
        // Start BT controller early so BLE can be enabled later without crash.
        // This is lightweight — it does NOT allocate the full BLE stack yet,
        // just ensures the controller is ready for coexistence with WiFi.
        btStart();

        // If unclaimed but has WiFi, check cloud first — user may have
        // claimed via serial while device was off.
        if (!_status.claimed) {
            _waitingForClaim = true;
        }

        _connectToWiFi();
        if (_status.wifi_connected) {
            _connectToCloud();
        }
    }
}

// ============================================================
// Lifecycle — loop()
// ============================================================

void SmartPlugSDK::loop() {
    unsigned long now = millis();
    _updateLED();

    // ── Setup mode ────────────────────────────────────────────
    if (_status.state == STATE_SETUP_MODE) {
        if (_bleCredentialsReceived) {
            _bleCredentialsReceived = false;
            Serial.printf("[Setup] Received WiFi: SSID=%s\n", _bleReceivedSSID.c_str());

            BLECharacteristic* statusChar = (BLECharacteristic*)_bleCharStatus;
            if (statusChar) {
                statusChar->setValue("connecting");
                statusChar->notify();
            }

            _prefs.begin(SDK_NVS_NAMESPACE, false);
            _prefs.putString("wifi_ssid", _bleReceivedSSID);
            _prefs.putString("wifi_pass", _bleReceivedPass);
            _prefs.end();

            _stopBLE();
            _wifiRetryCount = 0;
            _connectToWiFi();

            if (_status.wifi_connected) {
                statusChar = (BLECharacteristic*)_bleCharStatus;
                if (statusChar) {
                    statusChar->setValue("connected");
                    statusChar->notify();
                }
                delay(500);

                // Deinit BLE to free heap for SSL. If BLE needed again, device reboots.
                _stopBLE();
                BLEDevice::deinit(true);
                _bleInitialized = false;
                _bleServer = nullptr;
                _bleCharSSID = nullptr;
                _bleCharPass = nullptr;
                _bleCharStatus = nullptr;
                delay(300);

                _waitingForClaim = true;
                _connectToCloud();
            } else {
                Serial.println("[Setup] WiFi connection failed, restarting setup");
                _prefs.begin(SDK_NVS_NAMESPACE, false);
                _prefs.remove("wifi_ssid");
                _prefs.remove("wifi_pass");
                _prefs.end();

                delay(1000);
                _startSoftAP();
                _startBLEProvisioning();

                statusChar = (BLECharacteristic*)_bleCharStatus;
                if (statusChar) {
                    statusChar->setValue("auth_failed");
                    statusChar->notify();
                }
                _setupModeStart = millis();
            }
        }

        if (now - _setupModeStart > SDK_SETUP_TIMEOUT_MS) {
            Serial.println("[Setup] Timeout - rebooting");
            ESP.restart();
        }
        return;
    }

    // ── Unclaimed: BLE discovery until paired ──────────
    // Device stays on BLE indefinitely until user sends WiFi credentials.
    // After WiFi provisioned, BLE is deinited and device connects to cloud for serial claim.
    // If unclaimed again later, device reboots to get BLE back.
    if (_status.state == STATE_UNCLAIMED) {

        // --- Trigger: start BLE phase ---
        if (_blePendingStart && !_blePhaseActive) {
            _blePendingStart = false;
            _blePhaseActive = true;
            _blePhaseStart = now;

            Serial.println("[Unclaimed] Entering BLE phase — disconnecting cloud...");
            // Fully disable WebSocket so auto-reconnect stops firing
            _webSocket.disconnect();
            _webSocket.setReconnectInterval(0);
            _status.cloud_connected = false;
            delay(300);

            _startBLEProvisioning();
            Serial.printf("[Unclaimed] BLE advertising until paired (heap: %d)\n", ESP.getFreeHeap());
            return;
        }

        // --- BLE phase active ---
        if (_blePhaseActive) {

            // Check if user sent WiFi credentials via BLE
            if (_bleCredentialsReceived) {
                _bleCredentialsReceived = false;
                Serial.printf("[Unclaimed] Received new WiFi via BLE: SSID=%s\n", _bleReceivedSSID.c_str());

                // Save new WiFi credentials
                _prefs.begin(SDK_NVS_NAMESPACE, false);
                _prefs.putString("wifi_ssid", _bleReceivedSSID);
                _prefs.putString("wifi_pass", _bleReceivedPass);
                _prefs.end();

                // Notify mobile that we're connecting
                BLECharacteristic* statusChar = (BLECharacteristic*)_bleCharStatus;
                if (statusChar) {
                    statusChar->setValue("connecting");
                    statusChar->notify();
                }

                // Connect WiFi while BLE is still alive (BLE + WiFi STA coexist fine,
                // it's only BLE + SSL WebSocket that won't fit in heap)
                _wifiRetryCount = 0;
                _connectToWiFi();

                if (_status.wifi_connected) {
                    // Notify mobile that WiFi connected — app can move to serial claim screen
                    statusChar = (BLECharacteristic*)_bleCharStatus;
                    if (statusChar) {
                        statusChar->setValue("connected");
                        statusChar->notify();
                    }
                    Serial.println("[Unclaimed] WiFi connected — notified mobile via BLE");
                    delay(500); // Give mobile time to receive the notification

                    // Fully deinit BLE to free heap for SSL WebSocket.
                    // ESP32 can't reliably reinit BLE, so if we need BLE again we reboot.
                    _stopBLE();
                    _blePhaseActive = false;
                    BLEDevice::deinit(true);
                    _bleInitialized = false;
                    _bleServer = nullptr;
                    _bleCharSSID = nullptr;
                    _bleCharPass = nullptr;
                    _bleCharStatus = nullptr;
                    delay(300);

                    Serial.println("[Unclaimed] Connecting to cloud...");
                    _waitingForClaim = true;
                    _connectToCloud();
                } else {
                    // Notify mobile that WiFi failed
                    statusChar = (BLECharacteristic*)_bleCharStatus;
                    if (statusChar) {
                        statusChar->setValue("auth_failed");
                        statusChar->notify();
                    }
                    Serial.println("[Unclaimed] WiFi failed — reverting and restarting BLE...");
                    _prefs.begin(SDK_NVS_NAMESPACE, false);
                    _prefs.remove("wifi_ssid");
                    _prefs.remove("wifi_pass");
                    _prefs.end();
                    _status.state = STATE_UNCLAIMED;
                    _blePendingStart = true;
                }
                return;
            }

            // Stay on BLE indefinitely until user sends WiFi credentials
            delay(50);  // Light loop, no WebSocket to service
            return;
        }

        // Waiting for serial claim on cloud — let WebSocket loop run below
        if (_waitingForClaim) {
            // Fall through to _webSocket.loop() below
        } else {
            // Unclaimed but not in BLE phase and not waiting — restart BLE
            _blePendingStart = true;
            return;
        }
    }

    // ── Safety: never call _webSocket.loop() during BLE phase ─
    if (_blePhaseActive || _blePendingStart || _cloudFailBleActive) {
        // Cloud-fail recovery: BLE advertises once (first 60s), then we deinit BLE
        // permanently and retry cloud forever with the freed heap. ESP32 can't reliably
        // reinit BLE after deinit, so we never restart BLE — just keep retrying cloud.
        if (_cloudFailBleActive) {
            // NOTE: Do NOT call _webSocket.loop() while BLE is active — BLE consumes too much
            // heap for TLS to work, and failed TLS attempts can corrupt WebSocket library state.

            // If WiFi itself dropped, exit cloud-fail and let WiFi recovery take over
            if (WiFi.status() != WL_CONNECTED) {
                Serial.printf("[%lus] [Cloud-Fail] WiFi lost during recovery — handing off to WiFi recovery\n", millis() / 1000);
                if (_bleInitialized) _stopBLE();
                _cloudFailBleActive = false;
                _cloudFailStart = 0;
                _status.wifi_connected = false;
                return;
            }

            // Wait interval: 60s while BLE is advertising (give user time to pair),
            // 30s when BLE is already off (retry cloud more aggressively)
            unsigned long retryInterval = _bleInitialized
                ? SDK_CLOUD_FAIL_RETRY_INTERVAL   // 60s — BLE is advertising
                : 30000UL;                         // 30s — BLE off, just retry cloud

            if (now - _reconnectStart > retryInterval) {
                _cloudFailCycleCount++;

                // Hard restart after 10 failed cycles — clean slate recovery
                if (_cloudFailCycleCount > 10) {
                    Serial.printf("[%lus] [Cloud-Fail] 10 cycles failed — rebooting for clean recovery\n", millis() / 1000);
                    delay(100);
                    ESP.restart();
                    return;
                }

                // Deinit BLE to free heap for SSL. If BLE needed again, device reboots.
                if (_bleInitialized) {
                    Serial.printf("[%lus] [Cloud-Fail] Deinit BLE to free heap for cloud retry (heap: %d, cycle: %d)\n",
                                  millis() / 1000, ESP.getFreeHeap(), _cloudFailCycleCount);
                    _stopBLE();
                    BLEDevice::deinit(true);
                    _bleInitialized = false;
                    _bleServer = nullptr;
                    _bleCharSSID = nullptr;
                    _bleCharPass = nullptr;
                    _bleCharStatus = nullptr;
                    delay(300);
                } else {
                    Serial.printf("[%lus] [Cloud-Fail] Retrying cloud connection (heap: %d, cycle: %d)\n",
                                  millis() / 1000, ESP.getFreeHeap(), _cloudFailCycleCount);
                }

                // Set flag BEFORE disconnect — suppresses WStype_DISCONNECTED handler from
                // changing state or resetting timers during the retry window
                _cloudFailRetrying = true;

                Serial.printf("[%lus] [Cloud-Fail] Connecting to cloud (heap: %d)\n",
                              millis() / 1000, ESP.getFreeHeap());
                _webSocket.disconnect();

                // Flush the stale WStype_DISCONNECTED event that the library fires on the
                // first loop() after disconnect(). Without this, the event fires inside our
                // polling loop below and the library then waits reconnectInterval (5s!) before
                // actually attempting a new connection — wasting precious time.
                _webSocket.loop();
                delay(100);

                _connectToCloud();

                // Use aggressive reconnect interval (500ms) during retry instead of the
                // normal 5000ms. This ensures that if the first TLS attempt fails, the
                // library retries almost immediately instead of wasting 5 seconds.
                _webSocket.setReconnectInterval(500);

                // Give WebSocket 30 seconds to complete DNS + TLS handshake + WS upgrade.
                unsigned long tryStart = millis();
                while (millis() - tryStart < 30000 && !_status.cloud_connected) {
                    _webSocket.loop();
                    delay(50);
                }

                // Restore normal reconnect interval and clear retry flag
                _webSocket.setReconnectInterval(SDK_WS_RETRY_INTERVAL);
                _cloudFailRetrying = false;

                if (_status.cloud_connected) {
                    Serial.printf("[%lus] [Cloud-Fail] Cloud reconnected! Recovery complete (cycle %d)\n",
                                  millis() / 1000, _cloudFailCycleCount);
                    _cloudFailCycleCount = 0;
                } else {
                    // Don't restart BLE — ESP32 can't reliably reinit after deinit.
                    // Keep heap free and retry cloud again next cycle.
                    unsigned long nextRetry = _bleInitialized ? 60 : 30;
                    Serial.printf("[%lus] [Cloud-Fail] No cloud after 30s — cycle %d, next retry in %lus (heap: %d)\n",
                                  millis() / 1000, _cloudFailCycleCount, nextRetry, ESP.getFreeHeap());
                    _reconnectStart = millis();
                }
            }
        }
        delay(50);
        return;
    }

    // ── Normal operation ─────────────────────────────────────
    _webSocket.loop();

    // Refresh `now` — event handlers fired during loop() may have set timestamps
    // via millis() that are slightly later than the `now` captured at the top of loop().
    // Without this refresh, (now - _cloudFailStart) can underflow and instantly trigger
    // BLE recovery, skipping the 90-second timeout entirely.
    now = millis();

    // Cloud-fail BLE recovery: WiFi connected but cloud unreachable for too long
    if (_status.wifi_connected && !_status.cloud_connected &&
        !_cloudFailBleActive && !_blePhaseActive && !_wifiRecoveryBleActive &&
        _cloudFailStart > 0 &&
        (now - _cloudFailStart > SDK_CLOUD_FAIL_BLE_TIMEOUT)) {
        Serial.printf("[%lus] [Cloud] Cloud unreachable for %lus — starting BLE recovery (WiFi OK, no internet)\n",
                      millis() / 1000, (now - _cloudFailStart) / 1000);
        _cloudFailBleActive = true;
        _reconnectStart = now;
        _startBLEProvisioning();
    }

    // WiFi reconnection
    if (WiFi.status() != WL_CONNECTED) {
        _status.wifi_connected = false;

        // WiFi is down — reset cloud-fail tracking (WiFi-down recovery takes over)
        _cloudFailStart = 0;
        if (_cloudFailBleActive) {
            _stopBLE();
            _cloudFailBleActive = false;
        }

        // ── Offline energy tracking ──────────────────────────────────────────
        // Take a one-time snapshot when WiFi first goes down
        if (!_hasOfflineData && _status.claimed) {
            _saveOfflineSnapshot();
            _lastOfflineNvsSave = now;
        }
        // Refresh NVS running delta every 15 min so a reboot doesn't lose data
        if (_hasOfflineData && (now - _lastOfflineNvsSave >= SDK_OFFLINE_NVS_SAVE_INTERVAL)) {
            _lastOfflineNvsSave = now;
            _prefs.begin(SDK_NVS_NAMESPACE, false);
            char key[12];
            for (uint8_t i = 0; i < _status.relayCount; i++) {
                float runningDelta = _offlinePreRebootAccum[i] +
                                     (_status.nodes[i].power.energy_kwh - _offlineStartEnergy[i]);
                snprintf(key, sizeof(key), "%s%d", SDK_NVS_KEY_OFFLINE_DL_PREFIX, i);
                _prefs.putFloat(key, runningDelta);
            }
            _prefs.end();
            Serial.println("[Offline] NVS delta refreshed");
        }

        // Retry WiFi periodically
        unsigned long retryInterval = _wifiRecoveryBleActive
            ? SDK_WIFI_RECOVERY_RETRY_INTERVAL   // 10s during BLE recovery
            : SDK_WIFI_RETRY_INTERVAL;            // 5s normal
        if (now - _lastWiFiRetry > retryInterval) {
            _lastWiFiRetry = now;
            _wifiRetryCount++;
            Serial.printf("[WiFi] Reconnecting (attempt %d)...\n", _wifiRetryCount);
            WiFi.reconnect();
        }

        // ── Claimed device: start BLE immediately after first WiFi failure ──
        // BLE + WiFi STA coexist fine (no heap issue — only BLE + SSL is a problem,
        // and there's no WebSocket when WiFi is down).
        if (_status.claimed && !_wifiRecoveryBleActive && _wifiRetryCount >= 1) {
            Serial.println("[WiFi Recovery] Claimed device lost WiFi — starting BLE immediately");
            _wifiRecoveryBleActive = true;
            _status.state = STATE_RECOVERY;

            _startBLEProvisioning();
            BLECharacteristic* statusChar = (BLECharacteristic*)_bleCharStatus;
            if (statusChar) {
                statusChar->setValue("wifi_recovery");
                statusChar->notify();
            }
        }

        // ── Handle BLE credentials received during WiFi recovery ──
        if (_wifiRecoveryBleActive && _bleCredentialsReceived) {
            _bleCredentialsReceived = false;
            Serial.printf("[WiFi Recovery] Received new WiFi via BLE: SSID=%s\n",
                          _bleReceivedSSID.c_str());

            BLECharacteristic* statusChar = (BLECharacteristic*)_bleCharStatus;
            if (statusChar) {
                statusChar->setValue("connecting");
                statusChar->notify();
            }

            // Do NOT save to NVS yet — test in memory first.
            // Old credentials remain safe in NVS in case new ones fail.

            // Stop BLE to prepare for WiFi test
            _stopBLE();

            // Fully disconnect WiFi (clears "sta is connecting" state)
            WiFi.disconnect(true);
            delay(500);

            // Try new credentials directly (bypass _connectToWiFi which reads NVS)
            Serial.printf("[WiFi Recovery] Trying new WiFi: %s\n", _bleReceivedSSID.c_str());
            WiFi.mode(WIFI_STA);
            WiFi.begin(_bleReceivedSSID.c_str(), _bleReceivedPass.c_str());

            int attempts = 0;
            while (WiFi.status() != WL_CONNECTED && attempts < 20) {
                delay(500);
                Serial.print(".");
                attempts++;
            }

            if (WiFi.status() == WL_CONNECTED) {
                // Success — NOW save new credentials to NVS
                Serial.printf("\n[WiFi Recovery] New WiFi connected! IP: %s\n",
                              WiFi.localIP().toString().c_str());
                _status.wifi_connected = true;
                _status.ip_address = WiFi.localIP().toString();
                _status.wifi_rssi = WiFi.RSSI();
                _wifiRetryCount = 0;

                _prefs.begin(SDK_NVS_NAMESPACE, false);
                _prefs.putString("wifi_ssid", _bleReceivedSSID);
                _prefs.putString("wifi_pass", _bleReceivedPass);
                _prefs.end();

                statusChar = (BLECharacteristic*)_bleCharStatus;
                if (statusChar) {
                    statusChar->setValue("connected");
                    statusChar->notify();
                }
                delay(500);

                // Deinit BLE to free heap for SSL. If BLE needed again, device reboots.
                _stopBLE();
                BLEDevice::deinit(true);
                _bleInitialized = false;
                _bleServer = nullptr;
                _bleCharSSID = nullptr;
                _bleCharPass = nullptr;
                _bleCharStatus = nullptr;
                _wifiRecoveryBleActive = false;
                delay(300);

                _status.state = STATE_ACTIVE;
                _connectToCloud();
            } else {
                // Failed — old credentials are still in NVS (untouched)
                Serial.println("\n[WiFi Recovery] New WiFi failed — keeping old credentials");

                statusChar = (BLECharacteristic*)_bleCharStatus;
                if (statusChar) {
                    statusChar->setValue("auth_failed");
                    statusChar->notify();
                }

                // Disconnect the failed attempt fully
                WiFi.disconnect(true);
                delay(500);

                // Resume WiFi retries with old credentials from NVS
                _wifiRetryCount = 0;
                WiFi.mode(WIFI_STA);
                // Let the loop() WiFi.reconnect() pick it back up with old creds
                _connectToWiFi();

                // Restart BLE advertising for another attempt
                _startBLEProvisioning();
                statusChar = (BLECharacteristic*)_bleCharStatus;
                if (statusChar) {
                    statusChar->setValue("wifi_recovery");
                    statusChar->notify();
                }
            }
            return;
        }

        // ── Unclaimed device: use old timeout-based recovery ──
        _checkRecovery();
        return;

    } else if (!_status.wifi_connected) {
        // WiFi just reconnected (either on its own or after new creds)
        _status.wifi_connected = true;
        _status.wifi_rssi = WiFi.RSSI();
        _wifiRetryCount = 0;

        // If BLE was active for WiFi recovery, shut it down to free heap
        if (_wifiRecoveryBleActive) {
            Serial.println("[WiFi Recovery] WiFi reconnected on its own — stopping BLE");
            _stopBLE();
            BLEDevice::deinit(true);
            _bleInitialized = false;
            _bleServer = nullptr;
            _bleCharSSID = nullptr;
            _bleCharPass = nullptr;
            _bleCharStatus = nullptr;
            _wifiRecoveryBleActive = false;
            delay(300);
            _status.state = STATE_ACTIVE;
        }

        Serial.println("[WiFi] Reconnected!");
        if (!_status.cloud_connected) {
            _connectToCloud();
        }
    }

    // Read power sensors
    if (now - _lastPowerRead >= SDK_POWER_READ_INTERVAL) {
        _lastPowerRead = now;
        for (uint8_t i = 0; i < _status.sensorCount; i++) {
            if (_status.nodes[i].hasSensor) {
                _readPower(i);
            }
        }
    }

    // Report power to cloud
    if (now - _lastPowerReport >= SDK_POWER_REPORT_INTERVAL) {
        _lastPowerReport = now;
        for (uint8_t i = 0; i < _status.relayCount; i++) {
            if (_status.nodes[i].hasSensor && _status.nodes[i].power.valid) {
                _sendPowerToCloud(i);
            }
        }
    }

    // Heartbeat
    if (now - _lastHeartbeat >= SDK_HEARTBEAT_INTERVAL) {
        _lastHeartbeat = now;
        if (_status.cloud_connected) {
            JsonDocument doc;
            doc["type"] = "ping";
            doc["serial"] = _status.serial;
            doc["uptime"] = millis();
            doc["free_heap"] = ESP.getFreeHeap();
            String json;
            serializeJson(doc, json);
            _webSocket.sendTXT(json);
        }
    }

    _checkRecovery();
    _status.wifi_rssi = WiFi.RSSI();
    _status.uptime_ms = millis();
}

// ============================================================
// Internal — Serial/Secret Generation
// ============================================================

void SmartPlugSDK::_generateSerial() {
    _prefs.begin(SDK_NVS_NAMESPACE, false);

    String serial = _prefs.getString(SDK_NVS_KEY_SERIAL, "");
    String secret = _prefs.getString(SDK_NVS_KEY_SECRET, "");

    if (serial.length() == 0) {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        const char* chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        char serialBuf[9];
        serialBuf[0] = 'S'; serialBuf[1] = 'P'; serialBuf[2] = '-';
        uint32_t seed = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
        randomSeed(seed);
        for (int i = 3; i < 8; i++) {
            serialBuf[i] = chars[random(36)];
        }
        serialBuf[8] = '\0';
        serial = String(serialBuf);
        _prefs.putString(SDK_NVS_KEY_SERIAL, serial);
        Serial.printf("[Init] Generated serial: %s\n", serial.c_str());
    }

    if (secret.length() == 0) {
        char secretBuf[17];
        const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
        uint64_t efuse = ESP.getEfuseMac();
        randomSeed((uint32_t)(efuse ^ (efuse >> 32)));
        for (int i = 0; i < 16; i++) {
            secretBuf[i] = chars[random(36)];
        }
        secretBuf[16] = '\0';
        secret = String(secretBuf);
        _prefs.putString(SDK_NVS_KEY_SECRET, secret);
    }

    _status.serial = serial;
    _status.secret_key = secret;
    _status.claimed = _prefs.getBool(SDK_NVS_KEY_CLAIMED, false);
    _status.firmware_version = SDK_FIRMWARE_VERSION;

    _prefs.end();
    Serial.printf("Serial: %s\n", _status.serial.c_str());
}

// ============================================================
// Internal — BLE Provisioning
// ============================================================

// One-time BLE stack + GATT setup. Called from begin() BEFORE WiFi
// so the Bluetooth controller starts with coexistence mode.
// Does NOT start advertising — call _startBLEProvisioning() for that.
void SmartPlugSDK::_initBLE() {
    if (_bleInitialized) return;
    _bleInitialized = true;

    String shortId = _status.serial.substring(3);
    String name = "SmartPlug_" + shortId;

    BLEDevice::init(name.c_str());
    BLEServer* server = BLEDevice::createServer();
    server->setCallbacks(new _SDKBLEServerCB());
    _bleServer = (void*)server;

    BLEService* svc = server->createService(SDK_BLE_SERVICE_UUID);

    BLECharacteristic* charSSID = svc->createCharacteristic(SDK_BLE_CHAR_SSID_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    charSSID->setCallbacks(new _SDKSSIDCharCB());
    _bleCharSSID = (void*)charSSID;

    BLECharacteristic* charPass = svc->createCharacteristic(SDK_BLE_CHAR_PASS_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    charPass->setCallbacks(new _SDKPassCharCB());
    _bleCharPass = (void*)charPass;

    BLECharacteristic* charStatus = svc->createCharacteristic(SDK_BLE_CHAR_STATUS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    charStatus->addDescriptor(new BLE2902());
    _bleCharStatus = (void*)charStatus;

    svc->start();

    // Configure advertising params once
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SDK_BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);

    Serial.printf("[BLE] Stack initialized as: %s\n", name.c_str());
}

// Start (or restart) BLE advertising. Lazily inits the BLE stack if needed.
void SmartPlugSDK::_startBLEProvisioning() {
    // Ensure BLE stack is initialized (safe to call multiple times)
    _initBLE();

    _bleCredentialsReceived = false;
    _bleReceivedSSID = "";
    _bleReceivedPass = "";

    BLECharacteristic* statusChar = (BLECharacteristic*)_bleCharStatus;
    if (statusChar) {
        statusChar->setValue("ready");
        statusChar->notify();
    }

    BLEDevice::getAdvertising()->start();

    String shortId = _status.serial.substring(3);
    Serial.printf("[BLE] Advertising as: SmartPlug_%s (ready for pairing)\n", shortId.c_str());
}

// void SmartPlugSDK::_stopBLE() {
//     if (_bleServer) {
//         BLEDevice::deinit(true);
//         _bleServer = nullptr;
//         _bleCharSSID = nullptr;
//         _bleCharPass = nullptr;
//         _bleCharStatus = nullptr;
//         _bleInitialized = false;
//         Serial.println("[BLE] Stopped");
//     }
// }
void SmartPlugSDK::_stopBLE() {
    if (_bleInitialized) {
        BLEDevice::getAdvertising()->stop();
        Serial.println("[BLE] Stopped");
        // Don't deinit — ESP32 can't reliably reinit BLE without reboot
        // Just reset state so _startBLEProvisioning() reuses existing server
    }
}
// ============================================================
// Internal — SoftAP Provisioning
// ============================================================

void SmartPlugSDK::_startSoftAP() {
    String apName = String(SDK_AP_PREFIX) + _status.serial.substring(3);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName.c_str(), SDK_AP_PASSWORD);
    Serial.printf("[AP] Started hotspot: %s\n", apName.c_str());
    Serial.printf("[AP] IP: %s\n", WiFi.softAPIP().toString().c_str());
}

// ============================================================
// Internal — WiFi Connection
// ============================================================

void SmartPlugSDK::_connectToWiFi() {
    _status.state = STATE_CONNECTING;

    _prefs.begin(SDK_NVS_NAMESPACE, true);
    String ssid = _prefs.getString("wifi_ssid", "");
    String pass = _prefs.getString("wifi_pass", "");
    _prefs.end();

    if (ssid.length() == 0) {
        Serial.println("[WiFi] No saved credentials, entering setup mode");
        _status.state = STATE_SETUP_MODE;
        return;
    }

    Serial.printf("[WiFi] Connecting to: %s\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected! IP: %s, RSSI: %d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        _status.wifi_connected = true;
        _status.ip_address = WiFi.localIP().toString();
        _status.wifi_rssi = WiFi.RSSI();
        _wifiRetryCount = 0;
    } else {
        Serial.println("\n[WiFi] Connection failed");
        _status.wifi_connected = false;
        _wifiRetryCount++;
    }
}

// ============================================================
// Internal — Cloud Connection
// ============================================================

void SmartPlugSDK::_connectToCloud() {
    _status.state = STATE_REGISTERING;
    Serial.printf("[%lus] [Cloud] Connecting to cloud server...\n", millis() / 1000);

    if (SDK_CLOUD_USE_SSL) {
    _webSocket.beginSSL(SDK_CLOUD_WS_HOST, SDK_CLOUD_WS_PORT, SDK_CLOUD_WS_PATH, "", "");
    _webSocket.setExtraHeaders("Origin: https://techs-solutions.com");
    } else {
        _webSocket.begin(SDK_CLOUD_WS_HOST, SDK_CLOUD_WS_PORT, SDK_CLOUD_WS_PATH);
    }

    _webSocket.onEvent(_wsEvent);
    _webSocket.setReconnectInterval(SDK_WS_RETRY_INTERVAL);
    _webSocket.enableHeartbeat(SDK_HEARTBEAT_INTERVAL, 5000, 2);
}

// ============================================================
// Internal — Handle Cloud Messages
// ============================================================

void SmartPlugSDK::_handleCloudMessage(uint8_t* payload, size_t length) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("[WS] JSON parse error: %s\n", err.c_str());
        return;
    }

    _status.last_cloud_contact = millis();
    const char* type = doc["type"];

    if (strcmp(type, "auth_fail") == 0) {
        const char* message = doc["message"] | "Unknown reason";
        Serial.printf("[Cloud] !! AUTH REJECTED: %s\n", message);
        Serial.println("[Cloud] Device cannot operate without valid authentication.");
        Serial.println("[Cloud] Check your developer token in config.h");
        // Halt — blink LED rapidly to signal auth failure
        while (true) {
            digitalWrite(_ledPin, HIGH); delay(200);
            digitalWrite(_ledPin, LOW);  delay(200);
        }
    }
    else if (strcmp(type, "auth_ok") == 0) {
        bool claimed = doc["claimed"] | false;
        _status.claimed = claimed;

        // Sync NVS with server's claimed state (server is source of truth)
        _prefs.begin(SDK_NVS_NAMESPACE, false);
        _prefs.putBool(SDK_NVS_KEY_CLAIMED, claimed);
        _prefs.end();

        if (claimed) {
            _status.state = STATE_ACTIVE;
            _waitingForClaim = false;
            Serial.println("[Cloud] Authenticated - device is claimed, entering ACTIVE mode");
            // Send any energy that accumulated while offline before this reconnect
            if (_hasOfflineData) {
                _sendOfflineEnergyReport();
            }
        } else {
            _status.state = STATE_UNCLAIMED;
            if (_waitingForClaim) {
                // Just provisioned WiFi via BLE — stay on cloud so user can
                // claim via serial number from the app. Don't cycle back to BLE.
                Serial.println("[Cloud] Authenticated - unclaimed, staying on cloud for serial claim");
            } else {
                Serial.println("[Cloud] Authenticated - device unclaimed, entering BLE discovery");
                // Set flag so loop() disconnects cloud and starts BLE advertising
                // indefinitely until a user pairs via the mobile app.
                _blePendingStart = true;
            }
        }
        _sendStatusToCloud();
    }
    else if (strcmp(type, "claimed") == 0) {
        _status.claimed = true;
        _status.state = STATE_ACTIVE;
        _waitingForClaim = false;
        _prefs.begin(SDK_NVS_NAMESPACE, false);
        _prefs.putBool(SDK_NVS_KEY_CLAIMED, true);
        _prefs.end();
        Serial.println("[Cloud] Device has been claimed by a user!");
        // Stop BLE advertising — device is now owned
        _stopBLE();
        _sendStatusToCloud();
    }
    else if (strcmp(type, "command") == 0) {
        const char* action = doc["action"];
        uint8_t nodeIndex = doc["node_index"] | 0;

        if (nodeIndex >= _status.relayCount) {
            Serial.printf("[Cloud] Invalid node_index %d (max %d)\n", nodeIndex, _status.relayCount - 1);
            return;
        }

        if (strcmp(action, "relay_on") == 0) {
            setRelay(nodeIndex, true);
            if (_relayCb) _relayCb(nodeIndex, true);
            // Confirm back
            JsonDocument resp;
            resp["type"] = "result";
            resp["action"] = "relay_on";
            resp["node_index"] = nodeIndex;
            resp["success"] = true;
            resp["relay"] = true;
            String json;
            serializeJson(resp, json);
            _webSocket.sendTXT(json);
            _sendStatusToCloud();
        }
        else if (strcmp(action, "relay_off") == 0) {
            setRelay(nodeIndex, false);
            if (_relayCb) _relayCb(nodeIndex, false);
            JsonDocument resp;
            resp["type"] = "result";
            resp["action"] = "relay_off";
            resp["node_index"] = nodeIndex;
            resp["success"] = true;
            resp["relay"] = false;
            String json;
            serializeJson(resp, json);
            _webSocket.sendTXT(json);
            _sendStatusToCloud();
        }
        else if (strcmp(action, "status") == 0) {
            _sendStatusToCloud();
        }
        else if (strcmp(action, "reboot") == 0) {
            Serial.println("[Cloud] Reboot command received");
            if (_rebootCb) _rebootCb();
            delay(500);
            ESP.restart();
        }
        else if (strcmp(action, "factory_reset") == 0) {
            Serial.println("[Cloud] Factory reset command received");
            if (_factoryResetCb) _factoryResetCb();
            _prefs.begin(SDK_NVS_NAMESPACE, false);
            _prefs.clear();
            _prefs.end();
            delay(500);
            ESP.restart();
        }
        else if (strcmp(action, "wifi_update") == 0) {
            const char* newSsid = doc["ssid"];
            const char* newPass = doc["password"];

            if (newSsid && strlen(newSsid) > 0) {
                Serial.printf("[Cloud] WiFi update command: SSID=%s\n", newSsid);

                // Store new creds in memory only — do NOT save to NVS yet
                String trySSID = String(newSsid);
                String tryPass = newPass ? String(newPass) : "";

                // Disconnect cloud WebSocket cleanly
                _webSocket.disconnect();
                _status.cloud_connected = false;
                delay(200);

                // Disconnect current WiFi fully before trying new one
                WiFi.disconnect(true);
                delay(500);

                // Try connecting with new credentials (in-memory, not from NVS)
                Serial.printf("[WiFi] Trying new WiFi: %s\n", trySSID.c_str());
                WiFi.mode(WIFI_STA);
                WiFi.begin(trySSID.c_str(), tryPass.c_str());

                int attempts = 0;
                while (WiFi.status() != WL_CONNECTED && attempts < 20) {
                    delay(500);
                    Serial.print(".");
                    attempts++;
                }

                if (WiFi.status() == WL_CONNECTED) {
                    // SUCCESS — new WiFi works, NOW save to NVS
                    Serial.printf("\n[WiFi] New WiFi connected! IP: %s\n",
                                  WiFi.localIP().toString().c_str());
                    _status.wifi_connected = true;
                    _status.ip_address = WiFi.localIP().toString();
                    _status.wifi_rssi = WiFi.RSSI();

                    _prefs.begin(SDK_NVS_NAMESPACE, false);
                    _prefs.putString("wifi_ssid", trySSID);
                    _prefs.putString("wifi_pass", tryPass);
                    _prefs.end();

                    // Reconnect to cloud on new WiFi
                    _connectToCloud();
                    // Wait briefly for WebSocket to connect so we can send the result
                    delay(2000);
                    _webSocket.loop();
                    delay(500);
                    _webSocket.loop();

                    JsonDocument resp;
                    resp["type"] = "result";
                    resp["action"] = "wifi_update";
                    resp["success"] = true;
                    String json;
                    serializeJson(resp, json);
                    _webSocket.sendTXT(json);
                } else {
                    // FAILED — revert to old WiFi from NVS (still intact)
                    Serial.println("\n[WiFi] New WiFi failed — reverting to old WiFi");
                    WiFi.disconnect(true);
                    delay(500);

                    // Reconnect using old credentials still stored in NVS
                    _connectToWiFi();

                    if (_status.wifi_connected) {
                        // Back on old WiFi — reconnect to cloud and report failure
                        _connectToCloud();
                        delay(2000);
                        _webSocket.loop();
                        delay(500);
                        _webSocket.loop();

                        JsonDocument resp;
                        resp["type"] = "result";
                        resp["action"] = "wifi_update";
                        resp["success"] = false;
                        resp["error"] = "connection_failed";
                        String json;
                        serializeJson(resp, json);
                        _webSocket.sendTXT(json);
                        Serial.println("[WiFi] Reverted to old WiFi, reported failure to cloud");
                    } else {
                        // Old WiFi also failed (unlikely) — restart to trigger recovery
                        Serial.println("[WiFi] Old WiFi also failed — restarting device");
                        delay(500);
                        ESP.restart();
                    }
                }
            } else {
                // Missing SSID — send error back
                JsonDocument resp;
                resp["type"] = "result";
                resp["action"] = "wifi_update";
                resp["success"] = false;
                resp["error"] = "missing_ssid";
                String json;
                serializeJson(resp, json);
                _webSocket.sendTXT(json);
            }
        }
    }
    else if (strcmp(type, "unclaimed") == 0) {
        // Owner removed this device from the app — go back to unclaimed state.
        _prefs.begin(SDK_NVS_NAMESPACE, false);
        _prefs.putBool(SDK_NVS_KEY_CLAIMED, false);
        _prefs.remove("wifi_ssid");
        _prefs.remove("wifi_pass");
        _prefs.end();
        Serial.println("[Cloud] Device unclaimed by owner — rebooting into BLE discovery");
        delay(500);
        ESP.restart();
    }
    else if (strcmp(type, "pong") == 0) {
        // Heartbeat response
    }
    else if (strcmp(type, "ota") == 0) {
        const char* url = doc["url"];
        const char* ver = doc["version"] | "unknown";
        Serial.printf("[Cloud] OTA update available: %s (v%s)\n", url, ver);
        _status.state = STATE_OTA_UPDATE;

        // Perform the OTA update
        if (url && strlen(url) > 0) {
            Serial.println("[OTA] Starting firmware update...");

            // Disconnect WebSocket to free heap for TLS
            _webSocket.disconnect();
            delay(200);

            WiFiClientSecure otaClient;
            otaClient.setInsecure(); // Skip certificate verification for OTA

            httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            t_httpUpdate_return ret = httpUpdate.update(otaClient, String(url));

            switch (ret) {
                case HTTP_UPDATE_OK:
                    Serial.println("[OTA] Update successful! Rebooting...");
                    delay(500);
                    ESP.restart();
                    break;
                case HTTP_UPDATE_FAILED:
                    Serial.printf("[OTA] Update FAILED: %s (err %d)\n",
                        httpUpdate.getLastErrorString().c_str(),
                        httpUpdate.getLastError());
                    break;
                case HTTP_UPDATE_NO_UPDATES:
                    Serial.println("[OTA] No update available.");
                    break;
            }

            // If we get here, OTA failed — reconnect to cloud
            _status.state = STATE_ACTIVE;
            Serial.println("[OTA] Reconnecting to cloud...");
            delay(500);
            _connectToCloud();
        }
    }
}

// ============================================================
// Internal — Send Status to Cloud (multi-node)
// ============================================================

void SmartPlugSDK::_sendStatusToCloud() {
    if (!_status.cloud_connected) return;

    JsonDocument doc;
    doc["type"] = "status";
    doc["serial"] = _status.serial;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["wifi_ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime"] = millis();
    doc["firmware"] = SDK_FIRMWARE_VERSION;
    doc["free_heap"] = ESP.getFreeHeap();

    // Multi-node: send nodes array
    JsonArray nodesArr = doc["nodes"].to<JsonArray>();
    for (uint8_t i = 0; i < _status.relayCount; i++) {
        JsonObject node = nodesArr.add<JsonObject>();
        node["index"] = i;
        node["relay"] = _status.nodes[i].relayOn;

        if (_status.nodes[i].hasSensor && _status.nodes[i].power.valid) {
            JsonObject power = node["power"].to<JsonObject>();
            power["watts"] = round(_status.nodes[i].power.watts * 100.0) / 100.0;
            power["voltage"] = round(_status.nodes[i].power.voltage * 10.0) / 10.0;
            power["current"] = round(_status.nodes[i].power.current * 1000.0) / 1000.0;
            power["energy_kwh"] = round(_status.nodes[i].power.energy_kwh * 1000.0) / 1000.0;
            power["power_factor"] = round(_status.nodes[i].power.power_factor * 100.0) / 100.0;
            power["frequency"] = round(_status.nodes[i].power.frequency * 10.0) / 10.0;
        }
    }

    // Backward compat: also send flat relay/power for single-node devices
    if (_status.relayCount == 1) {
        doc["relay"] = _status.nodes[0].relayOn;
        if (_status.nodes[0].power.valid) {
            JsonObject power = doc["power"].to<JsonObject>();
            power["watts"] = round(_status.nodes[0].power.watts * 100.0) / 100.0;
            power["voltage"] = round(_status.nodes[0].power.voltage * 10.0) / 10.0;
            power["current"] = round(_status.nodes[0].power.current * 1000.0) / 1000.0;
            power["energy_kwh"] = round(_status.nodes[0].power.energy_kwh * 1000.0) / 1000.0;
            power["power_factor"] = round(_status.nodes[0].power.power_factor * 100.0) / 100.0;
            power["frequency"] = round(_status.nodes[0].power.frequency * 10.0) / 10.0;
        }
    }

    String json;
    serializeJson(doc, json);
    _webSocket.sendTXT(json);
}

// ============================================================
// Internal — Send Power Data to Cloud (per-node)
// ============================================================

void SmartPlugSDK::_sendPowerToCloud(uint8_t nodeIndex) {
    if (!_status.cloud_connected) return;
    if (nodeIndex >= _status.relayCount) return;

    const PowerReading& p = _status.nodes[nodeIndex].power;
    if (!p.valid) return;

    JsonDocument doc;
    doc["type"] = "power_log";
    doc["serial"] = _status.serial;
    doc["node_index"] = nodeIndex;
    doc["watts"] = round(p.watts * 100.0) / 100.0;
    doc["voltage"] = round(p.voltage * 10.0) / 10.0;
    doc["current"] = round(p.current * 1000.0) / 1000.0;
    doc["energy_kwh"] = round(p.energy_kwh * 1000.0) / 1000.0;
    doc["power_factor"] = round(p.power_factor * 100.0) / 100.0;
    doc["frequency"] = round(p.frequency * 10.0) / 10.0;
    doc["relay"] = _status.nodes[nodeIndex].relayOn;
    doc["timestamp"] = millis();

    String json;
    serializeJson(doc, json);
    _webSocket.sendTXT(json);
}

// ============================================================
// Internal — Power Monitor (PZEM-004T protocol)
// ============================================================

void SmartPlugSDK::_readPower(uint8_t nodeIndex) {
    if (nodeIndex >= SMARTPLUG_MAX_NODES) return;

    // PZEM-004T Modbus RTU request
    uint8_t addr = _status.nodes[nodeIndex].modbusAddr;
    uint8_t request[] = {addr, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00};
    // Calculate CRC for the request
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 6; i++) {
        crc ^= request[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    _pzemSerial.write(request, sizeof(request));
    delay(100);

    if (_pzemSerial.available() >= 25) {
        uint8_t response[25];
        _pzemSerial.readBytes(response, 25);

        float voltage = ((response[3] << 8) | response[4]) * 0.1;
        float current = (((uint32_t)response[7] << 24) | ((uint32_t)response[8] << 16) |
                         ((uint32_t)response[5] << 8) | response[6]) * 0.001;
        float power = (((uint32_t)response[11] << 24) | ((uint32_t)response[12] << 16) |
                       ((uint32_t)response[9] << 8) | response[10]) * 0.1;
        float energy = (((uint32_t)response[15] << 24) | ((uint32_t)response[16] << 16) |
                        ((uint32_t)response[13] << 8) | response[14]);
        float frequency = ((response[17] << 8) | response[18]) * 0.1;
        float pf = ((response[19] << 8) | response[20]) * 0.01;

        if (voltage > 0 && voltage < 300 && current >= 0 && current < 100) {
            _status.nodes[nodeIndex].power.voltage = voltage;
            _status.nodes[nodeIndex].power.current = current;
            _status.nodes[nodeIndex].power.watts = power;
            _status.nodes[nodeIndex].power.energy_kwh = energy / 1000.0;
            _status.nodes[nodeIndex].power.frequency = frequency;
            _status.nodes[nodeIndex].power.power_factor = pf;
            _status.nodes[nodeIndex].power.timestamp = millis();
            _status.nodes[nodeIndex].power.valid = true;
        }
    } else if (_status.nodes[nodeIndex].hasRating) {
        // No PZEM sensor — use rated values from config
        const PowerRating& r = _status.nodes[nodeIndex].rating;
        if (_status.nodes[nodeIndex].relayOn) {
            _status.nodes[nodeIndex].power.voltage = r.voltage;
            _status.nodes[nodeIndex].power.watts = r.watts;
            _status.nodes[nodeIndex].power.current = (r.power_factor > 0)
                ? r.watts / (r.voltage * r.power_factor)
                : r.watts / r.voltage;
            _status.nodes[nodeIndex].power.power_factor = r.power_factor;
            _status.nodes[nodeIndex].power.frequency = r.frequency;
            _status.nodes[nodeIndex].power.energy_kwh +=
                r.watts * (SDK_POWER_READ_INTERVAL / 3600000.0) / 1000.0;
            _status.nodes[nodeIndex].power.timestamp = millis();
            _status.nodes[nodeIndex].power.valid = true;
        } else {
            _status.nodes[nodeIndex].power.watts = 0;
            _status.nodes[nodeIndex].power.current = 0;
            _status.nodes[nodeIndex].power.voltage = r.voltage;
            _status.nodes[nodeIndex].power.power_factor = r.power_factor;
            _status.nodes[nodeIndex].power.frequency = r.frequency;
            // energy_kwh stays cumulative (no addition when off)
            _status.nodes[nodeIndex].power.timestamp = millis();
            _status.nodes[nodeIndex].power.valid = true;
        }
    }
}

// ============================================================
// Internal — LED Status Indicator
// ============================================================

void SmartPlugSDK::_updateLED() {
    static unsigned long lastBlink = 0;
    static bool ledState = false;

    switch (_status.state) {
        case STATE_FACTORY_NEW:
        case STATE_SETUP_MODE:
            if (millis() - lastBlink > 250) {
                ledState = !ledState;
                digitalWrite(_ledPin, ledState);
                lastBlink = millis();
            }
            break;

        case STATE_CONNECTING:
        case STATE_REGISTERING:
            if (millis() - lastBlink > 500) {
                ledState = !ledState;
                digitalWrite(_ledPin, ledState);
                lastBlink = millis();
            }
            break;

        case STATE_RECONNECTING:
            if (millis() - lastBlink > 1000) {
                ledState = !ledState;
                digitalWrite(_ledPin, ledState);
                lastBlink = millis();
            }
            break;

        case STATE_ACTIVE:
        case STATE_UNCLAIMED:
            digitalWrite(_ledPin, HIGH);
            break;

        case STATE_RECOVERY:
            {
                unsigned long elapsed = (millis() - lastBlink) % 2000;
                if (elapsed < 200 || (elapsed > 400 && elapsed < 600)) {
                    digitalWrite(_ledPin, HIGH);
                } else {
                    digitalWrite(_ledPin, LOW);
                }
            }
            break;

        default:
            digitalWrite(_ledPin, LOW);
            break;
    }
}

// ============================================================
// Internal — Recovery Mode
// ============================================================

void SmartPlugSDK::_checkRecovery() {
    // Claimed devices use immediate BLE recovery in loop() — skip old timeout logic
    if (_status.claimed) return;

    if (_status.state == STATE_RECONNECTING) {
        if (millis() - _reconnectStart > SDK_MAX_WIFI_RETRIES * SDK_WIFI_RETRY_INTERVAL) {
            Serial.println("[Recovery] Too many retries, entering recovery AP mode");
            _status.state = STATE_RECOVERY;
            _startSoftAP();
            _startBLEProvisioning();
            _setupModeStart = millis();
        }
    }

    if (_status.state == STATE_RECOVERY) {
        if (millis() - _setupModeStart > SDK_RECOVERY_AP_TIMEOUT) {
            Serial.println("[Recovery] Recovery timeout, rebooting...");
            ESP.restart();
        }
    }
}

// ============================================================
// Internal — Offline Energy: Save Snapshot to NVS
// Called once when WiFi first goes down.
// Stores the starting energy baseline + initialises running delta to 0.
// ============================================================

void SmartPlugSDK::_saveOfflineSnapshot() {
    _offlineStartMs = millis();
    _hasOfflineData = true;
    _prefs.begin(SDK_NVS_NAMESPACE, false);
    _prefs.putBool(SDK_NVS_KEY_OFFLINE_FLAG, true);
    _prefs.putUInt(SDK_NVS_KEY_OFFLINE_MS, (uint32_t)_offlineStartMs);
    char key[12];
    for (uint8_t i = 0; i < _status.relayCount; i++) {
        _offlineStartEnergy[i]    = _status.nodes[i].power.energy_kwh;
        _offlinePreRebootAccum[i] = 0.0f;
        snprintf(key, sizeof(key), "%s%d", SDK_NVS_KEY_OFFLINE_DL_PREFIX, i);
        _prefs.putFloat(key, 0.0f);  // running delta starts at 0
    }
    _prefs.end();
    Serial.println("[Offline] Snapshot saved — energy tracking started");
}

// ============================================================
// Internal — Offline Energy: Send Catch-Up Report to Cloud
// Called after auth_ok when _hasOfflineData is true.
// Sends the total energy delta accumulated since going offline.
// Formula: total_delta = pre_reboot_accum + (current_ram_energy - this_boot_baseline)
// ============================================================

void SmartPlugSDK::_sendOfflineEnergyReport() {
    if (!_hasOfflineData || !_status.cloud_connected) return;

    unsigned long durationMs = millis() - _offlineStartMs;
    if (durationMs < 5000) return;  // Skip trivially short disconnects (< 5 seconds)

    JsonDocument doc;
    doc["type"]               = "offline_energy_report";
    doc["serial"]             = _status.serial;
    doc["offline_duration_ms"] = durationMs;

    JsonArray nodes = doc["nodes"].to<JsonArray>();
    for (uint8_t i = 0; i < _status.relayCount; i++) {
        float delta = _offlinePreRebootAccum[i] +
                      (_status.nodes[i].power.energy_kwh - _offlineStartEnergy[i]);
        if (delta > 0.0001f) {
            JsonObject n = nodes.add<JsonObject>();
            n["index"]            = i;
            n["energy_delta_kwh"] = round(delta * 10000.0f) / 10000.0f;
            n["relay_on"]         = _status.nodes[i].relayOn;
        }
    }

    String json;
    serializeJson(doc, json);
    _webSocket.sendTXT(json);

    // Clear NVS offline state now that we have reported
    _prefs.begin(SDK_NVS_NAMESPACE, false);
    _prefs.putBool(SDK_NVS_KEY_OFFLINE_FLAG, false);
    _prefs.end();

    _hasOfflineData = false;
    Serial.printf("[Offline] Report sent — offline duration: %lu ms\n", durationMs);
}
