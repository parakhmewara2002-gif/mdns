// ============================================================
//  wifi_manager.cpp  v2.0.6
//
//  WiFi Behavior:
//  1. Startup        -> AP ON  (192.168.4.1)
//  2. STA connects   -> wait 7s -> AP OFF
//  3. STA disconnect -> AP ON  (192.168.4.1)
//  4. AP IP always   -> 192.168.4.1 (never changes)
//  5. STA IP         -> assigned by router (dynamic)
// ============================================================
#include "wifi_manager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <NetBIOS.h>

WiFiManager wifiMgr;

WiFiManager::WiFiManager()
    : _scanPending(false),
      _lastReconnectAttempt(0),
      _reconnectInterval(5000),
      _staInitiated(false),
      _apActive(false),
      _wasConnected(false),
      _staConnected(false),
      _mdnsRunning(false),
      _apStopAt(0)
{}

// ── begin ─────────────────────────────────────────────────────
void WiFiManager::begin() {
    loadConfig();

    // Event: STA got IP from router
    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t i) {
        wifiMgr._onStaGotIP();
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    // Event: STA disconnected
    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t i) {
        wifiMgr._onStaDisconnected();
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    delay(100);

    // Always start with AP ON
    _startAP();

    if (_cfg.staEnabled && _cfg.staSSID.length() > 0) {
        _startSTA();
    }
}

// ── loop ──────────────────────────────────────────────────────
void WiFiManager::loop() {
    // Delayed AP shutdown after STA connected
    if (_apStopAt > 0 && millis() >= _apStopAt) {
        _apStopAt = 0;
        if (_staConnected) {  // use cache - avoids WiFi.status() in loop()
            _stopAP();
        }
    }
    _handleReconnect();
}

