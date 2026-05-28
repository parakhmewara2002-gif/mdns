// ============================================================
//  ir_database.cpp
//
//  SD card integration features added:
//    Feature 1 - Auto-sync irDB to SD after LittleFS save
//    Feature 2 - SD overflow: spill button to SD when LittleFS is full
//    Feature 4 - Auto RAW dump to SD when autoRawDump is enabled
//
//  Storage improvements in this version:
//    Fix 1 - Auto-save received IR signals
//             autoSaveReceived() checks for duplicates (same
//             protocol + code) before adding to the DB.
//             Setting persisted to IR_AUTO_SAVE_FILE.
//
//    Fix 2 - Lazy save (batch flash writes)
//             add/update/remove/clear call _markDirty() instead
//             of save().  loop() flushes after DB_LAZY_SAVE_MS ms
//             of inactivity.  importJson/clear still flush
//             immediately because they replace the whole DB.
//
//    Fix 3 - RAW memory guard
//             add() rejects new RAW buttons when _rawCount()
//             >= MAX_RAW_BUTTONS (default 16).
//
//    Fix 4 - Streaming JSON save
//             save() serialises one IRButton at a time directly
//             to the LittleFS file.  Peak extra RAM ≈ 1 IRButton
//             (~1 KB) instead of a full snapshot vector
//             (up to 136 KB with 128 RAW buttons).
//
//  Pre-existing design kept intact:
//    * FreeRTOS mutex guards vector mutations (replaces portMUX spinlock).
//    * Atomic temp-file rename prevents partial-write corruption.
//    * Heap-alloc ops (push_back, erase, String copy) stay
//      outside the mutex lock (malloc never runs under lock).
// ============================================================
#include "ir_database.h"
#include "sd_manager.h"

// ── Global singleton ─────────────────────────────────────────
IRDatabase irDB;

// ── Constructor ──────────────────────────────────────────────
IRDatabase::IRDatabase()
    : _nextId(1),
      _mux(xSemaphoreCreateMutex()),
      _dirty(false),
      _dirtyMs(0),
      _autoSave(IR_AUTO_SAVE_DEFAULT)
{}

// ── begin ────────────────────────────────────────────────────
bool IRDatabase::begin() {
    _buttons.clear();
    // FIX: pre-allocate for MAX_BUTTONS to avoid 7+ reallocs during load.
    // std::vector doubles its capacity each time - without reserve(), loading
    // 100 buttons triggers log2(100)≈7 realloc+copy cycles on the heap.
    _buttons.reserve(MAX_BUTTONS);
    _nextId  = 1;
    _dirty   = false;
    _dirtyMs = 0;

    // Fix 1: restore auto-save preference from flash
    _loadAutoSave();

    if (!LittleFS.exists(DB_FILE)) {
        Serial.println(DEBUG_TAG " No DB file - starting with empty database.");
        return true;
    }

    File f = LittleFS.open(DB_FILE, "r");
    if (!f) {
        Serial.println(DEBUG_TAG " ERROR: Cannot open DB file for reading.");
        return false;
    }

    // Stream-parse directly from the file to minimise RAM usage
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err != DeserializationError::Ok) {
        Serial.printf(DEBUG_TAG " DB parse error: %s - starting fresh.\n",
                      err.c_str());
        return false;
    }

    if (!doc["buttons"].is<JsonArrayConst>()) {
        Serial.println(DEBUG_TAG " DB has no 'buttons' array - starting fresh.");
        return false;
    }

    std::vector<IRButton> loaded;
    uint32_t maxId = 0;
    for (JsonObjectConst obj : doc["buttons"].as<JsonArrayConst>()) {
        if ((int)loaded.size() >= MAX_BUTTONS) break;
        IRButton btn;
        if (btn.fromJson(obj)) {
            loaded.push_back(btn);
            if (btn.id > maxId) maxId = btn.id;
        }
    }

    xSemaphoreTake(_mux, portMAX_DELAY);
    _buttons = std::move(loaded);
    _nextId  = maxId + 1;
    xSemaphoreGive(_mux);

    _rebuildIndex();   // FIX: build O(1) lookup maps after full load

    Serial.printf(DEBUG_TAG " Loaded %d button(s) from DB (RAW: %u, autoSave: %s).\n",
                  (int)_buttons.size(), (unsigned)_rawCount(),
                  _autoSave ? "ON" : "OFF");
    return true;
}

