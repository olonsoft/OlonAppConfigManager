#pragma once

// ============================================================
//  AppConfigManager.h
//  WiFi + App configuration manager for ESP8266 / ESP32
// ============================================================

#if !defined(ESP32) && !defined(ESP8266)
    #error "AppConfigManager: unsupported platform. ESP32 or ESP8266 required."
#endif

#include <Arduino.h>
#include <EventBus.h>

#include <functional>

#if defined(ESP32)
    #include <ESPmDNS.h>
    #include <WiFi.h>
#elif defined(ESP8266)
    #include <ESP8266WiFi.h>
    #include <ESP8266mDNS.h>
#endif

// Forward declarations — actual headers included in .cpp only
class AsyncWebServer;
class DNSServer;
class AsyncWebServerRequest;

// ============================================================
//  AppConfig struct
//  Owned by this library. Main program copies to its own struct
//  inside the load/save callbacks.
// ============================================================

struct WiFiProfile {
    char ssid[33]     = { 0 };
    char password[65] = { 0 };
};

struct AppConfig {
    // WiFi profiles
    WiFiProfile primary;
    WiFiProfile secondary;

    // Static IP (shared between both profiles)
    bool      useStaticIP = false;
    IPAddress staticIP;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns1;
    IPAddress dns2;

    // Device
    char appName[33]  = "MyDevice";
    char hostname[64] = "esp-device";

    // MQTT
    char     mqttBroker[129]          = "";
    uint16_t mqttPort                 = 1883;
    char     mqttUser[65]             = "";
    char     mqttPassword[65]         = "";
    char     mqttClientId[33]         = "";
    char     mqttBaseTopic[65]        = "home/device";
    char     mqttHADiscoveryTopic[65] = "homeassistant";

    // Time
    char ntpServer[129]    = "pool.ntp.org";
    char posixTimezone[65] = "UTC0";

    // OTA
    char otaUrl[129] = "";

    // Web portal
    char webPortalPassword[33] = "admin";
};

// ============================================================
//  WiFi state machine states
// ============================================================

enum class AppWiFiState : uint8_t {
    // ---- Startup ----
    STATE_START,                    // Initial state; loads config, resets flags

    // ---- Normal connection flow ----
    STATE_DISCONNECTED,             // Idle; will attempt connect if creds exist
    STATE_CONNECTING_START,         // Arms the connection (selects profile, static IP)
    STATE_CONNECTING,               // WiFi.begin() called
    STATE_CONNECTING_WAIT,          // Polling for WL_CONNECTED / timeout

    // ---- Connected ----
    STATE_CONNECTED,                // Stable connection; RSSI checks run here
    STATE_MDNS_RESTART,             // Transient: MDNS.end() → next tick → MDNS.begin()

    // ---- Connection lost ----
    STATE_CONNECTION_LOST,          // WiFi dropped unexpectedly; brief landing state
    STATE_AUTH_FAILED,              // Wrong password; skip retries for this profile

    // ---- Retry logic ----
    STATE_RETRYING_START,           // Evaluate retry counter
    STATE_RETRYING,                 // Increment counter, publish RETRY event
    STATE_RETRYING_WAIT,            // Non-blocking delay between retries

    // ---- AP switching ----
    STATE_AP_SWITCH_START,          // Flip _activeProfile; go to CONNECTING_START

    // ---- Captive portal (AP mode) ----
    STATE_START_CAPTIVE_PORTAL,     // Entry: set up AP + DNS + web server
    STATE_STARTING_CAPTIVE_PORTAL,  // Waiting for AP + server to be ready
    STATE_PORTAL_ACTIVE,            // Portal serving; poll for timeout / save
    STATE_PORTAL_COMPLETE,          // Save received; send response; set flag for teardown
    STATE_STOPPING_CAPTIVE_PORTAL,  // Teardown AP + DNS + server; free memory

    // ---- Web portal (STA mode, device already connected) ----
    STATE_START_WEB_PORTAL,         // Entry: start web server on STA interface
    STATE_STARTING_WEB_PORTAL,      // Waiting for server ready
    STATE_WEB_PORTAL_ACTIVE,        // Portal serving; poll for /exit / WiFi drop
    STATE_STOPPING_WEB_PORTAL,      // Teardown server; free memory

    // ---- Post-portal re-validation ----
    STATE_RECONNECTING,             // Check if STA is still up after portal closes

    // ---- Intentional offline ----
    STATE_NETWORK_DISABLED,         // disconnect() was called; no reconnect attempts

    // ---- Fatal ----
    STATE_ERROR                     // Unrecoverable (init failure, etc.)
};

// ----  Event payloads  ---------------------------------------

struct RetryPayload {
    uint8_t retryCount;   // Current attempt number (1-based)
    uint8_t maxRetries;
};

struct RssiPayload {
    int8_t rssi;
    int8_t threshold;
};

struct StateChangePayload {
    AppWiFiState prevState;
    AppWiFiState nextState;
};

// ============================================================
//  AppConfigManager
// ============================================================

