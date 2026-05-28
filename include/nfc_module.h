#pragma once
// ============================================================
//  nfc_module.h  -  PN532 NFC Module (Real HW via Adafruit)
// ============================================================
#include <Arduino.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>

#define NFC_SAVE_FILE     "/nfc_tags.json"
#define NFC_GPIO_FILE     "/nfc_gpio.json"
#define NFC_MAX_TAGS      32
#define NFC_POLL_MS       500   // polling interval

enum class NfcIface { I2C, SPI, UART };

struct NfcTag {
    uint32_t id = 0;
    String   name;
    String   uid;       // hex colon-separated  "A1:B2:C3:D4"
    String   type;      // "MIFARE Classic 1K", "NTAG213", etc.
    String   atqa;
    String   sak;
    std::vector<String> blocks;
};

struct NfcGpioConfig {
    bool     enabled = false;  // master on/off
    NfcIface iface  = NfcIface::I2C;
    uint8_t  spiBus = 0;    // 0=VSPI, 1=HSPI (SPI mode only)
    uint8_t  sda   = 21;
    uint8_t  scl   = 22;
    uint8_t  irq   = 13;   // GPIO4 conflicts with SD CS - moved to GPIO13 (interrupt input, safe to share)
    uint8_t  reset = 13;   // GPIO33 conflicts with IR TX-3 - moved to GPIO13 (init-only pulse)
    uint8_t  ss    = 13;   // GPIO5 forbidden (boot HIGH strapping) - moved to GPIO13
    uint8_t  sck   = 18;
    uint8_t  mosi  = 23;
    uint8_t  miso  = 19;
    uint8_t  tx    = 17;
    uint8_t  rx    = 16;
};



class NfcModule {
public:
    NfcModule() = default;
    void begin();
    void loop();
    void reinit();

    bool isConnected()   const { return _hwConnected; }
    bool isEnabled()     const { return _moduleEnabled; }
    void setEnabled(bool en);
    bool enabledFromConfig() const;

    bool     startRead();
    void     stopRead();
    bool     isReading()     const { return _reading; }
    NfcTag   lastTag()       const;
    bool     tagAvailable()  const;
    void     clearTag();

    bool     startCloneRead();
    bool     writeClone();
    bool     hasCloneSource() const { return _cloneReady; }

    bool     startEmulate(uint32_t tagId);
    void     stopEmulate();
    bool     isEmulating()   const { return _emulating; }

    void     startDictAttack(bool keyB = true);   // keyB=true -> also try Key B
    void     stopDictAttack();
    bool     isDictRunning() const { return _dictRunning; }
    String   pollDictResult();
    // Returns progress JSON: {sector, keyIdx, totalKeys, foundCount, elapsed, done}
    String   dictProgressJson() const;

    uint32_t saveTag(NfcTag& tag);
    bool     deleteTag(uint32_t id);
    bool     getTag(uint32_t id, NfcTag& out) const;
    String   tagsToJson() const;
    bool     importFlipperNfc(const String& content, NfcTag& out);
    String   exportFlipperNfc(uint32_t id) const;
    String   parseTag(const NfcTag& tag) const;

    void        saveGpioConfig(const NfcGpioConfig& cfg);
    NfcGpioConfig loadGpioConfig() const;
    String      statusJson() const;

    // ── SD integration (features 18-21) ───────────────────────
    bool backupToSD(const String& tag);
    bool restoreFromSD(const String& tag);

    bool exportFlipperToSD(uint32_t tagId);
    bool exportAllFlipperToSD();

    std::vector<String> listSdTags() const;
    bool importTagFromSD(const String& filename);

private:
    SemaphoreHandle_t _nfcMutex  = nullptr;
    SPIClass*     _nfcSpi      = nullptr;  // dedicated SPI instance for PN532 SPI mode
    bool          _hwConnected   = false;
    bool          _moduleEnabled  = false;
    bool          _reading     = false;
    bool          _emulating   = false;
    bool          _dictRunning = false;
    bool          _tagReady    = false;
    bool          _cloneReady  = false;
    NfcTag        _lastTag;
    NfcTag        _cloneSource;
    uint32_t      _emulatingId = 0;
    NfcGpioConfig _cfg;
    uint32_t      _fwVersion   = 0;

    uint8_t       _dictSector  = 0;
    uint8_t       _dictKeyIdx  = 0;
    uint8_t       _dictKeyType = 0;   // 0=KeyA phase, 1=KeyB phase
    bool          _dictDoKeyB  = true;
    uint8_t       _dictFoundCount = 0;
    unsigned long _dictTimer    = 0;
    unsigned long _dictStartMs  = 0;
    unsigned long _pollTimer    = 0;
    unsigned long _emulateTimer = 0;
    String        _dictPending;

    void*         _nfc = nullptr;   // Adafruit_PN532* - heap allocated

    std::vector<NfcTag> _tags;

    bool   _initPN532();
    void   _deinitPN532();
    void   _pollForTag();
    void   _readMifareBlocks(NfcTag& tag);
    void   _loadTags();
    void   _saveTags() const;
    String _fmtUid(const uint8_t* uid, uint8_t len) const;
    String _detectType(uint8_t sak, const uint8_t* atqa) const;
    bool   _tryMifareKey(uint8_t sector, uint8_t keyType, const char* keyHex);
};

extern NfcModule nfcModule;