// ── Fix 2: loop ───────────────────────────────────────────────
// Called from main loop() every iteration.
// Flushes the dirty flag to flash after DB_LAZY_SAVE_MS ms.
void IRDatabase::loop() {
    if (!_dirty) return;
    if ((millis() - _dirtyMs) >= DB_LAZY_SAVE_MS) {
        save();
        // save() clears _dirty on success; leave it set on failure
        // so we retry next time loop() fires.
    }
}

// ── Fix 2: _markDirty ────────────────────────────────────────
void IRDatabase::_markDirty() {
    if (!_dirty) {
        _dirty   = true;
        _dirtyMs = millis();
    }
    // If already dirty, keep the original timestamp so the
    // 5-second window counts from the FIRST unsaved change.
}

// ── Fix 4: save (streaming) ───────────────────────────────────
// Serialises one IRButton at a time directly to the temp file.
// Peak extra RAM ≈ 1 JsonDocument + 1 IRButton (~1.5 KB)
// regardless of how many buttons are in the DB.
bool IRDatabase::save() {
    const char* tmpFile = "/ir_db.tmp";

    File f = LittleFS.open(tmpFile, "w");
    if (!f) {
        Serial.println(DEBUG_TAG " ERROR: Cannot open temp DB file for writing.");
        return false;
    }

    // Write opening wrapper
    f.print("{\"buttons\":[");

    size_t totalWritten = 10; // length of "{\"buttons\":["
    bool   first        = true;

    // Snapshot the count and IDs under lock, then serialise each
    // button individually outside the lock to keep heap-alloc
    // (JsonDocument, String) away from the critical section.
    size_t count = 0;
    std::vector<uint32_t> ids;
    {
        xSemaphoreTake(_mux, portMAX_DELAY);
        count = _buttons.size();
        ids.reserve(count);
        for (size_t i = 0; i < count; ++i) ids.push_back(_buttons[i].id);
        xSemaphoreGive(_mux);
    }

    for (uint32_t id : ids) {
        // Safe copy outside spinlock (IRButton has String/vector members)
        IRButton btn = findById(id);
        if (!btn.id) continue;    // button was removed after we took the snapshot

        JsonDocument doc;
        JsonObject obj = doc.to<JsonObject>();
        btn.toJson(obj);

        if (!first) { f.print(","); totalWritten += 1; }
        first = false;
        size_t written = serializeJson(doc, f);
        if (written == 0) {
            Serial.println(DEBUG_TAG " ERROR: Button serialization produced 0 bytes.");
            f.close();
            LittleFS.remove(tmpFile);
            return false;
        }
        totalWritten += written;
    }

    f.print("]}");
    totalWritten += 2;
    f.close();

    // Atomic rename - prevents partial-write corruption on power loss
    LittleFS.remove(DB_FILE);
    if (!LittleFS.rename(tmpFile, DB_FILE)) {
        Serial.println(DEBUG_TAG " ERROR: DB rename failed.");
        return false;
    }

    _dirty = false;   // clear dirty flag on successful save
    Serial.printf(DEBUG_TAG " DB saved (%u button(s), %u bytes).\n",
                  (unsigned)count, (unsigned)totalWritten);

    // Feature 1: Auto-sync irDB to SD after successful LittleFS save
#ifndef SD_AUTO_SYNC_DISABLE
    if (sdMgr.isAvailable()) {
        sdMgr.log("irDB auto-sync to SD triggered", SdLogLevel::INFO, "IRDB");
        sdMgr.exportIRLibrary("auto_sync");
    }
#endif

    return true;
}

