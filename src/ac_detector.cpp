// ============================================================
//  ac_detector.cpp  —  Non-Contact AC Power Detector v1.0
//
//  Method: floating ADC pin as EMF antenna (Theory to Thing)
//  No external components needed — just a jumper wire.
//
//  RMS Algorithm:
//    1. Collect AC_SAMPLE_COUNT ADC readings
//    2. Remove DC offset (subtract mean)
//    3. Compute sqrt(mean(x^2)) = RMS
//    4. Compare with threshold + hysteresis
// ============================================================
#include "ac_detector.h"
#include "audit_manager.h"
#include "wifi_manager.h"
#include "gpio_config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <math.h>

// Pins known to be used by other modules — conflict check
// (IR TX/RX, SD, NRF24, SubGHz, NFC, RFID defaults)
static const uint8_t KNOWN_USED_PINS[] = {
    14,  // IR RX default
    27, 26, 25, 33, 32, 17, 16, 4,  // IR TX defaults
    18, 19, 23,  // SD SPI (SCK, MISO, MOSI)
    1, 3         // UART
};
static const uint8_t KNOWN_USED_COUNT =
    sizeof(KNOWN_USED_PINS) / sizeof(KNOWN_USED_PINS[0]);

AcDetector acDetector;

// ─────────────────────────────────────────────────────────────
AcDetector::AcDetector()
    : _lastSampleMs(0),
      _lastBuzzerMs(0),
      _sampleIdx(0),
      _bufFull(false),
      _buzzerOn(false)
{
    memset(_sampleBuf, 0, sizeof(_sampleBuf));
    _status = {};
}

// ─────────────────────────────────────────────────────────────
void AcDetector::begin() {
    loadConfig();
    if (_cfg.enabled) _applyPin();
    Serial.printf("[AC] Detector v1.0 — pin=GPIO%u  threshold=%u  %s\n",
                  _cfg.pin, _cfg.threshold,
                  _cfg.enabled ? "ENABLED" : "disabled");
}

// ─────────────────────────────────────────────────────────────
void AcDetector::loop() {
    if (!_cfg.enabled) return;

    uint32_t now = millis();

    // ── Sample ADC ───────────────────────────────────────────
    if (now - _lastSampleMs >= AC_SAMPLE_INTERVAL_MS) {
        _lastSampleMs = now;

        // analogRead on input-only pin — no pinMode needed
        int raw = analogRead(_cfg.pin);   // 0–4095 (12-bit)
        _sampleBuf[_sampleIdx] = raw;
        _sampleIdx = (_sampleIdx + 1) % AC_SAMPLE_COUNT;
        if (_sampleIdx == 0) _bufFull = true;
    }

    // ── Update RMS once buffer has enough samples ─────────────
    if (!_bufFull && _sampleIdx < AC_SAMPLE_COUNT / 2) return;

    float rms = _calcRms();
    _status.rms        = rms;
    _status.rmsPercent = (rms / 4095.0f) * 100.0f;
    _status.signalBars = (uint8_t)min(5.0f, rms / (4095.0f / 5.0f) * 5.0f);

    // ── Threshold detection with hysteresis ──────────────────
    bool wasDetected = _status.acDetected;

    if (!_status.acDetected && rms >= _cfg.threshold) {
        // Rising edge — AC detected
        _status.acDetected = true;
        _status.detectedAt = now;
        _status.detectionCount++;
        Serial.printf("[AC] DETECTED — RMS=%.1f  pin=GPIO%u\n",
                      rms, _cfg.pin);
        auditMgr.log(AuditSource::SYSTEM, "AC_DETECTED",
                     String("RMS=") + String(rms, 1) +
                     " GPIO=" + _cfg.pin);
        if (onAcDetected) onAcDetected();

    } else if (_status.acDetected &&
               rms < (_cfg.threshold - _cfg.hysteresis)) {
        // Falling edge — AC lost (hysteresis prevents flip-flop)
        _status.acDetected = false;
        _status.lostAt     = now;
        Serial.printf("[AC] LOST — RMS=%.1f  pin=GPIO%u\n",
                      rms, _cfg.pin);
        auditMgr.log(AuditSource::SYSTEM, "AC_LOST",
                     String("RMS=") + String(rms, 1) +
                     " GPIO=" + _cfg.pin);
        if (onAcLost) onAcLost();
    }

    // ── Buzzer ───────────────────────────────────────────────
    if (_cfg.buzzerEnabled && _cfg.buzzerPin > 0) {
        _updateBuzzer(rms);
    }
}

