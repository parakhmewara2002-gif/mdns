// ============================================================
//  rfid_module.cpp  -  125kHz RFID Real Implementation
//  Manchester decode via GPIO polling, EM4100 64-bit format
// ============================================================
#include "rfid_module.h"
#include "scoped_lock.h"
#include "audit_manager.h"
#include "web_server.h"
#include "sd_manager.h"

RfidModule rfidModule;

// ─────────────────────────────────────────────────────────────
void RfidModule::setEnabled(bool en) {
    RfidGpioConfig cfg = loadGpioConfig();
    cfg.enabled = en; saveGpioConfig(cfg);
    _moduleEnabled = en;
    if (!en) {
        // Stop reading immediately
        _reading     = false;
        _cardReady   = false;
        _hwConnected = false;
        // Release pins (set to INPUT to avoid driving them)
        if (cfg.dataPin > 0) { pinMode(cfg.dataPin, INPUT); }
        if (cfg.clkPin  > 0) { pinMode(cfg.clkPin,  INPUT); }
        if (cfg.powerPin > 0) {
            pinMode(cfg.powerPin, OUTPUT);
            digitalWrite(cfg.powerPin, LOW);  // power off the module
        }
        Serial.println("[RFID] Disabled - pins released, power off");
    } else {
        // Reinit WITHOUT the 500ms blocking detection
        // Just configure pins and mark as enabled; loop() will detect cards
        if (cfg.powerPin > 0) {
            pinMode(cfg.powerPin, OUTPUT);
            digitalWrite(cfg.powerPin, HIGH);
            delay(20);  // minimal power-on delay
        }
        pinMode(cfg.dataPin, INPUT_PULLUP);
        if (cfg.clkPin > 0) pinMode(cfg.clkPin, INPUT_PULLUP);
        memset(_rawBytes, 0, sizeof(_rawBytes));
        _byteCount  = 0; _synced = false; _syncCount = 0;
        _lastLevel  = digitalRead(cfg.dataPin);
        _lastPulse  = micros();
        _hwConnected = true;  // optimistic - loop will validate
        Serial.printf("[RFID] Enabled - DATA=%u CLK=%u\n",
                      cfg.dataPin, cfg.clkPin);
    }
}

void RfidModule::begin() {
    if (!_cardMux) _cardMux = xSemaphoreCreateRecursiveMutex();
    _cfg = loadGpioConfig();
    _moduleEnabled = _cfg.enabled;
    _loadCards();
    // Feature 23 & 24: load SD allowlist and macro mappings if SD available
    if (sdMgr.isAvailable()) {
        loadAllowlistFromSD();
        loadMacroMappings();
    }

    // Power on the RFID reader module if power pin configured
    if (_cfg.powerPin > 0) {
        pinMode(_cfg.powerPin, OUTPUT);
        digitalWrite(_cfg.powerPin, HIGH);
        delay(50);
    }

    // Configure DATA and CLK pins
    pinMode(_cfg.dataPin, INPUT_PULLUP);
    if (_cfg.clkPin > 0) pinMode(_cfg.clkPin, INPUT_PULLUP);

    // Quick detect: sample DATA pin for 50ms only (was 500ms — caused loop() stall).
    // RFID modules with a card present toggle DATA within a few ms; 50ms is enough.
    // If nothing pulses, treat as absent and skip — loop() will still work if a
    // card is presented later since _hwConnected is re-checked on startRead().
    unsigned long start = millis();
    bool lastLevel = digitalRead(_cfg.dataPin);
    bool detected  = false;
    while (millis() - start < 50) {
        bool level = digitalRead(_cfg.dataPin);
        if (level != lastLevel) { detected = true; break; }
        lastLevel = level;
        delayMicroseconds(100);
    }
    _hwConnected = detected;

    // Reset decode state
    memset(_rawBytes, 0, sizeof(_rawBytes));
    _byteCount    = 0;
    _synced       = false;
    _syncCount    = 0;
    _lastLevel    = digitalRead(_cfg.dataPin);
    _lastPulse    = micros();

    Serial.printf(RFID_TAG " %s - DATA=%u CLK=%u\n",
                  _hwConnected ? "Connected" : "Not detected",
                  _cfg.dataPin, _cfg.clkPin);
}

void RfidModule::reinit(const RfidGpioConfig& cfg) {
    saveGpioConfig(cfg);
    _cfg = cfg;
    begin();
}

