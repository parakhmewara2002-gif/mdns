// ============================================================
//  nfc_module.cpp  -  PN532 NFC Real Hardware (Adafruit lib)
// ============================================================
#include "nfc_module.h"
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include "audit_manager.h"
#include "web_server.h"
#include "sd_manager.h"

// MIFARE classic dictionary keys for brute-force sector authentication
static const char* const MIFARE_DICT_KEYS[] = {
    "FFFFFFFFFFFF", "000000000000", "A0A1A2A3A4A5",
    "B0B1B2B3B4B5", "D3F7D3F7D3F7", "AABBCCDDEEFF",
    "1234567890AB", "000102030405", "FFFFFFFFFFFE",
    "4B0B20107CCB", "6A1987C40A21", "AACCDDEE0011"
};
static const uint8_t MIFARE_DICT_LEN = 12;

NfcModule nfcModule;

static Adafruit_PN532* _getNfc(void* ptr) {
    return reinterpret_cast<Adafruit_PN532*>(ptr);
}

// ── PN532 init ────────────────────────────────────────────────
bool NfcModule::_initPN532() {
    _deinitPN532();
    _hwConnected = false;
    _fwVersion   = 0;

    Adafruit_PN532* pn = nullptr;

    switch (_cfg.iface) {
        case NfcIface::I2C:
            Wire.begin(static_cast<int>(_cfg.sda),
                       static_cast<int>(_cfg.scl));
            pn = new Adafruit_PN532(_cfg.irq, _cfg.reset);
            break;
        case NfcIface::SPI: {
            // Use dedicated SPIClass instance so we don't interfere with
            // SD card (global SPI / VSPI) or other SPI modules.
            SPIClass* nfcSpi = (_cfg.spiBus == 1)
                ? new SPIClass(HSPI)
                : new SPIClass(VSPI);
            nfcSpi->begin(_cfg.sck, _cfg.miso, _cfg.mosi, _cfg.ss);
            // Store in _nfcSpi so we can delete later
            if (_nfcSpi) { _nfcSpi->end(); delete _nfcSpi; }
            _nfcSpi = nfcSpi;
            pn = new Adafruit_PN532(_cfg.ss, nfcSpi);
            Serial.printf("[NFC] SPI bus=%s SCK=%u MISO=%u MOSI=%u SS=%u\n",
                          _cfg.spiBus==1?"HSPI":"VSPI",
                          _cfg.sck, _cfg.miso, _cfg.mosi, _cfg.ss);
            break;
        }
        case NfcIface::UART:
            // UART not supported by Adafruit_PN532 on ESP32
            Serial.println("[NFC] UART interface not supported");
            return false;
        default:
            return false;
    }

    pn->begin();
    _fwVersion = pn->getFirmwareVersion();
    if (!_fwVersion) {
        Serial.println("[NFC] PN532 not found - getFirmwareVersion() = 0");
        delete pn;
        return false;
    }

    pn->SAMConfig();   // configure to read RFID tags

    _nfc = pn;
    _hwConnected = true;
    Serial.printf("[NFC] PN532 connected - FW: %u.%u\n",
                  (_fwVersion >> 16) & 0xFF,
                  (_fwVersion >>  8) & 0xFF);
    return true;
}

void NfcModule::_deinitPN532() {
    if (_nfc) {
        delete _getNfc(_nfc);
        _nfc = nullptr;
    }
    if (_nfcSpi) {
        _nfcSpi->end();
        delete _nfcSpi;
        _nfcSpi = nullptr;
    }
    _hwConnected = false;
}

// ── Lifecycle ─────────────────────────────────────────────────
void NfcModule::setEnabled(bool en) {
    NfcGpioConfig cfg = loadGpioConfig();
    cfg.enabled = en;
    saveGpioConfig(cfg);
    _moduleEnabled = en;
    if (!en) {
        // Stop any active operation first
        _reading    = false;
        _emulating  = false;
        _dictRunning = false;
        // Properly release I2C/SPI bus
        _deinitPN532();
        // Release I2C bus if it was used
        if (cfg.iface == NfcIface::I2C) Wire.end();
        Serial.println("[NFC] Disabled - bus released");
    } else {
        reinit();
    }
}

