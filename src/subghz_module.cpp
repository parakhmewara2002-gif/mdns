// ============================================================
//  subghz_module.cpp  -  CC1101 Sub-1GHz Real Implementation
//  Direct SPI register access - no external CC1101 library needed
// ============================================================
#include "subghz_module.h"
#include <SPI.h>
#include "sd_manager.h"

#define SUBGHZ_TAG "[SUBGHZ]"

// CC1101 registers
#define CC1101_SRES     0x30
#define CC1101_SRX      0x34
#define CC1101_STX      0x35
#define CC1101_SIDLE    0x36
#define CC1101_VERSION  0xF1  // status register 0x31 burst
#define CC1101_FREQ2    0x0D
#define CC1101_FREQ1    0x0E
#define CC1101_FREQ0    0x0F
#define CC1101_MDMCFG4  0x10
#define CC1101_MDMCFG3  0x11  // data rate mantissa
#define CC1101_MDMCFG2  0x12
#define CC1101_DEVIATN  0x15  // FSK/GFSK frequency deviation
#define CC1101_PKTCTRL0 0x08
#define CC1101_IOCFG2   0x00
#define CC1101_IOCFG0   0x02

// ── Built-in modulation presets ───────────────────────────────
// mdmcfg2 values: 0x30=ASK/OOK no-sync, 0x02=2-FSK 16-bit-sync,
//                 0x12=GFSK 16-bit-sync, 0x72=4-FSK 16-bit-sync
static const SubGhzPreset BUILTIN_PRESETS[] = {
    // name              mod                      freq    cfg4  cfg3   cfg2  deviatn
    { "ASK_OOK_433",  SubGhzModulation::ASK_OOK, 433.92f, 0x87, 0x83, 0x30, 0x00 },
    { "ASK_OOK_315",  SubGhzModulation::ASK_OOK, 315.00f, 0x87, 0x83, 0x30, 0x00 },
    { "ASK_OOK_868",  SubGhzModulation::ASK_OOK, 868.35f, 0x87, 0x83, 0x30, 0x00 },
    { "FSK2_433",     SubGhzModulation::FSK2,     433.92f, 0x87, 0x83, 0x02, 0x47 },
    { "FSK2_868",     SubGhzModulation::FSK2,     868.35f, 0x87, 0x83, 0x02, 0x47 },
    { "GFSK_433",     SubGhzModulation::GFSK,     433.92f, 0x87, 0x83, 0x12, 0x47 },
    { "GFSK_868",     SubGhzModulation::GFSK,     868.35f, 0x87, 0x83, 0x12, 0x47 },
    { "FSK4_433",     SubGhzModulation::FSK4,     433.92f, 0x87, 0x83, 0x72, 0x47 },
};

static SPIClass* _cc1101Spi = nullptr;

SubGhzModule subGhzModule;

// ─────────────────────────────────────────────────────────────
void SubGhzModule::_spiByte(uint8_t b) {
    (void)_cc1101Spi->transfer(b);
}

void SubGhzModule::_spiWrite(uint8_t addr, uint8_t val) {
    digitalWrite(_cfg.cs, LOW);
    _cc1101Spi->transfer(addr & 0x3F);
    _cc1101Spi->transfer(val);
    digitalWrite(_cfg.cs, HIGH);
}

uint8_t SubGhzModule::_spiRead(uint8_t addr) {
    digitalWrite(_cfg.cs, LOW);
    _cc1101Spi->transfer((addr & 0x3F) | 0x80);
    uint8_t val = _cc1101Spi->transfer(0);
    digitalWrite(_cfg.cs, HIGH);
    return val;
}

uint8_t SubGhzModule::_spiReadStatus(uint8_t addr) {
    digitalWrite(_cfg.cs, LOW);
    _cc1101Spi->transfer((addr & 0x3F) | 0xC0);
    uint8_t val = _cc1101Spi->transfer(0);
    digitalWrite(_cfg.cs, HIGH);
    return val;
}

void SubGhzModule::_spiCommand(uint8_t cmd) {
    digitalWrite(_cfg.cs, LOW);
    _cc1101Spi->transfer(cmd);
    digitalWrite(_cfg.cs, HIGH);
}