// ── Fix 3: _rawCount ─────────────────────────────────────────
uint8_t IRDatabase::_rawCount() const {
    uint8_t n = 0;
    for (const auto& b : _buttons)
        if (b.protocol == IRProtocol::RAW) ++n;
    return n;
}

// ── add ──────────────────────────────────────────────────────
uint32_t IRDatabase::add(IRButton& btn) {
    if (!btn.isValid()) {
        Serial.println(DEBUG_TAG " ERROR: Button failed validation.");
        return 0;
    }

    // Feature 2: SD overflow - if LittleFS is nearly full, spill button to SD
    if ((LittleFS.totalBytes() - LittleFS.usedBytes()) < 4096) {
        if (sdMgr.isAvailable()) {
            Serial.println(DEBUG_TAG " WARNING: LittleFS nearly full - spilling button to SD overflow library.");
            sdMgr.log("LittleFS full - button spilled to SD overflow library", SdLogLevel::WARN, "IRDB");
            _sdOverflow = true;
            // Assign a temporary id for the library save (will not be persisted to LittleFS)
            btn.id = _nextId;
            sdMgr.saveButtonToLibrary("overflow", btn.id);
            return 0;   // button not added to LittleFS DB
        }
    }

    // MUTEX FIX: all reads of _buttons (size, _rawCount) plus the mutation
    // (push_back + rebuildIndex) happen under one critical section so a
    // reader holding _mux can't observe a torn vector. _mux is a FreeRTOS
    // mutex (not portMUX); heap allocs are safe to perform under it.
    bool full = false, rawFull = false;
    {
        xSemaphoreTake(_mux, portMAX_DELAY);
        if ((int)_buttons.size() >= MAX_BUTTONS) {
            full = true;
        } else if (btn.protocol == IRProtocol::RAW &&
                   _rawCount() >= MAX_RAW_BUTTONS) {
            rawFull = true;
        } else {
            btn.id = newId();
            _buttons.push_back(btn);
            _rebuildIndex();
        }
        xSemaphoreGive(_mux);
    }
    if (full) {
        Serial.println(DEBUG_TAG " ERROR: MAX_BUTTONS limit reached.");
        return 0;
    }
    if (rawFull) {
        Serial.printf(DEBUG_TAG " ERROR: RAW button limit (%d) reached - "
                      "delete an existing RAW button first.\n", MAX_RAW_BUTTONS);
        return 0;
    }

    // Fix 2: mark dirty instead of saving immediately
    _markDirty();

    Serial.printf(DEBUG_TAG " Added button id=%u name='%s'%s\n",
                  btn.id, btn.name.c_str(),
                  btn.protocol == IRProtocol::RAW ? " [RAW]" : "");
    return btn.id;
}

// ── update ───────────────────────────────────────────────────
bool IRDatabase::update(uint32_t id, const IRButton& updated) {
    bool found = false;
    IRButton replacement = updated;
    replacement.id = id;

    xSemaphoreTake(_mux, portMAX_DELAY);
    for (IRButton& b : _buttons) {
        if (b.id == id) {
            std::swap(b, replacement);
            found = true;
            break;
        }
    }
    xSemaphoreGive(_mux);

    if (!found) {
        Serial.printf(DEBUG_TAG " update: id=%u not found.\n", id);
        return false;
    }

    // Fix 2: mark dirty instead of saving immediately
    _markDirty();

    Serial.printf(DEBUG_TAG " Updated button id=%u\n", id);
    return true;
}