void NfcModule::begin() {
    if (!_nfcMutex) _nfcMutex = xSemaphoreCreateMutex();
    _loadTags();
    _cfg = loadGpioConfig();
    // Skip 1-second I2C probe when NFC is disabled in config
    if (_cfg.enabled) _initPN532();
}

NfcTag NfcModule::lastTag() const {
    if (_nfcMutex) xSemaphoreTake(_nfcMutex, pdMS_TO_TICKS(100));
    NfcTag t = _lastTag;
    if (_nfcMutex) xSemaphoreGive(_nfcMutex);
    return t;
}

bool NfcModule::tagAvailable() const {
    if (_nfcMutex) xSemaphoreTake(_nfcMutex, pdMS_TO_TICKS(100));
    bool v = _tagReady;
    if (_nfcMutex) xSemaphoreGive(_nfcMutex);
    return v;
}

void NfcModule::clearTag() {
    if (_nfcMutex) xSemaphoreTake(_nfcMutex, pdMS_TO_TICKS(100));
    _tagReady = false;
    if (_nfcMutex) xSemaphoreGive(_nfcMutex);
}

void NfcModule::loop() {
    if (!_hwConnected || !_nfc) return;

    // Card emulation tick — keep PN532 armed in target mode.
    // When a reader approaches, tgGetData() receives the SELECT/READ command.
    // We respond with a HALT (0x50 0x00) to end the transaction gracefully.
    if (_emulating) {
        if (millis() - _emulateTimer >= 300) {
            _emulateTimer = millis();
            bool vspiEmu = (_cfg.iface == NfcIface::SPI && _cfg.spiBus != 1);
            if (vspiEmu) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
            Adafruit_PN532* pn = _getNfc(_nfc);
            uint8_t rxBuf[64] = {};
            uint8_t rxLen = sizeof(rxBuf);
            if (pn->getDataTarget(rxBuf, &rxLen)) {
                Serial.printf("[NFC] Emulate: reader sent %u bytes\n", rxLen);
                // Respond to HALT command (0x50 0x00 CRC) — end transaction
                if (rxLen >= 2 && rxBuf[0] == 0x50) {
                    stopEmulate();
                } else {
                    // ACK all other commands with an empty response
                    uint8_t empty[1] = {0};
                    pn->setDataTarget(empty, 0);
                }
            }
            // Re-arm: put PN532 back into target mode for next reader poll
            if (_emulating) {
                pn->AsTarget();
            }
            if (vspiEmu) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
        }
        return; // skip normal read/dict while emulating
    }

    if (_reading && millis() - _pollTimer >= NFC_POLL_MS) {
        _pollTimer = millis();
        bool vspi = (_cfg.iface == NfcIface::SPI && _cfg.spiBus != 1);
        if (vspi) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
        _pollForTag();
        if (vspi) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
    }

    // Dict attack tick
    if (_dictRunning && millis() - _dictTimer >= 200) {
        _dictTimer = millis();
        if (_dictSector < 16 && _dictKeyIdx < MIFARE_DICT_LEN) {
            // Try current key for current sector + key type (0=KeyA, 1=KeyB)
            bool vspiDict = (_cfg.iface == NfcIface::SPI && _cfg.spiBus != 1);
            if (vspiDict) xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(100));
            bool found = _tryMifareKey(_dictSector, _dictKeyType,
                                       MIFARE_DICT_KEYS[_dictKeyIdx]);
            if (vspiDict) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
            if (found) {
                String line = "S";
                line += _dictSector;
                line += ":Key";
                line += (_dictKeyType == 0 ? "A" : "B");
                line += ":";
                line += MIFARE_DICT_KEYS[_dictKeyIdx];
                line += "\n";
                _dictPending += line;
                _dictFoundCount++;
                // Key found for this sector+type -> move to next sector or KeyB phase
                if (_dictKeyType == 0 && _dictDoKeyB) {
                    // Try KeyB for same sector next
                    _dictKeyType = 1;
                    _dictKeyIdx  = 0;
                } else {
                    _dictSector++;
                    _dictKeyType = 0;
                    _dictKeyIdx  = 0;
                }
            } else {
                _dictKeyIdx++;
                if (_dictKeyIdx >= MIFARE_DICT_LEN) {
                    // All keys tried for this type - move to KeyB or next sector
                    if (_dictKeyType == 0 && _dictDoKeyB) {
                        _dictKeyType = 1;
                        _dictKeyIdx  = 0;
                    } else {
                        _dictSector++;
                        _dictKeyType = 0;
                        _dictKeyIdx  = 0;
                    }
                }
            }
            if (_dictSector >= 16) {
                _dictRunning = false;
                String summary = "DONE:";
                summary += _dictFoundCount;
                summary += "\n";
                _dictPending += summary;
                Serial.printf("[NFC] Dict attack complete - %u keys found\n", _dictFoundCount);
            }
        }
    }
}