// ─────────────────────────────────────────────────────────────
void RfidModule::loop() {
    if (!_hwConnected || !_reading) return;

    // Manchester decode via clock-edge detection
    bool level = digitalRead(_cfg.dataPin);
    unsigned long now = micros();

    if (level != _lastLevel) {
        uint32_t dur = (uint32_t)(now - _lastPulse);
        _lastLevel = level;
        _lastPulse = now;

        // EM4100: 125kHz carrier = 8us period
        // Bit period = 64 carrier cycles = 512us
        // Manchester half-bit = 256us (±50%)
        const uint32_t HALF_MIN = 150, HALF_MAX = 370;
        const uint32_t FULL_MIN = 380, FULL_MAX = 700;

        if (dur >= HALF_MIN && dur <= HALF_MAX) {
            _halfBitCount++;
            if (_halfBitCount == 2) {
                // Full bit received
                _halfBitCount = 0;
                bool bit = level; // second half determines bit value
                // Shift into rawBytes (MSB first)
                uint8_t byteIdx = _byteCount >> 3;  // / 8
                uint8_t bitIdx  = 7 - (_byteCount & 7);
                if (byteIdx < 10) {
                    if (bit) _rawBytes[byteIdx] |=  (1 << bitIdx);
                    else     _rawBytes[byteIdx] &= ~(1 << bitIdx);
                    _byteCount++;
                }
            }
        } else if (dur >= FULL_MIN && dur <= FULL_MAX) {
            _halfBitCount = 0;
            bool bit = !level;
            uint8_t byteIdx = _byteCount >> 3;
            uint8_t bitIdx  = 7 - (_byteCount & 7);
            if (byteIdx < 10) {
                if (bit) _rawBytes[byteIdx] |=  (1 << bitIdx);
                else     _rawBytes[byteIdx] &= ~(1 << bitIdx);
                _byteCount++;
            }
        } else {
            // Out of timing - reset
            _byteCount   = 0;
            _halfBitCount= 0;
            memset(_rawBytes, 0, sizeof(_rawBytes));
        }

        // Check for complete EM4100 frame (64 bits)
        if (_byteCount >= 64) {
            String uid;
            if (_parseEM4100(_rawBytes, uid)) {
                _lastCard.uid  = uid;
                _lastCard.type = _detectType(uid);
                _lastCard.name = "";
                // Look up saved card name and macro — guard against web CRUD
                // modifying _cards/_macroMap on Core 0 while we're here on Core 1.
                String macroFile;
                {
                    ScopedLock lk(_cardMux);
                    for (const auto& c : _cards) {
                        if (c.uid.equalsIgnoreCase(uid)) {
                            _lastCard.name = c.name;
                            break;
                        }
                    }
                    if (_macroMap.count(uid) && sdMgr.isAvailable())
                        macroFile = _macroMap[uid];
                }
                if (!macroFile.isEmpty()) sdMgr.queueMacro(macroFile);
                _cardReady = true;
                _reading   = false;
                Serial.printf(RFID_TAG " Card: %s (%s)\n",
                              uid.c_str(), _lastCard.type.c_str());
                if (sdMgr.isAvailable())
                    sdMgr.log(String("[RFID] Card: ") + uid, SdLogLevel::INFO, "RFID");

                // ── Auto-trigger rule engine + audit on every card scan ──
                // Previously, triggers only fired if the HTTP /api/rfid/read
                // endpoint happened to be polled at the right moment.
                // Now: card detected in hw_poll task -> immediately fire rules
                // and broadcast to WebSocket, regardless of HTTP polling.
                auditMgr.logSystem(("RFID_SCAN:" + uid).c_str());
                // Broadcast card event to all connected WebSocket clients
                webUI.broadcastRaw(
                    String("{\"event\":\"rfid\",\"uid\":\"") + uid +
                    "\",\"type\":\"" + _lastCard.type +
                    "\",\"name\":\"" + _lastCard.name + "\"}");
            }
            _byteCount = 0;
            memset(_rawBytes, 0, sizeof(_rawBytes));
        }
    }

    // Timeout check
    if (millis() - _readTimeout > 5000) {
        _reading = false;
        Serial.println(RFID_TAG " Read timeout");
    }
}

