// ============================================================
//  audit_manager.cpp  -  Audit Trail / Event Log System
//  Batch 1 - Logs every significant event with timestamp
// ============================================================
#include "audit_manager.h"
#include "scheduler.h"   // for NTP time
#include "sd_manager.h"  // Feature 36-38: SD integration
#include <ctime>

#define AUDIT_SAVE_INTERVAL_MS  30000UL   // flush to flash every 30s if dirty
#define AUDIT_TAG               "[AUDIT]"

AuditManager auditMgr;

// ─────────────────────────────────────────────────────────────
AuditManager::AuditManager()
    : _nextId(1), _totalLogged(0), _dirty(false), _lastSave(0) {
    // MUTEX FIX: create the entries mutex up front. Static-init order:
    // global auditMgr constructs before main(), and FreeRTOS APIs for
    // mutex creation are safe to call this early on arduino-esp32 (the
    // RTOS is already up).
    _mux = xSemaphoreCreateMutex();
}

// ─────────────────────────────────────────────────────────────
void AuditManager::begin() {
    _load();
    // Always log a boot event
    logSystem("BOOT");
    Serial.printf(AUDIT_TAG " Started - %u entries loaded\n", (unsigned)_entries.size());
}

// ─────────────────────────────────────────────────────────────
void AuditManager::loop() {
    if (_dirty && (millis() - _lastSave >= AUDIT_SAVE_INTERVAL_MS)) {
        // LittleFS write needs ~8KB heap for its internal page buffer.
        // Skip this tick if heap is low — next tick will retry.
        if (ESP.getFreeHeap() >= 24000) save();
    }
}

// ─────────────────────────────────────────────────────────────
//  Core log function
// ─────────────────────────────────────────────────────────────
void AuditManager::log(AuditSource source,
                        const String& event,
                        const String& detail,
                        bool success) {
    AuditEntry e;
    // MUTEX FIX: _nextId increment must also be guarded.
    if (_mux) xSemaphoreTake(_mux, portMAX_DELAY);
    e.id        = _nextId++;
    if (_mux) xSemaphoreGive(_mux);
    e.timestamp = _getTimestamp();
    e.timeStr   = _buildTimeStr();
    e.source    = source;
    e.sourceStr = _sourceToStr(source);
    e.event     = event;
    e.detail    = detail.length() > AUDIT_MAX_DETAIL
                    ? detail.substring(0, AUDIT_MAX_DETAIL)
                    : detail;
    e.success   = success;

    if (_mux) xSemaphoreTake(_mux, portMAX_DELAY);
    _addEntry(e);
    if (_mux) xSemaphoreGive(_mux);

    Serial.printf(AUDIT_TAG " [%s] %s - %s (%s)\n",
                  e.sourceStr.c_str(),
                  e.event.c_str(),
                  e.detail.c_str(),
                  success ? "OK" : "FAIL");

    // Feature 38: SD mirror - write every entry to SD log in real-time
    if (_sdMirror && sdMgr.isAvailable()) {
        sdMgr.log(String("[AUDIT] ") + event + " | " + detail,
                  SdLogLevel::INFO, "AUDIT");
    }
}

// ─────────────────────────────────────────────────────────────
//  Convenience wrappers
// ─────────────────────────────────────────────────────────────
void AuditManager::logIrTx(const String& buttonName, uint32_t buttonId) {
    log(AuditSource::IR_TX, "IR_TRANSMIT",
        String("Button: ") + buttonName + " (id=" + buttonId + ")");
}

void AuditManager::logIrRx(const String& protocol, const String& code) {
    log(AuditSource::IR_RX, "IR_RECEIVED",
        String("Protocol: ") + protocol + " Code: " + code);
}

void AuditManager::logRfid(const String& uid, const String& cardName, bool known) {
    String detail = String("UID: ") + uid;
    if (known)  detail += " Name: " + cardName;
    else        detail += " (UNKNOWN CARD)";
    log(AuditSource::RFID, known ? "RFID_KNOWN" : "RFID_UNKNOWN", detail, known);
}