// ─────────────────────────────────────────────────────────────
//  RMS Calculation
//  1. Compute mean (DC offset)
//  2. Subtract mean from each sample (AC coupling)
//  3. RMS of AC component only
// ─────────────────────────────────────────────────────────────
float AcDetector::_calcRms() const {
    uint8_t count = _bufFull ? AC_SAMPLE_COUNT : _sampleIdx;
    if (count == 0) return 0.0f;

    // Step 1: mean (DC offset removal)
    int64_t sum = 0;
    for (uint8_t i = 0; i < count; i++) sum += _sampleBuf[i];
    float mean = (float)sum / count;

    // Step 2 + 3: RMS of AC component
    float sumSq = 0.0f;
    for (uint8_t i = 0; i < count; i++) {
        float ac = _sampleBuf[i] - mean;
        sumSq += ac * ac;
    }
    return sqrtf(sumSq / count);
}

// ─────────────────────────────────────────────────────────────
//  Buzzer — beep frequency proportional to RMS (proximity)
//  Near wire  → fast beep
//  Far away   → slow beep / silent
// ─────────────────────────────────────────────────────────────
void AcDetector::_updateBuzzer(float rms) {
    if (_cfg.buzzerPin == 0) return;
    uint32_t now = millis();

    // Map RMS to beep interval: high RMS = short interval
    // threshold..4095 → 50ms..500ms interval
    uint32_t interval = 500;
    if (rms >= _cfg.threshold) {
        float factor = (rms - _cfg.threshold) / (4095.0f - _cfg.threshold);
        factor = min(1.0f, factor);
        interval = (uint32_t)(500 - factor * 450);  // 500ms → 50ms
    }

    // Non-blocking buzzer LOW after 10ms pulse
    if (_buzzerOn && (millis() - _buzzerHighMs >= 10)) {
        digitalWrite(_cfg.buzzerPin, LOW);
        _buzzerOn = false;
    }

    if (now - _lastBuzzerMs >= interval) {
        _lastBuzzerMs = now;
        if (rms >= _cfg.threshold) {
            // Short beep - set HIGH now, LOW will be applied non-blockingly above
            digitalWrite(_cfg.buzzerPin, HIGH);
            _buzzerHighMs = millis();
            _buzzerOn = true;
        }
    }
}

// ─────────────────────────────────────────────────────────────
void AcDetector::_applyPin() {
    // ADC input-only pins need no pinMode — just analogRead
    // But set attenuation for full 0-3.3V range (0-4095)
    analogSetPinAttenuation(_cfg.pin, ADC_11db);
    Serial.printf("[AC] ADC pin GPIO%u ready (11dB atten, 0-3.3V)\n",
                  _cfg.pin);

    if (_cfg.buzzerEnabled && _cfg.buzzerPin > 0) {
        pinMode(_cfg.buzzerPin, OUTPUT);
        digitalWrite(_cfg.buzzerPin, LOW);
    }
}

// ─────────────────────────────────────────────────────────────
void AcDetector::enable(bool on) {
    _cfg.enabled = on;
    if (on) {
        _applyPin();
        // Reset sample buffer
        memset(_sampleBuf, 0, sizeof(_sampleBuf));
        _sampleIdx = 0;
        _bufFull   = false;
        _status    = {};
    }
    saveConfig();
}

// ─────────────────────────────────────────────────────────────
void AcDetector::setConfig(const AcDetectorConfig& cfg) {
    bool pinChanged = (cfg.pin != _cfg.pin);
    _cfg = cfg;
    if (_cfg.enabled) {
        if (pinChanged) {
            memset(_sampleBuf, 0, sizeof(_sampleBuf));
            _sampleIdx = 0;
            _bufFull   = false;
        }
        _applyPin();
    }
    saveConfig();
}

// ─────────────────────────────────────────────────────────────
bool AcDetector::isPinValid(uint8_t pin) const {
    for (uint8_t i = 0; i < AC_VALID_COUNT; i++)
        if (AC_VALID_PINS[i] == pin) return true;
    return false;
}