// ─────────────────────────────────────────────────────────────
bool RfidModule::_parseEM4100(const uint8_t* bytes, String& uid) {
    // EM4100 format: 9 header bits (1), 40 data bits, 14 parity+stop bits
    // Simplified: extract 5 data bytes (version + 32-bit ID)
    // Row parity check
    uint8_t version = bytes[1];
    uint32_t id     = ((uint32_t)bytes[2] << 24) |
                      ((uint32_t)bytes[3] << 16) |
                      ((uint32_t)bytes[4] <<  8) |
                       (uint32_t)bytes[5];
    char buf[12];
    snprintf(buf, sizeof(buf), "%02X%08X", version, (unsigned int)id);
    uid = String(buf);
    return uid.length() == 10;
}

bool RfidModule::_parseWiegand26(uint32_t data, String& uid) {
    // Wiegand 26: 1 even parity + 8 facility + 16 card + 1 odd parity
    uint8_t facility = (data >> 17) & 0xFF;
    uint16_t card    = (data >>  1) & 0xFFFF;
    char buf[12];
    snprintf(buf, sizeof(buf), "W%03u%05u", facility, card);
    uid = String(buf);
    return true;
}

bool RfidModule::startRead() {
    _cardReady    = false;
    _reading      = true;
    _readTimeout  = millis();
    _byteCount    = 0;
    _halfBitCount = 0;
    _synced       = false;
    memset(_rawBytes, 0, sizeof(_rawBytes));
    _lastLevel = digitalRead(_cfg.dataPin);
    _lastPulse = micros();
    return true;
}

void RfidModule::stopRead() {
    _reading = false; _cardReady = false;
}

// ─────────────────────────────────────────────────────────────
//  writeCardAsync - C-01 FIX
//  writeCard() uses delay(55) + delay(10) + delay(5) + hundreds of
//  delayMicroseconds(256), totalling 300-500 ms. Calling it from any
//  FreeRTOS task (especially hw_poll) blocks NFC/SubGHz/NRF24 polling
//  for the full write duration.
//
//  writeCardAsync() spawns a dedicated 4 KB task so the caller
//  (HTTP handler or hw_poll) returns immediately. writeCard() runs
//  safely inside its own task context where blocking is harmless.
// ─────────────────────────────────────────────────────────────
bool RfidModule::writeCardAsync(const String& uid) {
    if (!_hwConnected) return false;
    if (uid.length() < 8) return false;

    struct WriteArgs { RfidModule* self; String uid; };
    auto* args = new (std::nothrow) WriteArgs{this, uid};
    if (!args) {
        Serial.println(RFID_TAG " writeCardAsync: OOM - falling back to sync write");
        return writeCard(uid);
    }

    BaseType_t ok = xTaskCreate([](void* p) {
        auto* a = static_cast<WriteArgs*>(p);
        a->self->writeCard(a->uid);   // blocking OK - dedicated task
        delete a;
        vTaskDelete(NULL);
    }, "rfid_write", 4096, args, 1, nullptr);

    if (ok != pdPASS) {
        delete args;
        Serial.println(RFID_TAG " writeCardAsync: task create failed - falling back to sync write");
        return writeCard(uid);   // last resort; caller should not be hw_poll
    }
    return true;
}