void NfcModule::reinit() {
    _cfg = loadGpioConfig();
    _initPN532();
}

// ── Poll for NFC tag ──────────────────────────────────────────
void NfcModule::_pollForTag() {
    if (!_nfc) return;
    Adafruit_PN532* pn = _getNfc(_nfc);

    uint8_t uid[7]  = {};
    uint8_t uidLen  = 0;

    // FIX: Timeout reduced from 100ms to 10ms.
    // hw_poll task runs on a 20ms vTaskDelayUntil() tick. A 100ms blocking
    // readPassiveTargetID call stalls ALL hw_poll siblings (RFID, SubGHz, NRF24)
    // for 5× the intended tick rate - degrading scan responsiveness and potentially
    // causing spurious WDT module stall warnings.
    // 10ms is sufficient for PN532 tag presence detection (ATQA response is <1ms).
    if (!pn->readPassiveTargetID(PN532_MIFARE_ISO14443A,
                                  uid, &uidLen, 10)) return;

    NfcTag tag;
    tag.uid  = _fmtUid(uid, uidLen);
    // UID length heuristic: 4=MIFARE Classic 1K/4K, 7=MIFARE Ultralight/DESFire
    tag.type = (uidLen == 4) ? "MIFARE Classic" :
               (uidLen == 7) ? "MIFARE Ultralight" : "ISO14443A";
    tag.sak  = "";
    tag.atqa = "";

    // Read MIFARE blocks if Classic only (not Ultralight/NTAG)
    if (tag.type.indexOf("MIFARE Classic") != -1) {
        _readMifareBlocks(tag);
    }

    if (_nfcMutex) xSemaphoreTake(_nfcMutex, pdMS_TO_TICKS(100));
    _lastTag  = tag;
    _tagReady = true;
    _reading  = false;
    if (_nfcMutex) xSemaphoreGive(_nfcMutex);

    // Feature 20: NFC scan -> SD log
    if (sdMgr.isAvailable())
        sdMgr.log(String("[NFC] Tag: ") + tag.uid + " Type: " + tag.type, SdLogLevel::INFO, "NFC");

    // If this read was initiated by startCloneRead(), capture the source
    if (!_cloneReady) {
        _cloneSource = tag;
        _cloneReady  = true;
    }

    Serial.printf("[NFC] Tag read: UID=%s Type=%s\n",
                  tag.uid.c_str(), tag.type.c_str());

    // ── Auto-trigger rule engine + audit on every tag scan ───
    // Previously, NFC triggers only fired if /api/nfc/read was polled.
    // Now: tag detected in hw_poll task -> immediately fire rules and
    // broadcast to WebSocket, regardless of HTTP polling.
    //
    // Look up saved tag name for the rule trigger param.
    String tagName = "";
    // (NFC tags are stored in nfcModule's tag list - scan for match)
    for (const auto& saved : _tags) {
        if (saved.uid.equalsIgnoreCase(tag.uid)) {
            tagName = saved.name;
            break;
        }
    }
    auditMgr.logSystem(("NFC_SCAN:" + tag.uid).c_str());
    webUI.broadcastRaw(
        String("{\"event\":\"nfc\",\"uid\":\"") + tag.uid +
        "\",\"type\":\"" + tag.type +
        "\",\"name\":\"" + tagName + "\"}");
}

