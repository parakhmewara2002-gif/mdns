#pragma once
// ============================================================
//  wifi_manager.h  v2.0.6
//
//  WiFi behavior:
//    Startup       -> AP ON  (192.168.4.1)
//    STA connects  -> 7s delay -> AP OFF
//    STA drops     -> AP ON  (192.168.4.1)
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "config.h"
#include "gpio_config.h"

struct WiFiConfig {
    String  apSSID;
    String  apPass;
    uint8_t apChannel;
    bool    apHidden;
    String  staSSID;
    String  staPass;
    bool    staEnabled;

    WiFiConfig()
        : apSSID(DEFAULT_AP_SSID), apPass(DEFAULT_AP_PASS),
          apChannel(DEFAULT_AP_CHANNEL), apHidden(DEFAULT_AP_HIDDEN),
          staSSID(DEFAULT_STA_SSID), staPass(DEFAULT_STA_PASS),
          staEnabled(false) {}
};

class WiFiManager {
public:
    WiFiManager();
    void   begin();
    void   loop();

    bool   saveConfig();
    bool   loadConfig();
    bool   saveIrPins(const IrPinConfig& pins);
    bool   loadIrPins(IrPinConfig& pins);

    WiFiConfig&       config()       { return _cfg; }
    const WiFiConfig& config() const { return _cfg; }

    String apIP()        const;
    String staIP()       const;
    bool   apActive()    const { return _apActive; }
    bool   staConnected()const;
    int8_t staRSSI()     const;
    String staSSID()     const { return _cfg.staSSID; }
    String staStatus()   const;

    void   startScan();
    bool   scanInProgress() const;
    String scanResultsJson() const;

    void   applyStaConfig();

    // mDNS — started when STA gets an IP, stopped on STA disconnect
    void   startMdns();
    void   stopMdns();
    bool   mdnsActive() const { return _mdnsRunning; }

private:
    WiFiConfig    _cfg;
    bool          _scanPending;
    unsigned long _lastReconnectAttempt;
    unsigned long _reconnectInterval;
    bool          _staInitiated;
    bool          _apActive;
    bool          _wasConnected;
    bool          _staConnected;  // cached - updated by event callbacks, avoids WiFi.status() per tick
    bool          _mdnsRunning;   // true while MDNS.begin() is active
    unsigned long _apStopAt;      // millis() when to stop AP (0=not scheduled)

    void _startAP();
    void _stopAP();
    void _startSTA();
    void _handleReconnect();
    void _onStaGotIP();
    void _onStaDisconnected();
};

extern WiFiManager wifiMgr;