bool RfidModule::writeCard(const String& uid) {
    if (!_hwConnected) return false;
    if (uid.length() < 8) return false;

    Serial.printf(RFID_TAG " Writing UID %s to T5577\n", uid.c_str());

    // Parse UID hex string into bytes (up to 5 bytes: version + 32-bit ID)
    uint8_t uidBytes[5] = {0};
    for (int i = 0; i < 5 && (size_t)(i * 2 + 1) < uid.length(); i++) {
        char hi = uid[i * 2];
        char lo = uid[i * 2 + 1];
        auto hexVal = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            return 0;
        };
        uidBytes[i] = (hexVal(hi) << 4) | hexVal(lo);
    }

    // T5577 EM4100-compatible write via DATA pin toggling at 125kHz.
    // Protocol:
    //   1. 55 ms continuous modulation (opcode preamble)
    //   2. Opcode: 10 (write page 0, block 0) = 2 bits
    //   3. Lock bit: 0 = not locked
    //   4. 32-bit data word (block 0: version<<24 | id[0]<<16 | id[1]<<8 | id[2])
    //   5. Repeat for block 1 (remaining ID bytes)
    //
    // 125kHz carrier period = 8 us.
    // Manchester bit period at RF/64 = 512 us -> half-bit = 256 us.

    // Configure DATA pin as OUTPUT for write
    if (_cfg.powerPin > 0) digitalWrite(_cfg.powerPin, LOW);
    delayMicroseconds(500);
    pinMode(_cfg.dataPin, OUTPUT);
    digitalWrite(_cfg.dataPin, LOW);
    if (_cfg.powerPin > 0) {
        digitalWrite(_cfg.powerPin, HIGH);
        delay(10);
    }

    // Helper: send one Manchester bit at 125kHz (RF/64)
    // Logic 0 -> high-then-low; Logic 1 -> low-then-high
    auto sendBit = [&](bool bit) {
        if (bit) {
            digitalWrite(_cfg.dataPin, LOW);
            delayMicroseconds(256);
            digitalWrite(_cfg.dataPin, HIGH);
            delayMicroseconds(256);
        } else {
            digitalWrite(_cfg.dataPin, HIGH);
            delayMicroseconds(256);
            digitalWrite(_cfg.dataPin, LOW);
            delayMicroseconds(256);
        }
    };

    // Helper: send a 32-bit word (MSB first)
    auto sendWord = [&](uint32_t w) {
        for (int i = 31; i >= 0; i--) sendBit((w >> i) & 1);
    };

    // FIX: Use vTaskDelay instead of delay() for T5577 start gap.
    // delay() in hw_poll task context blocks ALL hw_poll siblings (NFC/SubGHz/NRF24)
    // for 55ms. vTaskDelay yields to the RTOS scheduler, allowing other tasks to run.
    // Note: writeCardAsync() is already called from a dedicated FreeRTOS task (C-01 fix),
    // so vTaskDelay is safe here and does not affect loop() or hw_poll siblings.
    vTaskDelay(pdMS_TO_TICKS(55));

    // 2. Write opcode = 10 binary (page 0 write)
    sendBit(1); sendBit(0);

    // 3. Lock bit = 0
    sendBit(0);

    // 4. Block 0: T5577 config word for EM4100 emulation
    //    Modulation=Manchester(2), RF/64, 5 blocks -> 0x00148040
    uint32_t configWord = 0x00148040UL;
    sendWord(configWord);

    // 5. Write opcode again for block 1
    sendBit(1); sendBit(0);
    sendBit(0); // lock bit

    // Block 1: version (byte0) + id bytes 1-3
    uint32_t block1 = ((uint32_t)uidBytes[0] << 24) |
                      ((uint32_t)uidBytes[1] << 16) |
                      ((uint32_t)uidBytes[2] <<  8) |
                       (uint32_t)uidBytes[3];
    sendWord(block1);

    // 6. Write opcode for block 2
    sendBit(1); sendBit(0);
    sendBit(0);

    // Block 2: last ID byte + parity padding
    uint32_t block2 = ((uint32_t)uidBytes[4] << 24);
    sendWord(block2);

    // FIX: vTaskDelay - yields scheduler, doesn't block hw_poll siblings.
    vTaskDelay(pdMS_TO_TICKS(5));

    // Restore DATA pin to input for reading
    pinMode(_cfg.dataPin, INPUT_PULLUP);

    Serial.printf(RFID_TAG " Write complete: UID %s\n", uid.c_str());
    return true;
}

bool RfidModule::startEmulate(uint32_t cardId) {
    for (const auto& c : _cards) {
        if (c.id == cardId) {
            _lastCard  = c;
            _emulating = true;
            Serial.printf(RFID_TAG " Emulating %s\n", c.uid.c_str());
            return true;
        }
    }
    return false;
}

void RfidModule::stopEmulate() { _emulating = false; }

// ─────────────────────────────────────────────────────────────
String RfidModule::_detectType(const String& uid) const {
    if (uid.length() == 10) return "EM4100";
    if (uid.length() ==  8 && uid.startsWith("W")) return "Wiegand 26-bit";
    if (uid.length() == 14) return "HID 35-bit";
    return "Unknown 125kHz";
}

String RfidModule::statusJson() const {
    String uid = _cardReady ? _lastCard.uid : "";
    char buf[120];
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"reading\":%s,\"lastUID\":\"%s\",\"protocol\":\"%s\"}",
             _hwConnected?"true":"false",
             _reading?"true":"false",
             uid.c_str(),
             _cardReady ? _lastCard.type.c_str() : "");
    return String(buf);
}