void AuditManager::logNfc(const String& uid, const String& tagName, bool known) {
    String detail = String("UID: ") + uid;
    if (known)  detail += " Tag: " + tagName;
    else        detail += " (UNKNOWN TAG)";
    log(AuditSource::NFC, known ? "NFC_KNOWN" : "NFC_UNKNOWN", detail, known);
}

void AuditManager::logScheduler(const String& entryName, uint32_t buttonId) {
    log(AuditSource::SCHEDULER, "SCHEDULE_FIRE",
        String("Entry: ") + entryName + " buttonId=" + buttonId);
}

void AuditManager::logMacro(const String& macroName, bool success) {
    log(AuditSource::MACRO, success ? "MACRO_COMPLETE" : "MACRO_FAILED",
        String("Macro: ") + macroName, success);
}

void AuditManager::logWifi(const String& event, const String& detail) {
    log(AuditSource::WIFI, event, detail);
}

void AuditManager::logSystem(const String& event) {
    String detail = String("Heap=") + ESP.getFreeHeap()
                  + " Chip=" + ESP.getChipModel()
                  + " v" + FIRMWARE_VERSION;
    log(AuditSource::SYSTEM, event, detail);
}

void AuditManager::logApi(const String& endpoint, const String& method) {
    log(AuditSource::API, "API_CALL",
        method + " " + endpoint);
}

// ─────────────────────────────────────────────────────────────
//  Filter + Serialize
// ─────────────────────────────────────────────────────────────
std::vector<AuditEntry> AuditManager::filter(int sourceFilter, size_t limit) const {
    std::vector<AuditEntry> result;
    // RACE FIX: take mutex while iterating _entries.
    // _addEntry() is called from loop() (Core 1) and hw_poll task (Core 1) via
    // nfcModule/rfidModule -> auditMgr.log(). filter() is called from HTTP
    // handlers on AsyncTCP task (Core 0). Without this lock a concurrent
    // push_back() that triggers reallocation invalidates our iterator,
    // causing a crash or corrupted result.
    if (_mux) xSemaphoreTake(_mux, portMAX_DELAY);
    // Iterate newest-first (reverse)
    for (int i = (int)_entries.size() - 1; i >= 0 && result.size() < limit; --i) {
        const auto& e = _entries[i];
        if (sourceFilter < 0 || (int)e.source == sourceFilter) {
            result.push_back(e);
        }
    }
    if (_mux) xSemaphoreGive(_mux);
    return result;
}

String AuditManager::toJson(int sourceFilter, size_t limit) const {
    auto filtered = filter(sourceFilter, limit);
    String out;
    out.reserve(filtered.size() * 120);
    out += "{\"total\":";
    out += _totalLogged;
    out += ",\"count\":";
    out += filtered.size();
    out += ",\"entries\":[";
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (i) out += ",";
        const auto& e = filtered[i];
        out += "{\"id\":";    out += e.id;
        out += ",\"ts\":";    out += e.timestamp;
        out += ",\"time\":\""; out += e.timeStr; out += "\"";
        out += ",\"src\":\"";  out += e.sourceStr; out += "\"";
        out += ",\"event\":\""; out += e.event; out += "\"";
        // Escape detail for JSON
        String det = e.detail;
        det.replace("\\", "\\\\");
        det.replace("\"", "\\\"");
        out += ",\"detail\":\""; out += det; out += "\"";
        out += ",\"ok\":";     out += e.success ? "true" : "false";
        out += "}";
    }
    out += "]}";
    return out;
}

// ─────────────────────────────────────────────────────────────
//  Clear
// ─────────────────────────────────────────────────────────────
void AuditManager::clear() {
    // MUTEX FIX: guard the vector clear; other tasks may be iterating.
    if (_mux) xSemaphoreTake(_mux, portMAX_DELAY);
    _entries.clear();
    _totalLogged = 0;
    _nextId = 1;
    _dirty = true;
    if (_mux) xSemaphoreGive(_mux);
    save();
    Serial.println(AUDIT_TAG " Log cleared");
}

