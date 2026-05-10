// ============================================================
//  AppConfigManager.cpp
//  WiFi + App configuration manager for ESP8266 / ESP32
// ============================================================

#include "AppConfigManager.h"

#include <ArduinoJson.h>  // v7
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

#include "EventBus.h"

// ============================================================
//  PROGMEM HTML — split into head, body chunks, closing
//  Keep each chunk under 4 KB for safe send_P on ESP8266.
// ============================================================
// (Defined in AppConfigManager_HTML.h)
#include "AppConfigManager_HTML.h"

// ============================================================
//  Constants
// ============================================================
const char* const TAG = "WiFiManager";

static constexpr uint8_t  DNS_PORT         = 53;
static constexpr uint16_t RETRY_WAIT_MS    = 5000;   // delay between retries
static constexpr uint16_t AP_READY_WAIT_MS = 500;    // wait after WiFi.softAP()

// Captive portal probe paths (no auth, return 200/204)
static const char* const CAPTIVE_PATHS[] = {
    // all these endpoints will direct to root
    "/hotspot-detect.html",        // Apple
    "/library/test/success.html",  // Apple (older)
    "/generate_204",               // Android / Chrome
    "/generate204",                // Android too
    "/gen_204",                    // Android (older)
    "/ncsi.txt",                   // Windows
    "/redirect",                   // Android (some)
    "/fwlink",
    "/canonical.html",             // linux
    "/probe",
    "/mtuprobe",
    "/startpage",
    nullptr,
};

// ============================================================
//  Construction
// ============================================================

AppConfigManager::AppConfigManager(EventBus& eventBus) : _eventBus(eventBus) {}

// ============================================================
//  Mandatory callbacks
// ============================================================

void AppConfigManager::setSaveCallback(std::function<bool(const AppConfig&)> fn) {
    _saveCallback = fn;
}

void AppConfigManager::setLoadCallback(std::function<bool(AppConfig&)> fn) {
    _loadCallback = fn;
}

// ============================================================
//  Tuning setters
// ============================================================

void AppConfigManager::setPortalCredentials(const String& ssid, const String& password) {
    _portalSsid     = ssid;
    _portalPassword = password;
}

void AppConfigManager::setConnectTimeout(unsigned long ms) {
    _connectTimeoutMs = ms;
}
void AppConfigManager::setPortalTimeout(unsigned long ms) {
    _portalTimeoutMs = ms;
}
void AppConfigManager::setConnectRetries(uint8_t retries) {
    _maxRetries = retries;
}
void AppConfigManager::setWebPortalPassword(const String& p) {
    _webPortalPassword = p;
}
void AppConfigManager::setWebPortalPort(uint16_t port) {
    _webPortalPort = port;
}

void AppConfigManager::setRssiThreshold(int8_t dbm, uint16_t checkIntervalSec) {
    _rssiThreshold         = dbm;
    _rssiCheckIntervalSec  = checkIntervalSec;
    _rssiMonitoringEnabled = true;
}

// ============================================================
//  Lifecycle
// ============================================================

void AppConfigManager::begin() {
    _intentionalDisconnect = false;
    _state                 = AppWiFiState::STATE_START;

#if defined(ESP8266)
    WiFi.persistent(false);   // We manage credentials ourselves
#endif
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(false);
}

void AppConfigManager::loop() {
    // Drive mDNS on ESP8266 (ESP32 handles it internally)
#if defined(ESP8266)
    if (_mdnsActive) {
        MDNS.update();
    }
#endif

    // Drive DNS server when captive portal is active
    if (_dnsServer) {
        _dnsServer->processNextRequest();
    }

    // Main state machine dispatch
    switch (_state) {
        case AppWiFiState::STATE_START:                   handleState_Start(); break;
        case AppWiFiState::STATE_DISCONNECTED:            handleState_Disconnected(); break;
        case AppWiFiState::STATE_CONNECTING_START:        handleState_ConnectingStart(); break;
        case AppWiFiState::STATE_CONNECTING:              handleState_Connecting(); break;
        case AppWiFiState::STATE_CONNECTING_WAIT:         handleState_ConnectingWait(); break;
        case AppWiFiState::STATE_CONNECTED:               handleState_Connected(); break;
        case AppWiFiState::STATE_MDNS_RESTART:            handleState_MdnsRestart(); break;
        case AppWiFiState::STATE_CONNECTION_LOST:         handleState_ConnectionLost(); break;
        case AppWiFiState::STATE_AUTH_FAILED:             handleState_AuthFailed(); break;
        case AppWiFiState::STATE_RETRYING_START:          handleState_RetryingStart(); break;
        case AppWiFiState::STATE_RETRYING:                handleState_Retrying(); break;
        case AppWiFiState::STATE_RETRYING_WAIT:           handleState_RetryingWait(); break;
        case AppWiFiState::STATE_AP_SWITCH_START:         handleState_ApSwitchStart(); break;
        case AppWiFiState::STATE_START_CAPTIVE_PORTAL:    handleState_StartCaptivePortal(); break;
        case AppWiFiState::STATE_STARTING_CAPTIVE_PORTAL: handleState_StartingCaptivePortal(); break;
        case AppWiFiState::STATE_PORTAL_ACTIVE:           handleState_PortalActive(); break;
        case AppWiFiState::STATE_PORTAL_COMPLETE:         handleState_PortalComplete(); break;
        case AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL: handleState_StoppingCaptivePortal(); break;
        case AppWiFiState::STATE_START_WEB_PORTAL:        handleState_StartWebPortal(); break;
        case AppWiFiState::STATE_STARTING_WEB_PORTAL:     handleState_StartingWebPortal(); break;
        case AppWiFiState::STATE_WEB_PORTAL_ACTIVE:       handleState_WebPortalActive(); break;
        case AppWiFiState::STATE_STOPPING_WEB_PORTAL:     handleState_StoppingWebPortal(); break;
        case AppWiFiState::STATE_RECONNECTING:            handleState_Reconnecting(); break;
        case AppWiFiState::STATE_NETWORK_DISABLED:        handleState_NetworkDisabled(); break;
        case AppWiFiState::STATE_ERROR:                   handleState_Error(); break;
    }
}

