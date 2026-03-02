#ifndef SMARTPLUG_DEVICE_STATE_H
#define SMARTPLUG_DEVICE_STATE_H

#include <Arduino.h>

// Maximum nodes (relays/sensors) per device
#define SMARTPLUG_MAX_NODES 8

// Device lifecycle states
enum DeviceState {
    STATE_FACTORY_NEW,    // First boot, no config
    STATE_SETUP_MODE,     // AP/BLE active, waiting for WiFi creds
    STATE_CONNECTING,     // Trying to connect to WiFi
    STATE_REGISTERING,    // Connected to WiFi, connecting to cloud
    STATE_UNCLAIMED,      // Cloud connected, waiting for user to claim
    STATE_ACTIVE,         // Normal operation
    STATE_RECONNECTING,   // Lost connection, trying to reconnect
    STATE_OTA_UPDATE,     // Firmware update in progress
    STATE_RECOVERY        // Failed too many times, AP mode for reconfig
};

// Power reading snapshot
struct PowerReading {
    float watts;          // Active power (W), range: 0 - 22000
    float voltage;        // Voltage (V), range: 0 - 300
    float current;        // Current (A), range: 0 - 100
    float energy_kwh;     // Cumulative energy (kWh)
    float power_factor;   // Power factor, range: 0.0 - 1.0
    float frequency;      // Grid frequency (Hz), range: 45 - 65
    unsigned long timestamp;  // millis() when read
    bool valid;           // Set to true when reading is valid
};

// Rated power specs (set by user in config.h)
struct PowerRating {
    float watts;              // Rated power (W), e.g. 60.0 for a 60W lamp
    float voltage;            // Rated voltage (V), e.g. 220.0
    float frequency;          // Grid frequency (Hz), e.g. 50.0
    float power_factor;       // Power factor (0.0 - 1.0), e.g. 1.0
};

// Per-node configuration
struct NodeConfig {
    uint8_t relayPin;         // GPIO pin for relay
    bool relayActiveHigh;     // true = HIGH turns on relay
    uint8_t sensorRxPin;      // UART RX pin for PZEM (0 = no sensor)
    uint8_t sensorTxPin;      // UART TX pin for PZEM
    uint8_t modbusAddr;       // PZEM modbus address (1-247)
    bool hasSensor;           // Whether this node has a power sensor
    bool hasRating;           // Whether this node has rated power specs
    bool relayOn;             // Current relay state
    PowerRating rating;       // User-defined rated specs
    PowerReading power;       // Latest power reading
};

// Full device status
struct DeviceStatus {
    DeviceState state;
    NodeConfig nodes[SMARTPLUG_MAX_NODES];
    uint8_t relayCount;           // Number of registered relays
    uint8_t sensorCount;          // Number of registered power sensors
    String serial;
    String secret_key;
    String firmware_version;
    String ip_address;
    bool wifi_connected;
    bool cloud_connected;
    bool claimed;
    int wifi_rssi;
    unsigned long uptime_ms;
    unsigned long last_cloud_contact;
};

#endif // SMARTPLUG_DEVICE_STATE_H
