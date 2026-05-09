# AppConfigManager

A non-blocking WiFi and application configuration manager for **ESP8266** and **ESP32**.

Handles the full connection lifecycle — initial provisioning, reconnection, dual-AP fallback, captive portal, and a web-based configuration portal — through a single state machine driven from `loop()`. The main program only needs to provide load/save callbacks and subscribe to events.

---

## Features

- Non-blocking state machine — never stalls `loop()`
- Dual WiFi profile support (primary + secondary) with automatic fallback
- Secondary-to-primary promotion when secondary connects (persisted)
- Captive portal (AP mode) for first-time setup
- Web portal (STA mode) for reconfiguration while connected
- Single-page configuration UI with tabs: **WiFi · App · MQTT · System**
- Async WiFi network scan with signal strength, sorted by RSSI
- Static IP support (shared across both profiles)
- mDNS (`hostname.local`) with automatic restart on hostname change
- HTTP Basic Auth on the web portal (optional)
- Full validation of hostname, MQTT broker, NTP server, and static IP
- RSSI monitoring with configurable threshold and hysteresis
- All HTML stored in PROGMEM; web server heap-allocated only when portal is active
- Works identically on ESP8266 and ESP32

---

## Dependencies

| Library                    | Source                                          |
| -------------------------- | ----------------------------------------------- |
| ESPAsyncWebServer          | https://github.com/ESP32Async/ESPAsyncWebServer |
| ESPAsyncTCP (ESP8266 only) | bundled with ESPAsyncWebServer                  |
| ArduinoJson v7             | https://arduinojson.org                         |
| DNSServer                  | bundled with Arduino ESP8266/ESP32 core         |
| ESP8266mDNS / ESPmDNS      | bundled with Arduino ESP8266/ESP32 core         |

---

## Files

| File                      | Purpose                                                            |
| ------------------------- | ------------------------------------------------------------------ |
| `AppConfigManager.h`      | Class declaration, `AppConfig` struct, enums, `EventBus` interface |
| `AppConfigManager.cpp`    | Full state machine, web server routes, validation, scan            |
| `AppConfigManager_HTML.h` | Single-page portal HTML/CSS/JS stored as a PROGMEM string          |

---

## Quick Start

```cpp
#include "AppConfigManager.h"

EventBus          eventBus;
AppConfigManager  wifiManager(eventBus);

// Your own config storage (Preferences, SPIFFS, etc.)
MyConfig myConfig;

void setup() {
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
    wifiManager.setConnectTimeout(15000);    // 15 s per connection attempt
    wifiManager.setPortalTimeout(180000);    // 3 min captive portal lifetime
    wifiManager.setConnectRetries(3);        // retries per profile before switching
    wifiManager.setWebPortalPassword("admin");
    wifiManager.setWebPortalPort(80);
    wifiManager.setRssiThreshold(-80, 30);  // dBm, check every 30 s

    // 3. Subscribe to events
    eventBus.subscribe(EventType::APP_WIFI_CONNECTED, [](EventType, const void*) {
        Serial.println("WiFi connected");
    });

    eventBus.subscribe(EventType::APP_WIFI_DISCONNECTED, [](EventType, const void*) {
        Serial.println("WiFi disconnected");
    });

    eventBus.subscribe(EventType::APP_WIFI_RETRY, [](EventType, const void* p) {
        auto* r = static_cast<const RetryPayload*>(p);
        Serial.printf("Retry %d/%d\n", r->retryCount, r->maxRetries);
    });

    eventBus.subscribe(EventType::APP_WIFI_CONFIG_SAVED, [](EventType, const void*) {
        // Re-read config and apply MQTT / NTP / etc. changes
    });

    eventBus.subscribe(EventType::APP_WIFI_RSSI_LOW, [](EventType, const void* p) {
        auto* r = static_cast<const RssiPayload*>(p);
        Serial.printf("Signal low: %d dBm (threshold %d)\n", r->rssi, r->threshold);
    });

    // 4. Start
    wifiManager.begin();
}

void loop() {
    wifiManager.loop();   // Must be called every iteration

    // Open web portal on demand (e.g. button press)
    if (portalButtonPressed && wifiManager.isConnected()) {
        wifiManager.startWebPortal();
    }
}
```

---