// ============================================================
//  Runtime control
// ============================================================

void AppConfigManager::startWebPortal() {
    if (_state == AppWiFiState::STATE_CONNECTED) {
        transitionTo(AppWiFiState::STATE_START_WEB_PORTAL);
    }
}

void AppConfigManager::stopPortal() {
    if (_state == AppWiFiState::STATE_PORTAL_ACTIVE || _state == AppWiFiState::STATE_STARTING_CAPTIVE_PORTAL) {
        transitionTo(AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL);
    } else if (_state == AppWiFiState::STATE_WEB_PORTAL_ACTIVE || _state == AppWiFiState::STATE_STARTING_WEB_PORTAL) {
        transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
    }
}

void AppConfigManager::disconnect() {
    _intentionalDisconnect = true;

    // If a portal is up, begin tearing it down first.
    // The stopping states check _intentionalDisconnect and route
    // to STATE_NETWORK_DISABLED instead of their normal next state.
    if (_state == AppWiFiState::STATE_PORTAL_ACTIVE || _state == AppWiFiState::STATE_STARTING_CAPTIVE_PORTAL ||
        _state == AppWiFiState::STATE_PORTAL_COMPLETE) {
        transitionTo(AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL);
    } else if (_state == AppWiFiState::STATE_WEB_PORTAL_ACTIVE || _state == AppWiFiState::STATE_STARTING_WEB_PORTAL) {
        transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
    } else {
        // No portal running — disconnect WiFi directly and land in disabled
        WiFi.disconnect(true);
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
        _eventBus.publish(EventType::APP_WIFI_DISCONNECTED);
    }
}

void AppConfigManager::resetConfig(bool fullWipe) {
    if (fullWipe) {
        _config = AppConfig{};  // Reset to defaults
    } else {
        // Wipe only WiFi credentials
        _config.primary   = WiFiProfile{};
        _config.secondary = WiFiProfile{};
    }
    if (_saveCallback) _saveCallback(_config);

    // Stop whatever is running and go to captive portal
    stopWebServer();
    WiFi.disconnect(true);
    stopMdns();
    transitionTo(AppWiFiState::STATE_START_CAPTIVE_PORTAL);
}

// ============================================================
//  Status queries
// ============================================================

bool AppConfigManager::isConnected() const {
    return _state == AppWiFiState::STATE_CONNECTED || _state == AppWiFiState::STATE_WEB_PORTAL_ACTIVE ||
           _state == AppWiFiState::STATE_MDNS_RESTART;
}

AppWiFiState AppConfigManager::getState() const {
    return _state;
}

const AppConfig& AppConfigManager::getConfig() const {
    return _config;
}

String AppConfigManager::getStateString(AppWiFiState state) const {
    switch (state) {
        case AppWiFiState::STATE_START:                   return F("START");
        case AppWiFiState::STATE_DISCONNECTED:            return F("DISCONNECTED");
        case AppWiFiState::STATE_CONNECTING_START:        return F("CONNECTING_START");
        case AppWiFiState::STATE_CONNECTING:              return F("CONNECTING");
        case AppWiFiState::STATE_CONNECTING_WAIT:         return F("CONNECTING_WAIT");
        case AppWiFiState::STATE_CONNECTED:               return F("CONNECTED");
        case AppWiFiState::STATE_MDNS_RESTART:            return F("MDNS_RESTART");
        case AppWiFiState::STATE_CONNECTION_LOST:         return F("CONNECTION_LOST");
        case AppWiFiState::STATE_AUTH_FAILED:             return F("AUTH_FAILED");
        case AppWiFiState::STATE_RETRYING_START:          return F("RETRYING_START");
        case AppWiFiState::STATE_RETRYING:                return F("RETRYING");
        case AppWiFiState::STATE_RETRYING_WAIT:           return F("RETRYING_WAIT");
        case AppWiFiState::STATE_AP_SWITCH_START:         return F("AP_SWITCH_START");
        case AppWiFiState::STATE_START_CAPTIVE_PORTAL:    return F("START_CAPTIVE_PORTAL");
        case AppWiFiState::STATE_STARTING_CAPTIVE_PORTAL: return F("STARTING_CAPTIVE_PORTAL");
        case AppWiFiState::STATE_PORTAL_ACTIVE:           return F("PORTAL_ACTIVE");
        case AppWiFiState::STATE_PORTAL_COMPLETE:         return F("PORTAL_COMPLETE");
        case AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL: return F("STOPPING_CAPTIVE_PORTAL");
        case AppWiFiState::STATE_START_WEB_PORTAL:        return F("START_WEB_PORTAL");
        case AppWiFiState::STATE_STARTING_WEB_PORTAL:     return F("STARTING_WEB_PORTAL");
        case AppWiFiState::STATE_WEB_PORTAL_ACTIVE:       return F("WEB_PORTAL_ACTIVE");
        case AppWiFiState::STATE_STOPPING_WEB_PORTAL:     return F("STOPPING_WEB_PORTAL");
        case AppWiFiState::STATE_RECONNECTING:            return F("RECONNECTING");
        case AppWiFiState::STATE_NETWORK_DISABLED:        return F("NETWORK_DISABLED");
        case AppWiFiState::STATE_ERROR:                   return F("ERROR");
        default:                                          return F("UNKNOWN");
    }
}

// ============================================================
//  State machine — transition helper
// ============================================================

void AppConfigManager::transitionTo(AppWiFiState nextState) {
    _prevState = _state;
    StateChangePayload payload { _prevState, nextState };
    _eventBus.publish(EventType::APP_WIFI_STATE_CHANGE, &payload);
    _state = nextState;
}

// ============================================================
//  STATE_START
//  Load config. Reset per-boot flags. Decide first move.
// ============================================================

void AppConfigManager::handleState_Start() {
    // Reset per-boot tracking
    _activeProfileIndex  = 0;
    _retryCount          = 0;
    _primaryAuthFailed   = false;
    _secondaryAuthFailed = false;
    _rssiLowPublished    = false;
    _portalSaveComplete  = false;
    _saveInProgress      = false;
    _wifiChangedOnSave   = false;
    _scanRunning         = false;

    // Load config via callback.
    // If loading fails (nothing saved yet) go straight to captive portal.
    bool hasConfig = false;
    if (_loadCallback) {
        hasConfig = _loadCallback(_config);
    }

    if (_intentionalDisconnect) {
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
    } else if (!hasConfig) {
        transitionTo(AppWiFiState::STATE_START_CAPTIVE_PORTAL);
    } else {
        transitionTo(AppWiFiState::STATE_DISCONNECTED);
    }
}