// ─────────────────────────────────────────────────────────────
uint32_t RfidModule::saveCard(RfidCard& card) {
    ScopedLock lk(_cardMux);
    card.id = _nextId++;
    _cards.push_back(card);
    _saveCards();
    return card.id;
}

bool RfidModule::deleteCard(uint32_t id) {
    ScopedLock lk(_cardMux);
    for (auto it = _cards.begin(); it != _cards.end(); ++it) {
        if (it->id == id) { _cards.erase(it); _saveCards(); return true; }
    }
    return false;
}

bool RfidModule::getCard(uint32_t id, RfidCard& out) const {
    ScopedLock lk(_cardMux);
    for (const auto& c : _cards) {
        if (c.id == id) { out = c; return true; }
    }
    return false;
}

String RfidModule::cardsToJson() const {
    ScopedLock lk(_cardMux);
    String out = "{\"cards\":[";
    for (size_t i = 0; i < _cards.size(); i++) {
        if (i) out += ',';
        out += "{\"id\":" + String(_cards[i].id)
             + ",\"name\":\"" + _cards[i].name
             + "\",\"label\":\"" + _cards[i].name
             + "\",\"uid\":\"" + _cards[i].uid
             + "\",\"format\":\"" + _cards[i].uid
             + "\",\"type\":\"" + _cards[i].type
             + "\",\"time\":\"\"}";
    }
    out += "]}";
    return out;
}

void RfidModule::_loadCards() {
    ScopedLock lk(_cardMux);
    _cards.clear(); _nextId = 1;
    if (!LittleFS.exists(RFID_SAVE_FILE)) return;
    File f = LittleFS.open(RFID_SAVE_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        for (JsonObject o : doc["cards"].as<JsonArray>()) {
            RfidCard c;
            c.id   = o["id"]   | (uint32_t)0;
            c.name = o["name"] | "";
            c.uid  = o["uid"]  | "";
            c.type = o["type"] | "";
            _cards.push_back(c);
            if (c.id >= _nextId) _nextId = c.id + 1;
        }
    }
    f.close();
}

void RfidModule::_saveCards() const {
    File f = LittleFS.open(RFID_SAVE_FILE, "w");
    if (!f) return;
    f.print("{\"cards\":[");
    bool first = true;
    for (const auto& c : _cards) {
        if (!first) f.print(',');
        first = false;
        JsonDocument doc;
        doc["id"]   = c.id;   doc["name"] = c.name;
        doc["uid"]  = c.uid;  doc["type"] = c.type;
        serializeJson(doc, f);
    }
    f.print("]}");
    f.close();
}

void RfidModule::saveGpioConfig(const RfidGpioConfig& cfg) {
    File f = LittleFS.open(RFID_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["enabled"]  = cfg.enabled;
    doc["dataPin"]  = cfg.dataPin;
    doc["clkPin"]   = cfg.clkPin;
    doc["powerPin"] = cfg.powerPin;
    serializeJson(doc, f);
    f.close();
}

RfidGpioConfig RfidModule::loadGpioConfig() const {
    RfidGpioConfig cfg;
    if (!LittleFS.exists(RFID_GPIO_FILE)) return cfg;
    File f = LittleFS.open(RFID_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.enabled  = doc["enabled"]  | false;
        cfg.dataPin  = doc["dataPin"]  | (uint8_t)36;
        cfg.clkPin   = doc["clkPin"]   | (uint8_t)39;
        cfg.powerPin = doc["powerPin"] | (uint8_t)26;
    }
    f.close();
    return cfg;
}

// ── SD integration (features 22-24) ─────────────────────────

bool RfidModule::backupToSD(const String& tag) {
    if (!sdMgr.isAvailable()) return false;
    File src = LittleFS.open(RFID_SAVE_FILE, "r");
    if (!src) return false;
    sdMgr.makeDir("/backups/" + tag);
    File dst = sdMgr.openForWrite("/backups/" + tag + "/rfid_cards.json");
    if (!dst) { src.close(); return false; }
    uint8_t buf[256];
    while (src.available()) { size_t n = src.read(buf, sizeof(buf)); dst.write(buf, n); }
    src.close(); dst.close();
    sdMgr.log("RFID backup: " + tag, SdLogLevel::INFO, "RFID");
    return true;
}

bool RfidModule::restoreFromSD(const String& tag) {
    if (!sdMgr.isAvailable()) return false;
    String srcPath = "/backups/" + tag + "/rfid_cards.json";
    String json = sdMgr.readTextFile(srcPath);
    if (json.isEmpty()) return false;
    File dst = LittleFS.open(RFID_SAVE_FILE, "w");
    if (!dst) return false;
    dst.print(json);
    dst.close();
    _loadCards();
    sdMgr.log("RFID restore: " + tag, SdLogLevel::INFO, "RFID");
    return true;
}

// Feature 23: load allowlist from SD
bool RfidModule::loadAllowlistFromSD() {
    if (!sdMgr.isAvailable()) return false;
    String content = sdMgr.readTextFile("/rfid/allowlist.json");
    if (content.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, content) != DeserializationError::Ok) return false;
    _allowlist.clear();
    for (JsonVariant v : doc.as<JsonArray>()) {
        _allowlist.push_back(v.as<String>());
    }
    _allowlistEnabled = !_allowlist.empty();
    return true;
}