void NfcModule::_readMifareBlocks(NfcTag& tag) {
    Adafruit_PN532* pn = _getNfc(_nfc);
    if (!pn) return;

    uint8_t key[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t uid[4] = {};
    // PARSE FIX: tag.uid is formatted "AA:BB:CC:DD" so the last UID
    // byte sits at offsets length-2..length-1 (no trailing colon).
    // The previous integer-division (length/3) of "AA:BB:CC:DD" (len=11)
    // gave 3 — we parsed only 3 of 4 bytes, then MIFARE auth failed and
    // every block came back as "(locked)". Round up so the trailing byte
    // is included.
    int uidBytes = ((int)tag.uid.length() + 1) / 3;
    if (uidBytes > 4) uidBytes = 4;
    for (int i = 0; i < uidBytes; i++) {
        uid[i] = static_cast<uint8_t>(
            strtoul(tag.uid.substring(i*3, i*3+2).c_str(), nullptr, 16));
    }

    for (uint8_t block = 0; block < 64; block++) {
        if (!pn->mifareclassic_AuthenticateBlock(uid, uidBytes, block, 0, key)) {
            tag.blocks.push_back("(locked)");
            continue;
        }
        uint8_t data[16] = {};
        if (pn->mifareclassic_ReadDataBlock(block, data)) {
            char hex[33];
            for (int i = 0; i < 16; i++) snprintf(hex+i*2, 3, "%02X", data[i]);
            tag.blocks.push_back(String(hex));
        } else {
            tag.blocks.push_back("(error)");
        }
    }
}

// ── Read / Clone / Emulate ────────────────────────────────────
bool NfcModule::startRead() {
    if (!_hwConnected) return false;
    _reading   = true;
    _tagReady  = false;
    _pollTimer = millis();
    Serial.println("[NFC] Read started");
    return true;
}

void NfcModule::stopRead() {
    _reading   = false;
    _tagReady  = false;
}

bool NfcModule::startCloneRead() {
    if (!startRead()) return false;
    _cloneReady = false;
    return true;
}

bool NfcModule::writeClone() {
    if (!_hwConnected || !_cloneReady || !_nfc) return false;
    Adafruit_PN532* pn = _getNfc(_nfc);

    uint8_t uid[7] = {};
    uint8_t uidLen = 0;
    if (!pn->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 500))
        return false;

    uint8_t key[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    for (uint8_t block = 0; block < (uint8_t)_cloneSource.blocks.size(); block++) {
        if (_cloneSource.blocks[block] == "(locked)" ||
            _cloneSource.blocks[block] == "(error)") continue;
        if (!pn->mifareclassic_AuthenticateBlock(uid, uidLen, block, 0, key))
            continue;
        uint8_t data[16] = {};
        const String& hex = _cloneSource.blocks[block];
        for (int i = 0; i < 16 && i*2+1 < (int)hex.length(); i++) {
            data[i] = static_cast<uint8_t>(
                strtoul(hex.substring(i*2, i*2+2).c_str(), nullptr, 16));
        }
        pn->mifareclassic_WriteDataBlock(block, data);
    }
    Serial.println("[NFC] Clone write complete");
    return true;
}

bool NfcModule::startEmulate(uint32_t tagId) {
    if (!_nfc) return false;
    NfcTag tag;
    if (!getTag(tagId, tag)) return false;

    Adafruit_PN532* pn = _getNfc(_nfc);

    // AsTarget: put PN532 in card-emulation mode.
    // The library's AsTarget() arms the chip to respond as a passive NFC target.
    // loop() will poll getDataTarget() and re-arm as needed.
    pn->AsTarget();

    _emulating   = true;
    _emulatingId = tagId;
    _emulateTimer = millis();
    Serial.printf("[NFC] Emulating tag: %s\n", tag.uid.c_str());
    return true;
}

void NfcModule::stopEmulate() {
    _emulating = false;
    // Return PN532 to normal initiator mode so tag scanning resumes
    if (_nfc) {
        Adafruit_PN532* pn = _getNfc(_nfc);
        pn->SAMConfig(); // exits target mode and re-enables SAMConfig
    }
    Serial.println("[NFC] Emulation stopped");
}

// ── Dict attack ───────────────────────────────────────────────
void NfcModule::startDictAttack(bool keyB) {
    if (!_hwConnected) return;
    _dictRunning    = true;
    _dictSector     = 0;
    _dictKeyIdx     = 0;
    _dictKeyType    = 0;   // start with KeyA
    _dictDoKeyB     = keyB;
    _dictFoundCount = 0;
    _dictPending    = "";
    _dictTimer      = millis();
    _dictStartMs    = millis();
    Serial.println("[NFC] Dictionary attack started (KeyA" + String(keyB ? "+KeyB" : " only") + ")");
}

void NfcModule::stopDictAttack() {
    _dictRunning = false;
}

String NfcModule::dictProgressJson() const {
    // Total steps = 16 sectors x DICT_LEN keys x 2 key types (A+B)
    uint16_t totalKeys   = MIFARE_DICT_LEN * (_dictDoKeyB ? 2 : 1);
    uint16_t totalSteps  = 16 * totalKeys;
    // Current step = completed sectors * totalKeys + (keyType phase * DICT_LEN) + keyIdx
    uint16_t currentStep = (uint16_t)_dictSector * totalKeys
                         + (uint16_t)_dictKeyType * MIFARE_DICT_LEN
                         + _dictKeyIdx;
    uint8_t  pct         = (uint8_t)((uint32_t)currentStep * 100 / totalSteps);
    uint32_t elapsed     = (millis() - _dictStartMs) / 1000;
    String j = "{";
    j += "\"sector\":"    + String(_dictSector)     + ",";
    j += "\"keyIdx\":"    + String(_dictKeyIdx)     + ",";
    j += "\"keyType\":"   + String(_dictKeyType)    + ",";
    j += "\"totalKeys\":" + String(totalKeys)        + ",";
    j += "\"pct\":"       + String(pct)              + ",";
    j += "\"found\":"     + String(_dictFoundCount)  + ",";
    j += "\"elapsed\":"   + String(elapsed)          + ",";
    j += String("\"running\":") + (_dictRunning ? "true" : "false") + "}";
    return j;
}

String NfcModule::pollDictResult() {
    String r = _dictPending;
    _dictPending = "";
    return r;
}

bool NfcModule::_tryMifareKey(uint8_t sector, uint8_t keyType,
                               const char* keyHex) {
    if (!_nfc) return false;
    Adafruit_PN532* pn = _getNfc(_nfc);

    uint8_t uid[4] = {0,0,0,0};
    uint8_t uidLen = 0;
    if (!pn->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 200))
        return false;

    uint8_t key[6] = {};
    for (int i = 0; i < 6; i++) {
        char hex[3] = {keyHex[i*2], keyHex[i*2+1], 0};
        key[i] = static_cast<uint8_t>(strtoul(hex, nullptr, 16));
    }

    uint8_t block = static_cast<uint8_t>(sector * 4);
    return pn->mifareclassic_AuthenticateBlock(uid, uidLen, block, keyType, key);
}