void SubGhzModule::_reset() {
    _spiCommand(CC1101_SRES);
    delayMicroseconds(100);
}

bool SubGhzModule::_detectCC1101() {
    _reset();
    uint8_t ver = _spiReadStatus(0x31); // CC1101_VERSION status reg
    Serial.printf(SUBGHZ_TAG " CC1101 version byte: 0x%02X\n", ver);
    return (ver == 0x14 || ver == 0x04);
}

// ─────────────────────────────────────────────────────────────
void SubGhzModule::setEnabled(bool en) {
    SubGhzGpioConfig cfg = loadGpioConfig();
    cfg.enabled = en; saveGpioConfig(cfg);
    _moduleEnabled = en;
    if (!en) {
        // Stop any active capture
        _capturing   = false;
        _hwConnected = false;
        // Put CC1101 to sleep via SPI if still alive
        if (_cc1101Spi) {
            bool vspiDis = (cfg.spiBus != 1);
            if (vspiDis) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
            // SPWD strobe = 0x39 - puts CC1101 in power-down mode
            pinMode(cfg.cs, OUTPUT);
            _cc1101Spi->beginTransaction(SPISettings(4000000,MSBFIRST,SPI_MODE0));
            digitalWrite(cfg.cs, LOW);
            _cc1101Spi->transfer(0x39);  // SPWD
            digitalWrite(cfg.cs, HIGH);
            _cc1101Spi->endTransaction();
            _cc1101Spi->end();
            delete _cc1101Spi;
            _cc1101Spi = nullptr;
            if (vspiDis) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
        }
        // Release GDO pins
        if (cfg.gdo0 > 0) pinMode(cfg.gdo0, INPUT);
        if (cfg.gdo2 > 0) pinMode(cfg.gdo2, INPUT);
        Serial.println("[SUBGHZ] Disabled - CC1101 power-down, SPI released");
    } else {
        // Fresh init - begin() handles _cc1101Spi allocation
        begin();
    }
}

void SubGhzModule::begin() {
    _cfg = loadGpioConfig();
    _moduleEnabled = _cfg.enabled;
    _loadSignals();

    if (_cc1101Spi) { _cc1101Spi->end(); delete _cc1101Spi; _cc1101Spi = nullptr; }
    _hwConnected = false;

    // Lazy init: do a quick SPI probe to check if CC1101 is present.
    // If not found, free the SPIClass immediately — saves ~8KB RAM at boot.
    SPIClass* spiProbe = (_cfg.spiBus == 1)
        ? new SPIClass(HSPI)
        : new SPIClass(VSPI);
    spiProbe->begin(_cfg.sck, _cfg.miso, _cfg.mosi, _cfg.cs);
    spiProbe->setFrequency(4000000);
    spiProbe->setDataMode(SPI_MODE0);
    _cc1101Spi = spiProbe; // _detectCC1101() uses _cc1101Spi internally

    pinMode(_cfg.cs,   OUTPUT); digitalWrite(_cfg.cs,   HIGH);
    pinMode(_cfg.gdo0, INPUT);
    if (_cfg.gdo2 != 0) pinMode(_cfg.gdo2, INPUT);

    _hwConnected = _detectCC1101();

    if (_hwConnected) {
        _setFrequency(433.92f);
        _spiWrite(CC1101_PKTCTRL0, 0x00); // infinite packet length (raw)
        _spiWrite(CC1101_IOCFG0,   0x0D); // GDO0 = carrier sense
        _applyModulation(_modulation);    // sets MDMCFG4/3/2 + DEVIATN
        _setModeIdle();
        Serial.printf(SUBGHZ_TAG " CC1101 connected - GDO0=%u CS=%u mod=%s\n",
                      _cfg.gdo0, _cfg.cs,
                      SubGhzModule::modulationName(_modulation).c_str());
    } else {
        // No CC1101 found — free SPI to reclaim RAM
        _cc1101Spi->end();
        delete _cc1101Spi;
        _cc1101Spi = nullptr;
        Serial.println(SUBGHZ_TAG " CC1101 not detected");
    }
}