## AppConfig Struct

Defined in `AppConfigManager.h`. The library owns this struct. Your load/save callbacks translate between it and your own storage format.

```cpp
struct WiFiProfile {
    char ssid[33];       // Max 32 chars + null
    char password[65];   // Max 64 chars + null
};

struct AppConfig {
    // WiFi
    WiFiProfile primary;
    WiFiProfile secondary;

    // Static IP — shared between both profiles
    bool      useStaticIP;   // false = DHCP
    IPAddress staticIP;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns1;
    IPAddress dns2;

    // Device identity
    char appName[33];    // Shown in portal title
    char hostname[64];   // Used for mDNS: hostname.local

    // MQTT
    char     mqttBroker[129];           // host or host:port, no protocol prefix
    uint16_t mqttPort;                  // default 1883
    char     mqttUser[65];
    char     mqttPassword[65];
    char     mqttClientId[33];
    char     mqttBaseTopic[65];         // default "home/device"
    char     mqttHADiscoveryTopic[65];  // default "homeassistant"

    // Time
    char ntpServer[129];    // default "pool.ntp.org"
    char posixTimezone[65]; // POSIX string e.g. "EET-2EEST,M3.5.0/3,M10.5.0/4"

    // OTA — stored only; used by main program
    char otaUrl[129];

    // Web portal
    char webPortalPassword[33];  // HTTP Basic Auth password; empty = no auth
};
```

Password fields are **never pre-filled** in the browser form and are sent as `"****"` placeholders when unchanged. The library detects the placeholder and leaves the stored value untouched.

---

## Public API

### Construction

```cpp
explicit AppConfigManager(EventBus& eventBus);
```

Pass a reference to your application's `EventBus` instance. The library publishes all events through it.

---

### Mandatory Callbacks

Must be set **before** calling `begin()`.

```cpp
void setLoadCallback(std::function<bool(AppConfig&)> fn);
```

Called once at startup. Fill the provided `AppConfig` from your storage (Preferences, SPIFFS, etc.). Return `true` if valid config was loaded, `false` if nothing is stored yet. Returning `false` causes the manager to go directly to the captive portal.

```cpp
void setSaveCallback(std::function<bool(const AppConfig&)> fn);
```

Called whenever config is saved — from the web portal, or when the secondary profile is promoted to primary. Write `AppConfig` fields to your storage. Return `true` on success.

---

### Lifecycle

```cpp
void begin();
```

Initialises the WiFi subsystem and drives the state machine into its first state. Calls `loadCallback` internally. Must be called once from `setup()`.

```cpp
void loop();
```

Drives the state machine and DNS server. **Must be called on every iteration of `loop()`** with no blocking delays between calls.

---

### Tuning (call before `begin()`)

```cpp
void setPortalCredentials(const String& ssid, const String& password = "");
```

SSID and password for the captive-portal access point. Pass an empty password for an open AP.

```cpp
void setConnectTimeout(unsigned long ms);   // default: 15000
```

How long to wait for a single connection attempt before declaring a timeout and moving to retry or AP-switch logic.

```cpp
void setPortalTimeout(unsigned long ms);    // default: 180000
```

How long the captive portal remains active before timing out. On timeout, the portal is torn down and the last known credentials are retried.

```cpp
void setConnectRetries(uint8_t retries);    // default: 3
```

Number of retry attempts per profile before the manager either switches to the secondary profile or opens the captive portal.

```cpp
void setWebPortalPassword(const String& password);
```

Password for HTTP Basic Auth on the web portal (`/`, `/save`, `/scan`, `/status`, `/exit`). Username is always `admin`. Pass an empty string to disable authentication.

```cpp
void setWebPortalPort(uint16_t port);       // default: 80
```

TCP port the web server listens on in both captive and web portal modes.

```cpp
void setRssiThreshold(int8_t dbm, uint16_t checkIntervalSec = 30);
```

Enable RSSI monitoring while connected. Publishes `APP_WIFI_RSSI_LOW` when RSSI drops below `dbm`. Re-arms (hysteresis) once RSSI rises back to or above the threshold. Set `checkIntervalSec` to control polling frequency.

---

### Runtime Control

```cpp
void startWebPortal();
```