// ── Event: STA got IP ─────────────────────────────────────────
void WiFiManager::_onStaGotIP() {
    _wasConnected         = true;
    _staConnected         = true;   // cache - avoids WiFi.status() per tick
    _reconnectInterval    = 5000UL;
    _lastReconnectAttempt = millis();

    Serial.printf(DEBUG_TAG " [WiFi] STA connected!\n");
    Serial.printf(DEBUG_TAG "   SSID : %s\n", WiFi.SSID().c_str());
    Serial.printf(DEBUG_TAG "   IP   : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf(DEBUG_TAG "   RSSI : %d dBm\n", (int)WiFi.RSSI());
    Serial.printf(DEBUG_TAG " AP still ON at 192.168.4.1 (turning off in 7s)\n");

    // If WiFi never connects (bad creds in new firmware), this line is never
    // reached, boot counter stays elevated, and safe mode or rollback activates.

    // Start mDNS so the device is reachable at http://ir-remote.local
    startMdns();

    // Schedule AP shutdown after 7 seconds
    _apStopAt = millis() + 7000;
}

// ── Event: STA disconnected ───────────────────────────────────
void WiFiManager::_onStaDisconnected() {
    bool wasConn = _wasConnected;
    _wasConnected = false;
    _staConnected = false;  // cache - avoids WiFi.status() per tick
    _apStopAt     = 0;   // cancel pending AP shutdown

    if (wasConn) {
        Serial.println(DEBUG_TAG " [WiFi] STA disconnected - AP fallback ON");
        stopMdns();
    } else {
        Serial.println(DEBUG_TAG " [WiFi] STA connect failed - AP still ON");
    }

    // Ensure AP is running so user can reconnect
    if (!_apActive) {
        _startAP();
    }
}

// ── _startAP ──────────────────────────────────────────────────
void WiFiManager::_startAP() {
    IPAddress ip, gw, mask;
    ip.fromString(AP_IP);
    gw.fromString(AP_IP);
    mask.fromString("255.255.255.0");
    WiFi.softAPConfig(ip, gw, mask);

    const char* pass = (_cfg.apPass.length() >= 8)
                       ? _cfg.apPass.c_str() : nullptr;
    bool ok = WiFi.softAP(
        _cfg.apSSID.c_str(), pass,
        _cfg.apChannel,
        _cfg.apHidden ? 1 : 0,
        DEFAULT_AP_MAX_CONN
    );
    _apActive = ok;

    if (ok)
        Serial.printf(DEBUG_TAG " AP ON: SSID='%s' IP=%s\n",
                      _cfg.apSSID.c_str(),
                      WiFi.softAPIP().toString().c_str());
    else
        Serial.println(DEBUG_TAG " ERROR: AP start failed");
}

// ── _stopAP ───────────────────────────────────────────────────
void WiFiManager::_stopAP() {
    if (!_apActive) return;
    WiFi.softAPdisconnect(true);
    _apActive = false;
    Serial.println(DEBUG_TAG " AP OFF - STA connected to router");
    Serial.printf(DEBUG_TAG " Web GUI: http://%s\n",
                  WiFi.localIP().toString().c_str());
}

// ── _startSTA ─────────────────────────────────────────────────
void WiFiManager::_startSTA() {
    Serial.printf(DEBUG_TAG " STA: connecting to '%s'...\n",
                  _cfg.staSSID.c_str());
    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(_cfg.staSSID.c_str(), _cfg.staPass.c_str());
    _staInitiated         = true;
    _lastReconnectAttempt = millis();
    _reconnectInterval    = STA_CONNECT_TIMEOUT;
}

// ── applyStaConfig ────────────────────────────────────────────
void WiFiManager::applyStaConfig() {
    if (_cfg.staEnabled && _cfg.staSSID.length() > 0) {
        _startSTA();
    } else {
        WiFi.disconnect(false);
        _staInitiated = false;
        _wasConnected = false;
        _apStopAt     = 0;
        if (!_apActive) _startAP();
        Serial.println(DEBUG_TAG " STA disabled - AP ON");
    }
}

// ── _handleReconnect ──────────────────────────────────────────
void WiFiManager::_handleReconnect() {
    if (!_cfg.staEnabled || _cfg.staSSID.length() == 0) return;
    if (_staConnected) return;  // use cache - avoids WiFi.status() every tick

    unsigned long now = millis();
    if (_staInitiated && (now - _lastReconnectAttempt) >= _reconnectInterval) {
        Serial.printf(DEBUG_TAG " STA: retry '%s' (next in %lus)\n",
                      _cfg.staSSID.c_str(),
                      min(300000UL, _reconnectInterval * 2) / 1000UL);
        WiFi.disconnect(false);
        WiFi.begin(_cfg.staSSID.c_str(), _cfg.staPass.c_str());
        _lastReconnectAttempt = now;
        _reconnectInterval    = min(300000UL, _reconnectInterval * 2);
    }
}

// ── staStatus ─────────────────────────────────────────────────
String WiFiManager::staStatus() const {
    if (!_cfg.staEnabled)           return "Disabled";
    if (_cfg.staSSID.length() == 0) return "No SSID configured";
    switch (WiFi.status()) {
        case WL_CONNECTED:      return "Connected";
        case WL_CONNECT_FAILED: return "Wrong password";
        case WL_NO_SSID_AVAIL:  return "Network not found";
        default: return _staInitiated ? "Connecting..." : "Disconnected";
    }
}

// ── Wi-Fi scan ────────────────────────────────────────────────
void WiFiManager::startScan() {
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        WiFi.scanNetworks(true);
        _scanPending = true;
        Serial.println(DEBUG_TAG " WiFi scan started");
    }
}
bool WiFiManager::scanInProgress() const {
    return WiFi.scanComplete() == WIFI_SCAN_RUNNING;
}
String WiFiManager::scanResultsJson() const {
    int n = WiFi.scanComplete();
    JsonDocument doc;
    doc["scanning"] = (n == WIFI_SCAN_RUNNING);
    JsonArray arr = doc["networks"].to<JsonArray>();
    if (n > 0) {
        int limit = min(n, WIFI_SCAN_MAX_RESULTS);
        for (int i = 0; i < limit; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"]    = WiFi.SSID(i);
            o["rssi"]    = WiFi.RSSI(i);
            o["enc"]     = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)
                           ? "OPEN" : "WPA";
            o["channel"] = WiFi.channel(i);
        }
    }
    String out; serializeJson(doc, out);
    return out;
}

// ── Config persistence ────────────────────────────────────────
bool WiFiManager::loadConfig() {
    if (!LittleFS.exists(CFG_FILE)) return true;
    File f = LittleFS.open(CFG_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return false; }
    f.close();
    _cfg.apSSID     = doc["apSSID"]     | DEFAULT_AP_SSID;
    _cfg.apPass     = doc["apPass"]     | DEFAULT_AP_PASS;
    _cfg.apChannel  = doc["apChannel"]  | (uint8_t)DEFAULT_AP_CHANNEL;
    _cfg.apHidden   = doc["apHidden"]   | false;
    _cfg.staSSID    = doc["staSSID"]    | (const char*)"";
    _cfg.staPass    = doc["staPass"]    | (const char*)"";
    _cfg.staEnabled = doc["staEnabled"] | false;
    if (_cfg.apChannel < 1 || _cfg.apChannel > 13) _cfg.apChannel = 1;
    return true;
}