// ============================================================
//  STATE_DISCONNECTED
//  Decide: connect if creds exist, otherwise open captive portal.
// ============================================================

void AppConfigManager::handleState_Disconnected() {
    // Intentional disconnect takes priority
    if (_intentionalDisconnect) {
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
        return;
    }

    bool hasPrimary   = (_config.primary.ssid[0] != '\0');
    bool hasSecondary = (_config.secondary.ssid[0] != '\0');

    if (hasPrimary || hasSecondary) {
        _activeProfileIndex  = 0;   // Always start with primary
        _retryCount          = 0;
        _primaryAuthFailed   = false;
        _secondaryAuthFailed = false;
        transitionTo(AppWiFiState::STATE_CONNECTING_START);
    } else {
        transitionTo(AppWiFiState::STATE_START_CAPTIVE_PORTAL);
    }
}

// ============================================================
//  STATE_CONNECTING_START
//  Set up WiFi mode, apply static IP if needed, call WiFi.begin().
// ============================================================

void AppConfigManager::handleState_ConnectingStart() {
    WiFiProfile& profile = activeProfile();

    // Safety: if this profile has no SSID, skip it
    if (profile.ssid[0] == '\0') {
        transitionTo(AppWiFiState::STATE_AP_SWITCH_START);
        return;
    }

    WiFi.mode(WIFI_STA);

    if (_config.useStaticIP) {
        applyStaticIP();
    }

    WiFi.begin(profile.ssid, profile.password);
    _connectStartMs = millis();

    _eventBus.publish(EventType::APP_WIFI_CONNECTING);
    transitionTo(AppWiFiState::STATE_CONNECTING);
}

// ============================================================
//  STATE_CONNECTING
//  One-shot entry; immediately move to WAIT.
//  Kept separate so future hooks have a clean landing point.
// ============================================================

void AppConfigManager::handleState_Connecting() {
    transitionTo(AppWiFiState::STATE_CONNECTING_WAIT);
}

// ============================================================
//  STATE_CONNECTING_WAIT
//  Poll WiFi status. Handle success, auth failure, timeout.
// ============================================================

void AppConfigManager::handleState_ConnectingWait() {
    wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
        // Connected — check if secondary promoted to primary
        if (_activeProfileIndex == 1) {
            promoteSecondaryToPrimary();
        }
        _eventBus.publish(EventType::APP_WIFI_GOT_IP);
        startMdns();
        _rssiLowPublished = false;
        _lastRssiCheckMs  = millis();
        transitionTo(AppWiFiState::STATE_CONNECTED);
        _eventBus.publish(EventType::APP_WIFI_CONNECTED);
        return;
    }

#if defined(ESP8266)
    if (status == WL_WRONG_PASSWORD) {
#elif defined(ESP32)
    if (status == WL_CONNECT_FAILED) {
        // ESP32 surfaces auth failure as WL_CONNECT_FAILED after several attempts
#endif
        transitionTo(AppWiFiState::STATE_AUTH_FAILED);
        return;
    }

    // Timeout
    if (millis() - _connectStartMs >= _connectTimeoutMs) {
        transitionTo(AppWiFiState::STATE_RETRYING_START);
    }
}

// ============================================================
//  STATE_CONNECTED
//  Steady state. Poll for drop. Run RSSI checks.
// ============================================================

void AppConfigManager::handleState_Connected() {
    // Handle pending mDNS restart (hostname changed via web portal)
    if (_pendingMdnsRestart) {
        _pendingMdnsRestart = false;
        stopMdns();
        transitionTo(AppWiFiState::STATE_MDNS_RESTART);
        return;
    }

    // Detect WiFi drop
    if (WiFi.status() != WL_CONNECTED) {
        transitionTo(AppWiFiState::STATE_CONNECTION_LOST);
        return;
    }

    // RSSI monitoring (non-blocking side-effect)
    if (_rssiMonitoringEnabled) {
        checkRssi();
    }
}

// ============================================================
//  STATE_MDNS_RESTART
//  One tick gap between MDNS.end() and MDNS.begin() for stability.
// ============================================================

void AppConfigManager::handleState_MdnsRestart() {
    startMdns();
    transitionTo(AppWiFiState::STATE_CONNECTED);
}

// ============================================================
//  STATE_CONNECTION_LOST
//  Brief landing after unexpected drop.
// ============================================================

void AppConfigManager::handleState_ConnectionLost() {
    stopMdns();
    _eventBus.publish(EventType::APP_WIFI_DISCONNECTED);

    if (_intentionalDisconnect) {
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
        return;
    }

    _retryCount          = 0;
    _activeProfileIndex  = 0;
    _primaryAuthFailed   = false;
    _secondaryAuthFailed = false;
    transitionTo(AppWiFiState::STATE_RETRYING_START);
}

// ============================================================
//  STATE_AUTH_FAILED
//  Wrong password. Mark profile, try other, or go to portal.
// ============================================================

void AppConfigManager::handleState_AuthFailed() {
    WiFi.disconnect(true);
    _eventBus.publish(EventType::APP_WIFI_AUTH_FAILED);

    if (_activeProfileIndex == 0) {
        _primaryAuthFailed = true;
        // Try secondary if it exists and hasn't already failed
        if (_config.secondary.ssid[0] != '\0' && !_secondaryAuthFailed) {
            _activeProfileIndex = 1;
            transitionTo(AppWiFiState::STATE_CONNECTING_START);
        } else {
            transitionTo(AppWiFiState::STATE_START_CAPTIVE_PORTAL);
        }
    } else {
        _secondaryAuthFailed = true;
        transitionTo(AppWiFiState::STATE_START_CAPTIVE_PORTAL);
    }
}

// ============================================================
//  STATE_RETRYING_START
//  Decide whether to retry, switch AP, or open portal.
// ============================================================