// ── remove ───────────────────────────────────────────────────
bool IRDatabase::remove(uint32_t id) {
    int foundIdx = -1;
    // MUTEX FIX: hold _mux across the whole mutation (search + swap +
    // pop_back + rebuildIndex). Previous version released after the swap,
    // leaving pop_back/_rebuildIndex running unlocked while readers held
    // the same _mux.
    xSemaphoreTake(_mux, portMAX_DELAY);
    for (int i = 0; i < (int)_buttons.size(); ++i) {
        if (_buttons[i].id == id) { foundIdx = i; break; }
    }
    if (foundIdx >= 0) {
        std::swap(_buttons[foundIdx], _buttons.back());
        _buttons.pop_back();
        _rebuildIndex();
    }
    xSemaphoreGive(_mux);

    if (foundIdx < 0) {
        Serial.printf(DEBUG_TAG " remove: id=%u not found.\n", id);
        return false;
    }

    // Fix 2: mark dirty instead of saving immediately
    _markDirty();

    Serial.printf(DEBUG_TAG " Deleted button id=%u\n", id);
    return true;
}

// ── _rebuildIndex ─────────────────────────────────────────────
// O(N) rebuild - called only after mutations (add/update/remove/import/clear).
// Amortises to O(1) per lookup. With MAX_BUTTONS=200 the rebuild takes ~50μs.
void IRDatabase::_rebuildIndex() {
    _idIndex.clear();
    _nameIndex.clear();
    _idIndex.reserve(_buttons.size());
    _nameIndex.reserve(_buttons.size());
    for (size_t i = 0; i < _buttons.size(); ++i) {
        _idIndex[_buttons[i].id]               = i;
        _nameIndex[_buttons[i].name.c_str()]   = i;
    }
}

// ── findById - O(1) via hash map ─────────────────────────────
IRButton IRDatabase::findById(uint32_t id) const {
    IRButton copy;
    xSemaphoreTake(_mux, portMAX_DELAY);
    auto it = _idIndex.find(id);
    if (it != _idIndex.end() && it->second < _buttons.size()) {
        copy = _buttons[it->second];
    }
    xSemaphoreGive(_mux);
    return copy;
}

// ── findByName - O(1) via hash map ───────────────────────────
IRButton IRDatabase::findByName(const String& name) const {
    IRButton copy;
    xSemaphoreTake(_mux, portMAX_DELAY);
    auto it = _nameIndex.find(name.c_str());
    if (it != _nameIndex.end() && it->second < _buttons.size()) {
        copy = _buttons[it->second];
    }
    xSemaphoreGive(_mux);
    return copy;
}

// ── Fix 1: autoSaveReceived ───────────────────────────────────
// Called from the IR receive callback (main.cpp onIRReceived).
// Adds the button to the DB only when:
//   1. autoSave is enabled
//   2. The (protocol, code) pair is not already in the DB
//   3. The DB is not full (MAX_BUTTONS / MAX_RAW_BUTTONS checks
//      are done inside add())
uint32_t IRDatabase::autoSaveReceived(IRButton& btn) {
    if (!_autoSave) return 0;

    // RAW signals have code == 0 - each capture is unique by
    // definition, so skip the duplicate check for RAW.
    // MUTEX FIX: iterate under _mux. Previously the IR-receiver callback
    // could be reading _buttons while an API task was reallocating it.
    if (btn.protocol != IRProtocol::RAW) {
        xSemaphoreTake(_mux, portMAX_DELAY);
        bool dup = false;
        for (const auto& b : _buttons) {
            if (b.protocol == btn.protocol && b.code == btn.code) {
                dup = true;
                break;
            }
        }
        xSemaphoreGive(_mux);
        if (dup) return 0;
    }

    uint32_t id = add(btn);
    if (id) {
        Serial.printf(DEBUG_TAG " Auto-saved: id=%u '%s'\n",
                      id, btn.name.c_str());

        // Feature 4: Auto RAW dump to SD if enabled and button is RAW type
        if (btn.protocol == IRProtocol::RAW
            && sdMgr.isAvailable()
            && sdMgr.autoRawDumpEnabled()
            && !btn.rawData.empty()) {
            sdMgr.log(String("Auto RAW dump: ") + btn.name, SdLogLevel::INFO, "IRDB");
            sdMgr.saveRawDump(btn.name, btn.rawData.data(), btn.rawData.size(), btn.freqKHz);
        }
    }
    return id;
}

