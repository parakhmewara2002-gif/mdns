#pragma once
// ============================================================
//  wifi_pen_module.h  —  WiFi Penetration Testing Module v4.0
//
//  Full port of esp32-wifi-penetration-tool into IR Remote GUI.
//  Includes proper frame_analyzer, pcap_serializer, hccapx_serializer
//  and wsl_bypasser logic — all adapted to Arduino/C++.
//
//  Attack types  : PMKID, Handshake, DoS, BeaconFlood, ProbeSniffer,
//                  EVIL_TWIN(4 in WpenMethod), AUTH_FLOOD(6), DISASSOC_FLOOD(7)
//  Attack methods: Passive, Broadcast-Deauth, Rogue-AP, Combine
//  Extras        : Channel hopper, timeout watchdog, PCAP + HCCAPX,
//                  EAPOL replay dedup, client tracking, OUI lookup,
//                  hidden SSID reveal, SAE capture, attack log,
//                  LittleFS target persistence, SD PCAP save,
//                  PMKID dedup, multi-target, WPA2/WPA3 auto-detect,
//                  frame count, rate-limited deauth, evil twin + captive portal,
//                  auth flood, disassoc flood, WPS detection,
//                  channel utilization, hidden AP fingerprint,
//                  mesh/repeater detection, vendor IE parser,
//                  RSSI history, attack profiles, hcxtools .22000 format,
//                  CSV AP export, AP uptime from TSF
// ============================================================
#include <Arduino.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "arpa/inet.h"
#include <ESPAsyncWebServer.h>

// ── Enums ──────────────────────────────────────────────────
enum class WpenAttackType : uint8_t {
    PASSIVE        = 0,
    HANDSHAKE      = 1,
    PMKID          = 2,
    DOS            = 3,
    BEACON_FLOOD   = 4,
    PROBE_SNIFFER  = 5,
    AUTH_FLOOD     = 6,
    DISASSOC_FLOOD = 7
};

enum class WpenMethod : uint8_t {
    ROGUE_AP   = 0,   // clone BSSID+SSID
    BROADCAST  = 1,   // raw deauth via WSL bypasser
    PASSIVE    = 2,   // listen only
    COMBINE    = 3,   // rogue + broadcast (DoS)
    EVIL_TWIN  = 4    // rogue + captive portal
};

enum class WpenState : uint8_t {
    IDLE     = 0,
    RUNNING  = 1,
    FINISHED = 2,
    TIMEOUT  = 3
};

