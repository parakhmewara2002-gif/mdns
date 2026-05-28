#pragma once
// ============================================================
//  nrf24_module.h  -  NRF24L01 2.4GHz - Real HW Implementation
//  Added: Jammer, Spectrum, Mousejack (ported from Bruce firmware)
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SPI.h>
#include <RF24.h>
#include <vector>

#define NRF24_GPIO_FILE  "/nrf24_gpio.json"
#define NRF24_CHANNELS   125

enum class Nrf24Module_t { NRF24L01, NRF24L01_PA_LNA, NRF24L01_SMA };
enum class Nrf24DataRate  { RATE_250K, RATE_1M, RATE_2M };
enum class Nrf24PaLevel   { PA_MIN, PA_LOW, PA_HIGH, PA_MAX };

// ── Jammer ───────────────────────────────────────────────────
enum class NrfJamMode : uint8_t {
    FULL = 0, WIFI, BLE, BLE_ADV, BLUETOOTH,
    USB, VIDEO, RC, ZIGBEE, DRONE,
    COUNT
};
#define NRF_JAM_MODE_COUNT 10

struct NrfJamConfig {
    uint8_t paLevel   = 3;  // 0=MIN 1=LOW 2=HIGH 3=MAX
    uint8_t dataRate  = 1;  // 0=1M 1=2M 2=250K
    uint8_t _pad0     = 0;
    uint8_t strategy  = 2;  // 0=CW 1=FLOOD 2=TURBO
    uint8_t burstSize = 20;
    uint8_t randomHop = 1;
    uint8_t _res      = 0;
};

// ── Mousejack ────────────────────────────────────────────────
#define MJ_MAX_TARGETS 16
#define MJ_MOD_NONE    0x00
#define MJ_MOD_LSHIFT  0x02
#define MJ_KEY_SPACE   0x2C
#define MJ_KEY_A       0x04
#define MJ_KEY_0       0x27
#define MJ_KEY_1       0x1E
#define MJ_KEY_2       0x1F
#define MJ_KEY_3       0x20
#define MJ_KEY_4       0x21
#define MJ_KEY_5       0x22
#define MJ_KEY_6       0x23
#define MJ_KEY_7       0x24
#define MJ_KEY_8       0x25
#define MJ_KEY_9       0x26
#define MJ_KEY_ENTER   0x28
#define MJ_KEY_MINUS   0x2D
#define MJ_KEY_EQUAL   0x2E
#define MJ_KEY_LBRACKET 0x2F
#define MJ_KEY_RBRACKET 0x30
#define MJ_KEY_BACKSLASH 0x31
#define MJ_KEY_SEMICOLON 0x33
#define MJ_KEY_QUOTE   0x34
#define MJ_KEY_GRAVE   0x35
#define MJ_KEY_COMMA   0x36
#define MJ_KEY_DOT     0x37
#define MJ_KEY_SLASH   0x38

struct MjHidKey { uint8_t mod; uint8_t key; };

struct MjTarget {
    uint8_t  addr[5]   = {};
    uint8_t  addrLen   = 0;
    uint8_t  channel   = 0;
    uint8_t  devType   = 0; // 0=unknown 1=mouse 2=keyboard
    uint32_t lastSeen  = 0;
    bool     valid     = false;
};

struct Nrf24GpioConfig {
    bool          enabled    = false;
    Nrf24Module_t moduleType = Nrf24Module_t::NRF24L01;
    uint8_t  ce      = 16;
    uint8_t  csn     = 17;
    uint8_t  sck     = 14;
    uint8_t  mosi    = 13;
    uint8_t  miso    = 26;
    uint8_t  irq     = 0;
    uint8_t  spiBus  = 1;
    Nrf24DataRate dataRate = Nrf24DataRate::RATE_1M;
    Nrf24PaLevel  paLevel  = Nrf24PaLevel::PA_HIGH;
};

struct Nrf24Packet {
    uint8_t  channel   = 0;
    uint32_t timestamp = 0;
    String   data;
};

class Nrf24Module {
public:
    void   begin();
    void   loop();
    void   reinit(const Nrf24GpioConfig& cfg);

    bool   isConnected()  const { return _hwConnected; }
    bool   isEnabled()    const { return _moduleEnabled; }
    void   setEnabled(bool en);
    bool   isScanning()   const { return _scanning; }
    bool   isSniffing()   const { return _sniffing; }
    bool   isCapturingReplay() const { return _capReplay; }