// ─────────────────────────────────────────────────────────────
// Modulation helpers
// ─────────────────────────────────────────────────────────────
void SubGhzModule::_applyModulation(SubGhzModulation mod) {
    switch (mod) {
        case SubGhzModulation::ASK_OOK:
            _spiWrite(CC1101_MDMCFG4, 0x87);
            _spiWrite(CC1101_MDMCFG3, 0x83);
            _spiWrite(CC1101_MDMCFG2, 0x30); // ASK/OOK, no preamble/sync
            _spiWrite(CC1101_DEVIATN, 0x00);
            break;
        case SubGhzModulation::FSK2:
            _spiWrite(CC1101_MDMCFG4, 0x87);
            _spiWrite(CC1101_MDMCFG3, 0x83);
            _spiWrite(CC1101_MDMCFG2, 0x02); // 2-FSK, 16-bit sync word
            _spiWrite(CC1101_DEVIATN, 0x47); // ±47.6 kHz deviation
            break;
        case SubGhzModulation::GFSK:
            _spiWrite(CC1101_MDMCFG4, 0x87);
            _spiWrite(CC1101_MDMCFG3, 0x83);
            _spiWrite(CC1101_MDMCFG2, 0x12); // GFSK, 16-bit sync word
            _spiWrite(CC1101_DEVIATN, 0x47);
            break;
        case SubGhzModulation::FSK4:
            _spiWrite(CC1101_MDMCFG4, 0x87);
            _spiWrite(CC1101_MDMCFG3, 0x83);
            _spiWrite(CC1101_MDMCFG2, 0x72); // 4-FSK, 16-bit sync word
            _spiWrite(CC1101_DEVIATN, 0x47);
            break;
    }
}

bool SubGhzModule::setModulation(SubGhzModulation mod) {
    if (!_hwConnected) return false;
    _modulation = mod;
    _setModeIdle();
    _applyModulation(mod);
    Serial.printf(SUBGHZ_TAG " Modulation -> %s\n",
                  modulationName(mod).c_str());
    return true;
}

String SubGhzModule::modulationName(SubGhzModulation m) {
    switch (m) {
        case SubGhzModulation::ASK_OOK: return "ASK/OOK";
        case SubGhzModulation::FSK2:    return "2-FSK";
        case SubGhzModulation::GFSK:    return "GFSK";
        case SubGhzModulation::FSK4:    return "4-FSK";
        default:                        return "UNKNOWN";
    }
}

const SubGhzPreset* SubGhzModule::builtinPresets(uint8_t& count) {
    count = sizeof(BUILTIN_PRESETS) / sizeof(BUILTIN_PRESETS[0]);
    return BUILTIN_PRESETS;
}

void SubGhzModule::reinit(const SubGhzGpioConfig& cfg) {
    saveGpioConfig(cfg);
    _cfg = cfg;
    begin();
}

// ─────────────────────────────────────────────────────────────
void SubGhzModule::_setFrequency(float mhz) {
    // CC1101 supported range: 300-928 MHz. Outside this range the chip
    // enters undefined behavior and could be damaged.
    if (mhz < 300.0f || mhz > 928.0f) {
        Serial.printf(SUBGHZ_TAG " ERROR: freq %.2f MHz out of range (300-928 MHz)\n", mhz);
        return;
    }
    _freqMhz = mhz;
    uint32_t freq = (uint32_t)((mhz * 1000000.0f / 26000000.0f) * 65536.0f);
    _spiWrite(CC1101_FREQ2, (freq >> 16) & 0xFF);
    _spiWrite(CC1101_FREQ1, (freq >>  8) & 0xFF);
    _spiWrite(CC1101_FREQ0,  freq        & 0xFF);
}

void SubGhzModule::_setModeTx()   { _spiCommand(CC1101_STX); }
void SubGhzModule::_setModeRx()   { _spiCommand(CC1101_SRX); }
void SubGhzModule::_setModeIdle() { _spiCommand(CC1101_SIDLE); }