// ── PCAP structures (Wireshark format) ─────────────────────
#pragma pack(push, 1)
struct pcap_global_hdr_t {
    uint32_t magic_number  = 0xa1b2c3d4;
    uint16_t version_major = 2;
    uint16_t version_minor = 4;
    int32_t  thiszone      = 0;
    uint32_t sigfigs       = 0;
    uint32_t snaplen       = 65535;
    uint32_t network       = 105;  // LINKTYPE_IEEE802_11
};
struct pcap_rec_hdr_t {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
#pragma pack(pop)

// ── HCCAPX structure (hashcat format) ─────────────────────
#pragma pack(push, 1)
struct hccapx_t {
    uint32_t signature   = 0x58504348;  // "HCPX"
    uint32_t version     = 4;
    uint8_t  message_pair= 255;
    uint8_t  essid_len   = 0;
    uint8_t  essid[32]   = {};
    uint8_t  keyver      = 2;  // WPA2
    uint8_t  keymic[16]  = {};
    uint8_t  mac_ap[6]   = {};
    uint8_t  nonce_ap[32]= {};
    uint8_t  mac_sta[6]  = {};
    uint8_t  nonce_sta[32]={};
    uint16_t eapol_len   = 0;
    uint8_t  eapol[256]  = {};
};
#pragma pack(pop)

// ── 802.11 typed structs (ported from frame_analyzer_types.h) ──
#pragma pack(push, 1)
struct frame_ctrl_t {
    uint8_t  protocol_version : 2;
    uint8_t  type             : 2;
    uint8_t  subtype          : 4;
    uint8_t  to_ds            : 1;
    uint8_t  from_ds          : 1;
    uint8_t  more_fragments   : 1;
    uint8_t  retry            : 1;
    uint8_t  power_management : 1;
    uint8_t  more_data        : 1;
    uint8_t  protected_frame  : 1;
    uint8_t  htc_order        : 1;
};
struct mac_hdr_t {
    frame_ctrl_t fc;
    uint16_t     duration;
    uint8_t      addr1[6];
    uint8_t      addr2[6];
    uint8_t      addr3[6];
    uint16_t     seq_ctrl;
};
struct llc_snap_t {
    uint8_t dsap;
    uint8_t ssap;
    uint8_t ctrl;
    uint8_t enc[3];
};
struct eapol_hdr_t {
    uint8_t  version;
    uint8_t  packet_type;
    uint16_t packet_body_len;
};
struct eapol_key_t {
    uint8_t  descriptor_type;
    uint8_t  key_info[2];
    uint16_t key_length;
    uint8_t  replay_counter[8];
    uint8_t  key_nonce[32];
    uint8_t  key_iv[16];
    uint8_t  key_rsc[8];
    uint8_t  reserved[8];
    uint8_t  key_mic[16];
    uint16_t key_data_length;
    // key_data follows
};
struct key_data_kde_t {
    uint8_t  type;
    uint8_t  length;
    uint8_t  oui[3];
    uint8_t  data_type;
    // data follows
};
#pragma pack(pop)

#define EAPOL_KEY_PACKET_TYPE 3

// ── Per-AP record ──────────────────────────────────────────
struct WpenAP {
    char    ssid[33];
    uint8_t bssid[6];
    int8_t  rssi;
    uint8_t channel;
    uint8_t authmode;
    bool    wps = false;
};

// ── PMKID linked list ──────────────────────────────────────
struct PmkidItem {
    uint8_t    pmkid[16];
    PmkidItem *next;
};

// ── Client station record ──────────────────────────────────
struct WpenClient {
    uint8_t mac[6];
};

// ── Probe sniffer record ───────────────────────────────────
struct ProbeEntry {
    uint8_t mac[6];
    char    ssid[33];
    int8_t  rssi;
};

// ── Vendor IE record ──────────────────────────────────────
struct VendorIE {
    uint8_t oui[3];
    uint8_t data_type;
};

// ── RSSI history ring buffer per AP ───────────────────────
struct RssiHistory {
    int8_t  samples[8];
    uint8_t head;
    uint8_t len;
};

// ── Attack profile preset ──────────────────────────────────
struct WpenProfile {
    char           name[24];
    WpenAttackType type;
    WpenMethod     method;
    uint8_t        timeoutSec;
    uint8_t        deauthPps;
};

// ── Module class ───────────────────────────────────────────
class WifiPenModule {
public:
    void begin();
    void loop();

    // Scan
    bool    scan();
    uint8_t apCount()   const { return _apCount; }
    String  apListJson()const;
    String  apListCsv() const;
    const WpenAP* getAP(uint8_t i) const;

    // WPS probe
    bool probeForWps(uint8_t apIdx, uint32_t timeoutMs);

    // Attack
    bool startAttack(WpenAttackType t, WpenMethod m,
                     uint8_t apIdx, uint8_t timeoutSec);
    void stopAttack();
    void resetAttack();

    // Multi-target
    void setMultiTargets(const uint8_t* idxList, uint8_t count);

    // WPA2/WPA3 auto-detect
    WpenAttackType autoDetectAttackType(uint8_t apIdx);

    // Rate-limited deauth
    void setDeauthRate(uint8_t pps);

    // Attack profiles
    bool   applyProfile(uint8_t profileIdx, uint8_t apIdx);
    String profileListJson() const;

    // Status
    WpenState      state()     const { return _state; }
    WpenAttackType attackType()const { return _atype; }
    String         statusJson()const;

    // Results
    const uint8_t* pcapBuf()     const { return _pcap; }
    size_t         pcapSize()    const { return _pcapSz; }
    const hccapx_t* hccapx()    const { return &_hccapx; }
    bool           hasPcap()     const { return _pcapSz > 0; }
    bool           hasHccapx()   const { return _hccapx.message_pair != 255; }
    String         pmkidHashcat()const;
    String         pmkidHcx22000()const;

    // Channel hop
    void setHop(bool en);
    bool hopActive() const { return _hopActive; }

    // Enable flag
    void setEnabled(bool en);
    bool enabled()   const { return _enabled; }