// ── Flipper NFC format ────────────────────────────────────────
bool NfcModule::importFlipperNfc(const String& content, NfcTag& out) {
    int uidLine = content.indexOf("UID:");
    if (uidLine < 0) return false;

    int eol  = content.indexOf('\n', uidLine);
    String uidStr = content.substring(uidLine + 4, eol);
    uidStr.trim();
    // Convert "DE AD BE EF" to "DE:AD:BE:EF"
    uidStr.replace(" ", ":");
    out.uid = uidStr;

    int typeLine = content.indexOf("MIFARE type:");
    if (typeLine >= 0) {
        int eol2 = content.indexOf('\n', typeLine);
        out.type = content.substring(typeLine + 12, eol2);
        out.type.trim();
    }
    return true;
}

String NfcModule::exportFlipperNfc(uint32_t id) const {
    NfcTag tag;
    if (!getTag(id, tag)) return "";
    String uid = tag.uid;
    uid.replace(":", " ");
    String out = "Filetype: Flipper NFC device\nVersion: 2\n";
    out += "Device type: " + tag.type + "\n";
    out += "UID: " + uid + "\n";
    out += "ATQA: " + tag.atqa + "\n";
    out += "SAK: " + tag.sak + "\n";
    return out;
}

String NfcModule::parseTag(const NfcTag& tag) const {
    String s = "UID: " + tag.uid + "\nType: " + tag.type;
    if (!tag.blocks.empty()) {
        s += "\nBlocks: " + String((uint32_t)tag.blocks.size());
        s += "\nBlock 0: " + (tag.blocks.empty() ? "-" : tag.blocks[0]);
    }
    return s;
}