// ─────────────────────────────────────────────────────────────
void SubGhzModule::loop() {
    if (!_hwConnected) return;
    if (_capturing) _captureLoop();

    // ── Brute Force tick ──────────────────────────────────────
    if (_bruteRunning) {
        uint32_t now = millis();
        if (now - _bruteLastMs < _bruteDelayMs) return;
        _bruteLastMs = now;

        if (_bruteCurrent > _bruteEnd) {
            stopBruteForce();
            return;
        }

        // Build OOK raw timing for current code
        uint8_t bits = _bruteBits;
        uint32_t code = _bruteCurrent;

        bool vspi = (_cfg.spiBus != 1);
        if (vspi) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(50));

        _setFrequency(_freqMhz);
        _setModeTx();

        // Preamble
        _spiWrite(0x3E, 0xC0); delayMicroseconds(500);
        _spiWrite(0x3E, 0x00); delayMicroseconds(250);

        // Bits MSB first
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = (code >> b) & 1;
            _spiWrite(0x3E, 0xC0); delayMicroseconds(bit ? 600 : 300);
            _spiWrite(0x3E, 0x00); delayMicroseconds(bit ? 300 : 600);
        }
        _spiWrite(0x3E, 0x00); delayMicroseconds(1000);
        _setModeIdle();

        if (vspi) xSemaphoreGiveRecursive(g_spi_vspi_mutex);

        _bruteCurrent++;
        _bruteSent++;
    }
}

void SubGhzModule::_captureLoop() {
    bool gdo0 = digitalRead(_cfg.gdo0);
    if (gdo0 != _lastGdo0) {
        unsigned long now = micros();
        uint16_t dur = (uint16_t)min((unsigned long)65535UL, now - _lastEdge);
        _captureTimings.push_back(dur);
        _lastGdo0 = gdo0;
        _lastEdge = now;
        if (_captureTimings.size() > 2000) stopCapture();
    }
    // Auto-stop after 2s of silence
    if (_captureTimings.size() > 10 &&
        (micros() - _lastEdge) > 2000000UL) {
        stopCapture();
    }
}

bool SubGhzModule::startCapture(float freqMhz, SubGhzModulation mod) {
    if (!_hwConnected) return false;
    _setModeIdle();
    if (mod != _modulation) {
        _modulation = mod;
        _applyModulation(mod);
    }
    _setFrequency(freqMhz);
    _captureTimings.clear();
    _capBuffer      = "";
    _capturing      = true;
    _captureStart   = millis();
    _lastEdge       = micros();
    _lastGdo0       = digitalRead(_cfg.gdo0);
    _setModeRx();
    Serial.printf(SUBGHZ_TAG " Capture started @ %.2f MHz mod=%s\n",
                  freqMhz, modulationName(mod).c_str());
    return true;
}

void SubGhzModule::stopCapture() {
    _capturing = false;
    _setModeIdle();
    if (!_captureTimings.empty()) {
        _capBuffer = _rawToHex(_captureTimings);
        Serial.printf(SUBGHZ_TAG " Capture done: %u timings\n",
                      (unsigned)_captureTimings.size());

        // Feature 25: auto-save capture to SD
        if (_autoSdSave && sdMgr.isAvailable()) {
            sdMgr.makeDir("/subghz");
            String fname = String((uint32_t)millis()) + "_" + String((int)_freqMhz) + "MHz";
            File f = sdMgr.openForWrite("/subghz/" + fname + ".csv");
            if (f) {
                f.printf("# SubGHz capture %.2f MHz mod=%s\nidx,us\n",
                         _freqMhz, modulationName(_modulation).c_str());
                for (size_t i = 0; i < _captureTimings.size(); i++) {
                    f.printf("%u,%u\n", (unsigned)i, (unsigned)_captureTimings[i]);
                }
                f.close();
                sdMgr.log("[SUBGHZ] Capture saved: " + fname, SdLogLevel::INFO, "SUBGHZ");
            }
        }

        // Feature 28: PCAP-style log append
        if (_sdLog && sdMgr.isAvailable()) {
            sdMgr.makeDir("/subghz");
            String logLine = String((uint32_t)millis()) + "," + String(_freqMhz, 2);
            for (uint16_t t : _captureTimings) {
                logLine += ',';
                logLine += String(t);
            }
            logLine += '\n';
            sdMgr.writeAsync("/subghz/capture.log", logLine, true);
        }
    }
    _captureTimings.clear();
}

String SubGhzModule::pollCaptured() {
    String r = _capBuffer;
    _capBuffer = "";
    return r;
}

