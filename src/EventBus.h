#pragma once
#include <Arduino.h>

#include <functional>
#include <vector>

enum class EventType : uint16_t {
    // --- Config ---
    APP_CONFIG_LOADED,
    APP_CONFIG_CHANGED,

    // --- WiFi ---
    APP_WIFI_STATE_CHANGE,       // Published when state machine changes state
    APP_WIFI_CONNECTING,         // Starting a connection attempt
    APP_WIFI_CONNECTED,          // Fully connected + IP assigned
    APP_WIFI_GOT_IP,             // IP address assigned (may fire before CONNECTED)
    APP_WIFI_DISCONNECTED,       // Connection lost or disconnect() confirmed
    APP_WIFI_AUTH_FAILED,        // Wrong password — will not retry this profile
    APP_WIFI_RETRY,              // Retry attempt in progress — payload: RetryPayload*
    APP_WIFI_PORTAL_STARTED,     // Captive portal (AP mode) is active
    APP_WIFI_PORTAL_TIMEOUT,     // Captive portal timed out
    APP_WIFI_PORTAL_STOPPED,     // Captive portal torn down
    APP_WIFI_WEB_PORTAL_STARTED, // Web portal (STA mode) is active
    APP_WIFI_WEB_PORTAL_STOPPED, // Web portal torn down
    APP_WIFI_CONFIG_SAVED,       // Config written — payload: nullptr
    APP_WIFI_RSSI_LOW,           // RSSI crossed low threshold — payload: RssiPayload*

    // --- OTA ---
    APP_OTA_AVAILABLE,
    APP_OTA_PROGRESS,
    APP_OTA_DONE,
    APP_OTA_FAILED,

    // --- MQTT ---
    APP_MQTT_CLIENT_CONNECTED,
    APP_MQTT_CLIENT_DISCONNECTED,
    APP_MQTT_MESSAGE_RECEIVED,

    // --- System ---
    APP_HEARTBEAT,
    APP_FACTORY_RESET_WARNING,
    APP_FACTORY_RESET,
    APP_FACTORY_RESET_COMPLETE,
    APP_SYSTEM_RESTART_WARNING,
    APP_SYSTEM_RESTART,
    APP_SYSTEM_SLEEP_PREPARING,
    APP_SYSTEM_SLEEP,
    APP_ERROR_RECOVERABLE,   // flash error pattern once, return to current state
    APP_ERROR_CRITICAL,      // permanent error state, device needs attention

    // Time
    APP_NTP_SYNCED,

    // --- Button ---
    APP_BUTTON_CLICK,
    APP_BUTTON_DOUBLE_CLICK,
    APP_BUTTON_LONGPRESS_START,
    APP_BUTTON_LONGPRESS_PROGRESS,
    APP_BUTTON_LONGPRESS_STOP,

    APP_SENSOR_NEW_DATA, // generic event for new sensor data, payload should point to struct with details

    // --- Device custom events ---
    // Reserve 100-200 for device projects
    // Usage in device project:
    // constexpr EventType MY_EVENT = static_cast<EventType>(100);
    // or
    // constexpr EventType MY_EVENT = EventType::APP_CUSTOM_EVENT_1;
    APP_CUSTOM_EVENT_1 = 100,
    APP_CUSTOM_EVENT_2 = 101,
    APP_CUSTOM_EVENT_3 = 102,
    APP_CUSTOM_EVENT_4 = 103
};

using EventCallback = std::function<void(EventType, const void*)>;

class EventBus {
  public:
    void subscribe(EventType event, EventCallback cb);
    void publish(EventType event, const void* payload = nullptr);

  private:
    struct Subscriber {
        EventType     event;
        EventCallback cb;
    };

    std::vector<Subscriber> subscribers;
};