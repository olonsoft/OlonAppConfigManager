// ============================================================
//  AppConfigManager.cpp
//  WiFi + App configuration manager for ESP8266 / ESP32
// ============================================================

#include "AppConfigManager.h"

#include <ArduinoJson.h>  // v7

#include <algorithm>    // std::sort  — needed by getScanResultsJson()
#include <vector>       // std::vector — needed by getScanResultsJson()

#include "EventBus.h"

// ============================================================
//  PROGMEM HTML
// ============================================================
#include "AppConfigManager_HTML.h"

#ifdef OLON_DEBUG
    #define LOG_IMPL(color, level, tag, format, ...)                                           \
        do {                                                                                   \
            Serial.printf_P(PSTR("\033[" color "m%10lu [" level "] [%s] " format "\033[0m\n"), \
                            millis(),                                                          \
                            tag,                                                               \
                            ##__VA_ARGS__);                                                    \
        } while (0)

    #define LOGD(tag, format, ...) LOG_IMPL("0;36", "D", tag, format, ##__VA_ARGS__)
    #define LOGI(tag, format, ...) LOG_IMPL("0;32", "I", tag, format, ##__VA_ARGS__)
    #define LOGW(tag, format, ...) LOG_IMPL("0;33", "W", tag, format, ##__VA_ARGS__)
    #define LOGE(tag, format, ...) LOG_IMPL("0;31", "E", tag, format, ##__VA_ARGS__)
    #define LOGRAW(format, ...)                           \
        do {                                              \
            Serial.printf_P(PSTR(format), ##__VA_ARGS__); \
        } while (0)
#else
    #define LOGD(tag, format, ...) ((void)0)
    #define LOGI(tag, format, ...) ((void)0)
    #define LOGW(tag, format, ...) ((void)0)
    #define LOGE(tag, format, ...) ((void)0)
    #define LOGRAW(...)            ((void)0)
#endif

// ============================================================
//  Constants
// ============================================================
const char* const TAG = "WiFi";

static constexpr uint8_t  DNS_PORT            = 53;
static constexpr uint16_t RETRY_WAIT_MS       = 5000;   // delay between retries
static constexpr uint16_t AP_READY_WAIT_MS    = 500;    // wait after WiFi.softAP()
static constexpr size_t   MAX_POST_BODY_BYTES = 4096;   // guard against oversized POST bodies

// Captive portal probe paths (no auth, redirect to root)
static const char* const CAPTIVE_PATHS[] = {
    "/hotspot-detect.html", "/generate_204", "/ncsi.txt", "/redirect", "/fwlink", "/canonical.html", nullptr,
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
    // Debug heap usage
    static uint32_t minHeap = UINT32_MAX;
    uint32_t        mem     = ESP.getFreeHeap();
    if (mem < minHeap) {
        minHeap = mem;
        Serial.printf("Min Heap changed: %u bytes\n", minHeap);
    }

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

    // Handle /exit request set by async callback — consume here on main task.
    // FIX: was calling transitionTo() directly from the async callback (data race on ESP32).
    if (_pendingExitRequest) {
        _pendingExitRequest = false;
        if (_state == AppWiFiState::STATE_WEB_PORTAL_ACTIVE) {
            transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
            return;
        }
    }

    // Process a pending POST /save on the main task.
    // WDT FIX: on ESP8266 the async callback runs on the network interrupt
    // stack which is very shallow (~512 bytes).  Allocating a JsonDocument
    // and building a full AppConfig candidate there overflows the stack,
    // corrupts memory, and produces a rst cause:4 WDT reset.
    // The callback now just stores the request pointer and sets this flag;
    // all JSON work happens here on the full main stack.
    if (_pendingSaveReady) {
        _pendingSaveReady = false;
        processPendingSave();
        return;
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
        case AppWiFiState::STATE_WAITING_AP_DOWN:         handleState_WaitingApDown(); break;
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

    if (_state == AppWiFiState::STATE_PORTAL_ACTIVE || _state == AppWiFiState::STATE_STARTING_CAPTIVE_PORTAL ||
        _state == AppWiFiState::STATE_PORTAL_COMPLETE) {
        transitionTo(AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL);
    } else if (_state == AppWiFiState::STATE_WEB_PORTAL_ACTIVE || _state == AppWiFiState::STATE_STARTING_WEB_PORTAL) {
        transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
    } else {
        WiFi.disconnect(true);
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
        _eventBus.publish(EventType::APP_WIFI_DISCONNECTED);
    }
}

void AppConfigManager::resetConfig(bool fullWipe) {
    if (fullWipe) {
        _config = AppConfig{};
    } else {
        _config.primary   = WiFiProfile{};
        _config.secondary = WiFiProfile{};
    }
    if (_saveCallback) _saveCallback(_config);

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
        case AppWiFiState::STATE_WAITING_AP_DOWN:         return F("WAITING_AP_DOWN");
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
    StateChangePayload payload{ _prevState, nextState };
    _eventBus.publish(EventType::APP_WIFI_STATE_CHANGE, &payload);
    _state = nextState;
}

// ============================================================
//  STATE_START
// ============================================================

void AppConfigManager::handleState_Start() {
    _activeProfileIndex  = 0;
    _retryCount          = 0;
    _primaryAuthFailed   = false;
    _secondaryAuthFailed = false;
    _rssiLowPublished    = false;
    _portalSaveComplete  = false;
    _saveInProgress      = false;
    _wifiChangedOnSave   = false;
    _pendingExitRequest  = false;
    _pendingSaveReady    = false;   // WDT FIX: reset deferred-save state on boot
    _scanRunning         = false;

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
// ============================================================

void AppConfigManager::handleState_Disconnected() {
    if (_intentionalDisconnect) {
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
        return;
    }

    bool hasPrimary   = (_config.primary.ssid[0] != '\0');
    bool hasSecondary = (_config.secondary.ssid[0] != '\0');

    if (hasPrimary || hasSecondary) {
        _activeProfileIndex  = 0;
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
// ============================================================

void AppConfigManager::handleState_ConnectingStart() {
    WiFiProfile& profile = activeProfile();

    if (profile.ssid[0] == '\0') {
        transitionTo(AppWiFiState::STATE_AP_SWITCH_START);
        return;
    }

    WiFi.mode(WIFI_STA);

    // Clear stale association before WiFi.begin() — required on ESP8266
    // after a timeout or auth failure to prevent silent failure on retry.
    WiFi.disconnect(false);

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
// ============================================================

void AppConfigManager::handleState_Connecting() {
    transitionTo(AppWiFiState::STATE_CONNECTING_WAIT);
}

// ============================================================
//  STATE_CONNECTING_WAIT
// ============================================================

void AppConfigManager::handleState_ConnectingWait() {
    wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
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
#endif
        transitionTo(AppWiFiState::STATE_AUTH_FAILED);
        return;
    }

    if (millis() - _connectStartMs >= _connectTimeoutMs) {
        transitionTo(AppWiFiState::STATE_RETRYING_START);
    }
}

// ============================================================
//  STATE_CONNECTED
// ============================================================

void AppConfigManager::handleState_Connected() {
    if (_pendingMdnsRestart) {
        _pendingMdnsRestart = false;
        stopMdns();
        transitionTo(AppWiFiState::STATE_MDNS_RESTART);
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        transitionTo(AppWiFiState::STATE_CONNECTION_LOST);
        return;
    }

    if (_rssiMonitoringEnabled) {
        checkRssi();
    }
}

// ============================================================
//  STATE_MDNS_RESTART
// ============================================================

void AppConfigManager::handleState_MdnsRestart() {
    startMdns();
    transitionTo(AppWiFiState::STATE_CONNECTED);
}

// ============================================================
//  STATE_CONNECTION_LOST
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
// ============================================================

void AppConfigManager::handleState_AuthFailed() {
    WiFi.disconnect(true);
    _eventBus.publish(EventType::APP_WIFI_AUTH_FAILED);

    if (_activeProfileIndex == 0) {
        _primaryAuthFailed = true;
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
// ============================================================

void AppConfigManager::handleState_RetryingStart() {
    if (_retryCount < _maxRetries) {
        transitionTo(AppWiFiState::STATE_RETRYING);
    } else {
        transitionTo(AppWiFiState::STATE_AP_SWITCH_START);
    }
}

// ============================================================
//  STATE_RETRYING
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
// ============================================================

void AppConfigManager::handleState_RetryingWait() {
    if (millis() - _retryWaitStartMs >= RETRY_WAIT_MS) {
        transitionTo(AppWiFiState::STATE_CONNECTING_START);
    }
}

// ============================================================
//  STATE_AP_SWITCH_START
// ============================================================

void AppConfigManager::handleState_ApSwitchStart() {
    if (_activeProfileIndex == 0 && _config.secondary.ssid[0] != '\0' && !_secondaryAuthFailed) {
        _activeProfileIndex = 1;
        _retryCount         = 0;
        transitionTo(AppWiFiState::STATE_CONNECTING_START);
    } else {
        transitionTo(AppWiFiState::STATE_START_CAPTIVE_PORTAL);
    }
}

// ============================================================
//  STATE_START_CAPTIVE_PORTAL
// ============================================================

void AppConfigManager::handleState_StartCaptivePortal() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);

    if (_portalPassword.length() > 0) {
        WiFi.softAP(_portalSsid.c_str(), _portalPassword.c_str(), 1);
    } else {
        WiFi.softAP(_portalSsid.c_str(), nullptr, 1);
    }

    _portalStartMs      = millis();
    _portalSaveComplete = false;
    _saveInProgress     = false;

    _redirectUrl = "http://" + WiFi.softAPIP().toString() + "/";

    _dnsServer = new DNSServer();
    _dnsServer->setTTL(300);
    _dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    _dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());

    startWebServer();
    transitionTo(AppWiFiState::STATE_STARTING_CAPTIVE_PORTAL);
}

// ============================================================
//  STATE_STARTING_CAPTIVE_PORTAL
// ============================================================

void AppConfigManager::handleState_StartingCaptivePortal() {
    if (millis() - _portalStartMs >= AP_READY_WAIT_MS) {
        _eventBus.publish(EventType::APP_WIFI_PORTAL_STARTED);
        transitionTo(AppWiFiState::STATE_PORTAL_ACTIVE);
    }
}

// ============================================================
//  STATE_PORTAL_ACTIVE
// ============================================================

void AppConfigManager::handleState_PortalActive() {
    if (_portalSaveComplete) {
        transitionTo(AppWiFiState::STATE_PORTAL_COMPLETE);
        return;
    }
    if (millis() - _portalStartMs >= _portalTimeoutMs) {
        _eventBus.publish(EventType::APP_WIFI_PORTAL_TIMEOUT);
        transitionTo(AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL);
    }
}

// ============================================================
//  STATE_PORTAL_COMPLETE
// ============================================================

void AppConfigManager::handleState_PortalComplete() {
    // Wait a short time to let the HTTP response flush before tearing down
    if (_portalCompleteMs == 0) {
        _portalCompleteMs = millis();   // arm the timer on first entry
        return;
    }
    if (millis() - _portalCompleteMs < 200) {
        return;                          // still waiting
    }
    _portalCompleteMs = 0;               // reset for next use
    transitionTo(AppWiFiState::STATE_STOPPING_CAPTIVE_PORTAL);
}

// ============================================================
//  STATE_STOPPING_CAPTIVE_PORTAL
// ============================================================

void AppConfigManager::handleState_StoppingCaptivePortal() {
    // Do NOT call WiFi.mode(WIFI_STA) here — on ESP32 the lwIP AP netif is
    // destroyed asynchronously and the memory has not been released yet.
    stopWebServer();

    if (_dnsServer) {
        _dnsServer->stop();
        delete _dnsServer;
        _dnsServer = nullptr;
    }

    WiFi.softAPdisconnect(true);
    _apDownStartMs = millis();
    transitionTo(AppWiFiState::STATE_WAITING_AP_DOWN);
}

// ============================================================
//  STATE_WAITING_AP_DOWN
// ============================================================

void AppConfigManager::handleState_WaitingApDown() {
    bool apGone   = !(WiFi.getMode() & WIFI_AP);
    bool timedOut = (millis() - _apDownStartMs) >= 500UL;

    if (!apGone && !timedOut) return;

    WiFi.mode(WIFI_STA);
    _eventBus.publish(EventType::APP_WIFI_PORTAL_STOPPED);

    if (_intentionalDisconnect) {
        // softAPdisconnect(true) already tore down the radio; calling
        // WiFi.disconnect() again can assert on some ESP32 SDK versions.
        _eventBus.publish(EventType::APP_WIFI_DISCONNECTED);
        transitionTo(AppWiFiState::STATE_NETWORK_DISABLED);
        return;
    }

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
// ============================================================

void AppConfigManager::handleState_StartWebPortal() {
    startWebServer();
    transitionTo(AppWiFiState::STATE_STARTING_WEB_PORTAL);
}

// ============================================================
//  STATE_STARTING_WEB_PORTAL
// ============================================================

void AppConfigManager::handleState_StartingWebPortal() {
    _eventBus.publish(EventType::APP_WIFI_WEB_PORTAL_STARTED);
    _lastSaveMs     = 0;
    _saveInProgress = false;
    transitionTo(AppWiFiState::STATE_WEB_PORTAL_ACTIVE);
}

// ============================================================
//  STATE_WEB_PORTAL_ACTIVE
// ============================================================

void AppConfigManager::handleState_WebPortalActive() {
    if (WiFi.status() != WL_CONNECTED) {
        transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
        return;
    }

    if (_portalSaveComplete && _wifiChangedOnSave) {
        transitionTo(AppWiFiState::STATE_STOPPING_WEB_PORTAL);
        return;
    }

    if (_portalSaveComplete && !_wifiChangedOnSave) {
        _portalSaveComplete = false;
        _saveInProgress     = false;
        _eventBus.publish(EventType::APP_WIFI_CONFIG_SAVED);

        if (_pendingMdnsRestart) {
            _pendingMdnsRestart = false;
            stopMdns();
            transitionTo(AppWiFiState::STATE_MDNS_RESTART);
        }
    }
}

// ============================================================
//  STATE_STOPPING_WEB_PORTAL
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

    _portalSaveComplete = false;
    _saveInProgress     = false;
    transitionTo(AppWiFiState::STATE_RECONNECTING);
}

// ============================================================
//  STATE_RECONNECTING
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
// ============================================================

void AppConfigManager::handleState_NetworkDisabled() {
    // Idle. Main program must call begin() to re-enable.
}

// ============================================================
//  STATE_ERROR
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
    std::swap(_config.primary, _config.secondary);
    _activeProfileIndex = 0;
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
        _rssiLowPublished = false;
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
    return WiFi.scanComplete() >= 0;
}

String AppConfigManager::getScanResultsJson() {
    int n = WiFi.scanComplete();
    if (n < 0) return F("{\"scanning\":true,\"networks\":[]}");

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
    LOGD(TAG, "validateHostname() %s", hostname);
    size_t len = strlen(hostname);
    if (len == 0 || len > 63) {
        err = F("Hostname must be 1-63 characters");
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
    LOGD(TAG, "validateMqttBroker() %s", broker);

    if (strlen(broker) == 0) return true;

    if (strncmp(broker, "mqtt://", 7) == 0 || strncmp(broker, "mqtts://", 8) == 0) {
        err = F("Do not include protocol prefix (mqtt://)");
        return false;
    }

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
        long port = portStr.toInt();
        if (port < 1 || port > 65535) {
            err = F("Port must be between 1 and 65535");
            return false;
        }
    }
    return true;
}

bool AppConfigManager::validateNtpServer(const char* server, String& err) const {
    LOGD(TAG, "validateNtpServer() %s", server);
    if (strlen(server) == 0) {
        err = F("NTP server cannot be empty");
        return false;
    }
    String s(server);
    if (s.indexOf(' ') >= 0) {
        err = F("NTP server contains invalid characters");
        return false;
    }
    return true;
}

bool AppConfigManager::validateStaticIP(const AppConfig& cfg, String& err) const {
    LOGD(TAG, "validateStaticIP()");
    if (!cfg.useStaticIP) return true;

    if (cfg.gateway == IPAddress(0, 0, 0, 0)) {
        err = F("Gateway address is required for static IP");
        return false;
    }
    if (cfg.subnet == IPAddress(0, 0, 0, 0)) {
        err = F("Subnet mask is required for static IP");
        return false;
    }
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
    LOGD(TAG, "validatePort() %s", port);
    // uint16_t is unsigned — check for zero, not < 1
    if (port == 0) {
        err = F("Port must be between 1 and 65535");
        return false;
    }
    return true;
}

// ============================================================
//  Web server — lifecycle
// ============================================================

void AppConfigManager::startWebServer() {
    if (_webServer) return;
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

void AppConfigManager::redirectToPortal(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(302);
    response->addHeader(F("Location"), _redirectUrl);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    request->send(response);
}

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
            req->redirect(_redirectUrl);
        });
    }
    _webServer->on("/connecttest.txt", HTTP_GET, [this](AsyncWebServerRequest* request) {
        request->redirect("http://logout.net");
    });
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
    _webServer->on("/", HTTP_GET, [this](AsyncWebServerRequest* req) {
        onGetRoot(req);
    });

    setupCaptivePortalHandlers();

    _webServer->on("/favicon.ico", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(204, "text/plain", "");
    });
    _webServer->on("/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
        onGetScan(req);
    });
    _webServer->on("/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        onGetStatus(req);
    });

    // Body accumulation — runs on async task; only concat, nothing heavy.
    // MAX_POST_BODY_BYTES guards against heap exhaustion from oversized bodies.
    auto bodyHandler = [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        if (index == 0) _pendingBody = "";
        if (_pendingBody.length() + len <= MAX_POST_BODY_BYTES) {
            _pendingBody.concat(reinterpret_cast<const char*>(data), len);
        }
    };

    _webServer->on("/save", HTTP_POST, [this](AsyncWebServerRequest* req) {
        onPostSave(req);
    }, nullptr, bodyHandler);

    _webServer->on("/exit", HTTP_GET, [this](AsyncWebServerRequest* req) {
        onGetExit(req);
    });

    _webServer->onNotFound([this](AsyncWebServerRequest* req) {
        req->redirect(_redirectUrl);
    });
}