String SubGhzModule::_rawToHex(const std::vector<uint16_t>& t) const {
    String s;
    s.reserve(t.size() * 5);
    for (size_t i = 0; i < t.size(); i++) {
        if (i) s += ',';
        s += String(t[i]);
    }
    return s;
}

// ─────────────────────────────────────────────────────────────
bool SubGhzModule::replaySignal(uint32_t id) {
    if (!_hwConnected) return false;
    SubGhzSignal sig;
    if (!getSignal(id, sig)) return false;

    bool vspi = (_cfg.spiBus != 1);
    if (vspi) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));

    _setFrequency(sig.freqMhz);
    _setModeTx();

    // Parse timings from rawData
    std::vector<uint16_t> timings;
    String raw = sig.rawData;
    int pos = 0;
    while (pos < (int)raw.length()) {
        int comma = raw.indexOf(',', pos);
        String tok = (comma < 0) ? raw.substring(pos) : raw.substring(pos, comma);
        timings.push_back((uint16_t)tok.toInt());
        if (comma < 0) break;
        pos = comma + 1;
    }

    // Bit-bang OOK on GDO0 (TX path via CC1101 modulation)
    bool high = true;
    for (uint16_t t : timings) {
        // CC1101 in ASK: set power on/off via PATABLE
        _spiWrite(0x3E, high ? 0xC0 : 0x00); // PATABLE[0]
        delayMicroseconds(t);
        high = !high;
    }
    _setModeIdle();

    if (vspi) xSemaphoreGiveRecursive(g_spi_vspi_mutex);

    Serial.printf(SUBGHZ_TAG " Replayed signal #%u\n", id);
    return true;
}

// ─────────────────────────────────────────────────────────────
String SubGhzModule::statusJson() const {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"frequency\":%.2f,\"mode\":\"%s\",\"modulation\":\"%s\"}",
             _hwConnected?"true":"false",
             _freqMhz,
             _capturing?"RX":"IDLE",
             modulationName(_modulation).c_str());
    return String(buf);
}

// ─────────────────────────────────────────────────────────────
uint32_t SubGhzModule::saveSignal(SubGhzSignal& sig) {
    sig.id = _nextId++;
    _signals.push_back(sig);
    _saveSignals();
    return sig.id;
}

bool SubGhzModule::deleteSignal(uint32_t id) {
    for (auto it = _signals.begin(); it != _signals.end(); ++it) {
        if (it->id == id) { _signals.erase(it); _saveSignals(); return true; }
    }
    return false;
}

bool SubGhzModule::renameSignal(uint32_t id, const String& name) {
    for (auto& s : _signals) {
        if (s.id == id) { s.name = name; _saveSignals(); return true; }
    }
    return false;
}

bool SubGhzModule::getSignal(uint32_t id, SubGhzSignal& out) const {
    for (const auto& s : _signals) {
        if (s.id == id) { out = s; return true; }
    }
    return false;
}

String SubGhzModule::signalsToJson() const {
    String out = "{\"signals\":[";
    for (size_t i = 0; i < _signals.size(); i++) {
        if (i) out += ',';
        const auto& s = _signals[i];
        out += "{\"id\":" + String(s.id)
             + ",\"name\":\"" + s.name
             + "\",\"freqMhz\":" + String(s.freqMhz, 2)
             + ",\"freq\":\"" + String(s.freqMhz, 2)
             + "\",\"protocol\":\"" + s.protocol
             + "\",\"modulation\":\"" + s.protocol
             + "\",\"captured\":\"" + s.captured
             + "\",\"duration\":\"" + s.captured + "\"}";
    }
    out += "]}";
    return out;
}

// ─────────────────────────────────────────────────────────────
void SubGhzModule::_loadSignals() {
    _signals.clear(); _nextId = 1;
    if (!LittleFS.exists(SUBGHZ_SAVE_FILE)) return;
    File f = LittleFS.open(SUBGHZ_SAVE_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        for (JsonObject o : doc["signals"].as<JsonArray>()) {
            SubGhzSignal s;
            s.id       = o["id"]       | (uint32_t)0;
            s.name     = o["name"]     | "";
            s.freqMhz  = o["freqMhz"]  | 433.92f;
            s.protocol = o["protocol"] | "";
            s.rawData  = o["rawData"]  | "";
            s.captured = o["captured"] | "";
            _signals.push_back(s);
            if (s.id >= _nextId) _nextId = s.id + 1;
        }
    }
    f.close();
}

