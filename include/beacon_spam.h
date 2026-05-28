#pragma once
// ============================================================
//  beacon_spam.h  -  Fake AP Beacon Flood (ported from Bruce)
//  WebUI controlled — no screen needed
// ============================================================
#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define BEACON_MAX_SSIDS   64
#define BEACON_PKT_LEN    109

enum class BeaconMode : uint8_t {
    RANDOM = 0,   // random SSIDs each beacon
    LIST   = 1,   // SSIDs from user-provided list
    CLONE  = 2,   // clone nearby APs
};

class BeaconSpam {
public:
    void   begin();
    void   loop();

    // start/stop
    void   start(BeaconMode mode = BeaconMode::RANDOM,
                 uint8_t channel = 0);   // 0 = hop
    void   stop();
    bool   isActive() const { return _active; }

    // custom SSID list (for LIST mode)
    void   clearSsids();
    void   addSsid(const String& ssid);
    void   setSsids(const std::vector<String>& list);

    String statusJson() const;

private:
    bool     _active    = false;
    BeaconMode _mode    = BeaconMode::RANDOM;
    uint8_t  _channel   = 1;
    bool     _hop       = true;
    uint32_t _lastTxMs  = 0;
    uint32_t _lastHopMs = 0;
    uint32_t _sentCount = 0;
    uint32_t _startMs   = 0;
    uint8_t  _ssidIdx   = 0;

    std::vector<String> _ssids;
    // Guards _ssids: set/clear from web handlers (Core 0), read in loop() (Core 1).
    SemaphoreHandle_t _ssidMux = nullptr;

    uint8_t _pkt[BEACON_PKT_LEN];

    void _sendBeacon(const char* ssid, uint8_t ssidLen,
                     const uint8_t* mac, uint8_t channel);
    void _randomMac(uint8_t* mac);
    String _randomSsid();
    void _nextChannel();
};

extern BeaconSpam beaconSpam;
