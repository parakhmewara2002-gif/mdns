// ============================================================
//  log_rotation.cpp  -  Batch 4: Log Rotation + CSV Export
// ============================================================
#include "log_rotation.h"
#include <SD.h>
#include "scheduler.h"
#include "sd_manager.h"
#include <ctime>
#include <vector>

LogRotationManager logRotMgr;

// ─────────────────────────────────────────────────────────────
LogRotationManager::LogRotationManager()
    : _retentionDays(LOG_ROT_DEFAULT_DAYS), _lastCheck(0) {}

void LogRotationManager::begin() {
    loadConfig();
    // Create archive dir if missing
    if (!LittleFS.exists(LOG_ARCHIVE_DIR))
        LittleFS.mkdir(LOG_ARCHIVE_DIR);
    Serial.printf(LOG_ROT_TAG " Started - retention=%u days\n", _retentionDays);
}

void LogRotationManager::loop() {
    if (millis() - _lastCheck < LOG_ROT_CHECK_MS) return;
    _lastCheck = millis();
    // LittleFS dir open needs ~2KB heap; skip if low to avoid OOM abort
    if (ESP.getFreeHeap() < 20000) return;
    pruneOldLogs();
}

// ─────────────────────────────────────────────────────────────
//  Rotate: archive current audit log to /log_archive/YYYY-MM-DD.json
// ─────────────────────────────────────────────────────────────
bool LogRotationManager::rotate() {
    if (auditMgr.size() == 0) return true;

    String today   = _todayStr();
    String path    = String(LOG_ARCHIVE_DIR) + "/" + today + ".json";

    // Write JSON archive
    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf(LOG_ROT_TAG " ERROR: Cannot create archive %s\n", path.c_str());
        return false;
    }
    f.print(auditMgr.toJson(-1, AUDIT_MAX_ENTRIES));
    f.close();

    Serial.printf(LOG_ROT_TAG " Rotated %u entries to %s\n",
                  (unsigned)auditMgr.size(), path.c_str());

    // Also write CSV archive to SD if available
    if (sdMgr.isAvailable()) {
        String csvPath = "/logs/audit_" + today + ".csv";
        String csv = auditToCsv(-1, AUDIT_MAX_ENTRIES);
        // CSV FIX: previously called sdMgr.log(csv) which appends a single
        // line to /logs/activity.log — csvPath was constructed but never
        // used. Now writes to the intended CSV path via the atomic helper.
        if (!sdMgr.safeWriteFile(csvPath, csv)) {
            Serial.printf(LOG_ROT_TAG " WARN: CSV write to %s failed\n",
                          csvPath.c_str());
        }
    }

    // Clear in-memory audit log
    auditMgr.clear();
    return true;
}