// ── Status JSON ───────────────────────────────────────────────
String NfcModule::statusJson() const {
    String s = "{\"connected\":";
    s += _hwConnected ? "true" : "false";
    s += ",\"firmware\":\"";
    if (_fwVersion) {
        s += String((_fwVersion>>16)&0xFF); s += ".";
        s += String((_fwVersion>> 8)&0xFF);
    }
    s += "\"";
    s += ",\"reading\":";   s += _reading   ? "true" : "false";
    s += ",\"emulating\":"; s += _emulating ? "true" : "false";
    if (_tagReady) {
        s += ",\"lastUID\":\""; s += _lastTag.uid; s += "\"";
    }
    s += "}";
    return s;
}

// ── Helpers ───────────────────────────────────────────────────
String NfcModule::_fmtUid(const uint8_t* uid, uint8_t len) const {
    String s;
    s.reserve(static_cast<uint16_t>(len * 3));
    for (uint8_t i = 0; i < len; i++) {
        if (i) s += ':';
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", uid[i]);
        s += hex;
    }
    return s;
}

String NfcModule::_detectType(uint8_t sak, const uint8_t* atqa) const {
    (void)atqa;
    if (sak == 0x08 || sak == 0x88) return "MIFARE Classic 1K";
    if (sak == 0x18)                 return "MIFARE Classic 4K";
    if (sak == 0x20)                 return "MIFARE DESFire";
    if (sak == 0x00)                 return "NTAG/Ultralight";
    return "Unknown NFC";
}

// ── Tag storage ───────────────────────────────────────────────
uint32_t NfcModule::saveTag(NfcTag& tag) {
    if (_tags.size() >= NFC_MAX_TAGS) return 0;
    tag.id = static_cast<uint32_t>(millis()) ^ esp_random();
    if (!tag.id) tag.id = 1;
    _tags.push_back(tag);
    _saveTags();
    return tag.id;
}

bool NfcModule::deleteTag(uint32_t id) {
    for (auto it = _tags.begin(); it != _tags.end(); ++it) {
        if (it->id == id) { _tags.erase(it); _saveTags(); return true; }
    }
    return false;
}

