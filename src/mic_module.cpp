// ============================================================
//  mic_module.cpp  -  Hybrid I2S + ADC Mic Module
// ============================================================
#include "mic_module.h"
#include "sd_manager.h"
#include <LittleFS.h>

MicModule micModule;

// ── Reserved pins - must not conflict ────────────────────────
static const uint8_t RESERVED[] = {
    1,3,4,6,7,8,9,10,11,12,14,15,18,19,20,23,24,27
};
// Valid ADC1 pins (work with WiFi ON)
static const uint8_t ADC1_PINS[] = {32,33,34,35,36,39};

// ── WAV header builder ────────────────────────────────────────
static void buildWavHdr(uint8_t* h, uint32_t dataBytes,
                        uint32_t sr, uint16_t ch) {
    uint32_t byteRate   = sr * ch * 2;
    uint16_t blockAlign = ch * 2;
    uint32_t chunkSize  = 36 + dataBytes;
    memcpy(h,    "RIFF", 4); memcpy(h+4,  &chunkSize,  4);
    memcpy(h+8,  "WAVE", 4); memcpy(h+12, "fmt ",      4);
    uint32_t f=16; memcpy(h+16,&f,4);
    uint16_t pcm=1; memcpy(h+20,&pcm,2); memcpy(h+22,&ch,2);
    memcpy(h+24,&sr,4); memcpy(h+28,&byteRate,4);
    memcpy(h+30,&blockAlign,2);
    uint16_t b=16; memcpy(h+32,&b,2);
    memcpy(h+36,"data",4); memcpy(h+40,&dataBytes,4);
}

// ── Pin helpers ───────────────────────────────────────────────
bool MicModule::_pinConflict(uint8_t pin) const {
    for (auto r : RESERVED) if (r == pin) return true;
    return false;
}
bool MicModule::_validAdcPin(uint8_t pin) const {
    for (auto p : ADC1_PINS) if (p == pin) return true;
    return false;
}

// ── I2S init/deinit ───────────────────────────────────────────
bool MicModule::_initI2S() {
    _deinitI2S();
    if (!_cfg.i2sEnabled) return false;

    i2s_config_t c = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX),
        .sample_rate          = _cfg.sampleRate,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = MIC_DMA_BUF_COUNT,
        .dma_buf_len          = MIC_DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0,
    };
    i2s_pin_config_t p = {
        .bck_io_num   = _cfg.sck,
        .ws_io_num    = _cfg.ws,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = _cfg.sd,
    };
    if (i2s_driver_install(MIC_I2S_PORT, &c, 0, NULL) != ESP_OK) {
        Serial.println("[MIC] I2S install failed");
        return false;
    }
    if (i2s_set_pin(MIC_I2S_PORT, &p) != ESP_OK) {
        Serial.println("[MIC] I2S pin failed");
        i2s_driver_uninstall(MIC_I2S_PORT);
        return false;
    }
    i2s_zero_dma_buffer(MIC_I2S_PORT);
    _i2sInited = true;
    Serial.printf("[MIC] I2S OK  WS=%u SCK=%u SD=%u %s\n",
        _cfg.ws, _cfg.sck, _cfg.sd, _cfg.stereo?"STEREO":"MONO");
    return true;
}

void MicModule::_deinitI2S() {
    if (_i2sInited) { i2s_driver_uninstall(MIC_I2S_PORT); _i2sInited = false; }
}

// ── ADC init/deinit ───────────────────────────────────────────
bool MicModule::_initADC() {
    _deinitADC();
    _adcCount = 0;
    for (uint8_t i = 0; i < MIC_ADC_MAX; i++) {
        const auto& a = _cfg.adc[i];
        if (!a.enabled || !_validAdcPin(a.pin)) continue;
        // Configure pin as ADC input
        pinMode(a.pin, INPUT);
        analogSetPinAttenuation(a.pin, ADC_11db); // 0-3.3V range
        _adcPins[_adcCount]  = a.pin;
        _adcGains[_adcCount] = a.gain;
        _adcCount++;
        Serial.printf("[MIC] ADC mic #%u on GPIO %u gain=%u\n",
                      _adcCount, a.pin, a.gain);
    }
    if (_adcCount == 0) return false;
    // Set ADC resolution 12-bit
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    _adcInited = true;
    Serial.printf("[MIC] ADC OK  %u mic(s)\n", _adcCount);
    return true;
}