    // Target client (directed deauth)
    void setTargetClient(const uint8_t* mac);
    void clearTargetClient();

    // Auto channel lock
    void setAutoChannelLock(bool en);

    // Client list (detected associated stations)
    String clientListJson() const;

    // Probe sniffer list
    String probeListJson() const;

    // Hashcat command generation
    String hashcatCmd() const;

    // Save PCAP to SD card
    bool savePcapToSd(const char* path);

    // Persist targets to / from LittleFS
    void saveTargets();
    void loadTargets();

    // Attack log
    String attackLogJson() const;

    // OUI lookup (static, offline table)
    static const char* ouiLookup(const uint8_t* mac);

    // Captive portal password
    String captivePassword() const;

    // Channel utilization
    String channelUtilJson() const;

    // Hidden AP fingerprint
    String hiddenApJson() const;

    // Mesh/repeater detection
    String meshApJson() const;

    // Vendor IE
    String vendorIEJson() const;

    // RSSI history
    String rssiHistJson(uint8_t apIdx) const;

    // AP uptime from TSF
    uint32_t targetUptimeSec() const;

    // ── SD integration (features 29-33) ───────────────────────
    void setAutoSavePcap(bool en) { _autoSavePcap = en; }
    bool autoSavePcap() const { return _autoSavePcap; }

    bool saveHcx22000ToSD();
    bool savePmkidToSD();
    void setAutoSavePmkid(bool en) { _autoSavePmkid = en; }
    bool autoSavePmkid() const { return _autoSavePmkid; }

    bool flushAttackLogToSD();
    void setAutoLogFlush(bool en) { _autoLogFlush = en; }
    bool autoLogFlush() const { return _autoLogFlush; }

    bool exportClientsToSD();
    bool exportProbesToSD();

    // ── Called from static promiscuous callback ─────────────
    void _rxFrame(void* buf, wifi_promiscuous_pkt_type_t type);

private:
    bool     _enabled   = false;
    uint8_t  _apCount   = 0;
    WpenAP   _aps[20];

    WpenState      _state = WpenState::IDLE;
    WpenAttackType _atype = WpenAttackType::PASSIVE;
    WpenMethod     _method= WpenMethod::PASSIVE;
    uint8_t        _tIdx  = 0;
    unsigned long  _tStart= 0;
    unsigned long  _tOutMs= 30000;
    bool           _sniff = false;
    bool           _rogueActive = false;
    uint8_t        _origMac[6] = {};
    bool           _hopActive  = false;

    // timers
    esp_timer_handle_t _deauthTimer    = nullptr;
    esp_timer_handle_t _hopTimer       = nullptr;
    esp_timer_handle_t _beaconTimer    = nullptr;
    esp_timer_handle_t _authFloodTimer = nullptr;
    esp_timer_handle_t _disassocTimer  = nullptr;

    // PCAP
    uint8_t* _pcap   = nullptr;
    size_t   _pcapSz = 0;

    // HCCAPX state machine
    hccapx_t _hccapx;
    unsigned _msgAP  = 0;
    unsigned _msgSTA = 0;
    unsigned _eapolSrc = 0;

    // PMKID linked list
    PmkidItem* _pmkidHead = nullptr;

    // EAPOL replay dedup ring buffer (8 slots × 8 bytes)
    uint8_t _replayRing[8][8];
    uint8_t _replayRingIdx = 0;

    // Client station tracking
    WpenClient _clients[16];
    uint8_t    _clientCount = 0;

    // Probe sniffer
    ProbeEntry _probes[32];
    uint8_t    _probeCount = 0;

    // SAE frame counter (WPA3)
    uint8_t _saeFrameCount = 0;

    // Directed deauth target
    uint8_t _targetClient[6] = {};
    bool    _hasTargetClient  = false;

    // Auto channel lock
    bool    _autoChannelLock = false;
    uint8_t _lockedChannel   = 0;

    // Attack log ring buffer  (64 entries × 80 chars)
    char    _attackLog[64][80];
    uint8_t _attackLogHead = 0;
    uint8_t _attackLogLen  = 0;

    // ── Feature: Multi-target ──────────────────────────────
    uint8_t _targetIdxList[8]  = {};
    uint8_t _targetIdxCount    = 0;
    bool    _multiTarget       = false;
    uint8_t _matchedIdx        = 0;