// ── Fix 1: setAutoSave ───────────────────────────────────────
void IRDatabase::setAutoSave(bool en) {
    _autoSave = en;
    _saveAutoSave();
    Serial.printf(DEBUG_TAG " Auto-save %s\n", en ? "ENABLED" : "DISABLED");
}

// ── Fix 1: persist / restore auto-save setting ───────────────
bool IRDatabase::_saveAutoSave() {
    File f = LittleFS.open(IR_AUTO_SAVE_FILE, "w");
    if (!f) return false;
    f.printf("{\"autoSave\":%s}", _autoSave ? "true" : "false");
    f.close();
    return true;
}

bool IRDatabase::_loadAutoSave() {
    if (!LittleFS.exists(IR_AUTO_SAVE_FILE)) return false;
    File f = LittleFS.open(IR_AUTO_SAVE_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return false; }
    f.close();
    _autoSave = doc["autoSave"] | IR_AUTO_SAVE_DEFAULT;
    return true;
}

// ── exportJson ───────────────────────────────────────────────
String IRDatabase::exportJson() const {
    std::vector<IRButton> snapshot;
    xSemaphoreTake(_mux, portMAX_DELAY);
    snapshot = _buttons;
    xSemaphoreGive(_mux);

    JsonDocument doc;
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (const IRButton& btn : snapshot) {
        JsonObject obj = arr.add<JsonObject>();
        btn.toJson(obj);
    }
    String out;
    serializeJsonPretty(doc, out);
    return out;
}

// ── compactJson ──────────────────────────────────────────────
String IRDatabase::compactJson() const {
    std::vector<IRButton> snapshot;
    xSemaphoreTake(_mux, portMAX_DELAY);
    snapshot = _buttons;
    xSemaphoreGive(_mux);

    JsonDocument doc;
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (const IRButton& btn : snapshot) {
        JsonObject obj = arr.add<JsonObject>();
        btn.toJson(obj);
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ── importJson ───────────────────────────────────────────────
bool IRDatabase::importJson(const String& json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err != DeserializationError::Ok) {
        Serial.printf(DEBUG_TAG " Import parse error: %s\n", err.c_str());
        return false;
    }
    if (!doc["buttons"].is<JsonArrayConst>()) {
        Serial.println(DEBUG_TAG " Import: missing 'buttons' array.");
        return false;
    }

    std::vector<IRButton> imported;
    uint32_t maxId = 0;
    for (JsonObjectConst obj : doc["buttons"].as<JsonArrayConst>()) {
        if ((int)imported.size() >= MAX_BUTTONS) break;
        IRButton btn;
        if (btn.fromJson(obj) && btn.isValid()) {
            imported.push_back(btn);
            if (btn.id > maxId) maxId = btn.id;
        }
    }

    std::vector<IRButton> old;
    xSemaphoreTake(_mux, portMAX_DELAY);
    old      = std::move(_buttons);
    _buttons = std::move(imported);
    _nextId  = maxId + 1;
    xSemaphoreGive(_mux);
    // 'old' destructs here - heap free is safe outside the spinlock

    _rebuildIndex();   // FIX: new index for imported buttons

    // Import always flushes immediately (entire DB replaced)
    _dirty = false;
    save();
    Serial.printf(DEBUG_TAG " Imported %d button(s).\n", (int)_buttons.size());
    return true;
}

// ── mergeJson ────────────────────────────────────────────────
// Merges buttons from JSON into the existing DB without wiping it.
// Buttons whose name already exists in DB are skipped.
// IDs are re-assigned to avoid conflicts with existing buttons.
// Returns number of buttons added, or -1 on parse error.
int IRDatabase::mergeJson(const String& json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err != DeserializationError::Ok) {
        Serial.printf(DEBUG_TAG " Merge parse error: %s\n", err.c_str());
        return -1;
    }
    if (!doc["buttons"].is<JsonArrayConst>()) {
        Serial.println(DEBUG_TAG " Merge: missing 'buttons' array.");
        return -1;
    }

    int added = 0;
    for (JsonObjectConst obj : doc["buttons"].as<JsonArrayConst>()) {
        if ((int)_buttons.size() >= MAX_BUTTONS) break;
        IRButton btn;
        if (!btn.fromJson(obj) || !btn.isValid()) continue;
        // Skip if name already exists
        if (findByName(btn.name).id != 0) continue;
        // Re-assign ID to avoid conflicts
        btn.id = 0;  // add() will assign a fresh ID
        if (add(btn) != 0) ++added;
    }

    if (added > 0) {
        _dirty = false;
        save();
    }
    Serial.printf(DEBUG_TAG " Merged %d button(s).\n", added);
    return added;
}
void IRDatabase::clear() {
    xSemaphoreTake(_mux, portMAX_DELAY);
    _buttons.clear();
    _nextId = 1;
    xSemaphoreGive(_mux);
    _rebuildIndex();   // FIX: clear indexes after wipe

    // Clear always flushes immediately
    _dirty = false;
    save();
    Serial.println(DEBUG_TAG " Database cleared.");
}