// ─────────────────────────────────────────────────────────────
//  Persistence
// ─────────────────────────────────────────────────────────────
void AuditManager::save() const {
    File f = LittleFS.open(AUDIT_LOG_FILE, "w");
    if (!f) {
        Serial.println(AUDIT_TAG " ERROR: Cannot open audit log for write");
        return;
    }
    // Stream write - no full RAM copy
    f.print("{\"total\":");
    f.print(_totalLogged);
    f.print(",\"nextId\":");
    f.print(_nextId);
    f.print(",\"entries\":[");
    // Save last AUDIT_MAX_ENTRIES entries only
    size_t start = _entries.size() > AUDIT_MAX_ENTRIES
                   ? _entries.size() - AUDIT_MAX_ENTRIES : 0;
    bool first = true;
    for (size_t i = start; i < _entries.size(); ++i) {
        if (!first) f.print(",");
        first = false;
        const auto& e = _entries[i];
        f.printf("{\"id\":%u,\"ts\":%u,\"time\":\"%s\",\"src\":%u,\"event\":\"%s\",\"ok\":%s,\"detail\":\"",
                 e.id, e.timestamp, e.timeStr.c_str(),
                 (uint8_t)e.source, e.event.c_str(),
                 e.success ? "true" : "false");
        // Escape detail
        for (char c : e.detail) {
            if (c == '"')  f.print("\\\"");
            else if (c == '\\') f.print("\\\\");
            else           f.print(c);
        }
        f.print("\"}");
    }
    f.print("]}");
    f.close();
    _lastSave = millis();
    _dirty    = false;
    Serial.printf(AUDIT_TAG " Saved %u entries\n", (unsigned)_entries.size());
}

void AuditManager::_load() {
    if (!LittleFS.exists(AUDIT_LOG_FILE)) return;
    File f = LittleFS.open(AUDIT_LOG_FILE, "r");
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.println(AUDIT_TAG " WARNING: Corrupt audit log - starting fresh");
        return;
    }

    _totalLogged = doc["total"] | 0u;
    _nextId      = doc["nextId"] | 1u;

    JsonArray arr = doc["entries"].as<JsonArray>();
    for (JsonObject o : arr) {
        AuditEntry e;
        e.id        = o["id"]     | 0u;
        e.timestamp = o["ts"]     | 0u;
        e.timeStr   = o["time"]   | (const char*)"";
        e.source    = (AuditSource)(o["src"] | 255u);
        e.sourceStr = _sourceToStr(e.source);
        e.event     = o["event"]  | (const char*)"";
        e.detail    = o["detail"] | (const char*)"";
        e.success   = o["ok"]     | true;
        _entries.push_back(e);
    }
    Serial.printf(AUDIT_TAG " Loaded %u entries from flash\n", (unsigned)_entries.size());
}

// ─────────────────────────────────────────────────────────────
//  Internals
// ─────────────────────────────────────────────────────────────
void AuditManager::_addEntry(AuditEntry& e) {
    _entries.push_back(e);
    _totalLogged++;
    // Rotate - drop oldest if over limit
    if (_entries.size() > AUDIT_MAX_ENTRIES) {
        // Feature 36: overflow to SD before dropping from RAM
        if (_sdOverflow && sdMgr.isAvailable()) {
            const AuditEntry& oldest = _entries.front();
            // Append oldest entry as a JSON line to the overflow file
            String dir = "/logs";
            if (!SD.exists(dir)) SD.mkdir(dir);
            File of = SD.open("/logs/audit_overflow.json", FILE_APPEND);
            if (of) {
                String det = oldest.detail;
                det.replace("\\", "\\\\");
                det.replace("\"", "\\\"");
                of.printf("{\"id\":%u,\"ts\":%u,\"time\":\"%s\","
                          "\"src\":\"%s\",\"event\":\"%s\","
                          "\"ok\":%s,\"detail\":\"%s\"}\n",
                          oldest.id, oldest.timestamp, oldest.timeStr.c_str(),
                          oldest.sourceStr.c_str(), oldest.event.c_str(),
                          oldest.success ? "true" : "false",
                          det.c_str());
                of.close();
            }
        }
        _entries.erase(_entries.begin());
    }
    _dirty = true;
}