bool AcDetector::isPinConflict(uint8_t pin) const {
    for (uint8_t i = 0; i < KNOWN_USED_COUNT; i++)
        if (KNOWN_USED_PINS[i] == pin) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────
//  Config persistence
// ─────────────────────────────────────────────────────────────
bool AcDetector::loadConfig() {
    if (!LittleFS.exists(AC_CONFIG_FILE)) return true;  // use defaults
    File f = LittleFS.open(AC_CONFIG_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) {
        f.close(); return false;
    }
    f.close();
    _cfg.pin            = doc["pin"]            | (uint8_t)AC_DEFAULT_PIN;
    _cfg.threshold      = doc["threshold"]      | (uint16_t)AC_DEFAULT_THRESHOLD;
    _cfg.hysteresis     = doc["hysteresis"]     | (uint16_t)AC_HYSTERESIS;
    _cfg.enabled        = doc["enabled"]        | false;
    _cfg.buzzerEnabled  = doc["buzzerEnabled"]  | false;
    _cfg.buzzerPin      = doc["buzzerPin"]      | (uint8_t)0;
    _cfg.acFreq         = doc["acFreq"]         | (uint8_t)50;
    // Validate pin
    if (!isPinValid(_cfg.pin)) _cfg.pin = AC_DEFAULT_PIN;
    return true;
}

bool AcDetector::saveConfig() {
    File f = LittleFS.open(AC_CONFIG_FILE, "w");
    if (!f) return false;
    JsonDocument doc;
    doc["pin"]           = _cfg.pin;
    doc["threshold"]     = _cfg.threshold;
    doc["hysteresis"]    = _cfg.hysteresis;
    doc["enabled"]       = _cfg.enabled;
    doc["buzzerEnabled"] = _cfg.buzzerEnabled;
    doc["buzzerPin"]     = _cfg.buzzerPin;
    doc["acFreq"]        = _cfg.acFreq;
    size_t w = serializeJson(doc, f);
    f.close();
    return w > 0;
}

// ─────────────────────────────────────────────────────────────
//  JSON helpers
// ─────────────────────────────────────────────────────────────
String AcDetector::statusJson() const {
    JsonDocument doc;
    doc["enabled"]        = _cfg.enabled;
    doc["pin"]            = _cfg.pin;
    doc["rms"]            = _status.rms;
    doc["rmsPercent"]     = _status.rmsPercent;
    doc["acDetected"]     = _status.acDetected;
    doc["signalBars"]     = _status.signalBars;
    doc["detectionCount"] = _status.detectionCount;
    doc["detectedAt"]     = _status.detectedAt;
    doc["lostAt"]         = _status.lostAt;
    doc["threshold"]      = _cfg.threshold;
    doc["hysteresis"]     = _cfg.hysteresis;
    String out; serializeJson(doc, out);
    return out;
}

String AcDetector::configJson() const {
    JsonDocument doc;
    doc["pin"]           = _cfg.pin;
    doc["threshold"]     = _cfg.threshold;
    doc["hysteresis"]    = _cfg.hysteresis;
    doc["enabled"]       = _cfg.enabled;
    doc["buzzerEnabled"] = _cfg.buzzerEnabled;
    doc["buzzerPin"]     = _cfg.buzzerPin;
    doc["acFreq"]        = _cfg.acFreq;
    String out; serializeJson(doc, out);
    return out;
}

// pinMapJson: all 4 valid ADC1 pins with status
// status: "active" | "free" | "conflict"
String AcDetector::pinMapJson() const {
    JsonDocument doc;
    JsonArray arr = doc["pins"].to<JsonArray>();
    for (uint8_t i = 0; i < AC_VALID_COUNT; i++) {
        uint8_t p = AC_VALID_PINS[i];
        JsonObject o = arr.add<JsonObject>();
        o["gpio"] = p;
        o["name"] = (p == 36) ? "VP" :
                    (p == 39) ? "VN" :
                    (String("GPIO") + p);
        if (p == _cfg.pin && _cfg.enabled) {
            o["status"] = "active";
            o["label"]  = "AC Antenna (active)";
        } else if (isPinConflict(p)) {
            o["status"] = "conflict";
            o["label"]  = "Used by other module";
        } else {
            o["status"] = "free";
            o["label"]  = "Free — can use";
        }
        o["isDefault"] = (p == AC_DEFAULT_PIN);
    }
    String out; serializeJson(doc, out);
    return out;
}
