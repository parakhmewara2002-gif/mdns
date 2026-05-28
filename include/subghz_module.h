#pragma once
// ============================================================
//  subghz_module.h  -  CC1101 Sub-1GHz Real Implementation
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>

#define SUBGHZ_SAVE_FILE   "/subghz_signals.json"
#define SUBGHZ_GPIO_FILE   "/subghz_gpio.json"

// CC1101 modulation modes
enum class SubGhzModulation : uint8_t {
    ASK_OOK = 0,   // Amplitude-shift keying / On-Off Keying (default, garage doors, power sockets)
    FSK2    = 1,   // 2-FSK (two-frequency shift keying — weather stations, TPMS, some alarms)
    GFSK    = 2,   // Gaussian-FSK (smoother FSK — Bluetooth-compatible sensors)
    FSK4    = 3,   // 4-FSK (four-frequency, used in some advanced remotes)
};

// Named preset bundles register values for common use-cases
struct SubGhzPreset {
    const char*     name;
    SubGhzModulation mod;
    float           freqMhz;
    uint8_t         mdmcfg4;   // bandwidth + data-rate exponent
    uint8_t         mdmcfg3;   // data-rate mantissa
    uint8_t         mdmcfg2;   // modulation + sync word
    uint8_t         deviatn;   // FSK deviation (ignored for ASK/OOK)
};

struct SubGhzSignal {
    uint32_t id       = 0;
    String   name;
    float    freqMhz  = 433.92f;
    String   protocol;
    String   rawData;
    String   captured;
};

struct SubGhzGpioConfig {
    bool    enabled = false;
    uint8_t gdo0   = 34;
    uint8_t gdo2   = 35;
    uint8_t cs     = 32;
    // HSPI bus to avoid conflict with SD card (VSPI: SCK=18,MOSI=23,MISO=19)
    uint8_t sck    = 14;   // HSPI SCK
    uint8_t mosi   = 13;   // HSPI MOSI
    uint8_t miso   = 26;   // HSPI MISO (GPIO12 forbidden at boot - use 26, shares IR TX-1 mutually exclusive)
    uint8_t spiBus = 1;    // 1=HSPI (SD uses VSPI)
};

class SubGhzModule {
public:
    void   begin();
    void   loop();
    void   reinit(const SubGhzGpioConfig& cfg);

    bool   isConnected()  const { return _hwConnected; }
    bool   isEnabled()    const { return _moduleEnabled; }
    void   setEnabled(bool en);
    bool   isCapturing()  const { return _capturing; }
    bool   dataAvailable()const { return !_capBuffer.isEmpty(); }

    bool   startCapture(float freqMhz,
                        SubGhzModulation mod = SubGhzModulation::ASK_OOK);
    void   stopCapture();
    String pollCaptured();

    bool   setModulation(SubGhzModulation mod);
    SubGhzModulation getModulation() const { return _modulation; }
    static String modulationName(SubGhzModulation m);
    static const SubGhzPreset* builtinPresets(uint8_t& count);

    bool   replaySignal(uint32_t id);

    uint32_t saveSignal(SubGhzSignal& sig);
    bool     deleteSignal(uint32_t id);
    bool     renameSignal(uint32_t id, const String& name);
    bool     getSignal(uint32_t id, SubGhzSignal& out) const;
    String   signalsToJson() const;

    void             saveGpioConfig(const SubGhzGpioConfig& cfg);
    SubGhzGpioConfig loadGpioConfig() const;

    String statusJson() const;

    // ── SD integration (features 25-28) ───────────────────────
    bool autoSdSaveEnabled() const { return _autoSdSave; }
    void setAutoSdSave(bool en) { _autoSdSave = en; }

    bool backupSignalsToSD();
    bool restoreSignalsFromSD();

    bool replayFromSD(const String& filename);
    std::vector<String> listSdSignals() const;

    bool sdLogEnabled() const { return _sdLog; }
    void setSdLog(bool en) { _sdLog = en; }

    // ── Brute Force (Bruce port) ──────────────────────────────
    bool   startBruteForce(float freqMhz, const String& mod,
                            uint32_t startCode, uint32_t endCode,
                            uint8_t bits, uint32_t delayMs);
    void   stopBruteForce();
    bool   isBruteRunning() const { return _bruteRunning; }
    String bruteStatusJson() const;

private:
    bool    _hwConnected   = false;
    bool    _moduleEnabled  = false;
    bool    _capturing   = false;
    bool    _autoSdSave  = false;
    bool    _sdLog       = false;
    float   _freqMhz     = 433.92f;
    SubGhzModulation _modulation = SubGhzModulation::ASK_OOK;
    String  _capBuffer;
    unsigned long _captureStart = 0;
    SubGhzGpioConfig _cfg;

    std::vector<SubGhzSignal> _signals;
    uint32_t _nextId = 1;

    // brute force state
    bool     _bruteRunning  = false;
    uint32_t _bruteCurrent  = 0;
    uint32_t _bruteEnd      = 0;
    uint32_t _bruteSent     = 0;
    uint32_t _bruteTotal    = 0;
    uint8_t  _bruteBits     = 24;
    uint32_t _bruteDelayMs  = 100;
    uint32_t _bruteLastMs   = 0;

    void _loadSignals();
    void _saveSignals() const;

    // CC1101 SPI helpers (register-level, no external lib dependency)
    uint8_t  _spiRead(uint8_t addr);
    void     _spiWrite(uint8_t addr, uint8_t val);
    uint8_t  _spiReadStatus(uint8_t addr);
    void     _spiCommand(uint8_t cmd);
    void     _reset();
    bool     _detectCC1101();
    void     _setFrequency(float mhz);
    void     _setModeTx();
    void     _setModeRx();
    void     _setModeIdle();
    void     _spiByte(uint8_t b);
    void     _applyModulation(SubGhzModulation mod);

    // OOK/ASK raw capture via GDO0 pin
    void     _captureLoop();
    String   _rawToHex(const std::vector<uint16_t>& timings) const;

    std::vector<uint16_t> _captureTimings;
    bool     _lastGdo0   = false;
    unsigned long _lastEdge = 0;
};

extern SubGhzModule subGhzModule;