Opens the web configuration portal while already connected to WiFi. Only effective from `STATE_CONNECTED`. Transitions to `STATE_START_WEB_PORTAL`. The portal is accessible at the device's current IP address on the configured port.

```cpp
void stopPortal();
```

Gracefully closes whichever portal is currently active. The server and DNS are torn down and heap memory is freed. No-op if no portal is running.

```cpp
void disconnect();
```

Disconnects from WiFi and **suppresses all future reconnection attempts**. Publishes `APP_WIFI_DISCONNECTED` once the stack confirms disconnection. The device will not attempt to reconnect until `begin()` is called again. Use this before entering deep sleep or when you need full control of the radio.

```cpp
void resetConfig(bool fullWipe = false);
```

Wipes saved WiFi credentials and restarts the captive portal. If `fullWipe` is `true`, resets the entire `AppConfig` struct to defaults (including MQTT, NTP, OTA, etc.) before saving.

---

### Status Queries

```cpp
bool isConnected() const;
```

Returns `true` when the device is in `STATE_CONNECTED`, `STATE_WEB_PORTAL_ACTIVE`, or `STATE_MDNS_RESTART` — i.e. any state where a valid IP address is held.

```cpp
AppWiFiState getState() const;
```

Returns the current state machine state as an `AppWiFiState` enum value.

```cpp
String getStateString() const;
```

Returns the current state as a human-readable string, e.g. `"CONNECTED"`, `"PORTAL_ACTIVE"`. Useful for serial logging and the `/status` endpoint.

```cpp
const AppConfig& getConfig() const;
```

Returns a const reference to the in-memory config snapshot. Do not cache the reference across `loop()` calls. Use this after receiving `APP_WIFI_CONFIG_SAVED` to read updated MQTT/NTP/OTA settings.

---

## Events

Subscribe via `eventBus.subscribe(EventType::..., callback)`. All events are published from inside `loop()` (i.e. from the main thread — no concurrency concerns).

| Event                         | Payload         | Meaning                                               |
| ----------------------------- | --------------- | ----------------------------------------------------- |
| `APP_WIFI_CONNECTING`         | `nullptr`       | A connection attempt has started                      |
| `APP_WIFI_GOT_IP`             | `nullptr`       | IP address assigned by DHCP or static config          |
| `APP_WIFI_CONNECTED`          | `nullptr`       | Connection stable; mDNS started                       |
| `APP_WIFI_DISCONNECTED`       | `nullptr`       | Connection lost, or `disconnect()` confirmed          |
| `APP_WIFI_AUTH_FAILED`        | `nullptr`       | Wrong WiFi password; this profile will not be retried |
| `APP_WIFI_RETRY`              | `RetryPayload*` | Retry attempt in progress                             |
| `APP_WIFI_PORTAL_STARTED`     | `nullptr`       | Captive portal (AP mode) is now active                |
| `APP_WIFI_PORTAL_TIMEOUT`     | `nullptr`       | Captive portal timed out                              |
| `APP_WIFI_PORTAL_STOPPED`     | `nullptr`       | Captive portal torn down and memory freed             |
| `APP_WIFI_WEB_PORTAL_STARTED` | `nullptr`       | Web portal (STA mode) is now active                   |
| `APP_WIFI_WEB_PORTAL_STOPPED` | `nullptr`       | Web portal torn down and memory freed                 |
| `APP_WIFI_CONFIG_SAVED`       | `nullptr`       | New config written; call `getConfig()` to read it     |
| `APP_WIFI_RSSI_LOW`           | `RssiPayload*`  | RSSI dropped below configured threshold               |

### Event Payload Structs

```cpp
struct RetryPayload {
    uint8_t retryCount;   // Current attempt (1-based)
    uint8_t maxRetries;   // Configured maximum
};

struct RssiPayload {
    int8_t rssi;       // Current RSSI in dBm
    int8_t threshold;  // Configured threshold in dBm
};
```

Payload pointers are only valid for the duration of the callback. Do not store them.

---

## State Machine

The manager is driven entirely by a non-blocking state machine. Every `loop()` call executes exactly one state handler. Transitions are instantaneous (no blocking waits anywhere — delays are implemented as timestamp comparisons).

### State Reference