// ============================================================
//  Auth helper
// ============================================================

bool AppConfigManager::checkAuth(AsyncWebServerRequest* req) {
    // Never challenge in captive portal mode — OS detectors cannot pass
    // credentials and will show an error on any 401 response.
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
//  Route: GET /
// ============================================================

void AppConfigManager::onGetRoot(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    AsyncWebServerResponse* response =
        new AsyncProgmemResponse(200, "text/html", (const uint8_t*)htmlContent, sizeof(htmlContent) - 1);
    req->send(response);
}

// ============================================================
//  Route: GET /scan
// ============================================================

void AppConfigManager::onGetScan(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;

    if (!_scanRunning) {
        startScan();
        req->send(200, "application/json", F("{\"scanning\":true,\"networks\":[]}"));
        return;
    }
    if (!isScanComplete()) {
        req->send(200, "application/json", F("{\"scanning\":true,\"networks\":[]}"));
        return;
    }
    req->send(200, "application/json", getScanResultsJson());
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

    JsonObject cfg             = doc["config"].to<JsonObject>();
    cfg["app_name"]            = _config.appName;
    cfg["hostname"]            = _config.hostname;
    cfg["mqtt_broker"]         = _config.mqttBroker;
    cfg["mqtt_port"]           = _config.mqttPort;
    cfg["mqtt_user"]           = _config.mqttUser;
    cfg["mqtt_password"]       = (strlen(_config.mqttPassword) > 0) ? "****" : "";
    cfg["mqtt_client_id"]      = _config.mqttClientId;
    cfg["mqtt_base_topic"]     = _config.mqttBaseTopic;
    cfg["mqtt_ha_topic"]       = _config.mqttHADiscoveryTopic;
    cfg["ntp_server"]          = _config.ntpServer;
    cfg["posix_timezone"]      = _config.posixTimezone;
    cfg["ota_url"]             = _config.otaUrl;
    cfg["use_static_ip"]       = _config.useStaticIP;
    cfg["primary_ssid"]        = _config.primary.ssid;
    cfg["primary_password"]    = (strlen(_config.primary.password) > 0) ? "****" : "";
    cfg["secondary_ssid"]      = _config.secondary.ssid;
    cfg["secondary_password"]  = (strlen(_config.secondary.password) > 0) ? "****" : "";
    cfg["web_portal_password"] = (strlen(_config.webPortalPassword) > 0) ? "****" : "";
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
//  Route: POST /save  — THIN CALLBACK
//
//  Runs on the AsyncWebServer task.  On ESP8266 this is the same
//  thread as loop() but on a very shallow interrupt/async stack
//  (~512 bytes).  Allocating a JsonDocument and AppConfig candidate
//  there overflows the stack, corrupts memory, and causes a
//  rst cause:4 WDT reset.
//
//  This callback therefore does nothing heavy: it guards for
//  duplicate saves, stores the request pointer, and raises
//  _pendingSaveReady.  All JSON work is deferred to
//  processPendingSave() which loop() calls on the next tick,
//  safely on the full main stack.
// ============================================================

void AppConfigManager::onPostSave(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
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

    if (_pendingBody.length() == 0) {
        req->send(400, "application/json", F("{\"success\":false,\"errors\":{\"general\":\"No body received\"}}"));
        return;
    }

    _saveInProgress     = true;
    _pendingSaveRequest = req->pause();
    _pendingSaveReady   = true;
    // loop() calls processPendingSave() on the very next tick
}

// ============================================================
//  processPendingSave()
//  Called from loop() on the main task.  All JSON parsing,
//  validation, AppConfig mutation, and response sending live here.
// ============================================================

void AppConfigManager::processPendingSave() {
    auto req = _pendingSaveRequest.lock();
    _pendingSaveRequest.reset();

    if (!req) {
        _saveInProgress = false;
        return;
    }

    // ---- Parse body onto the HEAP ----
    auto body = std::make_unique<JsonDocument>();
    if (!body) {
        _saveInProgress = false;
        req->send(500, "application/json", F("{\"success\":false,\"errors\":{\"general\":\"Out of memory\"}}"));
        return;
    }

    if (_pendingBody.length() > 0) {
        DeserializationError err = deserializeJson(*body, _pendingBody);
        _pendingBody             = "";
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

    // ---- Candidate config on the heap (already done) ----
    auto candidate = std::make_unique<AppConfig>(_config);
    if (!candidate) {
        _saveInProgress = false;
        req->send(500, "application/json", F("{\"success\":false,\"errors\":{\"general\":\"Out of memory\"}}"));
        return;
    }

    // ---- Errors document on the heap ----
    auto errors = std::make_unique<JsonDocument>();
    if (!errors) {
        _saveInProgress = false;
        req->send(500, "application/json", F("{\"success\":false,\"errors\":{\"general\":\"Out of memory\"}}"));
        return;
    }

    bool hasErrors = false;

    // Reuse a single String for all validation error messages
    String errMsg;

    auto copyField = [](const char* src, char* dst, size_t maxLen) {
        if (src) strncpy(dst, src, maxLen - 1);
        dst[maxLen - 1] = '\0';
    };

    // App tab
    LOGD(TAG, "validating App tab");
    if ((*body)["app_name"].is<const char*>())
        copyField((*body)["app_name"], candidate->appName, sizeof(candidate->appName));

    if ((*body)["hostname"].is<const char*>()) {
        copyField((*body)["hostname"], candidate->hostname, sizeof(candidate->hostname));
        errMsg = "";
        if (!validateHostname(candidate->hostname, errMsg)) {
            (*errors)["hostname"] = errMsg;
            hasErrors             = true;
        }
    }

    // WiFi tab
    LOGD(TAG, "validating WiFi tab");
    bool wifiChanged = false;

    if ((*body)["primary_ssid"].is<const char*>()) {
        // Compare before copy to avoid a second String allocation
        if (strcmp(_config.primary.ssid, (*body)["primary_ssid"]) != 0) wifiChanged = true;
        copyField((*body)["primary_ssid"], candidate->primary.ssid, sizeof(candidate->primary.ssid));
    }
    if ((*body)["primary_password"].is<const char*>()) {
        const char* pw = (*body)["primary_password"];
        if (strcmp(pw, "****") != 0) {
            if (strcmp(_config.primary.password, pw) != 0) wifiChanged = true;
            copyField(pw, candidate->primary.password, sizeof(candidate->primary.password));
        }
    }
    if ((*body)["secondary_ssid"].is<const char*>()) {
        if (strcmp(_config.secondary.ssid, (*body)["secondary_ssid"]) != 0) wifiChanged = true;
        copyField((*body)["secondary_ssid"], candidate->secondary.ssid, sizeof(candidate->secondary.ssid));
    }
    if ((*body)["secondary_password"].is<const char*>()) {
        const char* pw = (*body)["secondary_password"];
        if (strcmp(pw, "****") != 0) {
            if (strcmp(_config.secondary.password, pw) != 0) wifiChanged = true;
            copyField(pw, candidate->secondary.password, sizeof(candidate->secondary.password));
        }
    }

    // Static IP
    LOGD(TAG, "validating static IP");
    if ((*body)["use_static_ip"].is<bool>()) {
        candidate->useStaticIP = (*body)["use_static_ip"].as<bool>();
        if (candidate->useStaticIP) {
            candidate->staticIP.fromString((*body)["static_ip"] | "");
            candidate->gateway.fromString((*body)["gateway"] | "");
            candidate->subnet.fromString((*body)["subnet"] | "");
            candidate->dns1.fromString((*body)["dns1"] | "");
            candidate->dns2.fromString((*body)["dns2"] | "");
            errMsg = "";
            if (!validateStaticIP(*candidate, errMsg)) {
                (*errors)["static_ip"] = errMsg;
                hasErrors              = true;
            }
        }
        wifiChanged = true;
    }

    // MQTT tab
    LOGD(TAG, "validating MQTT tab");
    if ((*body)["mqtt_broker"].is<const char*>()) {
        copyField((*body)["mqtt_broker"], candidate->mqttBroker, sizeof(candidate->mqttBroker));
        errMsg = "";
        if (!validateMqttBroker(candidate->mqttBroker, errMsg)) {
            (*errors)["mqtt_broker"] = errMsg;
            hasErrors                = true;
        }
    }
    if (!(*body)["mqtt_port"].isNull()) {
        long port = (*body)["mqtt_port"].as<long>();
        if (port >= 1 && port <= 65535) {
            candidate->mqttPort = (uint16_t)port;
        } else {
            (*errors)["mqtt_port"] = F("Port must be between 1 and 65535");
            hasErrors              = true;
        }
    }
    if ((*body)["mqtt_user"].is<const char*>())
        copyField((*body)["mqtt_user"], candidate->mqttUser, sizeof(candidate->mqttUser));
    if ((*body)["mqtt_password"].is<const char*>()) {
        const char* pw = (*body)["mqtt_password"];
        if (strcmp(pw, "****") != 0) copyField(pw, candidate->mqttPassword, sizeof(candidate->mqttPassword));
    }
    if ((*body)["mqtt_client_id"].is<const char*>())
        copyField((*body)["mqtt_client_id"], candidate->mqttClientId, sizeof(candidate->mqttClientId));
    if ((*body)["mqtt_base_topic"].is<const char*>())
        copyField((*body)["mqtt_base_topic"], candidate->mqttBaseTopic, sizeof(candidate->mqttBaseTopic));
    if ((*body)["mqtt_ha_topic"].is<const char*>())
        copyField((*body)["mqtt_ha_topic"], candidate->mqttHADiscoveryTopic, sizeof(candidate->mqttHADiscoveryTopic));

    // System tab
    if ((*body)["ntp_server"].is<const char*>()) {
        copyField((*body)["ntp_server"], candidate->ntpServer, sizeof(candidate->ntpServer));
        errMsg = "";
        if (!validateNtpServer(candidate->ntpServer, errMsg)) {
            (*errors)["ntp_server"] = errMsg;
            hasErrors               = true;
        }
    }
    if ((*body)["posix_timezone"].is<const char*>())
        copyField((*body)["posix_timezone"], candidate->posixTimezone, sizeof(candidate->posixTimezone));
    if ((*body)["ota_url"].is<const char*>())
        copyField((*body)["ota_url"], candidate->otaUrl, sizeof(candidate->otaUrl));

    // Web portal password
    if ((*body)["web_portal_password"].is<const char*>()) {
        const char* pw = (*body)["web_portal_password"];
        if (strcmp(pw, "****") != 0) {
            copyField(pw, candidate->webPortalPassword, sizeof(candidate->webPortalPassword));
            _webPortalPassword = candidate->webPortalPassword;
        }
    }

    // Free body early — no longer needed, recovers heap before building response
    body.reset();

    // ---- Return errors if any ----
    if (hasErrors) {
        _saveInProgress = false;
        String out;
        {
            // Scope the resp doc so it's freed before errors doc goes out of scope
            auto resp          = std::make_unique<JsonDocument>();
            (*resp)["success"] = false;
            (*resp)["errors"]  = *errors;
            serializeJson(*resp, out);
        }
        errors.reset();
        req->send(400, "application/json", out);
        return;
    }
    errors.reset();

    // ---- Detect hostname change ----
    bool hostnameChanged = (strcmp(_config.hostname, candidate->hostname) != 0);

    // ---- Commit ----
    _config = *candidate;
    candidate.reset();

    if (_saveCallback) _saveCallback(_config);
    _lastSaveMs = millis();
    if (hostnameChanged) _pendingMdnsRestart = true;

    // ---- Send success response ----
    LOGD(TAG, "sending success response");
    String out;
    {
        auto resp               = std::make_unique<JsonDocument>();
        (*resp)["success"]      = true;
        (*resp)["wifi_restart"] = wifiChanged;
        serializeJson(*resp, out);
    }
    req->send(200, "application/json", out);

    _wifiChangedOnSave  = wifiChanged;
    _portalSaveComplete = true;
}

// ============================================================
//  Route: GET /exit
//  Sets a flag instead of calling transitionTo() directly,
//  avoiding a data race on ESP32's dual-task model.
// ============================================================

void AppConfigManager::onGetExit(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    req->send(200, "application/json", F("{\"success\":true}"));
    _pendingExitRequest = true;
}