// ─────────────────────────────────────────────────────────────
//  Feature 36: enableSdOverflow
// ─────────────────────────────────────────────────────────────
bool AuditManager::enableSdOverflow(bool en) {
    _sdOverflow = en;
    return true;
}

// ─────────────────────────────────────────────────────────────
//  Feature 37: exportToSD
// ─────────────────────────────────────────────────────────────
bool AuditManager::exportToSD(const String& filename) {
    if (!sdMgr.isAvailable()) return false;

    // Build target path
    String path;
    if (filename.length() > 0) {
        path = filename;
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "/logs/audit_%lu.json",
                 (unsigned long)_getTimestamp());
        path = String(buf);
    }

    // Ensure logs dir exists
    String dir = "/logs";
    if (!SD.exists(dir)) SD.mkdir(dir);

    File f = sdMgr.openForWrite(path);
    if (!f) return false;

    // Serialize all current entries
    f.print("{\"total\":");
    f.print(_totalLogged);
    f.print(",\"exported\":");
    f.print(_entries.size());
    f.print(",\"entries\":[");
    bool first = true;
    for (const auto& e : _entries) {
        if (!first) f.print(",");
        first = false;
        String det = e.detail;
        det.replace("\\", "\\\\");
        det.replace("\"", "\\\"");
        f.printf("{\"id\":%u,\"ts\":%u,\"time\":\"%s\","
                 "\"src\":\"%s\",\"event\":\"%s\","
                 "\"ok\":%s,\"detail\":\"%s\"}",
                 e.id, e.timestamp, e.timeStr.c_str(),
                 e.sourceStr.c_str(), e.event.c_str(),
                 e.success ? "true" : "false",
                 det.c_str());
    }
    f.print("]}");
    f.close();

    Serial.printf(AUDIT_TAG " Exported %u entries to SD: %s\n",
                  (unsigned)_entries.size(), path.c_str());
    return true;
}

String AuditManager::_sourceToStr(AuditSource s) const {
    switch (s) {
        case AuditSource::IR_TX:     return "IR_TX";
        case AuditSource::IR_RX:     return "IR_RX";
        case AuditSource::RFID:      return "RFID";
        case AuditSource::NFC:       return "NFC";
        case AuditSource::SCHEDULER: return "SCHEDULER";
        case AuditSource::MACRO:     return "MACRO";
        case AuditSource::RULE:      return "RULE";
        case AuditSource::WIFI:      return "WIFI";
        case AuditSource::SYSTEM:    return "SYSTEM";
        case AuditSource::API:       return "API";
        case AuditSource::AUTH:      return "AUTH";
        default:                     return "UNKNOWN";
    }
}

String AuditManager::_buildTimeStr() const {
    // Try NTP time first
    uint32_t ts = _getTimestamp();
    if (ts > 1000000000UL) {   // looks like valid epoch (after ~2001)
        struct tm tmbuf;
        time_t t = (time_t)ts;
        localtime_r(&t, &tmbuf);
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 tmbuf.tm_year + 1900, tmbuf.tm_mon + 1, tmbuf.tm_mday,
                 tmbuf.tm_hour, tmbuf.tm_min, tmbuf.tm_sec);
        return String(buf);
    }
    // Fallback: uptime
    char buf[20];
    snprintf(buf, sizeof(buf), "uptime:%lus", millis() / 1000);
    return String(buf);
}

uint32_t AuditManager::_getTimestamp() const {
    time_t now;
    time(&now);
    return (uint32_t)now;
}