| State                           | Description                                                                    |
| ------------------------------- | ------------------------------------------------------------------------------ |
| `STATE_START`                   | Entry point. Calls `loadCallback`. Resets all per-boot flags.                  |
| `STATE_DISCONNECTED`            | Idle. Checks for saved credentials; transitions to connect or portal.          |
| `STATE_CONNECTING_START`        | Selects active profile, applies static IP if configured, calls `WiFi.begin()`. |
| `STATE_CONNECTING`              | One-tick landing after `WiFi.begin()`.                                         |
| `STATE_CONNECTING_WAIT`         | Polls `WiFi.status()` until connected, auth failure, or timeout.               |
| `STATE_CONNECTED`               | Stable connection. Polls for drops. Runs RSSI checks.                          |
| `STATE_MDNS_RESTART`            | Transient: one tick between `MDNS.end()` and `MDNS.begin()` for SDK stability. |
| `STATE_CONNECTION_LOST`         | Unexpected drop detected. Resets retry counters; transitions to retrying.      |
| `STATE_AUTH_FAILED`             | Wrong password. Marks profile as failed; tries secondary or opens portal.      |
| `STATE_RETRYING_START`          | Decides whether to retry or switch profiles.                                   |
| `STATE_RETRYING`                | Increments retry counter; publishes `APP_WIFI_RETRY`.                          |
| `STATE_RETRYING_WAIT`           | Non-blocking 5-second delay between retry attempts.                            |
| `STATE_AP_SWITCH_START`         | Flips active profile from primary to secondary (or goes to portal).            |
| `STATE_START_CAPTIVE_PORTAL`    | Starts AP mode, DNS server, and web server.                                    |
| `STATE_STARTING_CAPTIVE_PORTAL` | Brief wait for AP to become ready.                                             |
| `STATE_PORTAL_ACTIVE`           | Portal serving. Polls for save flag or timeout.                                |
| `STATE_PORTAL_COMPLETE`         | Save received and response sent. One-tick wait before teardown.                |
| `STATE_STOPPING_CAPTIVE_PORTAL` | Tears down AP, DNS, and web server. Frees heap memory.                         |
| `STATE_START_WEB_PORTAL`        | Starts web server on STA interface.                                            |
| `STATE_STARTING_WEB_PORTAL`     | One-tick wait; publishes `APP_WIFI_WEB_PORTAL_STARTED`.                        |
| `STATE_WEB_PORTAL_ACTIVE`       | Portal serving. Polls for WiFi drop, save flag, or `/exit`.                    |
| `STATE_STOPPING_WEB_PORTAL`     | Tears down web server. Routes to reconnect or disabled.                        |
| `STATE_RECONNECTING`            | One-tick check: is STA still up after portal closes?                           |
| `STATE_NETWORK_DISABLED`        | `disconnect()` was called. Fully idle; no reconnect.                           |
| `STATE_ERROR`                   | Fatal/unrecoverable error. Logs only.                                          |

### Transition Diagram

```
boot
 │
 ▼
STATE_START ──── loadCallback returns false ──────────────────► STATE_START_CAPTIVE_PORTAL
 │
 │ loadCallback returns true
 ▼
STATE_DISCONNECTED
 │
 ├── has primary or secondary SSID ──► STATE_CONNECTING_START
 │                                          │
 │                                          ▼
 │                                    STATE_CONNECTING_WAIT
 │                                     │         │         │
 │                                  success   auth fail  timeout
 │                                     │         │         │
 │                                     ▼         ▼         ▼
 │                               STATE_CONNECTED  STATE_AUTH_FAILED  STATE_RETRYING_START
 │                                     │               │                    │
 │                                     │     try other profile         retries left?
 │                                     │     or go to portal          yes → STATE_RETRYING
 │                                     │                              no  → STATE_AP_SWITCH_START
 │                                     │                                          │
 │                                     │               secondary exists? ─────────┤
 │                                     │               yes → STATE_CONNECTING_START (secondary)
 │                                     │               no  → STATE_START_CAPTIVE_PORTAL
 │                                     │
 │                              WiFi dropped
 │                                     │
 │                                     ▼
 │                               STATE_CONNECTION_LOST
 │                                     │
 │                                     ▼
 │                               STATE_RETRYING_START ──► (retry flow above)
 │
 └── no credentials ──────────────────► STATE_START_CAPTIVE_PORTAL
                                              │
                                        save received
                                              │
                                              ▼
                                        STATE_PORTAL_COMPLETE
                                              │
                                              ▼
                                        STATE_STOPPING_CAPTIVE_PORTAL
                                              │
                                              ▼
                                        STATE_CONNECTING_START  (fresh attempt)


STATE_CONNECTED ──── startWebPortal() ──► STATE_START_WEB_PORTAL
                                               │
                                               ▼
                                        STATE_WEB_PORTAL_ACTIVE
                                         │           │          │
                                      /exit      WiFi drop   save+WiFi change
                                         │           │          │
                                         └─────┬─────┘          │
                                               ▼                │
                                        STATE_STOPPING_WEB_PORTAL◄──┘
                                               │
                                               ▼
                                        STATE_RECONNECTING
                                         │            │
                                      still up     dropped
                                         │            │
                                         ▼            ▼
                                   STATE_CONNECTED  STATE_DISCONNECTED


any state ──── disconnect() ──► STATE_NETWORK_DISABLED
                                  (no reconnect until begin() called again)
```