// ── backup ───────────────────────────────────────────────────
// Writes the current live DB to DB_BACKUP_FILE using the same
// streaming strategy as save() - one button at a time so peak
// RAM stays at ~1 IRButton regardless of database size.
bool IRDatabase::backup() {
    // Snapshot IDs under lock; serialise outside
    std::vector<uint32_t> ids;
    {
        xSemaphoreTake(_mux, portMAX_DELAY);
        ids.reserve(_buttons.size());
        for (const auto& b : _buttons) ids.push_back(b.id);
        xSemaphoreGive(_mux);
    }

    const char* tmpFile = "/ir_bak.tmp";
    File f = LittleFS.open(tmpFile, "w");
    if (!f) {
        Serial.println(DEBUG_TAG " [Backup] ERROR: cannot open temp backup file.");
        return false;
    }

    f.print("{\"buttons\":[");
    bool first = true;
    for (uint32_t id : ids) {
        IRButton btn = findById(id);
        if (!btn.id) continue;
        JsonDocument doc;
        JsonObject obj = doc.to<JsonObject>();
        btn.toJson(obj);
        if (!first) f.print(",");
        first = false;
        serializeJson(doc, f);
    }
    f.print("]}");
    f.close();

    // Atomic rename to backup file
    LittleFS.remove(DB_BACKUP_FILE);
    if (!LittleFS.rename(tmpFile, DB_BACKUP_FILE)) {
        Serial.println(DEBUG_TAG " [Backup] ERROR: rename to backup file failed.");
        LittleFS.remove(tmpFile);
        return false;
    }

    Serial.printf(DEBUG_TAG " [Backup] Created %s (%u button(s))\n",
                  DB_BACKUP_FILE, (unsigned)ids.size());
    return true;
}