void AppConfigManager::handleState_RetryingStart() {
    if (_retryCount < _maxRetries) {
        transitionTo(AppWiFiState::STATE_RETRYING);
    } else {
        // Exhausted retries for this profile
        transitionTo(AppWiFiState::STATE_AP_SWITCH_START);
    }
}

// ============================================================
//  STATE_RETRYING
//  Increment counter, publish event, begin wait.
// ============================================================

void AppConfigManager::handleState_Retrying() {
    _retryCount++;
    RetryPayload payload{ _retryCount, _maxRetries };
    _eventBus.publish(EventType::APP_WIFI_RETRY, &payload);
    _retryWaitStartMs = millis();
    transitionTo(AppWiFiState::STATE_RETRYING_WAIT);
}

// ============================================================
//  STATE_RETRYING_WAIT
//  Non-blocking delay, then try connecting again.
// ============================================================

void AppConfigManager::handleState_RetryingWait() {
    if (millis() - _retryWaitStartMs >= RETRY_WAIT_MS) {
        transitionTo(AppWiFiState::STATE_CONNECTING_START);
    }
}

// ============================================================
//  STATE_AP_SWITCH_START
//  Flip active profile if possible; otherwise open portal.
// ============================================================

void AppConfigManager::handleState_ApSwitchStart() {
    if (_activeProfileIndex == 0 && _config.secondary.ssid[0] != '\0' && !_secondaryAuthFailed) {
        _activeProfileIndex = 1;
        _retryCount         = 0;   // Fresh retry budget for secondary
        transitionTo(AppWiFiState::STATE_CONNECTING_START);
    } else {
        transitionTo(AppWiFiState::STATE_START_CAPTIVE_PORTAL);
    }
}

// ============================================================
//  STATE_START_CAPTIVE_PORTAL
//  Bring up AP + DNS + web server.
// ============================================================

void AppConfigManager::handleState_StartCaptivePortal() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);

    // Use last known channel if we have a primary SSID (avoids channel switching)
    int channel = 1;
#if defined(ESP8266)
    // WiFi.channel() is only valid in STA mode; use 1 as safe default after disconnect
#endif

    if (_portalPassword.length() > 0) {
        WiFi.softAP(_portalSsid.c_str(), _portalPassword.c_str(), channel);
    } else {
        WiFi.softAP(_portalSsid.c_str(), nullptr, channel);
    }

    _portalStartMs      = millis();
    _portalSaveComplete = false;
    _saveInProgress     = false;

    // Start DNS — redirects all queries to our IP
        // Start DNS — redirects all queries to our IP.
    // Increase buffer beyond the 512-byte default so large EDNS queries
    // (common on Android) are handled rather than silently dropped.
    _dnsServer = new DNSServer();
    _dnsServer->setTTL(300);
    _dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    _dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());

    startWebServer();

    transitionTo(AppWiFiState::STATE_STARTING_CAPTIVE_PORTAL);
}

// ============================================================
//  STATE_STARTING_CAPTIVE_PORTAL
//  Wait for AP to be ready (brief fixed delay).
// ============================================================

void AppConfigManager::handleState_StartingCaptivePortal() {
    // AP + web server are ready after one short wait
    if (millis() - _portalStartMs >= AP_READY_WAIT_MS) {
        _eventBus.publish(EventType::APP_WIFI_PORTAL_STARTED);
        transitionTo(AppWiFiState::STATE_PORTAL_ACTIVE);
    }
}

// ============================================================
//  STATE_PORTAL_ACTIVE
//  Poll for save flag or timeout.
// ============================================================

void AppConfigManager::handleState_PortalActive() {
    // Save received from POST handler
    if (_portalSaveComplete) {
        transitionTo(AppWiFiState::STATE_PORTAL_COMPLETE);
        return;
    }

    // Portal timeout
    if (millis() - _portalStartMs >= _portalTimeoutMs) {
        _eventBus.publish(EventType::APP_WIFI_PORTAL_TIMEOUT);
        transitionTo(AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL);
    }
}

// ============================================================
//  STATE_PORTAL_COMPLETE
//  Response already sent to browser. Now tear down and connect.
// ============================================================

void AppConfigManager::handleState_PortalComplete() {
    // _portalSaveComplete was set; config already saved inside POST handler.
    // One tick has elapsed since the response was sent — safe to tear down.
    transitionTo(AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL);
}

// ============================================================
//  STATE_STOPPING_CAPTIVE_PORTAL
//  Tear down everything. Route to correct next state.
// ============================================================

void AppConfigManager::handleState_StoppingCaptivePortal() {
    stopWebServer();

    if (_dnsServer) {
        _dnsServer->stop();
        delete _dnsServer;
        _dnsServer = nullptr;
    }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);

    _eventBus.publish(EventType::APP_WIFI_PORTAL_STOPPED);

    if (_intentionalDisconnect) {
        _eventBus.publish(EventType::APP_WIFI_DISCONNECTED);
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
        return;
    }

    // After a portal save, start fresh connection with new credentials
    _activeProfileIndex  = 0;
    _retryCount          = 0;
    _primaryAuthFailed   = false;
    _secondaryAuthFailed = false;
    _portalSaveComplete  = false;
    _saveInProgress      = false;

    transitionTo(AppWiFiState::STATE_CONNECTING_START);
}

// ============================================================
//  STATE_START_WEB_PORTAL
//  Start web server while STA is still connected.
// ============================================================

void AppConfigManager::handleState_StartWebPortal() {
    startWebServer();
    transitionTo(AppWiFiState::STATE_STARTING_WEB_PORTAL);
}

// ============================================================
//  STATE_STARTING_WEB_PORTAL
//  AsyncWebServer is ready synchronously after begin(); one tick.
// ============================================================

void AppConfigManager::handleState_StartingWebPortal() {
    _eventBus.publish(EventType::APP_WIFI_WEB_PORTAL_STARTED);
    _lastSaveMs     = 0;
    _saveInProgress = false;
    transitionTo(AppWiFiState::STATE_WEB_PORTAL_ACTIVE);
}

// ============================================================
//  STATE_WEB_PORTAL_ACTIVE
//  Poll for WiFi drop (→ stop portal) or save-triggered reconnect.
// ============================================================