### Dual-Profile and Promotion Logic

The manager always starts each boot by trying the **primary** profile. If primary fails (timeout or auth failure), it tries **secondary**. If secondary also fails, or no secondary is configured, the captive portal opens.

When the secondary profile connects successfully, the structs are **swapped** and `saveCallback` is called immediately. On next boot, the previously-secondary network is tried first.

Auth failures short-circuit the retry loop — a profile with a wrong password is never retried within the same boot cycle.

---

## Web Portal

### Pages and Endpoints

| Method | Path        | Auth   | Description                                                        |
| ------ | ----------- | ------ | ------------------------------------------------------------------ |
| `GET`  | `/`         | Yes    | Serves the single-page configuration UI                            |
| `GET`  | `/status`   | Yes    | Returns current WiFi state, IP, RSSI, and masked config as JSON    |
| `GET`  | `/scan`     | Yes    | Triggers async WiFi scan or returns current scan state/results     |
| `POST` | `/save`     | Yes    | Validates and saves config; returns per-field error map on failure |
| `GET`  | `/exit`     | Yes    | Closes the web portal gracefully (web portal mode only)            |
| `GET`  | (OS probes) | **No** | Captive portal detection responses — always 200/204, no auth       |

Captive portal detection paths (no auth, always served):

- `/hotspot-detect.html` — Apple
- `/library/test/success.html` — Apple (older)
- `/generate_204` — Android / Chrome (returns 204)
- `/ncsi.txt` — Windows (returns `"Microsoft NCSI"`)
- `/connecttest.txt` — Windows 10+
- `/redirect` — Android (some)
- `/success.txt` — Firefox

All other unrecognised paths redirect to `/`.

### `/status` Response

```json
{
  "state": "CONNECTED",
  "ip": "192.168.1.100",
  "rssi": -52,
  "ssid": "HomeNet",
  "config": {
    "app_name": "MyDevice",
    "hostname": "sensor1",
    "primary_ssid": "HomeNet",
    "primary_password": "****",
    "secondary_ssid": "",
    "secondary_password": "",
    "use_static_ip": false,
    "mqtt_broker": "192.168.1.50",
    "mqtt_port": 1883,
    "mqtt_user": "user",
    "mqtt_password": "****",
    "mqtt_client_id": "sensor1",
    "mqtt_base_topic": "home/sensor1",
    "mqtt_ha_topic": "homeassistant",
    "ntp_server": "pool.ntp.org",
    "posix_timezone": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "ota_url": "http://example.com/firmware.bin"
  }
}
```

Password fields are always returned as `"****"`. If a field is empty, it is returned as `""`.

### `/scan` Response

```json
{ "scanning": true,  "networks": [] }
{ "scanning": false, "networks": [
    { "ssid": "HomeNet",  "rssi": -45, "encrypted": true  },
    { "ssid": "GuestNet", "rssi": -67, "encrypted": false }
  ]
}
```

Results are sorted by RSSI descending. Scan results are freed from SDK memory immediately after serialisation. The scan button and Save button are disabled while a scan is in progress.

### `/save` Request and Response