// ── validateRestoreJson ──────────────────────────────────────
// Validates a JSON string without modifying any state.
// Checks:
//   1. Valid JSON syntax
//   2. Top-level "buttons" array present
//   3. At least one button passes fromJson() + isValid()
//   4. No RAW-only DB (all buttons RAW would exhaust heap)
// Returns a RestoreResult with accepted/rejected counts and
// an error string on failure.
IRDatabase::RestoreResult
IRDatabase::validateRestoreJson(const String& json) const {
    RestoreResult res{false, 0, 0, ""};

    // ── 1. JSON syntax ────────────────────────────────────────
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err != DeserializationError::Ok) {
        res.error = String("JSON parse error: ") + err.c_str();
        Serial.printf(DEBUG_TAG " [Restore] Validation FAILED - %s\n",
                      res.error.c_str());
        return res;
    }

    // ── 2. Structure ──────────────────────────────────────────
    if (!doc["buttons"].is<JsonArrayConst>()) {
        res.error = "Missing top-level 'buttons' array";
        Serial.println(DEBUG_TAG " [Restore] Validation FAILED - " + res.error);
        return res;
    }

    JsonArrayConst arr = doc["buttons"].as<JsonArrayConst>();
    if (arr.size() == 0) {
        // Empty array is technically valid - allow it (restores to blank DB)
        res.ok       = true;
        res.accepted = 0;
        Serial.println(DEBUG_TAG " [Restore] Validation OK - empty buttons array");
        return res;
    }

    // ── 3. Per-button validation ──────────────────────────────
    uint8_t rawCount  = 0;
    uint8_t totalSeen = 0;
    for (JsonObjectConst obj : arr) {
        if (totalSeen >= MAX_BUTTONS) break;   // stop counting at the cap
        totalSeen++;
        IRButton btn;
        if (!btn.fromJson(obj) || !btn.isValid()) {
            res.rejected++;
            Serial.printf(DEBUG_TAG " [Restore] Button #%u rejected"
                          " (fromJson/isValid failed)\n", totalSeen);
            continue;
        }
        if (btn.protocol == IRProtocol::RAW) rawCount++;
        res.accepted++;
    }

    if (res.accepted == 0) {
        res.error = String("No valid buttons found (") +
                    res.rejected + " rejected)";
        Serial.println(DEBUG_TAG " [Restore] Validation FAILED - " + res.error);
        return res;
    }

    // ── 4. RAW memory guard ───────────────────────────────────
    if (rawCount > MAX_RAW_BUTTONS) {
        res.error = String("Too many RAW buttons (") + rawCount +
                    "), max allowed: " + MAX_RAW_BUTTONS;
        Serial.println(DEBUG_TAG " [Restore] Validation FAILED - " + res.error);
        return res;
    }

    res.ok = true;
    Serial.printf(DEBUG_TAG " [Restore] Validation OK - accepted=%u rejected=%u"
                  " raw=%u\n", res.accepted, res.rejected, rawCount);
    return res;
}

// ── restore ──────────────────────────────────────────────────
// Full pipeline: validate -> backup -> importJson (atomic swap).
// The live DB is never touched if validation fails.
IRDatabase::RestoreResult IRDatabase::restore(const String& json) {
    // Step 1 - validate before touching anything
    RestoreResult res = validateRestoreJson(json);
    if (!res.ok) return res;

    // Step 2 - backup current DB so the user can undo
    if (!backup()) {
        // Backup failure is non-fatal - warn but continue.
        // Losing the backup is better than refusing a valid restore.
        Serial.println(DEBUG_TAG " [Restore] WARNING: backup() failed - "
                       "proceeding without backup.");
        res.error = "WARNING: backup failed (restore proceeded anyway)";
    }

    // Step 3 - atomic swap via importJson
    // importJson() validates again internally; this is intentional -
    // it re-checks after backup so the live DB is always consistent.
    if (!importJson(json)) {
        res.ok    = false;
        res.error = "importJson() failed after backup - live DB unchanged";
        Serial.println(DEBUG_TAG " [Restore] ERROR: " + res.error);
        return res;
    }

    Serial.printf(DEBUG_TAG " [Restore] SUCCESS - %u button(s) loaded, "
                  "%u skipped\n", res.accepted, res.rejected);
    return res;
}

// ── backupJson ───────────────────────────────────────────────
// Returns the backup file as a pretty-printed JSON string for
// the /api/backup/download endpoint.
String IRDatabase::backupJson() const {
    if (!LittleFS.exists(DB_BACKUP_FILE)) {
        Serial.println(DEBUG_TAG " [Backup] No backup file found.");
        return "";
    }
    File f = LittleFS.open(DB_BACKUP_FILE, "r");
    if (!f) return "";
    String out = f.readString();
    f.close();
    return out;
}

bool IRDatabase::hasMatchingButton(IRProtocol protocol, uint64_t code) const {
    xSemaphoreTake(_mux, portMAX_DELAY);
    bool found = false;
    for (const auto& b : _buttons) {
        if (b.protocol == protocol && b.code == code) { found = true; break; }
    }
    xSemaphoreGive(_mux);
    return found;
}
