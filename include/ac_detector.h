#pragma once
// ============================================================
//  ac_detector.h  —  Non-Contact AC Power Detector v1.0
//
//  Principle (Theory to Thing method):
//    AC mains (50/60Hz) creates EMF around live wires.
//    ESP32 floating ADC pin acts as antenna — picks up this
//    EMF without any physical contact or extra components.
//    RMS of ADC samples gives signal strength (proximity).
//
//  Hardware required:
//    - 1x jumper wire on selected GPIO (antenna)
//    - Optional: GND wire twisted around it (shielding)
//      to reduce false positives from hand proximity.
//
//  ADC1 pins only (ADC2 shares WiFi — unstable readings):
//    GPIO 34 (ADC1_CH6) — input only
//    GPIO 35 (ADC1_CH7) — input only
//    GPIO 36 (ADC1_CH0) — input only, VP pin  ← default
//    GPIO 39 (ADC1_CH3) — input only, VN pin
//
//  Rule Engine triggers emitted:
//    AC_DETECTED  — RMS crosses threshold (rising edge)
//    AC_LOST      — RMS drops below threshold (falling edge)
// ============================================================
#include <Arduino.h>

// ── Sampling ─────────────────────────────────────────────────
// Nyquist: sample at >= 2× signal frequency.
// 50Hz AC → need >= 100 samples/sec → 1 sample per 10ms max.
// We sample every 5ms (200 samples/sec) — safe margin.
#define AC_SAMPLE_INTERVAL_MS   5      // ms between ADC reads
#define AC_SAMPLE_COUNT         40     // samples per RMS window (= 200ms window)
#define AC_BUZZER_INTERVAL_MS   50     // ms between buzzer updates

// ── Thresholds ───────────────────────────────────────────────
#define AC_DEFAULT_THRESHOLD    80     // RMS above this = AC detected
#define AC_HYSTERESIS           15     // dead-band to prevent rapid toggling
// Detected when RMS > threshold
// Lost     when RMS < (threshold - hysteresis)

// ── Default pin ──────────────────────────────────────────────
#define AC_DEFAULT_PIN          36     // GPIO36 VP — free by default

// ── Valid ADC1 pins for AC detection ─────────────────────────
static const uint8_t AC_VALID_PINS[]  = { 34, 35, 36, 39 };
static const uint8_t AC_VALID_COUNT   = 4;

// ── Config (persisted to /ac_detector.json) ──────────────────
#define AC_CONFIG_FILE  "/ac_detector.json"

struct AcDetectorConfig {
    uint8_t  pin;            // ADC pin (34/35/36/39)
    uint16_t threshold;      // RMS detection threshold (0-4095)
    uint16_t hysteresis;     // dead-band
    bool     enabled;        // module on/off
    bool     buzzerEnabled;  // beep on detection
    uint8_t  buzzerPin;      // buzzer GPIO (0 = disabled)
    uint8_t  acFreq;         // 50 or 60 Hz (display only)

    AcDetectorConfig()
        : pin(AC_DEFAULT_PIN),
          threshold(AC_DEFAULT_THRESHOLD),
          hysteresis(AC_HYSTERESIS),
          enabled(false),
          buzzerEnabled(false),
          buzzerPin(0),
          acFreq(50) {}
};

// ── Live status ───────────────────────────────────────────────
struct AcStatus {
    float    rms;            // current RMS value (0–4095 scale)
    float    rmsPercent;     // 0.0–100.0%
    bool     acDetected;     // true = AC live wire nearby
    uint32_t detectedAt;     // millis() when last detected
    uint32_t lostAt;         // millis() when last lost
    uint32_t detectionCount; // total detection events since boot
    uint8_t  signalBars;     // 0–5 bars for UI
};

// ─────────────────────────────────────────────────────────────
class AcDetector {
public:
    AcDetector();

    void begin();
    void loop();                     // call every loop() tick

    // ── Config ───────────────────────────────────────────────
    bool             loadConfig();
    bool             saveConfig();
    AcDetectorConfig getConfig()  const { return _cfg; }
    void             setConfig(const AcDetectorConfig& cfg);

    // ── Runtime control ──────────────────────────────────────
    void enable(bool on);
    bool isEnabled()    const { return _cfg.enabled; }
    bool isPinValid(uint8_t pin) const;
    bool isPinConflict(uint8_t pin) const;  // checks known used pins

    // ── Status ───────────────────────────────────────────────
    AcStatus  getStatus() const { return _status; }
    String    statusJson()  const;
    String    configJson()  const;
    String    pinMapJson()  const;   // all 4 pins with free/conflict status

    // ── Rule engine callbacks ─────────────────────────────────
    // Set by main.cpp — called on AC detected / lost events
    std::function<void()> onAcDetected;
    std::function<void()> onAcLost;

private:
    AcDetectorConfig _cfg;
    AcStatus         _status;

    // Sampling state
    uint32_t _lastSampleMs;
    uint32_t _lastBuzzerMs;
    int32_t  _sampleBuf[AC_SAMPLE_COUNT];
    uint8_t  _sampleIdx;
    bool     _bufFull;

    // Buzzer state
    bool     _buzzerOn;
    unsigned long _buzzerHighMs = 0;  // millis() when buzzer pin was set HIGH

    float    _calcRms() const;
    void     _updateBuzzer(float rms);
    void     _applyPin();
};

extern AcDetector acDetector;