bool NfcModule::getTag(uint32_t id, NfcTag& out) const {
    for (const auto& t : _tags) {
        if (t.id == id) { out = t; return true; }
    }
    return false;
}

String NfcModule::tagsToJson() const {
    String out = "{\"tags\":[";
    bool first = true;
    for (const auto& t : _tags) {
        if (!first) out += ',';
        first = false;
        out += "{\"id\":"; out += t.id;
        out += ",\"name\":\""; out += t.name; out += "\"";
        out += ",\"label\":\""; out += t.name; out += "\"";
        out += ",\"uid\":\"";  out += t.uid;  out += "\"";
        out += ",\"type\":\""; out += t.type; out += "\"}";
    }
    out += "]}";
    return out;
}

void NfcModule::_loadTags() {
    _tags.clear();
    if (!LittleFS.exists(NFC_SAVE_FILE)) return;
    File f = LittleFS.open(NFC_SAVE_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        for (JsonObject o : doc["tags"].as<JsonArray>()) {
            NfcTag t;
            t.id   = o["id"]   | (uint32_t)0;
            t.name = o["name"] | (const char*)"";
            t.uid  = o["uid"]  | (const char*)"";
            t.type = o["type"] | (const char*)"";
            t.atqa = o["atqa"] | (const char*)"";
            t.sak  = o["sak"]  | (const char*)"";
            _tags.push_back(t);
        }
    }
    f.close();
}

void NfcModule::_saveTags() const {
    File f = LittleFS.open(NFC_SAVE_FILE, "w");
    if (!f) return;
    f.print("{\"tags\":[");
    bool first = true;
    for (const auto& t : _tags) {
        if (!first) f.print(',');
        first = false;
        f.printf("{\"id\":%u,\"name\":\"%s\",\"uid\":\"%s\","
                 "\"type\":\"%s\",\"atqa\":\"%s\",\"sak\":\"%s\"}",
                 t.id, t.name.c_str(), t.uid.c_str(),
                 t.type.c_str(), t.atqa.c_str(), t.sak.c_str());
    }
    f.print("]}");
    f.close();
}

void NfcModule::saveGpioConfig(const NfcGpioConfig& cfg) {
    File f = LittleFS.open(NFC_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["enabled"] = cfg.enabled;
    doc["iface"]  = static_cast<int>(cfg.iface);
    doc["spiBus"] = cfg.spiBus;
    doc["sda"] = cfg.sda; doc["scl"] = cfg.scl;
    doc["irq"] = cfg.irq; doc["reset"]= cfg.reset;
    doc["ss"]  = cfg.ss;  doc["sck"] = cfg.sck;
    doc["mosi"]= cfg.mosi;doc["miso"]= cfg.miso;
    doc["tx"]  = cfg.tx;  doc["rx"]  = cfg.rx;
    serializeJson(doc, f);
    f.close();
}

NfcGpioConfig NfcModule::loadGpioConfig() const {
    NfcGpioConfig cfg;
    if (!LittleFS.exists(NFC_GPIO_FILE)) return cfg;
    File f = LittleFS.open(NFC_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.enabled = doc["enabled"] | false;
        cfg.iface   = static_cast<NfcIface>(doc["iface"] | 0);
        cfg.spiBus  = doc["spiBus"] | (uint8_t)0;
        cfg.sda  = doc["sda"]  | (uint8_t)21;
        cfg.scl  = doc["scl"]  | (uint8_t)22;
        cfg.irq  = doc["irq"]  | (uint8_t)4;
        cfg.reset = doc["reset"] | (uint8_t)33;
        cfg.ss   = doc["ss"]   | (uint8_t)5;
        cfg.sck  = doc["sck"]  | (uint8_t)18;
        cfg.mosi = doc["mosi"] | (uint8_t)23;
        cfg.miso = doc["miso"] | (uint8_t)19;
        cfg.tx   = doc["tx"]   | (uint8_t)17;
        cfg.rx   = doc["rx"]   | (uint8_t)16;
    }
    f.close();
    return cfg;
}

// ── SD integration (features 18-21) ─────────────────────────

// Feature 18: backup NFC tag DB to SD
bool NfcModule::backupToSD(const String& tag) {
    if (!sdMgr.isAvailable()) return false;
    File src = LittleFS.open(NFC_SAVE_FILE, "r");
    if (!src) return false;
    sdMgr.makeDir("/backups/" + tag);
    File dst = sdMgr.openForWrite("/backups/" + tag + "/nfc_tags.json");
    if (!dst) { src.close(); return false; }
    uint8_t buf[256];
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        dst.write(buf, n);
    }
    src.close();
    dst.close();
    sdMgr.log("[NFC] Backup to SD tag=" + tag, SdLogLevel::INFO, "NFC");
    return true;
}