void SubGhzModule::_saveSignals() const {
    File f = LittleFS.open(SUBGHZ_SAVE_FILE, "w");
    if (!f) return;
    f.print("{\"signals\":[");
    bool first = true;
    for (const auto& s : _signals) {
        if (!first) f.print(',');
        first = false;
        JsonDocument doc;
        doc["id"]       = s.id;
        doc["name"]     = s.name;
        doc["freqMhz"]  = s.freqMhz;
        doc["protocol"] = s.protocol;
        doc["rawData"]  = s.rawData;
        doc["captured"] = s.captured;
        serializeJson(doc, f);
    }
    f.print("]}");
    f.close();
}

void SubGhzModule::saveGpioConfig(const SubGhzGpioConfig& cfg) {
    File f = LittleFS.open(SUBGHZ_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["enabled"] = cfg.enabled;
    doc["gdo0"] = cfg.gdo0; doc["gdo2"] = cfg.gdo2;
    doc["cs"]   = cfg.cs;   doc["sck"]  = cfg.sck;
    doc["mosi"] = cfg.mosi; doc["miso"] = cfg.miso;
    doc["spiBus"] = cfg.spiBus;
    serializeJson(doc, f);
    f.close();
}

SubGhzGpioConfig SubGhzModule::loadGpioConfig() const {
    SubGhzGpioConfig cfg;
    if (!LittleFS.exists(SUBGHZ_GPIO_FILE)) return cfg;
    File f = LittleFS.open(SUBGHZ_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.enabled = doc["enabled"] | false;
        cfg.gdo0   = doc["gdo0"]   | (uint8_t)34;
        cfg.gdo2   = doc["gdo2"]   | (uint8_t)35;
        cfg.cs     = doc["cs"]     | (uint8_t)32;
        cfg.sck    = doc["sck"]    | (uint8_t)18;
        cfg.mosi   = doc["mosi"]   | (uint8_t)23;
        cfg.miso   = doc["miso"]   | (uint8_t)19;
        cfg.spiBus = doc["spiBus"] | (uint8_t)0;
    }
    f.close();
    return cfg;
}

// ── SD integration (features 26-27) ─────────────────────────

// Feature 26: backup signal library to SD
bool SubGhzModule::backupSignalsToSD() {
    if (!sdMgr.isAvailable()) return false;
    File src = LittleFS.open(SUBGHZ_SAVE_FILE, "r");
    if (!src) return false;
    sdMgr.makeDir("/subghz");
    File dst = sdMgr.openForWrite("/subghz/signals.json");
    if (!dst) { src.close(); return false; }
    uint8_t buf[256];
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        dst.write(buf, n);
    }
    src.close();
    dst.close();
    sdMgr.log("[SUBGHZ] Signals backed up", SdLogLevel::INFO, "SUBGHZ");
    return true;
}

// Feature 26: restore signal library from SD
bool SubGhzModule::restoreSignalsFromSD() {
    if (!sdMgr.isAvailable()) return false;
    File src = sdMgr.openForRead("/subghz/signals.json");
    if (!src) return false;
    File dst = LittleFS.open(SUBGHZ_SAVE_FILE, "w");
    if (!dst) { src.close(); return false; }
    uint8_t buf[256];
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        dst.write(buf, n);
    }
    src.close();
    dst.close();
    _loadSignals();
    sdMgr.log("[SUBGHZ] Signals restored from SD", SdLogLevel::INFO, "SUBGHZ");
    return true;
}