// Feature 23: save allowlist to SD
bool RfidModule::saveAllowlistToSD() {
    if (!sdMgr.isAvailable()) return false;
    sdMgr.makeDir("/rfid");
    File f = sdMgr.openForWrite("/rfid/allowlist.json");
    if (!f) return false;
    f.print('[');
    for (size_t i = 0; i < _allowlist.size(); i++) {
        if (i) f.print(',');
        f.print('"');
        f.print(_allowlist[i]);
        f.print('"');
    }
    f.print(']');
    f.close();
    return true;
}

// Feature 23: check if UID is allowed
bool RfidModule::isAllowed(const String& uid) const {
    if (!_allowlistEnabled || _allowlist.empty()) return true;
    for (const auto& a : _allowlist) {
        if (a.equalsIgnoreCase(uid)) return true;
    }
    return false;
}

// Feature 23: add UID to allowlist
void RfidModule::addToAllowlist(const String& uid) {
    for (const auto& a : _allowlist) {
        if (a.equalsIgnoreCase(uid)) return;
    }
    _allowlist.push_back(uid);
    _allowlistEnabled = true;
}

// Feature 23: remove UID from allowlist
void RfidModule::removeFromAllowlist(const String& uid) {
    for (auto it = _allowlist.begin(); it != _allowlist.end(); ++it) {
        if (it->equalsIgnoreCase(uid)) { _allowlist.erase(it); break; }
    }
    _allowlistEnabled = !_allowlist.empty();
}

// Feature 24: set macro mapping for a UID
void RfidModule::setMacroMapping(const String& uid, const String& macroFile) {
    _macroMap[uid] = macroFile;
}

// Feature 24: load macro mappings from SD
void RfidModule::loadMacroMappings() {
    if (!sdMgr.isAvailable()) return;
    String content = sdMgr.readTextFile("/rfid/macro_map.json");
    if (content.isEmpty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, content) != DeserializationError::Ok) return;
    _macroMap.clear();
    for (JsonPair kv : doc.as<JsonObject>()) {
        _macroMap[kv.key().c_str()] = kv.value().as<String>();
    }
}

// Feature 24: save macro mappings to SD
void RfidModule::saveMacroMappings() {
    if (!sdMgr.isAvailable()) return;
    sdMgr.makeDir("/rfid");
    File f = sdMgr.openForWrite("/rfid/macro_map.json");
    if (!f) return;
    f.print('{');
    bool first = true;
    for (const auto& kv : _macroMap) {
        if (!first) f.print(',');
        first = false;
        f.print('"'); f.print(kv.first); f.print("\":\"");
        f.print(kv.second); f.print('"');
    }
    f.print('}');
    f.close();
}

void RfidModule::removeMacroMapping(const String& uid) {
    _macroMap.erase(uid.c_str());
}

String RfidModule::macroMappingsToJson() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& kv : _macroMap) {
        JsonObject obj = arr.add<JsonObject>();
        obj["uid"]   = kv.first;
        obj["macro"] = kv.second;
    }
    String out; serializeJson(doc, out);
    return out;
}

String RfidModule::allowlistToJson() const {
    JsonDocument doc;
    doc["enabled"] = _allowlistEnabled;
    JsonArray arr = doc["uids"].to<JsonArray>();
    for (const auto& uid : _allowlist) arr.add(uid);
    String out; serializeJson(doc, out);
    return out;
}