void AppConfigManager::handleState_WebPortalActive() {
    // WiFi dropped while portal was open
    if (WiFi.status() != WL_CONNECTED) {
        // Stop server before going anywhere
        transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
        return;
    }

    // Save with WiFi change → need to reconnect
    if (_portalSaveComplete && _wifiChangedOnSave) {
        transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
        return;
    }

    // Save with no WiFi change → stay connected, just publish event
    if (_portalSaveComplete && !_wifiChangedOnSave) {
        _portalSaveComplete = false;
        _saveInProgress     = false;
        _eventBus.publish(EventType::APP_WIFI_CONFIG_SAVED);

        // Hostname may have changed
        if (_pendingMdnsRestart) {
            _pendingMdnsRestart = false;
            stopMdns();
            transitionTo(AppWiFiState::STATE_MDNS_RESTART);
        }
    }
}

// ============================================================
//  STATE_STOPPING_WEB_PORTAL
//  Tear down web server. Route based on why we stopped.
// ============================================================

void AppConfigManager::handleState_StoppingWebPortal() {
    stopWebServer();
    _eventBus.publish(EventType::APP_WIFI_WEB_PORTAL_STOPPED);

    if (_intentionalDisconnect) {
        WiFi.disconnect(true);
        stopMdns();
        _eventBus.publish(EventType::APP_WIFI_DISCONNECTED);
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
        return;
    }

    if (_portalSaveComplete && _wifiChangedOnSave) {
        _portalSaveComplete  = false;
        _saveInProgress      = false;
        _wifiChangedOnSave   = false;
        _activeProfileIndex  = 0;
        _retryCount          = 0;
        _primaryAuthFailed   = false;
        _secondaryAuthFailed = false;
        stopMdns();
        WiFi.disconnect(true);
        transitionTo(AppWiFiState::STATE_CONNECTING_START);
        return;
    }

    // Normal exit (/exit button, stopPortal(), or WiFi drop)
    _portalSaveComplete = false;
    _saveInProgress     = false;
    transitionTo(AppWiFiState::STATE_RECONNECTING);
}

// ============================================================
//  STATE_RECONNECTING
//  Verify STA connection is still up after portal teardown.
// ============================================================

void AppConfigManager::handleState_Reconnecting() {
    if (WiFi.status() == WL_CONNECTED) {
        transitionTo(AppWiFiState::STATE_CONNECTED);
    } else {
        stopMdns();
        _eventBus.publish(EventType::APP_WIFI_DISCONNECTED);
        _retryCount          = 0;
        _activeProfileIndex  = 0;
        _primaryAuthFailed   = false;
        _secondaryAuthFailed = false;
        transitionTo(AppWiFiState::STATE_DISCONNECTED);
    }
}

// ============================================================
//  STATE_NETWORK_DISABLED
//  Intentional offline. Do nothing until begin() is called again.
// ============================================================

void AppConfigManager::handleState_NetworkDisabled() {
    // Idle. The main program must call begin() to re-enable.
}

// ============================================================
//  STATE_ERROR
//  Fatal. Log and idle.
// ============================================================

void AppConfigManager::handleState_Error() {
    // Nothing to do; main program should observe getState() == STATE_ERROR.
}

// ============================================================
//  Connection helpers
// ============================================================

WiFiProfile& AppConfigManager::activeProfile() {
    return (_activeProfileIndex == 0) ? _config.primary : _config.secondary;
}

void AppConfigManager::applyStaticIP() {
    WiFi.config(_config.staticIP, _config.gateway, _config.subnet, _config.dns1, _config.dns2);
}

void AppConfigManager::promoteSecondaryToPrimary() {
    // Swap structs
    WiFiProfile tmp     = _config.primary;
    _config.primary     = _config.secondary;
    _config.secondary   = tmp;
    _activeProfileIndex = 0;

    // Persist — one-time blocking NVS write; acceptable per design decision
    if (_saveCallback) _saveCallback(_config);
}

// ============================================================
//  RSSI helpers
// ============================================================

void AppConfigManager::checkRssi() {
    uint32_t intervalMs = (uint32_t)_rssiCheckIntervalSec * 1000UL;
    if (millis() - _lastRssiCheckMs < intervalMs) return;

    _lastRssiCheckMs = millis();
    int8_t rssi      = (int8_t)WiFi.RSSI();

    if (rssi < _rssiThreshold && !_rssiLowPublished) {
        RssiPayload payload{ rssi, _rssiThreshold };
        _eventBus.publish(EventType::APP_WIFI_RSSI_LOW, &payload);
        _rssiLowPublished = true;
    } else if (rssi >= _rssiThreshold) {
        _rssiLowPublished = false;   // Re-arm hysteresis
    }
}

// ============================================================
//  mDNS helpers
// ============================================================

void AppConfigManager::startMdns() {
    if (_mdnsActive) return;
    if (MDNS.begin(_config.hostname)) {
        _mdnsActive = true;
    }
}

void AppConfigManager::stopMdns() {
    if (!_mdnsActive) return;
    MDNS.end();
    _mdnsActive = false;
}

// ============================================================
//  WiFi scan helpers
// ============================================================

void AppConfigManager::startScan() {
    if (_scanRunning) return;
    WiFi.scanNetworks(/*async=*/true);
    _scanRunning = true;
}

bool AppConfigManager::isScanComplete() const {
    return WiFi.scanComplete() >= 0;   // -1 = running, -2 = not started
}

String AppConfigManager::getScanResultsJson() {
    int n = WiFi.scanComplete();
    if (n < 0) return F("{\"scanning\":true,\"networks\":[]}");

    // Sort by RSSI descending (ESP8266 does not do this automatically)
    // Build index array and sort it
    std::vector<int> indices(n);
    for (int i = 0; i < n; i++)
        indices[i] = i;
    std::sort(indices.begin(), indices.end(), [](int a, int b) {
        return WiFi.RSSI(a) > WiFi.RSSI(b);
    });

    JsonDocument doc;
    doc["scanning"] = false;
    JsonArray arr   = doc["networks"].to<JsonArray>();

    for (int i = 0; i < n; i++) {
        int        idx   = indices[i];
        JsonObject net   = arr.add<JsonObject>();
        net["ssid"]      = WiFi.SSID(idx);
        net["rssi"]      = WiFi.RSSI(idx);
        net["encrypted"] = (WiFi.encryptionType(idx) !=
#if defined(ESP32)
                            WIFI_AUTH_OPEN
#else
                            ENC_TYPE_NONE
#endif
        );
    }

    // Free SDK scan memory now that we've consumed results
    WiFi.scanDelete();
    _scanRunning = false;

    String out;
    serializeJson(doc, out);
    return out;
}