void MicModule::_deinitADC() {
    _adcCount  = 0;
    _adcInited = false;
}

// ── Active mode resolver ──────────────────────────────────────
MicMixMode MicModule::_activeMode() const {
    switch (_cfg.mixMode) {
        case MicMixMode::I2S_ONLY: return MicMixMode::I2S_ONLY;
        case MicMixMode::ADC_ONLY: return MicMixMode::ADC_ONLY;
        case MicMixMode::HYBRID:
            // Hybrid needs both - fallback gracefully
            if (_i2sInited && _adcInited) return MicMixMode::HYBRID;
            if (_i2sInited)  return MicMixMode::I2S_ONLY;
            if (_adcInited)  return MicMixMode::ADC_ONLY;
            return MicMixMode::AUTO;
        case MicMixMode::AUTO:
        default:
            if (_i2sInited)  return MicMixMode::I2S_ONLY;
            if (_adcInited)  return MicMixMode::ADC_ONLY;
            return MicMixMode::AUTO;
    }
}

String MicModule::activeSourceStr() const {
    switch (_activeMode()) {
        case MicMixMode::I2S_ONLY: return "I2S";
        case MicMixMode::ADC_ONLY: return "ADC";
        case MicMixMode::HYBRID:   return "HYBRID";
        default:                   return "NONE";
    }
}

// ── Gain helper ───────────────────────────────────────────────
int16_t MicModule::_applyGain(int32_t raw, uint8_t gain) const {
    if (gain == 0) return (int16_t)constrain(raw, -32768, 32767);
    int32_t g = raw * (int32_t)(1 + gain);
    return (int16_t)constrain(g, -32768L, 32767L);
}