    // ── Feature: Frame count ───────────────────────────────
    uint32_t _frameCount = 0;

    // ── Feature: Rate-limited deauth ──────────────────────
    uint8_t _deauthPps = 10;

    // ── Feature: Evil Twin / Captive Portal ───────────────
    bool             _captiveActive   = false;
    uint16_t         _captivePort     = 8080;
    AsyncWebServer*  _captiveServer   = nullptr;
    char             _captivePassword[64] = {};

    // ── Feature: Vendor IE list ────────────────────────────
    VendorIE _vendorIEs[8]   = {};
    uint8_t  _vendorIECount  = 0;

    // ── Feature: RSSI history ─────────────────────────────
    RssiHistory _rssiHist[20] = {};

    // ── Feature: AP uptime from TSF ───────────────────────
    uint64_t _targetTsf = 0;

    // ── Feature: WPS probing ──────────────────────────────
    bool    _wpsProbing  = false;
    uint8_t _wpsProbeIdx = 0;
    bool    _wpsResult   = false;
    bool    _wpsDone     = false;

    // ── SD integration flags (features 29-33) ─────────────
    bool          _autoSavePcap   = false;
    bool          _autoSavePmkid  = false;
    bool          _autoLogFlush   = false;
    unsigned long _lastLogFlushMs = 0;

    // ── Private methods ──────────────────────────────────────
    // sniffer
    void _snifferStart(uint8_t ch);
    void _snifferStop();

    // wsl bypasser
    void _sendDeauthFrame();
    void _deauthTimerStart(uint32_t periodUs);
    void _deauthTimerStop();

    // rogue AP
    void _rogueStart(uint8_t idx);
    void _rogueStop();

    // channel hop
    void _hopStart();
    void _hopStop();

    // beacon flood
    void _beaconFloodStart();
    void _beaconFloodStop();

    // auth flood
    void _authFloodStart();
    void _authFloodStop();

    // disassoc flood
    void _disassocTimerStart(uint32_t periodUs);
    void _disassocTimerStop();

    // captive portal
    void _captiveStart();
    void _captiveStop();

    // frame parsing (ported from frame_analyzer_parser.c)
    bool           _isBssidMatch(const wifi_promiscuous_pkt_t* p);
    eapol_hdr_t*   _parseEapol(const wifi_promiscuous_pkt_t* p);
    eapol_key_t*   _parseEapolKey(const eapol_hdr_t* eh);
    PmkidItem*     _parsePmkid(const eapol_key_t* ek);

    // PCAP serializer (ported from pcap_serializer.c)
    void _pcapInit();
    void _pcapAppend(const uint8_t* buf, size_t len, uint32_t ts_us);
    void _pcapFree();

    // HCCAPX serializer (ported from hccapx_serializer.c)
    void _hccapxInit(const char* ssid, uint8_t ssidLen);
    void _hccapxAddFrame(const wifi_promiscuous_pkt_t* p);
    bool _hccapxArrayZero(const uint8_t* a, unsigned sz);
    int  _hccapxSaveEapol(const eapol_hdr_t* eh, const eapol_key_t* ek);

    // frame dispatch
    void _handleHandshakeFrame(const wifi_promiscuous_pkt_t* p);
    void _handlePmkidFrame(const wifi_promiscuous_pkt_t* p);
    void _handleMgmtFrame(const wifi_promiscuous_pkt_t* p);

    // PMKID helpers
    void _pmkidFreeList();

    // replay counter dedup
    bool _replaySeenAndStore(const uint8_t rc[8]);

    // client list helpers
    void _addClient(const uint8_t* mac);

    // probe sniffer helpers
    void _addProbe(const uint8_t* mac, const char* ssid, int8_t rssi);

    // RSSI history push helper
    void _rssiPush(uint8_t apSlot, int8_t rssi);

    // attack log
    void _logEvent(const char* fmt, ...);

    // static timer/promiscuous callbacks
    static void _cbDeauth(void* arg);
    static void _cbHop(void* arg);
    static void _cbBeacon(void* arg);
    static void _cbAuthFlood(void* arg);
    static void _cbDisassoc(void* arg);
    static void _cbPromiscuous(void* buf, wifi_promiscuous_pkt_type_t type);
};

extern WifiPenModule wifiPen;
