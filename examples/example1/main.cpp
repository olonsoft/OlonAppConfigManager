#include <AppConfigManager.h>
#include <Arduino.h>
#include <EventBus.h>

EventBus         eventBus;
AppConfigManager wifiManager(eventBus);

void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting...");

    // 1. Provide load/save callbacks — library calls these, you handle NVS
    wifiManager.setLoadCallback([](AppConfig& cfg) -> bool {
        // Fill cfg from your storage. Return false if nothing is saved yet.
        cfg.primary.ssid[0] = '\0';           // example: empty first run
        return false;
    });

    wifiManager.setSaveCallback([](const AppConfig& cfg) -> bool {
        // Write cfg fields to your storage. Return true on success.
        return true;
    });

    // 2. Tune behaviour (all optional — defaults shown)
    wifiManager.setPortalCredentials("MyDevice-Setup", ""); // open AP
    wifiManager.setWebPortalPassword("test123"); // password for web portal (STA mode)
    wifiManager.setConnectTimeout(15000);    // 15 s per connection attempt
    wifiManager.setPortalTimeout(180000);    // 3 min captive portal lifetime
    wifiManager.setConnectRetries(3);        // retries per profile before switching
    wifiManager.setWebPortalPort(80);
    wifiManager.setRssiThreshold(-80, 30);  // dBm, check every 30 s

    // 3. Subscribe to events

    eventBus.subscribe(EventType::APP_WIFI_STATE_CHANGE, [](EventType, const void* p) {
        auto* r = static_cast<const StateChangePayload*>(p);
        Serial.printf("State change : %s → %s\n",
                      wifiManager.getStateString(r->prevState).c_str(),
                      wifiManager.getStateString(r->nextState).c_str());
        Serial.printf("Heap --> %d\n", ESP.getFreeHeap());
    });

    eventBus.subscribe(EventType::APP_WIFI_CONNECTING, [](EventType, const void* p) {
        auto* r = static_cast<const ConnectionInfoPayload*>(p);
        Serial.printf("WiFi connecting to %s\n", r->ssid);
    });

    eventBus.subscribe(EventType::APP_WIFI_CONNECTED, [](EventType, const void* p) {
        auto* r = static_cast<const ConnectionInfoPayload*>(p);
        Serial.printf("WiFi connected to %s\n", r->ssid);
    });

    eventBus.subscribe(EventType::APP_WIFI_GOT_IP, [](EventType, const void* p) {
        auto* r = static_cast<const ConnectionInfoPayload*>(p);
        Serial.printf("WiFi %s got IP %s\n", r->ssid, r->ip.toString().c_str());
    });

    eventBus.subscribe(EventType::APP_WIFI_DISCONNECTED, [](EventType, const void* p) {
        auto* r = static_cast<const ConnectionInfoPayload*>(p);
        Serial.printf("WiFi disconnected from %s\n", r->ssid);
    });

    eventBus.subscribe(EventType::APP_WIFI_AUTH_FAILED, [](EventType, const void*) {
        Serial.println("WiFi auth failed");
    });

    eventBus.subscribe(EventType::APP_WIFI_RETRY, [](EventType, const void* p) {
        auto* r = static_cast<const RetryPayload*>(p);
        Serial.printf("Retry %d/%d\n", r->retryCount, r->maxRetries);
    });

    eventBus.subscribe(EventType::APP_WIFI_PORTAL_STARTED, [](EventType, const void*) {
        Serial.println("Captive portal started");
    });

    eventBus.subscribe(EventType::APP_WIFI_PORTAL_TIMEOUT, [](EventType, const void*) {
        Serial.println("Captive portal timeout");
    });

    eventBus.subscribe(EventType::APP_WIFI_PORTAL_STOPPED, [](EventType, const void*) {
        Serial.println("Captive portal stopped");
    });

    eventBus.subscribe(EventType::APP_WIFI_WEB_PORTAL_STARTED, [](EventType, const void*) {
        Serial.println("Web portal started");
    });

    eventBus.subscribe(EventType::APP_WIFI_WEB_PORTAL_STOPPED, [](EventType, const void*) {
        Serial.println("Web portal stopped");
    });

    eventBus.subscribe(EventType::APP_WIFI_CONFIG_SAVED, [](EventType, const void*) {
        Serial.println("WiFi config saved");
        // Re-read config and apply MQTT / NTP / etc. changes
    });

    eventBus.subscribe(EventType::APP_WIFI_RSSI_LOW, [](EventType, const void* p) {
        auto* r = static_cast<const RssiPayload*>(p);
        Serial.printf("Signal low: %d dBm (threshold %d)\n", r->rssi, r->threshold);
    });

    wifiManager.begin();
}

void loop() {
    wifiManager.loop();
}