class AppConfigManager {
  public:
    // ----------------------------------------------------------
    //  Construction
    // ----------------------------------------------------------

    /// @param eventBus  Reference to the application's EventBus instance.
    explicit AppConfigManager(EventBus& eventBus);

    // ----------------------------------------------------------
    //  Mandatory callbacks — call BEFORE begin()
    // ----------------------------------------------------------

    /// Called whenever the library needs to persist the current config.
    /// The main program writes AppConfig fields to its own NVS / Preferences.
    /// Return true on success.
    void setSaveCallback(std::function<bool(const AppConfig&)> fn);

    /// Called at startup and whenever the library needs to reload config.
    /// The main program fills the provided AppConfig from NVS / Preferences.
    /// Return true if valid config was loaded, false if nothing saved yet.
    void setLoadCallback(std::function<bool(AppConfig&)> fn);

    // ----------------------------------------------------------
    //  Lifecycle
    // ----------------------------------------------------------

    /// Initialise WiFi subsystem. Loads config via callback.
    /// Must be called once from setup(). Callbacks must be set first.
    void begin();

    /// Drive the state machine. Must be called every iteration of loop().
    /// Never blocks for more than a few microseconds (except the one-time
    /// NVS write on secondary-profile promotion — see notes in .cpp).
    void loop();

    // ----------------------------------------------------------
    //  Tuning — call before begin()
    // ----------------------------------------------------------

    /// SSID and password for the captive-portal AP.
    /// If password is empty the AP will be open.
    void setPortalCredentials(const String& ssid, const String& password = "");

    /// How long to wait for a connection before declaring timeout.
    /// Default: 15 000 ms
    void setConnectTimeout(unsigned long ms);

    /// How long the captive portal stays active before timing out.
    /// On timeout the portal is torn down and the last known credentials
    /// are retried. Default: 180 000 ms (3 minutes)
    void setPortalTimeout(unsigned long ms);

    /// Number of retry attempts per profile before switching AP or
    /// falling back to the captive portal. Default: 3
    void setConnectRetries(uint8_t retries);

    /// Optional HTTP Basic Auth password for the web portal.
    /// If empty, no authentication is required.
    /// The username is always "admin".
    void setWebPortalPassword(const String& password);

    /// TCP port the web server listens on. Default: 80
    void setWebPortalPort(uint16_t port);

    /// Enable RSSI monitoring while connected.
    /// @param dbm              Threshold in dBm (e.g. -80). Event fires when
    ///                         RSSI drops below this value (hysteresis: re-arms
    ///                         when RSSI rises back above threshold).
    /// @param checkIntervalSec How often to sample RSSI. Default: 30 s
    void setRssiThreshold(int8_t dbm, uint16_t checkIntervalSec = 30);

    // ----------------------------------------------------------
    //  Runtime control
    // ----------------------------------------------------------

    /// Open the web configuration portal while already connected to WiFi.
    /// Transitions: STATE_CONNECTED → STATE_START_WEB_PORTAL.
    /// No-op if not currently in STATE_CONNECTED.
    void startWebPortal();

    /// Gracefully close whichever portal is currently active.
    /// Transitions through STATE_STOPPING_* → STATE_RECONNECTING.
    /// No-op if no portal is active.
    void stopPortal();

    /// Disconnect from WiFi and suppress all future reconnection attempts.
    /// Publishes APP_WIFI_DISCONNECTED once the stack confirms disconnection.
    /// Call begin() to re-enable connection management.
    void disconnect();

    /// Wipe saved WiFi credentials (and optionally the full config) then
    /// restart the captive portal.
    /// @param fullWipe  If true, resets the entire AppConfig to defaults.
    void resetConfig(bool fullWipe = false);

    // ----------------------------------------------------------
    //  Status queries
    // ----------------------------------------------------------

    bool         isConnected() const;
    AppWiFiState getState() const;
    String       getStateString(AppWiFiState state) const;

    // ----------------------------------------------------------
    //  Config access (read-only snapshot of last loaded/saved config)
    // ----------------------------------------------------------

    /// Returns a const reference to the in-memory config.
    /// Do not cache the reference across loop() calls.
    const AppConfig& getConfig() const;

  private:
    // ----------------------------------------------------------
    //  State machine
    // ----------------------------------------------------------

    void handleState_Start();
    void handleState_Disconnected();
    void handleState_ConnectingStart();
    void handleState_Connecting();
    void handleState_ConnectingWait();
    void handleState_Connected();
    void handleState_MdnsRestart();
    void handleState_ConnectionLost();
    void handleState_AuthFailed();
    void handleState_RetryingStart();
    void handleState_Retrying();
    void handleState_RetryingWait();
    void handleState_ApSwitchStart();
    void handleState_StartCaptivePortal();
    void handleState_StartingCaptivePortal();
    void handleState_PortalActive();
    void handleState_PortalComplete();
    void handleState_StoppingCaptivePortal();
    void handleState_StartWebPortal();
    void handleState_StartingWebPortal();
    void handleState_WebPortalActive();
    void handleState_StoppingWebPortal();
    void handleState_Reconnecting();
    void handleState_NetworkDisabled();
    void handleState_Error();