// ── I2S read -> PCM int16 ─────────────────────────────────────
// MUTEX FIX: lazy-create a RECURSIVE mutex and serialise access to the
// static `raw` buffer below. Two callers (WS stream task via readChunk
// and the recording-loop tick) can otherwise tear it. Recursive type
// because _readHybrid takes the same mutex and then calls _readI2S.
size_t MicModule::_readI2S(int16_t* pcm, size_t maxSamples) {
    static int32_t raw[MIC_DMA_BUF_LEN * 2];
    if (!_readMutex) _readMutex = xSemaphoreCreateRecursiveMutex();
    if (_readMutex &&
        xSemaphoreTakeRecursive(_readMutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return 0;
    size_t bytesRead = 0;
    if (i2s_read(MIC_I2S_PORT, raw, sizeof(raw),
                 &bytesRead, pdMS_TO_TICKS(8)) != ESP_OK) {
        if (_readMutex) xSemaphoreGiveRecursive(_readMutex);
        return 0;
    }

    size_t n = bytesRead / sizeof(int32_t);
    size_t out = 0;
    int32_t peak = 0;

    for (size_t i = 0; i+1 < n && out+(_cfg.stereo?2:1) <= maxSamples; i+=2) {
        int16_t L = _applyGain(raw[i]   >> 11, _cfg.masterGain);
        int16_t R = _applyGain(raw[i+1] >> 11, _cfg.masterGain);
        if (_cfg.stereo) {
            pcm[out++] = L;
            pcm[out++] = R;
        } else {
            pcm[out++] = (int16_t)(((int32_t)L+R)/2);
        }
        if (abs((int32_t)L) > peak) peak = abs((int32_t)L);
    }
    _volume = (uint8_t)(peak * 100 / 32768);
    if (_readMutex) xSemaphoreGiveRecursive(_readMutex);
    return out;
}

// ── ADC read -> PCM int16 ─────────────────────────────────────
// ADC gives 0-4095 (12-bit). Center=2048, convert to -32768..32767
// MUTEX FIX: take the same recursive mutex as _readI2S so concurrent
// callers (streamer task + record-loop) cannot collide on _adcPins /
// _adcGains / _volume member state.
size_t MicModule::_readADC(int16_t* pcm, size_t maxSamples) {
    if (!_adcInited || _adcCount == 0) return 0;
    if (!_readMutex) _readMutex = xSemaphoreCreateRecursiveMutex();
    if (_readMutex &&
        xSemaphoreTakeRecursive(_readMutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return 0;

    // How many samples per mic to fill maxSamples
    // FIX: ternary was inverted - when _adcCount>0 we divided by 1 (no division
    // at all) instead of by _adcCount. Result: _adcCount=2 made sampPerMic =
    // maxSamples (not maxSamples/2), and inner mixing loop wrote the same slot
    // twice. _adcCount==0 path is already guarded above so the protection is
    // redundant, but keep min(1) for safety.
    size_t sampPerMic = maxSamples / (_adcCount > 0 ? _adcCount : 1);
    if (sampPerMic > 128) sampPerMic = 128; // keep loop fast

    // Delay between samples to approximate sample rate
    // At 16kHz, 1 sample = 62.5us
    size_t out = 0;
    int32_t peak = 0;

    for (size_t s = 0; s < sampPerMic && out < maxSamples; s++) {
        // Read all ADC mics and mix to mono
        // Note: analogRead takes ~10us on ESP32 - at 16kHz we need 62us/sample
        // Multiple mics fill the timing gap naturally - no extra delay needed
        int32_t mixed = 0;
        for (uint8_t m = 0; m < _adcCount; m++) {
            int32_t raw     = analogRead(_adcPins[m]); // 0-4095, ~10us each
            int32_t centered = raw - 2048;              // -2048..2047
            int32_t scaled   = centered * 16;           // -32768..32752
            mixed += _applyGain(scaled, _adcGains[m]);
        }
        mixed /= (_adcCount > 0 ? _adcCount : 1);
        mixed  = _applyGain(mixed, _cfg.masterGain);
        int16_t s16 = (int16_t)constrain(mixed, -32768, 32767);
        pcm[out++] = s16;
        if (abs((int32_t)s16) > peak) peak = abs((int32_t)s16);
        // Yield every 32 samples to prevent watchdog issues
        if ((s & 31) == 31) vTaskDelay(1);
    }
    _volume = (uint8_t)(peak * 100 / 32768);
    if (_readMutex) xSemaphoreGiveRecursive(_readMutex);
    return out;
}

// ── Hybrid read - blend I2S + ADC ────────────────────────────
// MUTEX FIX: i2sBuf / adcBuf are shared static buffers. Two concurrent
// calls would have the second's _readI2S overwrite the first's data
// (each _readI2S already locks but passes the same static i2sBuf
// pointer). Serialise the whole function with the same mutex.
size_t MicModule::_readHybrid(int16_t* pcm, size_t maxSamples) {
    static int16_t i2sBuf[MIC_DMA_BUF_LEN * 2];
    static int16_t adcBuf[MIC_DMA_BUF_LEN * 2];

    if (!_readMutex) _readMutex = xSemaphoreCreateRecursiveMutex();
    if (_readMutex &&
        xSemaphoreTakeRecursive(_readMutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return 0;

    size_t i2sN = _readI2S(i2sBuf, maxSamples);
    size_t adcN = _readADC(adcBuf, maxSamples);
    size_t n    = min(i2sN, adcN);
    if (n == 0) n = max(i2sN, adcN);

    uint8_t  wi  = _cfg.i2sWeight;              // 0-100
    uint8_t  wa  = 100 - wi;

    for (size_t i = 0; i < n && i < maxSamples; i++) {
        int32_t iv = (i < i2sN) ? i2sBuf[i] : 0;
        int32_t av = (i < adcN) ? adcBuf[i]  : 0;
        int32_t blended = (iv * wi + av * wa) / 100;
        pcm[i] = (int16_t)constrain(blended, -32768, 32767);
    }
    if (_readMutex) xSemaphoreGiveRecursive(_readMutex);
    return n;
}

// ── readChunk - called by WS stream task ─────────────────────
size_t MicModule::readChunk(uint8_t* outBuf, size_t maxLen) {
    if (!_streaming) return 0;
    size_t maxSamples = maxLen / sizeof(int16_t);
    int16_t* pcm = (int16_t*)outBuf;
    size_t n = 0;

    switch (_activeMode()) {
        case MicMixMode::I2S_ONLY: n = _readI2S(pcm, maxSamples);    break;
        case MicMixMode::ADC_ONLY: n = _readADC(pcm, maxSamples);    break;
        case MicMixMode::HYBRID:   n = _readHybrid(pcm, maxSamples); break;
        default: return 0;
    }
    return n * sizeof(int16_t);
}

// ── begin ─────────────────────────────────────────────────────
void MicModule::begin() {
    _cfg = loadConfig();
    if (!LittleFS.exists(MIC_REC_DIR)) LittleFS.mkdir(MIC_REC_DIR);

    bool anyOk = false;
    if (_cfg.i2sEnabled) anyOk |= _initI2S();
    // count enabled ADC mics
    for (auto& a : _cfg.adc) if (a.enabled) { anyOk |= _initADC(); break; }

    Serial.printf("[MIC] begin - source=%s\n", activeSourceStr().c_str());
}

// ── loop - recording tick ─────────────────────────────────────
void MicModule::loop() {
    if (!_recording) return;

    uint32_t elapsed = (millis() - _recStartMs) / 1000;
    if (elapsed >= MIC_MAX_REC_SECS) { stopRecording(); return; }

    static int16_t pcm[MIC_DMA_BUF_LEN * 2];  // static: avoids 1KB stack alloc
    size_t  n = 0;

    switch (_activeMode()) {
        case MicMixMode::I2S_ONLY: n = _readI2S(pcm, MIC_DMA_BUF_LEN*2);    break;
        case MicMixMode::ADC_ONLY: n = _readADC(pcm, MIC_DMA_BUF_LEN*2);    break;
        case MicMixMode::HYBRID:   n = _readHybrid(pcm, MIC_DMA_BUF_LEN*2); break;
        default: return;
    }
    if (n == 0 || !_recFile) return;

    _recFile.write((uint8_t*)pcm, n * sizeof(int16_t));
    _recBytes += n * sizeof(int16_t);
    if (_recOnSd && (_recBytes % 4096) < (n * sizeof(int16_t)))
        _recFile.flush();
}

// ── Stream control ────────────────────────────────────────────
bool MicModule::startStream() {
    if (_activeMode() == MicMixMode::AUTO) {
        Serial.println("[MIC] No source - configure I2S or ADC first");
        return false;
    }
    if (_i2sInited) i2s_zero_dma_buffer(MIC_I2S_PORT);
    _streaming = true;
    Serial.printf("[MIC] Stream start (%s)\n", activeSourceStr().c_str());
    return true;
}
void MicModule::stopStream() { _streaming = false; }

// ── Speaker output (A2DP sink playback) ──────────────────────
void MicModule::playSamples(const int16_t* samples, size_t count) {
    if (!_i2sInited || !samples || count == 0) return;
    _speakerActive = true;
    size_t written = 0;
    i2s_write(MIC_I2S_PORT, samples, count * sizeof(int16_t), &written, pdMS_TO_TICKS(20));
}

// ── Recording ─────────────────────────────────────────────────
bool MicModule::startRecording(const String& filename) {
    if (_activeMode() == MicMixMode::AUTO) return false;

    String base = filename.isEmpty()
        ? ("rec_" + String(millis()/1000)) : filename;
    int sl = base.lastIndexOf('/');
    if (sl >= 0) base = base.substring(sl+1);
    if (!base.endsWith(".wav")) base += ".wav";

    _recOnSd = false;

    // Try SD first
    if (sdMgr.isAvailable()) {
        if (!sdMgr.exists("/recordings")) sdMgr.makeDir("/recordings");
        String p = "/recordings/" + base;
        _recFile = sdMgr.openForWrite(p);
        if (_recFile) { _recFilename = p; _recOnSd = true; }
    }
    // LittleFS fallback
    if (!_recFile) {
        if (!LittleFS.exists(MIC_REC_DIR)) LittleFS.mkdir(MIC_REC_DIR);
        String p = String(MIC_REC_DIR) + "/" + base;
        _recFile = LittleFS.open(p, "w");
        if (!_recFile) { Serial.println("[MIC] Cannot open rec file"); return false; }
        _recFilename = p;
    }

    uint8_t hdr[WAV_HEADER_SIZE] = {};
    uint16_t ch = (_cfg.stereo && _activeMode()==MicMixMode::I2S_ONLY) ? 2 : 1;
    buildWavHdr(hdr, 0, _cfg.sampleRate, ch);
    _recFile.write(hdr, WAV_HEADER_SIZE);

    _recBytes = 0; _recStartMs = millis(); _recording = true;
    Serial.printf("[MIC] Rec -> %s (%s)\n",
        _recFilename.c_str(), _recOnSd?"SD":"LittleFS");
    return true;
}

void MicModule::_updateWavHeader() {
    if (!_recFile) return;
    _recFile.seek(0);
    uint8_t hdr[WAV_HEADER_SIZE];
    uint16_t ch = (_cfg.stereo && _activeMode()==MicMixMode::I2S_ONLY) ? 2 : 1;
    buildWavHdr(hdr, _recBytes, _cfg.sampleRate, ch);
    _recFile.write(hdr, WAV_HEADER_SIZE);
}

void MicModule::stopRecording() {
    if (!_recording) return;
    _recording = false;
    if (_recFile) { _updateWavHeader(); _recFile.close(); }
    Serial.printf("[MIC] Rec stop - %uKB - %s\n",
        _recBytes/1024, _recFilename.c_str());
}

uint32_t MicModule::recordingSecs() const {
    if (!_recording) return 0;
    return (millis() - _recStartMs) / 1000;
}

// ── applyConfig ───────────────────────────────────────────────
bool MicModule::applyConfig(const MicConfig& cfg) {
    if (_recording) stopRecording();
    if (_streaming) stopStream();
    _cfg = cfg;
    saveConfig(cfg);

    bool ok = false;
    _deinitI2S();
    _deinitADC();
    if (cfg.i2sEnabled) ok |= _initI2S();
    for (auto& a : cfg.adc) if (a.enabled) { ok |= _initADC(); break; }
    Serial.printf("[MIC] Config applied - source=%s\n",
                  activeSourceStr().c_str());
    return ok || (!cfg.i2sEnabled && _adcCount==0); // no error if both disabled
}

// ── Save / Load config ────────────────────────────────────────
void MicModule::saveConfig(const MicConfig& cfg) {
    File f = LittleFS.open(MIC_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["i2sEnabled"]  = cfg.i2sEnabled;
    doc["ws"]          = cfg.ws;
    doc["sck"]         = cfg.sck;
    doc["sd"]          = cfg.sd;
    doc["stereo"]      = cfg.stereo;
    doc["sampleRate"]  = cfg.sampleRate;
    doc["mixMode"]     = (uint8_t)cfg.mixMode;
    doc["i2sWeight"]   = cfg.i2sWeight;
    doc["masterGain"]  = cfg.masterGain;
    JsonArray arr = doc["adc"].to<JsonArray>();
    for (auto& a : cfg.adc) {
        JsonObject o = arr.add<JsonObject>();
        o["pin"]     = a.pin;
        o["gain"]    = a.gain;
        o["enabled"] = a.enabled;
    }
    serializeJson(doc, f);
    f.close();
}

MicConfig MicModule::loadConfig() const {
    MicConfig cfg;
    if (!LittleFS.exists(MIC_GPIO_FILE)) return cfg;
    File f = LittleFS.open(MIC_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.i2sEnabled  = doc["i2sEnabled"] | false;
        cfg.ws          = doc["ws"]         | (uint8_t)25;
        cfg.sck         = doc["sck"]        | (uint8_t)26;
        cfg.sd          = doc["sd"]         | (uint8_t)33;
        cfg.stereo      = doc["stereo"]     | false;
        cfg.sampleRate  = doc["sampleRate"] | (uint32_t)MIC_SAMPLE_RATE;
        cfg.mixMode     = (MicMixMode)(doc["mixMode"] | (uint8_t)0);
        cfg.i2sWeight   = doc["i2sWeight"]  | (uint8_t)60;
        cfg.masterGain  = doc["masterGain"] | (uint8_t)6;
        JsonArray arr = doc["adc"].as<JsonArray>();
        for (uint8_t i = 0; i < MIC_ADC_MAX && i < arr.size(); i++) {
            cfg.adc[i].pin     = arr[i]["pin"]     | (uint8_t)0;
            cfg.adc[i].gain    = arr[i]["gain"]    | (uint8_t)6;
            cfg.adc[i].enabled = arr[i]["enabled"] | false;
        }
    }
    f.close();
    return cfg;
}

// ── Status / GPIO JSON ────────────────────────────────────────
String MicModule::statusJson() const {
    JsonDocument doc;
    doc["i2sActive"]   = _i2sInited;
    doc["adcCount"]    = _adcCount;
    doc["source"]      = activeSourceStr();
    doc["streaming"]   = _streaming;
    doc["recording"]   = _recording;
    doc["recSecs"]     = recordingSecs();
    doc["recName"]     = _recFilename;
    doc["recOnSd"]     = _recOnSd;
    doc["recBytes"]    = _recBytes;
    doc["volume"]      = _volume;
    doc["sampleRate"]  = _cfg.sampleRate;
    doc["mixMode"]     = (uint8_t)_cfg.mixMode;
    doc["i2sWeight"]   = _cfg.i2sWeight;
    String out; serializeJson(doc, out);
    return out;
}

String MicModule::gpioJson() const {
    JsonDocument doc;
    doc["i2sEnabled"]  = _cfg.i2sEnabled;
    doc["ws"]          = _cfg.ws;
    doc["sck"]         = _cfg.sck;
    doc["sd"]          = _cfg.sd;
    doc["stereo"]      = _cfg.stereo;
    doc["sampleRate"]  = _cfg.sampleRate;
    doc["mixMode"]     = (uint8_t)_cfg.mixMode;
    doc["i2sWeight"]   = _cfg.i2sWeight;
    doc["masterGain"]  = _cfg.masterGain;
    JsonArray arr = doc["adc"].to<JsonArray>();
    for (auto& a : _cfg.adc) {
        JsonObject o = arr.add<JsonObject>();
        o["pin"] = a.pin; o["gain"] = a.gain; o["enabled"] = a.enabled;
    }
    String out; serializeJson(doc, out);
    return out;
}

// ── Recordings list ───────────────────────────────────────────
String MicModule::listRecordingsJson() const {
    JsonDocument doc;
    JsonArray arr = doc["files"].to<JsonArray>();
    uint16_t ch = (_cfg.stereo && _i2sInited) ? 2 : 1;

    if (sdMgr.isAvailable()) {
        for (auto& e : sdMgr.listDir("/recordings")) {
            if (e.isDir) continue;
            String n = e.name;
            if (!n.endsWith(".wav") && !n.endsWith(".WAV")) continue;
            JsonObject o = arr.add<JsonObject>();
            o["name"]  = n; o["size"] = (uint32_t)e.size; o["onSd"] = true;
            o["secs"]  = (uint32_t)(e.size > WAV_HEADER_SIZE
                ? (e.size - WAV_HEADER_SIZE) / (_cfg.sampleRate * ch * 2) : 0);
        }
    }
    File dir = LittleFS.open(MIC_REC_DIR);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String n = String(f.name());
                if (n.endsWith(".wav") || n.endsWith(".WAV")) {
                    JsonObject o = arr.add<JsonObject>();
                    o["name"]  = n; o["size"] = (uint32_t)f.size(); o["onSd"] = false;
                    o["secs"]  = (uint32_t)(f.size() > WAV_HEADER_SIZE
                        ? (f.size()-WAV_HEADER_SIZE)/(_cfg.sampleRate*ch*2) : 0);
                }
            }
            f = dir.openNextFile();
        }
    }
    doc["recording"]   = _recording;
    doc["streaming"]   = _streaming;
    doc["recName"]     = _recFilename;
    doc["recOnSd"]     = _recOnSd;
    doc["recSecs"]     = recordingSecs();
    doc["volume"]      = _volume;
    doc["sdAvailable"] = sdMgr.isAvailable();
    doc["source"]      = activeSourceStr();
    String out; serializeJson(doc, out);
    return out;
}

bool MicModule::deleteRecording(const String& name) {
    String clean = name; int sl = clean.lastIndexOf('/');
    if (sl>=0) clean = clean.substring(sl+1);
    String sdp = "/recordings/" + clean;
    if (sdMgr.isAvailable() && sdMgr.exists(sdp)) return sdMgr.deleteFile(sdp);
    return LittleFS.remove(String(MIC_REC_DIR)+"/"+clean);
}