// ============================================================
//  Validation helpers
// ============================================================

bool AppConfigManager::validateHostname(const char* hostname, String& err) const {
    size_t len = strlen(hostname);
    if (len == 0 || len > 63) {
        err = F("Hostname must be 1–63 characters");
        return false;
    }
    if (hostname[0] == '-' || hostname[len - 1] == '-') {
        err = F("Hostname cannot start or end with a hyphen");
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = hostname[i];
        if (!isalnum(c) && c != '-') {
            err = F("Hostname may only contain letters, digits, and hyphens");
            return false;
        }
        if (c == '-' && i > 0 && hostname[i - 1] == '-') {
            err = F("Hostname cannot contain consecutive hyphens");
            return false;
        }
    }
    return true;
}

bool AppConfigManager::validateMqttBroker(const char* broker, String& err) const {
    if (strlen(broker) == 0) return true;  // Empty is OK (MQTT optional)

    // Reject protocol prefixes
    if (strncmp(broker, "mqtt://", 7) == 0 || strncmp(broker, "mqtts://", 8) == 0) {
        err = F("Do not include protocol prefix (mqtt://)");
        return false;
    }

    // Format must be host or host:port
    String b(broker);
    int    colon = b.lastIndexOf(':');
    if (colon > 0) {
        String portStr = b.substring(colon + 1);
        for (char c : portStr) {
            if (!isdigit(c)) {
                err = F("Port must be numeric");
                return false;
            }
        }
        uint32_t port = portStr.toInt();
        if (port < 1 || port > 65535) {
            err = F("Port must be between 1 and 65535");
            return false;
        }
    }
    return true;
}

bool AppConfigManager::validateNtpServer(const char* server, String& err) const {
    if (strlen(server) == 0) {
        err = F("NTP server cannot be empty");
        return false;
    }
    // Basic check: no spaces, at least one dot or it's "localhost"
    String s(server);
    if (s.indexOf(' ') >= 0) {
        err = F("NTP server contains invalid characters");
        return false;
    }
    return true;
}

bool AppConfigManager::validateStaticIP(const AppConfig& cfg, String& err) const {
    if (!cfg.useStaticIP) return true;

    // Gateway must be non-zero
    if (cfg.gateway == IPAddress(0, 0, 0, 0)) {
        err = F("Gateway address is required for static IP");
        return false;
    }
    // Subnet must be non-zero
    if (cfg.subnet == IPAddress(0, 0, 0, 0)) {
        err = F("Subnet mask is required for static IP");
        return false;
    }
    // IP and gateway must be on the same subnet
    uint32_t ip  = (uint32_t)cfg.staticIP;
    uint32_t gw  = (uint32_t)cfg.gateway;
    uint32_t sub = (uint32_t)cfg.subnet;
    if ((ip & sub) != (gw & sub)) {
        err = F("Static IP and gateway are not on the same subnet");
        return false;
    }
    return true;
}

bool AppConfigManager::validatePort(uint16_t port, String& err) const {
    if (port < 1) {
        err = F("Port must be between 1 and 65535");
        return false;
    }
    return true;
}

// ============================================================
//  Web server — lifecycle
// ============================================================

void AppConfigManager::startWebServer() {
    if (_webServer) return;   // Already running
    _webServer = new AsyncWebServer(_webPortalPort);
    registerRoutes();
    _webServer->begin();
}

void AppConfigManager::stopWebServer() {
    if (!_webServer) return;
    _webServer->end();
    delete _webServer;
    _webServer = nullptr;
}

// Helper: Redirect to portal with proper cache-control headers
void AppConfigManager::redirectToPortal(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(302);
    response->addHeader(F("Location"), "http://" + WiFi.softAPIP().toString() + "/");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
}

// Helper: Send 204 No Content with cache headers (good for Android)
void AppConfigManager::send204(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(204);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
}

void AppConfigManager::setupCaptivePortalHandlers() {
    for (uint8_t i = 0; CAPTIVE_PATHS[i] != nullptr; i++) {
        _webServer->on(CAPTIVE_PATHS[i], HTTP_GET, [this](AsyncWebServerRequest* req) {
            redirectToPortal(req);
        });
    }

    // rest of the captive portal endpoints, not handled by redirect to root
    // Windows
    _webServer->on("/connecttest.txt", HTTP_GET, [this](AsyncWebServerRequest* request) {
        request->redirect("http://logout.net");
    });

    _webServer->on("/nm", HTTP_GET, [this](AsyncWebServerRequest* request) {
        request->send(204, "text/plain", "");
    });

    // Others
    _webServer->on("/wpad.dat", HTTP_GET, [this](AsyncWebServerRequest* request) {
        request->send(404);
    });

    _webServer->on("/success.txt", HTTP_GET, [this](AsyncWebServerRequest* request) {
        request->send(200);
    });
}

// ============================================================
//  Web server — route registration
// ============================================================

