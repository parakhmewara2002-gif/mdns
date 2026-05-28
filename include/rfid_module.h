#pragma once
// ============================================================
//  rfid_module.h  -  125kHz RFID Real Implementation
//  Manchester decode, EM4100, Wiegand 26/34-bit
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define RFID_SAVE_FILE  "/rfid_cards.json"
#define RFID_GPIO_FILE  "/rfid_gpio.json"
#define RFID_TAG        "[RFID]"

struct RfidCard {
    uint32_t id   = 0;
    String   name;
    String   uid;
    String   type;
};

struct RfidGpioConfig {
    bool    enabled  = false;
    uint8_t dataPin  = 36;
    uint8_t clkPin   = 39;
    uint8_t powerPin = 26;  // 0 = not used
};

class RfidModule {
public:
    void   begin();
    void   loop();
    void   reinit(const RfidGpioConfig& cfg);

    bool   isConnected()   const { return _hwConnected; }
    bool   isEnabled()     const { return _moduleEnabled; }
    void   setEnabled(bool en);
    bool   isReading()     const { return _reading; }
    bool   isEmulating()   const { return _emulating; }
    bool   cardAvailable() const { return _cardReady; }

    bool   startRead();
    void   stopRead();
    void   clearCard() { _cardReady = false; }
    RfidCard lastCard() const { return _lastCard; }

    bool   writeCard(const String& uid);
    bool   writeCardAsync(const String& uid);   // C-01: non-blocking, for hw_poll / HTTP context
    bool   startEmulate(uint32_t cardId);
    void   stopEmulate();

    uint32_t saveCard(RfidCard& card);
    bool     deleteCard(uint32_t id);
    bool     getCard(uint32_t id, RfidCard& out) const;
    String   cardsToJson() const;
    size_t   cardCount() const { return _cards.size(); }

    void           saveGpioConfig(const RfidGpioConfig& cfg);
    RfidGpioConfig loadGpioConfig() const;

    String statusJson() const;

    // ── SD integration (features 22-24) ───────────────────────
    bool backupToSD(const String& tag);
    bool restoreFromSD(const String& tag);
    void setAllowlistEnabled(bool en) { _allowlistEnabled = en; }
    bool loadAllowlistFromSD();
    bool saveAllowlistToSD();
    bool isAllowed(const String& uid) const;
    void addToAllowlist(const String& uid);
    void removeFromAllowlist(const String& uid);

    void setMacroMapping(const String& uid, const String& macroFile);
    void removeMacroMapping(const String& uid);
    String macroMappingsToJson() const;
    void loadMacroMappings();
    void saveMacroMappings();

    String allowlistToJson() const;

private:
    bool      _hwConnected   = false;
    bool      _moduleEnabled  = false;
    bool      _reading     = false;
    bool      _cardReady   = false;
    bool      _emulating   = false;
    RfidCard  _lastCard;
    RfidGpioConfig _cfg;

    std::vector<RfidCard> _cards;
    // Guards _cards, _allowlist, _macroMap: iterated in hw_poll task (Core 1)
    // on every card read; mutated by web CRUD handlers (Core 0).
    SemaphoreHandle_t _cardMux = nullptr;
    uint32_t _nextId = 1;

    // SD allowlist (feature 23)
    std::vector<String> _allowlist;
    bool _allowlistEnabled = false;

    // SD macro map (feature 24)
    std::map<String, String> _macroMap;

    // Manchester decode state machine
    static const int  EM4100_BITS = 64;
    volatile uint32_t _rawBits[3] = {0, 0, 0}; // 64 bits in 3 words
    volatile uint8_t  _bitCount   = 0;
    volatile bool     _newData    = false;

    unsigned long _readTimeout = 0;
    unsigned long _lastPulse   = 0;
    bool          _lastLevel   = false;
    uint8_t       _halfBitCount = 0;
    uint8_t       _currentByte = 0;
    uint8_t       _byteCount   = 0;
    uint8_t       _rawBytes[10] = {};
    bool          _synced       = false;
    uint8_t       _syncCount    = 0;

    bool     _parseEM4100(const uint8_t* bytes, String& uid);
    bool     _parseWiegand26(uint32_t data, String& uid);
    String   _detectType(const String& uid) const;

    void _loadCards();
    void _saveCards() const;
};

extern RfidModule rfidModule;