bool WiFiManager::saveConfig() {
    File f = LittleFS.open(CFG_FILE, "w");
    if (!f) return false;
    JsonDocument doc;
    doc["apSSID"]     = _cfg.apSSID;
    doc["apPass"]     = _cfg.apPass;
    doc["apChannel"]  = _cfg.apChannel;
    doc["apHidden"]   = _cfg.apHidden;
    doc["staSSID"]    = _cfg.staSSID;
    doc["staPass"]    = _cfg.staPass;
    doc["staEnabled"] = _cfg.staEnabled;
    size_t w = serializeJson(doc, f);
    f.close();
    if (w) Serial.println(DEBUG_TAG " Config saved");
    return w > 0;
}

// ── IR pin persistence ────────────────────────────────────────
#define IR_PINS_FILE "/ir_pins.json"

bool WiFiManager::saveIrPins(const IrPinConfig& pins) {
    File f = LittleFS.open(IR_PINS_FILE, "w");
    if (!f) return false;
    JsonDocument doc;
    doc["recvPin"]   = pins.recvPin;
    doc["emitCount"] = pins.emitCount;
    JsonArray ea = doc["emit"].to<JsonArray>();
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        JsonObject o = ea.add<JsonObject>();
        o["pin"]     = pins.emitPin[i];
        o["enabled"] = pins.emitEnabled[i];
    }
    size_t w = serializeJson(doc, f);
    f.close();
    return w > 0;
}

bool WiFiManager::loadIrPins(IrPinConfig& pins) {
    if (!LittleFS.exists(IR_PINS_FILE)) return false;
    File f = LittleFS.open(IR_PINS_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return false; }
    f.close();
    pins.recvPin   = doc["recvPin"]   | (uint8_t)IR_DEFAULT_RECV_PIN;
    pins.emitCount = doc["emitCount"] | (uint8_t)1;
    if (pins.emitCount > IR_MAX_EMITTERS) pins.emitCount = IR_MAX_EMITTERS;
    if (doc["emit"].is<JsonArrayConst>()) {
        uint8_t i = 0;
        for (JsonObjectConst o : doc["emit"].as<JsonArrayConst>()) {
            if (i >= IR_MAX_EMITTERS) break;
            pins.emitPin[i]     = o["pin"]     | pins.emitPin[i];
            pins.emitEnabled[i] = o["enabled"] | (i == 0);
            ++i;
        }
    }
    return true;
}

// ── mDNS ──────────────────────────────────────────────────────
// Advertises the device as http://ir-remote.local (configurable
// via MDNS_HOSTNAME in config.h).  Also announces the HTTP service
// so Bonjour / Avahi browsers can discover it automatically.
//
// Called from _onStaGotIP() - only meaningful on the STA interface
// because the AP subnet (192.168.4.x) clients don't have mDNS relay.
// stopMdns() is called on STA disconnect to release the responder.
void WiFiManager::startMdns() {
    if (_mdnsRunning) {
        // Already running - end first so hostname/IP updates are applied
        MDNS.end();
        _mdnsRunning = false;
    }

    if (MDNS.begin(MDNS_HOSTNAME)) {
        _mdnsRunning = true;

        // Announce HTTP service so Bonjour/Avahi browsers auto-discover
        MDNS.addService("http", "tcp", HTTP_PORT);

        // Convenience TXT records for identification
        MDNS.addServiceTxt("http", "tcp", "device",  "IR-Remote");
        MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);

        Serial.printf(DEBUG_TAG " [mDNS] Started: http://%s.local  (IP: %s)\n",
                      MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
    } else {
        Serial.println(DEBUG_TAG " [mDNS] ERROR: MDNS.begin() failed");
    }

    // NetBIOS: Windows can resolve \\IR-REMOTE or http://ir-remote
    // without Bonjour — uses UDP port 137 (NBNS broadcast)
    NBNS.begin(MDNS_HOSTNAME);
    Serial.printf(DEBUG_TAG " [NBNS] Started: http://%s  (Windows NetBIOS)\n",
                  MDNS_HOSTNAME);
}

void WiFiManager::stopMdns() {
    if (!_mdnsRunning) return;
    MDNS.end();
    NBNS.end();
    _mdnsRunning = false;
    Serial.println(DEBUG_TAG " [mDNS+NBNS] Stopped (STA disconnected)");
}

// ── Status helpers ────────────────────────────────────────────
String WiFiManager::apIP() const {
    return _apActive ? WiFi.softAPIP().toString() : "";
}
String WiFiManager::staIP() const {
    return _staConnected ? WiFi.localIP().toString() : "";
}
bool   WiFiManager::staConnected() const {
    // Returns cached value - updated by _onStaGotIP() / _onStaDisconnected()
    // events. Avoids WiFi.status() mutex acquisition on every loop() tick.
    return _staConnected;
}
int8_t WiFiManager::staRSSI() const {
    return _staConnected ? (int8_t)WiFi.RSSI() : 0;
}