void AppConfigManager::registerRoutes() {
    setupCaptivePortalHandlers();

    _webServer->on("/favicon.ico", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(204, "text/plain", "");
    });

    // ---- Config page ----
    _webServer->on("/", HTTP_GET, [this](AsyncWebServerRequest* req) {
        onGetRoot(req);
    });

    // ---- Scan ----
    _webServer->on("/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
        onGetScan(req);
    });

    // ---- Status ----
    _webServer->on("/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        onGetStatus(req);
    });

    // ---- Save ----
    _webServer->on("/save",
                   HTTP_POST,
                   [this](AsyncWebServerRequest* req) {
        onPostSave(req);
    },
                   nullptr,   // upload handler (not used)
                   [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            // Accumulate body chunks into _pendingBody.
            // index==0 means first chunk — reset the buffer.
        if (index == 0) _pendingBody = "";
        _pendingBody.concat(reinterpret_cast<const char*>(data), len);
    });

    // ---- Exit (web portal only) ----
    _webServer->on("/exit", HTTP_GET, [this](AsyncWebServerRequest* req) {
        onGetExit(req);
    });

    // ---- 404 — redirect everything to portal root (captive portal behaviour) ----
    // Use the actual AP IP rather than a hardcoded address.
    // Also send a 302 with explicit Location so all OS captive portal
    // detectors follow it correctly.
    _webServer->onNotFound([this](AsyncWebServerRequest* req) {
        redirectToPortal(req);
    });
}

// ============================================================
//  Auth helper
// ============================================================

bool AppConfigManager::checkAuth(AsyncWebServerRequest* req) {
    // Never challenge in captive portal mode — the OS captive portal detector
    // cannot pass credentials and will show ERR_HTTP_RESPONSE_CODE_FAILURE on
    // any 401 response. The AP itself is the security boundary in that mode.
    if (_state == AppWiFiState::STATE_PORTAL_ACTIVE || _state == AppWiFiState::STATE_STARTING_CAPTIVE_PORTAL ||
        _state == AppWiFiState::STATE_PORTAL_COMPLETE) {
        return true;
    }
    if (_webPortalPassword.length() == 0) return true;
    if (req->authenticate("admin", _webPortalPassword.c_str())) return true;
    req->requestAuthentication();
    return false;
}

// ============================================================
//  Route: GET / — serve main config page
// ============================================================

void AppConfigManager::onGetRoot(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    // HTML is in PROGMEM (AppConfigManager_HTML.h)
    req->send_P(200, "text/html", HTML_PAGE);
}

// ============================================================
//  Route: GET /scan — start or poll async scan
// ============================================================

void AppConfigManager::onGetScan(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;

    if (!_scanRunning) {
        startScan();
        // Return immediately with scanning:true
        req->send(200, "application/json", F("{\"scanning\":true,\"networks\":[]}"));
        return;
    }

    if (!isScanComplete()) {
        req->send(200, "application/json", F("{\"scanning\":true,\"networks\":[]}"));
        return;
    }

    // Scan done — serialise + free SDK memory
    String json = getScanResultsJson();
    req->send(200, "application/json", json);
}

// ============================================================
//  Route: GET /status
// ============================================================