// ─────────────────────────────────────────────────────────────
//  _archiveToCsv - write CSV copy of current audit log to SD
//  FIX: was using SD.open() directly, bypassing sdMgr safety/mutex.
//       Now uses sdMgr.log() API which handles SD state correctly.
bool LogRotationManager::_archiveToCsv() const {
    if (!sdMgr.isAvailable()) return false;
    time_t now; time(&now);
    struct tm tmbuf;
    struct tm* t = localtime_r(&now, &tmbuf);
    char today[12];
    snprintf(today, sizeof(today), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    String csv = auditToCsv(-1, AUDIT_MAX_ENTRIES);
    if (csv.isEmpty()) return false;
    // CSV FIX: write to a real per-day archive file rather than appending to
    // /logs/activity.log. The pre-fix code lost the "archive" semantics —
    // CSV header lines got mixed into the rolling activity log.
    String header = String("# Audit CSV Archive: ") + today + "\n";
    String archivePath = String("/logs/audit_") + today + ".csv";
    bool ok = sdMgr.safeWriteFile(archivePath, header + csv);
    if (ok) {
        Serial.printf(LOG_ROT_TAG " CSV archive written to %s (%u bytes)\n",
                      archivePath.c_str(), (unsigned)csv.length());
    } else {
        Serial.printf(LOG_ROT_TAG " WARN: CSV archive write to %s failed\n",
                      archivePath.c_str());
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────
//  Prune old archived logs
// ─────────────────────────────────────────────────────────────
void LogRotationManager::pruneOldLogs() {
    if (_retentionDays == 0) return;  // 0 = keep forever

    // Current epoch
    time_t now; time(&now);
    if (now < 1000000000UL) return;  // NTP not synced yet

    time_t cutoff = now - ((time_t)_retentionDays * 86400);

    File dir = LittleFS.open(LOG_ARCHIVE_DIR);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }

    // Collect paths to delete first - do not delete while iterating (UB on LittleFS)
    std::vector<String> toDelete;
    File f = dir.openNextFile();
    while (f) {
        String raw = String(f.name());
        // FILENAME FIX: some LittleFS builds return name with leading '/',
        // others return basename only. Previous strict length==15 check
        // skipped pruning entirely on the former, so /log_archive grew
        // forever. Strip any leading path so the format check works on
        // both.
        int lastSlash = raw.lastIndexOf('/');
        String name = (lastSlash >= 0) ? raw.substring(lastSlash + 1) : raw;
        // Filename format: YYYY-MM-DD.json
        if (name.endsWith(".json") && name.length() == 15) {
            int yr  = name.substring(0,4).toInt();
            int mo  = name.substring(5,7).toInt();
            int dy  = name.substring(8,10).toInt();
            struct tm tm = {}; tm.tm_year=yr-1900; tm.tm_mon=mo-1; tm.tm_mday=dy;
            time_t ft = mktime(&tm);
            if (ft > 0 && ft < cutoff) {
                toDelete.push_back(String(LOG_ARCHIVE_DIR) + "/" + name);
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    for (const auto& path : toDelete) {
        LittleFS.remove(path);
        Serial.printf(LOG_ROT_TAG " Pruned old log: %s\n", path.c_str());
    }
}

// ─────────────────────────────────────────────────────────────
//  CSV Export
// ─────────────────────────────────────────────────────────────
String LogRotationManager::auditToCsv(int sourceFilter, size_t limit) const {
    auto entries = auditMgr.filter(sourceFilter, limit);

    String csv;
    csv.reserve(entries.size() * 100);

    // Header
    csv += "id,timestamp,time,source,event,detail,success\r\n";

    for (const auto& e : entries) {
        csv += String(e.id)        + ",";
        csv += String(e.timestamp) + ",";
        csv += "\"" + e.timeStr   + "\",";
        csv += "\"" + e.sourceStr + "\",";
        csv += "\"" + e.event     + "\",";
        // Escape quotes in detail
        String det = e.detail;
        det.replace("\"", "\"\"");
        csv += "\"" + det + "\",";
        csv += (e.success ? "1" : "0");
        csv += "\r\n";
    }
    return csv;
}

// ─────────────────────────────────────────────────────────────
//  Config
// ─────────────────────────────────────────────────────────────
void LogRotationManager::setRetentionDays(uint8_t days) {
    _retentionDays = days;
    saveConfig();
}

bool LogRotationManager::loadConfig() {
    if (!LittleFS.exists(LOG_ROT_CFG_FILE)) return false;
    File f = LittleFS.open(LOG_ROT_CFG_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();
    _retentionDays = doc["retentionDays"] | (uint8_t)LOG_ROT_DEFAULT_DAYS;
    return true;
}

bool LogRotationManager::saveConfig() {
    File f = LittleFS.open(LOG_ROT_CFG_FILE, "w");
    if (!f) return false;
    JsonDocument doc;
    doc["retentionDays"] = _retentionDays;
    serializeJson(doc, f); f.close();
    return true;
}

String LogRotationManager::configJson() const {
    return String("{\"retentionDays\":") + _retentionDays + "}";
}

// ─────────────────────────────────────────────────────────────
//  List archived logs
// ─────────────────────────────────────────────────────────────
String LogRotationManager::listArchivesJson() const {
    if (ESP.getFreeHeap() < 20000) return "{\"archives\":[],\"error\":\"low heap\"}";
    String out = "{\"archives\":[";
    bool first = true;
    File dir = LittleFS.open(LOG_ARCHIVE_DIR);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                if (!first) out += ",";
                first = false;
                out += "{\"name\":\"" + String(f.name()) + "\","
                     + "\"size\":" + String(f.size()) + "}";
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }
    out += "]}";
    return out;
}

// ─────────────────────────────────────────────────────────────
String LogRotationManager::_todayStr() const {
    time_t now; time(&now);
    if (now > 1000000000UL) {
        struct tm t; localtime_r(&now, &t);
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                 t.tm_year+1900, t.tm_mon+1, t.tm_mday);
        return String(buf);
    }
    return String("uptime-") + (millis()/1000);
}