// Feature 18: restore NFC tag DB from SD
bool NfcModule::restoreFromSD(const String& tag) {
    if (!sdMgr.isAvailable()) return false;
    String sdPath = "/backups/" + tag + "/nfc_tags.json";
    File src = sdMgr.openForRead(sdPath);
    if (!src) return false;
    File dst = LittleFS.open(NFC_SAVE_FILE, "w");
    if (!dst) { src.close(); return false; }
    uint8_t buf[256];
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        dst.write(buf, n);
    }
    src.close();
    dst.close();
    begin(); // reload tags
    sdMgr.log("[NFC] Restore from SD tag=" + tag, SdLogLevel::INFO, "NFC");
    return true;
}

// Feature 19: export single tag as Flipper .nfc to SD
bool NfcModule::exportFlipperToSD(uint32_t tagId) {
    if (!sdMgr.isAvailable()) return false;
    String content = exportFlipperNfc(tagId);
    if (content.isEmpty()) return false;
    // Build filename from UID
    NfcTag tag;
    if (!getTag(tagId, tag)) return false;
    String uid = tag.uid;
    uid.replace(":", "");
    sdMgr.makeDir("/nfc");
    File f = sdMgr.openForWrite("/nfc/" + uid + ".nfc");
    if (!f) return false;
    f.print(content);
    f.close();
    sdMgr.log("[NFC] Flipper export: " + uid + ".nfc", SdLogLevel::INFO, "NFC");
    return true;
}

// Feature 19: export all tags as Flipper .nfc files to SD
bool NfcModule::exportAllFlipperToSD() {
    if (!sdMgr.isAvailable()) return false;
    sdMgr.makeDir("/nfc");
    bool ok = true;
    for (const auto& t : _tags) {
        if (!exportFlipperToSD(t.id)) ok = false;
    }
    return ok;
}

// Feature 21: list NFC files on SD
std::vector<String> NfcModule::listSdTags() const {
    std::vector<String> result;
    if (!sdMgr.isAvailable()) return result;
    auto entries = sdMgr.listDir("/nfc");
    for (const auto& e : entries) {
        if (!e.isDir) result.push_back(e.name);
    }
    return result;
}

// Feature 21: import a tag from SD Flipper file
bool NfcModule::importTagFromSD(const String& filename) {
    if (!sdMgr.isAvailable()) return false;
    String content = sdMgr.readTextFile("/nfc/" + filename);
    if (content.isEmpty()) return false;
    NfcTag tag;
    if (!importFlipperNfc(content, tag)) return false;
    // Use filename (minus .nfc) as tag name
    tag.name = filename;
    int dot = tag.name.lastIndexOf('.');
    if (dot > 0) tag.name = tag.name.substring(0, dot);
    saveTag(tag);
    sdMgr.log("[NFC] Imported from SD: " + filename, SdLogLevel::INFO, "NFC");
    return true;
}