void AppConfigManager::onGetStatus(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;

    JsonDocument doc;
    doc["state"] = getStateString(_state);

    if (WiFi.status() == WL_CONNECTED) {
        doc["ip"]   = WiFi.localIP().toString();
        doc["rssi"] = WiFi.RSSI();
        doc["ssid"] = WiFi.SSID();
    }

    JsonObject cfg            = doc["config"].to<JsonObject>();
    cfg["app_name"]           = _config.appName;
    cfg["hostname"]           = _config.hostname;
    cfg["mqtt_broker"]        = _config.mqttBroker;
    cfg["mqtt_port"]          = _config.mqttPort;
    cfg["mqtt_user"]          = _config.mqttUser;
    cfg["mqtt_password"]      = (strlen(_config.mqttPassword) > 0) ? "****" : "";
    cfg["mqtt_client_id"]     = _config.mqttClientId;
    cfg["mqtt_base_topic"]    = _config.mqttBaseTopic;
    cfg["mqtt_ha_topic"]      = _config.mqttHADiscoveryTopic;
    cfg["ntp_server"]         = _config.ntpServer;
    cfg["posix_timezone"]     = _config.posixTimezone;
    cfg["ota_url"]            = _config.otaUrl;
    cfg["use_static_ip"]      = _config.useStaticIP;
    cfg["primary_ssid"]       = _config.primary.ssid;
    cfg["primary_password"]   = (strlen(_config.primary.password) > 0) ? "****" : "";
    cfg["secondary_ssid"]     = _config.secondary.ssid;
    cfg["secondary_password"] = (strlen(_config.secondary.password) > 0) ? "****" : "";
    if (_config.useStaticIP) {
        cfg["static_ip"] = _config.staticIP.toString();
        cfg["gateway"]   = _config.gateway.toString();
        cfg["subnet"]    = _config.subnet.toString();
        cfg["dns1"]      = _config.dns1.toString();
        cfg["dns2"]      = _config.dns2.toString();
    }

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ============================================================
//  Route: POST /save
// ============================================================

void AppConfigManager::onPostSave(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;

    // Debounce — web portal only (captive portal server tears down after one save)
    bool isWebPortal = (_state == AppWiFiState::STATE_WEB_PORTAL_ACTIVE);
    if (isWebPortal) {
        if (_saveInProgress) {
            req->send(429,
                      "application/json",
                      F("{\"success\":false,\"errors\":{\"general\":\"Save already in progress\"}}"));
            return;
        }
        unsigned long now = millis();
        if (_lastSaveMs > 0 && (now - _lastSaveMs) < 2000UL) {
            req->send(429,
                      "application/json",
                      F("{\"success\":false,\"errors\":{\"general\":\"Please wait before saving again\"}}"));
            return;
        }
    }

    _saveInProgress = true;

    // ---- Parse body as JSON ----
    JsonDocument body;
    if (_pendingBody.length() > 0) {
        DeserializationError err = deserializeJson(body, _pendingBody);
        _pendingBody             = "";   // Free memory immediately after parsing
        if (err) {
            _saveInProgress = false;
            req->send(400, "application/json", F("{\"success\":false,\"errors\":{\"general\":\"Invalid JSON\"}}"));
            return;
        }
    } else {
        _saveInProgress = false;
        req->send(400, "application/json", F("{\"success\":false,\"errors\":{\"general\":\"No body received\"}}"));
        return;
    }

    // ---- Build candidate config (validate before touching _config) ----
    AppConfig    candidate = _config;
    JsonDocument errors;
    bool         hasErrors = false;

    // Helper lambda — copy string field safely
    auto copyField = [](const char* src, char* dst, size_t maxLen) {
        if (src) strncpy(dst, src, maxLen - 1);
        dst[maxLen - 1] = '\0';
    };

    // App tab
    if (body["app_name"].is<const char*>()) copyField(body["app_name"], candidate.appName, sizeof(candidate.appName));

    if (body["hostname"].is<const char*>()) {
        copyField(body["hostname"], candidate.hostname, sizeof(candidate.hostname));
        String err;
        if (!validateHostname(candidate.hostname, err)) {
            errors["hostname"] = err;
            hasErrors          = true;
        }
    }

    // WiFi tab
    bool wifiChanged = false;
    if (body["primary_ssid"].is<const char*>()) {
        String oldSsid = _config.primary.ssid;
        copyField(body["primary_ssid"], candidate.primary.ssid, sizeof(candidate.primary.ssid));
        if (oldSsid != candidate.primary.ssid) wifiChanged = true;
    }
    if (body["primary_password"].is<const char*>()) {
        const char* pw = body["primary_password"];
        if (strcmp(pw, "****") != 0) {  // Ignore masked placeholder
            String oldPw = _config.primary.password;
            copyField(pw, candidate.primary.password, sizeof(candidate.primary.password));
            if (oldPw != candidate.primary.password) wifiChanged = true;
        }
    }
    if (body["secondary_ssid"].is<const char*>()) {
        String oldSsid = _config.secondary.ssid;
        copyField(body["secondary_ssid"], candidate.secondary.ssid, sizeof(candidate.secondary.ssid));
        if (oldSsid != candidate.secondary.ssid) wifiChanged = true;
    }
    if (body["secondary_password"].is<const char*>()) {
        const char* pw = body["secondary_password"];
        if (strcmp(pw, "****") != 0) {
            String oldPw = _config.secondary.password;
            copyField(pw, candidate.secondary.password, sizeof(candidate.secondary.password));
            if (oldPw != candidate.secondary.password) wifiChanged = true;
        }
    }

    // Static IP
    if (body["use_static_ip"].is<bool>()) {
        candidate.useStaticIP = body["use_static_ip"].as<bool>();
        if (candidate.useStaticIP) {
            candidate.staticIP.fromString(body["static_ip"] | "");
            candidate.gateway.fromString(body["gateway"] | "");
            candidate.subnet.fromString(body["subnet"] | "");
            candidate.dns1.fromString(body["dns1"] | "");
            candidate.dns2.fromString(body["dns2"] | "");
            String ipErr;
            if (!validateStaticIP(candidate, ipErr)) {
                errors["static_ip"] = ipErr;
                hasErrors           = true;
            }
        }
        wifiChanged = true;
    }

    // MQTT tab
    if (body["mqtt_broker"].is<const char*>()) {
        copyField(body["mqtt_broker"], candidate.mqttBroker, sizeof(candidate.mqttBroker));
        String err;
        if (!validateMqttBroker(candidate.mqttBroker, err)) {
            errors["mqtt_broker"] = err;
            hasErrors             = true;
        }
    }
    if (body["mqtt_port"].is<uint16_t>()) candidate.mqttPort = body["mqtt_port"].as<uint16_t>();
    if (body["mqtt_user"].is<const char*>())
        copyField(body["mqtt_user"], candidate.mqttUser, sizeof(candidate.mqttUser));
    if (body["mqtt_password"].is<const char*>()) {
        const char* pw = body["mqtt_password"];
        if (strcmp(pw, "****") != 0) copyField(pw, candidate.mqttPassword, sizeof(candidate.mqttPassword));
    }
    if (body["mqtt_client_id"].is<const char*>())
        copyField(body["mqtt_client_id"], candidate.mqttClientId, sizeof(candidate.mqttClientId));
    if (body["mqtt_base_topic"].is<const char*>())
        copyField(body["mqtt_base_topic"], candidate.mqttBaseTopic, sizeof(candidate.mqttBaseTopic));
    if (body["mqtt_ha_topic"].is<const char*>())
        copyField(body["mqtt_ha_topic"], candidate.mqttHADiscoveryTopic, sizeof(candidate.mqttHADiscoveryTopic));

    // System tab
    if (body["ntp_server"].is<const char*>()) {
        copyField(body["ntp_server"], candidate.ntpServer, sizeof(candidate.ntpServer));
        String err;
        if (!validateNtpServer(candidate.ntpServer, err)) {
            errors["ntp_server"] = err;
            hasErrors            = true;
        }
    }
    if (body["posix_timezone"].is<const char*>())
        copyField(body["posix_timezone"], candidate.posixTimezone, sizeof(candidate.posixTimezone));
    if (body["ota_url"].is<const char*>()) copyField(body["ota_url"], candidate.otaUrl, sizeof(candidate.otaUrl));

    // Web portal password
    if (body["web_portal_password"].is<const char*>()) {
        const char* pw = body["web_portal_password"];
        if (strcmp(pw, "****") != 0) {
            copyField(pw, candidate.webPortalPassword, sizeof(candidate.webPortalPassword));
            _webPortalPassword = candidate.webPortalPassword;
        }
    }

    // ---- Return errors if any ----
    if (hasErrors) {
        _saveInProgress = false;
        JsonDocument resp;
        resp["success"] = false;
        resp["errors"]  = errors;
        String out;
        serializeJson(resp, out);
        req->send(400, "application/json", out);
        return;
    }

    // ---- Detect hostname change ----
    bool hostnameChanged = (strcmp(_config.hostname, candidate.hostname) != 0);

    // ---- Commit ----
    _config = candidate;
    if (_saveCallback) _saveCallback(_config);
    _lastSaveMs = millis();

    if (hostnameChanged) _pendingMdnsRestart = true;

    // ---- Send success response BEFORE setting state flags ----
    JsonDocument resp;
    resp["success"]      = true;
    resp["wifi_restart"] = wifiChanged;
    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);

    // ---- Set flags for state machine to act on next tick ----
    _wifiChangedOnSave  = wifiChanged;
    _portalSaveComplete = true;
    // _saveInProgress stays true until state machine clears it
}

// ============================================================
//  Route: GET /exit — close web portal gracefully
// ============================================================

void AppConfigManager::onGetExit(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    req->send(200, "application/json", F("{\"success\":true}"));
    // State machine acts next tick
    transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
}