// Feature 27: replay from SD CSV file
bool SubGhzModule::replayFromSD(const String& filename) {
    if (!sdMgr.isAvailable()) return false;
    if (!_hwConnected) return false;
    String content = sdMgr.readTextFile("/subghz/" + filename);
    if (content.isEmpty()) return false;

    // Parse timings from CSV (skip header lines starting with '#', skip "idx,us" header)
    std::vector<uint16_t> timings;
    float freqFromFile = _freqMhz;

    // Try to extract frequency from filename: "1234567_433MHz.csv"
    int mhzIdx = filename.indexOf("MHz");
    if (mhzIdx > 0) {
        int underIdx = filename.lastIndexOf('_', mhzIdx);
        if (underIdx >= 0) {
            freqFromFile = filename.substring(underIdx + 1, mhzIdx).toFloat();
            if (freqFromFile < 300.0f || freqFromFile > 928.0f) freqFromFile = _freqMhz;
        }
    }

    // Parse CSV rows
    int pos = 0;
    while (pos < (int)content.length()) {
        int eol = content.indexOf('\n', pos);
        String line = (eol < 0) ? content.substring(pos) : content.substring(pos, eol);
        line.trim();
        if (eol < 0) pos = content.length();
        else         pos = eol + 1;

        if (line.startsWith("#")) continue;
        if (line.startsWith("idx")) continue;  // header row

        // Format: "idx,us"
        int comma = line.indexOf(',');
        if (comma < 0) continue;
        String usStr = line.substring(comma + 1);
        uint16_t us = (uint16_t)usStr.toInt();
        if (us > 0) timings.push_back(us);
    }

    if (timings.empty()) return false;

    // Replay timings using CC1101
    bool vspi = (_cfg.spiBus != 1);
    if (vspi) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));

    _setFrequency(freqFromFile);
    _setModeTx();
    bool high = true;
    for (uint16_t t : timings) {
        _spiWrite(0x3E, high ? 0xC0 : 0x00); // PATABLE[0]
        delayMicroseconds(t);
        high = !high;
    }
    _setModeIdle();

    if (vspi) xSemaphoreGiveRecursive(g_spi_vspi_mutex);

    Serial.printf(SUBGHZ_TAG " Replayed from SD: %s (%u timings)\n",
                  filename.c_str(), (unsigned)timings.size());
    return true;
}

std::vector<String> SubGhzModule::listSdSignals() const {
    std::vector<String> result;
    if (!sdMgr.isAvailable()) return result;
    auto entries = sdMgr.listDir("/subghz");
    for (const auto& e : entries) {
        if (!e.isDir && (e.name.endsWith(".csv") || e.name.endsWith(".json")))
            result.push_back(e.name);
    }
    return result;
}

// ═════════════════════════════════════════════════════════════
//  BRUTE FORCE  (Bruce port)
// ═════════════════════════════════════════════════════════════
bool SubGhzModule::startBruteForce(float freqMhz, const String& mod,
                                    uint32_t startCode, uint32_t endCode,
                                    uint8_t bits, uint32_t delayMs) {
    if (!_hwConnected) { Serial.println(SUBGHZ_TAG " BF: no HW"); return false; }
    if (_bruteRunning) stopBruteForce();

    // Map modulation string
    SubGhzModulation m = SubGhzModulation::ASK_OOK;
    if (mod == "AM650")  m = SubGhzModulation::ASK_OOK;
    else if (mod == "FM238") m = SubGhzModulation::FSK2;
    else if (mod == "FM476") m = SubGhzModulation::GFSK;
    setModulation(m);

    _freqMhz       = freqMhz;
    _bruteCurrent  = startCode;
    _bruteEnd      = endCode;
    _bruteBits     = bits < 8 ? 8 : (bits > 64 ? 64 : bits);
    _bruteDelayMs  = delayMs < 10 ? 10 : delayMs;
    _bruteSent     = 0;
    _bruteTotal    = endCode - startCode + 1;
    _bruteRunning  = true;
    _bruteLastMs   = 0;

    Serial.printf(SUBGHZ_TAG " BF start: %.2fMHz mod=%s codes=0x%06lX-0x%06lX bits=%u\n",
                  freqMhz, mod.c_str(), startCode, endCode, bits);
    return true;
}

void SubGhzModule::stopBruteForce() {
    _bruteRunning = false;
    _setModeIdle();
    Serial.printf(SUBGHZ_TAG " BF stopped sent=%lu\n", _bruteSent);
}

String SubGhzModule::bruteStatusJson() const {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"running\":%s,\"current\":%lu,\"sent\":%lu,\"total\":%lu}",
        _bruteRunning ? "true" : "false",
        _bruteCurrent, _bruteSent, _bruteTotal);
    return String(buf);
}