Request body is JSON (`Content-Type: application/json`). All fields are optional — only send fields you want to change. Password fields set to `"****"` are ignored (existing value kept).

```json
{
  "app_name":        "MyDevice",
  "hostname":        "sensor1",
  "primary_ssid":    "HomeNet",
  "primary_password":"secret",
  "mqtt_broker":     "192.168.1.50",
  "mqtt_port":       1883,
  "ntp_server":      "pool.ntp.org",
  "posix_timezone":  "UTC0"
}
```

Success response:

```json
{ "success": true, "wifi_restart": false }
```

`wifi_restart: true` means WiFi credentials changed; the device will reconnect. In captive portal mode the portal closes regardless. In web portal mode the portal closes only if `wifi_restart` is true.

Error response:

```json
{
  "success": false,
  "errors": {
    "hostname":    "Hostname cannot start or end with a hyphen",
    "mqtt_broker": "Do not include protocol prefix (mqtt://)"
  }
}
```

The `general` key in `errors` indicates a non-field error (e.g. malformed JSON body). Per-field errors highlight the offending input and switch the UI to the relevant tab automatically.

### Validation Rules

**Hostname**

- Alphanumeric characters and hyphens only
- No leading or trailing hyphens
- No consecutive hyphens
- Maximum 63 characters

**MQTT Broker**

- Format: `host` or `host:port`
- No protocol prefix (`mqtt://`, `mqtts://`)
- Port (if specified) must be numeric and in range 1–65535

**NTP Server**

- Must not be empty
- Must not contain spaces

**Static IP**

- Gateway and subnet must not be `0.0.0.0`
- IP and gateway must be on the same subnet (checked at save time, before any connection attempt)

### Memory Management

The web server and DNS server are heap-allocated **only** when a portal is active. On exit they are stopped, deleted, and all handlers freed. This is critical on ESP8266 where the heap is small (~40 KB usable).

The HTML page (~29 KB) lives in flash (PROGMEM) and is served with `send_P()` — it is never copied to RAM.

---

## RSSI Monitoring

```cpp
wifiManager.setRssiThreshold(-80, 30);
```

While in `STATE_CONNECTED`, the manager samples `WiFi.RSSI()` every `checkIntervalSec` seconds. If RSSI drops below `dbm`, `APP_WIFI_RSSI_LOW` is published once. The event re-arms (fires again) only after RSSI recovers above the threshold and drops below it again (hysteresis).

---

## Deep Sleep / Intentional Offline

```cpp
// Before entering deep sleep:
wifiManager.disconnect();

// In your APP_WIFI_DISCONNECTED handler:
ESP.deepSleep(30e6);
```

`disconnect()` suppresses all reconnection logic. The manager sits idle in `STATE_NETWORK_DISABLED` until `begin()` is called. This is the correct way to hand the radio back before sleeping or doing radio-sensitive operations.

---

## Design Notes

### No Blocking Code

Every wait in the state machine is implemented as a `millis()` comparison. The longest single `loop()` execution is the one-time NVS write that occurs when the secondary profile is promoted to primary. All other operations return immediately.

### Body Parsing in AsyncWebServer

`ESPAsyncWebServer` delivers POST body bytes through a dedicated body callback, not via request parameters. The library accumulates chunks into `_pendingBody` (reset on each new request at `index == 0`) and parses it as JSON when the request handler fires.

### Save-Response Ordering

In the captive portal, the JSON success response is sent and the `_portalSaveComplete` flag is set **before** any server teardown begins. The state machine acts on the flag in the next `loop()` tick, giving the TCP stack time to flush the response to the client.

### Thread Safety

All state machine transitions and event publications happen on the Arduino main thread (inside `loop()`). `AsyncWebServer` callbacks on ESP8266/ESP32 also execute on the main thread. No mutexes are needed.

### Platform Differences

| Feature                  | ESP8266                            | ESP32               |
| ------------------------ | ---------------------------------- | ------------------- |
| mDNS update              | `MDNS.update()` called in `loop()` | Handled internally  |
| Auth failure status      | `WL_WRONG_PASSWORD`                | `WL_CONNECT_FAILED` |
| Encryption type check    | `ENC_TYPE_NONE`                    | `WIFI_AUTH_OPEN`    |
| AsyncWebServer stability | Lighter but less stable under load | More stable         |
