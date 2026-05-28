// ============================================================
//  macro_manager.cpp  -  Internal LittleFS macro subsystem
//  v2.2.0
// ============================================================
#include "macro_manager.h"

MacroManager macroMgr;

MacroManager::MacroManager()
    : _running(false), _stepIdx(0), _stepAt(0) {}

// ── begin ────────────────────────────────────────────────────
void MacroManager::begin() {
    if (!LittleFS.exists(MACRO_DIR)) {
        LittleFS.mkdir(MACRO_DIR);
        Serial.println(DEBUG_TAG " [Macro] Created /macros dir");
    }
    Serial.println(DEBUG_TAG " [Macro] MacroManager ready");
}

// ── loop ─────────────────────────────────────────────────────
void MacroManager::loop() {
    if (!_running || _steps.empty()) return;

    unsigned long now = millis();
    if (now < _stepAt) return;   // not yet time for next step

    if (_stepIdx >= _steps.size()) {
        // Macro finished
        Serial.printf(DEBUG_TAG " [Macro] '%s' complete (%u steps)\n",
                      _runName.c_str(), (unsigned)_steps.size());
        _running = false;
        return;
    }

    const MacroInternalStep& s = _steps[_stepIdx];
    Serial.printf(DEBUG_TAG " [Macro] step %u/%u  btn=%u  delayAfter=%ums\n",
                  _stepIdx + 1, (unsigned)_steps.size(),
                  s.buttonId, s.delayAfterMs);

    if (_txCb) _txCb(s.buttonId);

    _stepIdx++;
    uint32_t d = (s.delayAfterMs > MACRO_STEP_MAX_DELAY)
                 ? MACRO_STEP_MAX_DELAY : s.delayAfterMs;
    _stepAt = now + d;
}

// ── _path ────────────────────────────────────────────────────
String MacroManager::_path(const String& name) const {
    return String(MACRO_DIR) + "/" + name + ".json";
}

// ── _validName ───────────────────────────────────────────────
bool MacroManager::_validName(const String& name) const {
    if (name.isEmpty() || name.length() > MACRO_NAME_MAX) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        char c = name[i];
        if (!isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

// ── list ─────────────────────────────────────────────────────
std::vector<MacroInternalMeta> MacroManager::list() const {
    std::vector<MacroInternalMeta> result;
    File dir = LittleFS.open(MACRO_DIR);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();   // HANDLE FIX: close dir on non-dir path
        return result;
    }

    File f = dir.openNextFile();
    while (f) {
        String fname = String(f.name());
        f.close();

        // name() returns just the filename on ESP32 LittleFS
        if (!fname.endsWith(".json")) { f = dir.openNextFile(); continue; }
        String name = fname.substring(0, fname.length() - 5); // strip .json

        String label;
        std::vector<MacroInternalStep> steps;
        if (load(name, label, steps)) {
            MacroInternalMeta m;
            m.name      = name;
            m.label     = label;
            m.stepCount = (uint8_t)steps.size();
            result.push_back(m);
        }
        f = dir.openNextFile();
    }
    // HANDLE FIX: close the directory handle. Previously leaked one
    // LittleFS handle per list() call (default max_open_files ~5);
    // after a few rapid /api/macros calls subsequent SD/FS opens failed.
    dir.close();
    return result;
}

// ── load ─────────────────────────────────────────────────────
bool MacroManager::load(const String& name,
                         String& outLabel,
                         std::vector<MacroInternalStep>& outSteps) const {
    if (!_validName(name)) return false;
    String path = _path(name);
    if (!LittleFS.exists(path)) return false;

    File f = LittleFS.open(path, "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err != DeserializationError::Ok) return false;

    outLabel = doc["name"] | (const char*)name.c_str();
    outSteps.clear();

    if (!doc["steps"].is<JsonArrayConst>()) return false;
    for (JsonObjectConst step : doc["steps"].as<JsonArrayConst>()) {
        MacroInternalStep s;
        s.buttonId    = step["buttonId"]    | (uint32_t)0;
        s.delayAfterMs= step["delayAfterMs"]| (uint32_t)200;
        if (s.buttonId == 0) continue;   // skip invalid steps
        if (s.delayAfterMs > MACRO_STEP_MAX_DELAY)
            s.delayAfterMs = MACRO_STEP_MAX_DELAY;
        outSteps.push_back(s);
        if (outSteps.size() >= MACRO_MAX_STEPS) break;
    }
    return !outSteps.empty();
}

// ── save ─────────────────────────────────────────────────────
String MacroManager::save(const String& name,
                           const uint8_t* data, size_t len) {
    if (!_validName(name))
        return "Invalid macro name (use [A-Za-z0-9_-] only)";

    // Count existing macros to enforce limit (exclude current if overwrite)
    auto existing = list();
    bool isOverwrite = false;
    for (const auto& m : existing)
        if (m.name == name) { isOverwrite = true; break; }
    if (!isOverwrite && existing.size() >= MACRO_MAX_INTERNAL)
        return "Macro limit reached (" + String(MACRO_MAX_INTERNAL) + ")";

    // Validate JSON before writing
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err != DeserializationError::Ok)
        return String("JSON parse error: ") + err.c_str();
    if (!doc["steps"].is<JsonArrayConst>())
        return "Missing 'steps' array";
    if (doc["steps"].as<JsonArrayConst>().size() == 0)
        return "Macro has no steps";

    // Write to LittleFS
    File f = LittleFS.open(_path(name), "w");
    if (!f) return "LittleFS write failed";
    f.write(data, len);
    f.close();

    Serial.printf(DEBUG_TAG " [Macro] Saved '%s' (%u bytes)\n",
                  name.c_str(), (unsigned)len);
    return "";
}

// ── remove ───────────────────────────────────────────────────
bool MacroManager::remove(const String& name) {
    if (!_validName(name)) return false;
    String path = _path(name);
    if (!LittleFS.exists(path)) return false;
    bool ok = LittleFS.remove(path);
    if (ok) Serial.printf(DEBUG_TAG " [Macro] Deleted '%s'\n", name.c_str());
    return ok;
}

// ── run ──────────────────────────────────────────────────────
bool MacroManager::run(const String& name) {
    if (_running) {
        Serial.println(DEBUG_TAG " [Macro] Already running - abort first");
        return false;
    }
    String label;
    std::vector<MacroInternalStep> steps;
    if (!load(name, label, steps)) {
        Serial.printf(DEBUG_TAG " [Macro] Not found: '%s'\n", name.c_str());
        return false;
    }
    _steps   = steps;
    _runName = name;
    _stepIdx = 0;
    _stepAt  = millis();   // fire first step immediately
    _running = true;
    Serial.printf(DEBUG_TAG " [Macro] Starting '%s' (%u steps)\n",
                  name.c_str(), (unsigned)_steps.size());
    return true;
}

// ── abort ────────────────────────────────────────────────────
void MacroManager::abort() {
    if (!_running) return;
    Serial.printf(DEBUG_TAG " [Macro] Aborted '%s' at step %u\n",
                  _runName.c_str(), _stepIdx);
    _running = false;
    _steps.clear();
}