    // Channel scanner
    void   startScan();
    void   stopScan();
    const uint8_t* scanData() const { return _scanChannels; }

    // Sniffer
    void   startSniff();
    void   stopSniff();
    String pollSniffPacket();

    // Replay
    void   startReplayCapture();
    void   stopReplayCapture();
    size_t replayPacketCount() const { return _replayBuf.size(); }
    bool   replayPackets();

    // RC command
    void   sendRcCommand(char dir, uint8_t speed);

    // Runtime config
    void   setChannel(uint8_t ch);
    void   setDataRate(Nrf24DataRate r);

    // ── Jammer (Bruce port) ──────────────────────────────────
    void   startJammer(NrfJamMode mode = NrfJamMode::FULL);
    void   stopJammer();
    bool   isJamming()    const { return _jamming; }
    NrfJamMode jamMode()  const { return _jamMode; }
    String jammerStatusJson() const;

    // ── Spectrum (Bruce port) ────────────────────────────────
    void   startSpectrum();
    void   stopSpectrum();
    bool   isSpectrumRunning() const { return _spectrum; }
    String spectrumJson() const;   // returns "{ch0,ch1,...,ch79}" 80 values

    // ── Mousejack (Bruce port) ───────────────────────────────
    void   startMousejackScan();
    void   stopMousejackScan();
    bool   isMousejackScanning() const { return _mjScanning; }
    bool   mousejackInject(uint8_t targetIdx, const String& text);
    String mousejackTargetsJson() const;

    // GPIO config persistence
    void            saveGpioConfig(const Nrf24GpioConfig& cfg);
    Nrf24GpioConfig loadGpioConfig() const;

    // Status JSON
    String statusJson() const;

    // SD integration
    bool backupConfigToSD(const String& tag);
    bool sdLogEnabled() const { return _sdLog; }
    void setSdLog(bool en)    { _sdLog = en; }

private:
    RF24*        _radio         = nullptr;
    bool         _hwConnected   = false;
    bool         _moduleEnabled = false;
    bool         _scanning      = false;
    bool         _sniffing      = false;
    bool         _capReplay     = false;
    bool         _sdLog         = false;
    uint8_t      _channel       = 76;
    Nrf24DataRate _dataRate     = Nrf24DataRate::RATE_1M;
    Nrf24PaLevel  _paLevel      = Nrf24PaLevel::PA_HIGH;
    Nrf24GpioConfig _cfg;

    uint8_t       _scanChannels[NRF24_CHANNELS] = {};
    unsigned long _scanTimer    = 0;
    uint8_t       _scanChunkPos = 0;
    unsigned long _sniffTimer   = 0;
    String        _sniffPending;
    std::vector<Nrf24Packet> _replayBuf;
    SPIClass*     _spi          = nullptr;

    // ── Jammer state ─────────────────────────────────────────
    bool          _jamming      = false;
    NrfJamMode    _jamMode      = NrfJamMode::FULL;
    NrfJamConfig  _jamCfg;
    uint8_t       _jamChIdx     = 0;
    unsigned long _jamDwellMs   = 0;
    unsigned long _jamSweeps    = 0;
    unsigned long _jamStartMs   = 0;

    // ── Spectrum state ────────────────────────────────────────
    bool          _spectrum     = false;
    uint8_t       _specChans[80]= {};
    unsigned long _specTimer    = 0;
    uint8_t       _specChIdx    = 0;

    // ── Mousejack state ───────────────────────────────────────
    bool          _mjScanning   = false;
    MjTarget      _mjTargets[MJ_MAX_TARGETS] = {};
    uint8_t       _mjTargetCount= 0;
    uint8_t       _mjScanCh     = 0;
    unsigned long _mjTimer      = 0;

    rf24_datarate_e   _toRF24Rate(Nrf24DataRate r) const;
    rf24_pa_dbm_e     _toRF24Pa(Nrf24PaLevel p) const;
    void _applyConfig();

    // ── Jammer helpers ────────────────────────────────────────
    void _jamTick();
    void _jamApplyConfig();
    static const uint8_t* _jamGetChannels(NrfJamMode mode, size_t& count);

    // ── Spectrum helpers ──────────────────────────────────────
    void _spectrumTick();

    // ── Mousejack helpers ─────────────────────────────────────
    void _mjScanTick();
    bool _mjSendKey(uint8_t targetIdx, uint8_t mod, uint8_t key);
    static MjHidKey _asciiToHid(char c);
};

extern Nrf24Module nrf24Module;