    void transitionTo(AppWiFiState nextState);

    // ----------------------------------------------------------
    //  Web server helpers
    // ----------------------------------------------------------
    void redirectToPortal(AsyncWebServerRequest* request);
    void send204(AsyncWebServerRequest* request);
    void setupCaptivePortalHandlers();
    void startWebServer();
    void stopWebServer();
    void registerRoutes();

    // Route handlers (called from AsyncWebServer callbacks)
    void onGetRoot(AsyncWebServerRequest* request);
    void onGetScan(AsyncWebServerRequest* request);
    void onGetStatus(AsyncWebServerRequest* request);
    void onPostSave(AsyncWebServerRequest* request);
    void onGetExit(AsyncWebServerRequest* request);

    // Auth helper — returns false and sends 401 if auth fails
    bool checkAuth(AsyncWebServerRequest* req);

    // ----------------------------------------------------------
    //  Validation helpers
    // ----------------------------------------------------------

    bool validateHostname(const char* hostname, String& errorOut) const;
    bool validateMqttBroker(const char* broker, String& errorOut) const;
    bool validateNtpServer(const char* server, String& errorOut) const;
    bool validateStaticIP(const AppConfig& cfg, String& errorOut) const;
    bool validatePort(uint16_t port, String& errorOut) const;

    // ----------------------------------------------------------
    //  WiFi scan helpers
    // ----------------------------------------------------------

    void startScan();
    bool isScanComplete() const;
    // Results serialised to JSON; clears scan data from SDK after use
    String getScanResultsJson();

    // ----------------------------------------------------------
    //  Connection helpers
    // ----------------------------------------------------------

    WiFiProfile& activeProfile();       // Returns primary or secondary by _activeProfileIndex
    void         applyStaticIP();
    void         promoteSecondaryToPrimary(); // Swap structs + saveConfig()

    // ----------------------------------------------------------
    //  RSSI helpers (called inside handleState_Connected)
    // ----------------------------------------------------------

    void checkRssi();

    // ----------------------------------------------------------
    //  mDNS helpers
    // ----------------------------------------------------------

    void startMdns();
    void stopMdns();

    // ----------------------------------------------------------
    //  Dependencies
    // ----------------------------------------------------------

    EventBus& _eventBus;

    std::function<bool(const AppConfig&)> _saveCallback;
    std::function<bool(AppConfig&)>       _loadCallback;

    // ----------------------------------------------------------
    //  Configuration (set via setters before begin())
    // ----------------------------------------------------------

    String   _portalSsid        = "ESP-Config";
    String   _portalPassword    = "";
    String   _webPortalPassword = "";
    uint16_t _webPortalPort     = 80;

    unsigned long _connectTimeoutMs = 15000UL;
    unsigned long _portalTimeoutMs  = 180000UL;
    uint8_t       _maxRetries       = 3;

    int8_t   _rssiThreshold         = -80;
    uint16_t _rssiCheckIntervalSec  = 30;
    bool     _rssiMonitoringEnabled = false;

    // ----------------------------------------------------------
    //  Runtime state
    // ----------------------------------------------------------

    AppWiFiState _state     = AppWiFiState::STATE_START;
    AppWiFiState _prevState = AppWiFiState::STATE_START;

    AppConfig _config;

    uint8_t _activeProfileIndex = 0;   // 0 = primary, 1 = secondary
    uint8_t _retryCount         = 0;

    bool _primaryAuthFailed   = false;
    bool _secondaryAuthFailed = false;

    bool _intentionalDisconnect = false; // Set by disconnect(); cleared by begin()
    bool _pendingMdnsRestart    = false; // Set when hostname changes; consumed in STATE_MDNS_RESTART

    // Portal save flow
    bool _portalSaveComplete = false;    // Set in POST /save handler; acted on next tick
    bool _saveInProgress     = false;    // Guards against concurrent POST /save
    bool _wifiChangedOnSave  = false;    // True if WiFi credentials changed in last save

    // Scan state
    bool _scanRequested = false;
    bool _scanRunning   = false;

    // Timestamps
    unsigned long _connectStartMs   = 0;
    unsigned long _portalStartMs    = 0;
    unsigned long _retryWaitStartMs = 0;
    unsigned long _lastSaveMs       = 0;   // Debounce for web portal /save
    unsigned long _lastRssiCheckMs  = 0;

    // RSSI hysteresis
    bool _rssiLowPublished = false;

    // POST /save body accumulator (filled by AsyncWebServer body callback)
    String _pendingBody;

    // mDNS
    bool _mdnsActive = false;

    // ----------------------------------------------------------
    //  Web server + DNS (heap-allocated; nullptr when not in portal)
    // ----------------------------------------------------------

    AsyncWebServer* _webServer = nullptr;
    DNSServer*      _dnsServer = nullptr;
};