#pragma once
// ============================================================
//  mic_module.h  -  Hybrid Mic: I2S Digital + ADC Analog
//  I2S:  INMP441 / SPH0645 / MSM261S  (max 2, stereo)
//  ADC:  Any 2-pin electret mic        (max 4, GPIO 32-39)
//  Mix modes: I2S_ONLY / ADC_ONLY / HYBRID / AUTO
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>  // MUTEX FIX: serialize _readI2S/_readADC/
                              // _readHybrid which share static buffers

// ── File paths ────────────────────────────────────────────────
#define MIC_GPIO_FILE     "/mic_gpio.json"
#define MIC_REC_DIR       "/recordings"
#define WAV_HEADER_SIZE   44

// ── Audio constants ───────────────────────────────────────────
#define MIC_SAMPLE_RATE   16000
#define MIC_BITS          16
#define MIC_DMA_BUF_COUNT 4
#define MIC_DMA_BUF_LEN   256
#define MIC_STREAM_CHUNK  512
#define MIC_MAX_REC_SECS  300
#define MIC_I2S_PORT      I2S_NUM_0
#define MIC_ADC_MAX       6   // max analog mics (ADC1 pins 32-39)

// ── Mix modes ─────────────────────────────────────────────────
enum class MicMixMode : uint8_t {
    AUTO      = 0,  // I2S if available, else ADC
    I2S_ONLY  = 1,
    ADC_ONLY  = 2,
    HYBRID    = 3   // blend I2S + ADC
};

// ── ADC mic config (one entry per analog mic) ─────────────────
struct AdcMicConfig {
    uint8_t  pin    = 0;      // GPIO 32-39
    uint8_t  gain   = 6;      // 0-10 software gain
    bool     enabled = false;
};

// ── Full config saved to LittleFS ────────────────────────────
struct MicConfig {
    // I2S section
    bool     i2sEnabled  = false;
    uint8_t  ws          = 25;
    uint8_t  sck         = 26;
    uint8_t  sd          = 33;
    bool     stereo      = false;

    // ADC section
    AdcMicConfig adc[MIC_ADC_MAX];  // up to 6 analog mics

    // Common
    uint32_t   sampleRate  = MIC_SAMPLE_RATE;
    MicMixMode mixMode     = MicMixMode::AUTO;
    uint8_t    i2sWeight   = 60;   // % of I2S in HYBRID mix (0-100)
    uint8_t    masterGain  = 6;    // overall gain
};

// ── Backward-compat alias ─────────────────────────────────────
using MicGpioConfig = MicConfig;

// ─────────────────────────────────────────────────────────────
class MicModule {
public:
    void   begin();
    void   loop();

    // Config
    bool   applyConfig(const MicConfig& cfg);
    void   saveConfig(const MicConfig& cfg);
    MicConfig loadConfig() const;

    // Compat wrappers (old API)
    void          saveGpioConfig(const MicConfig& cfg) { saveConfig(cfg); }
    MicConfig     loadGpioConfig() const               { return loadConfig(); }
    // NOTE: applyConfig(const MicGpioConfig&) intentionally removed -
    // MicGpioConfig is a typedef alias for MicConfig, so they resolve to
    // the same signature and cause an overload conflict in C++.

    // Status
    String statusJson() const;
    String gpioJson()   const;

    // Streaming
    bool   startStream();
    void   stopStream();
    bool   isStreaming() const { return _streaming; }
    size_t readChunk(uint8_t* buf, size_t maxLen);
    uint8_t volumeLevel() const { return _volume; }
    void    setVolume(uint8_t v) { _volume = v; }

    // Speaker output (used by Bluetooth A2DP sink)
    bool    isSpeakerActive() const { return _speakerActive; }
    void    playSamples(const int16_t* samples, size_t count);

    // Recording
    bool     startRecording(const String& filename = "");
    void     stopRecording();
    bool     isRecording()     const { return _recording; }
    bool     isRecordingOnSd() const { return _recOnSd; }
    String   recordingName()   const { return _recFilename; }
    uint32_t recordingSecs()   const;
    uint32_t recordingBytes()  const { return _recBytes; }

    // Recordings list
    String listRecordingsJson() const;
    bool   deleteRecording(const String& name);

    // Source info
    bool    i2sActive()  const { return _i2sInited; }
    uint8_t adcCount()   const { return _adcCount; }
    String  activeSourceStr() const;

private:
    MicConfig _cfg;

    // I2S state
    bool    _i2sInited  = false;

    // ADC state
    uint8_t _adcPins[MIC_ADC_MAX] = {0};
    uint8_t _adcGains[MIC_ADC_MAX] = {6,6,6,6,6,6};
    uint8_t _adcCount   = 0;
    bool    _adcInited  = false;

    // Common
    bool    _streaming      = false;
    bool    _recording      = false;
    bool    _speakerActive  = false;
    uint8_t _volume         = 0;

    // MUTEX FIX: _readI2S/_readADC/_readHybrid all use function-static
    // buffers (raw[], i2sBuf[], adcBuf[]). When the WS streaming task
    // (calls readChunk) and the loop() recording tick run concurrently
    // they would tear those buffers. _readMutex serialises them.
    SemaphoreHandle_t _readMutex = nullptr;

    // Recording file
    File     _recFile;
    String   _recFilename;
    bool     _recOnSd    = false;
    uint32_t _recStartMs = 0;
    uint32_t _recBytes   = 0;

    // Private methods
    bool    _initI2S();
    void    _deinitI2S();
    bool    _initADC();
    void    _deinitADC();

    // Audio read per source
    size_t  _readI2S(int16_t* pcm, size_t maxSamples);
    size_t  _readADC(int16_t* pcm, size_t maxSamples);
    size_t  _readHybrid(int16_t* pcm, size_t maxSamples);

    // Active source based on mixMode + hw availability
    MicMixMode _activeMode() const;

    int16_t _applyGain(int32_t raw, uint8_t gain) const;
    void    _updateWavHeader();
    bool    _pinConflict(uint8_t pin) const;
    bool    _validAdcPin(uint8_t pin) const;
};

extern MicModule micModule;
