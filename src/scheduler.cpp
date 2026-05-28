// ============================================================
//  scheduler.cpp  -  NTP + cron-style IR scheduler
// ============================================================
#include "scheduler.h"
#include "scoped_lock.h"
#include "wifi_manager.h"
#include "sd_manager.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <ctime>

#define SD_DIR_SCHEDULES "/schedules"

Scheduler scheduler;

Scheduler::Scheduler()
    : _nextId(1), _tzOffset(NTP_TIMEZONE_OFFSET), _dstOffset(NTP_DST_OFFSET),
      _ntpStarted(false), _lastCheck(0), _lastNtpSync(0),
      _lastFiredMinute(-1)
{}

// ── NTP ───────────────────────────────────────────────────────
void Scheduler::_startNtp() {
    configTime(_tzOffset, _dstOffset, NTP_SERVER1, NTP_SERVER2);
    Serial.printf(DEBUG_TAG " NTP sync started (tz=%ld dst=%ld)\n",
                  _tzOffset, _dstOffset);
    _ntpStarted   = true;
    _lastNtpSync  = millis();
}

void Scheduler::setTimezone(long tzSec, long dstSec) {
    _tzOffset  = tzSec;
    _dstOffset = dstSec;
    if (_ntpStarted) {
        configTime(_tzOffset, _dstOffset, NTP_SERVER1, NTP_SERVER2);
        Serial.printf(DEBUG_TAG " Timezone updated tz=%ld dst=%ld\n",
                      _tzOffset, _dstOffset);
    }
    saveTimezone();  // persist so it survives reboot
}

bool Scheduler::saveTimezone() {
    File f = LittleFS.open("/ntp_config.json", "w");
    if (!f) { Serial.println(DEBUG_TAG " ERROR: Cannot write ntp_config.json"); return false; }
    JsonDocument doc;
    doc["tzOffset"]  = _tzOffset;
    doc["dstOffset"] = _dstOffset;
    size_t w = serializeJson(doc, f);
    f.close();
    return w > 0;
}

bool Scheduler::loadTimezone() {
    if (!LittleFS.exists("/ntp_config.json")) return false;
    File f = LittleFS.open("/ntp_config.json", "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err != DeserializationError::Ok) return false;
    _tzOffset  = doc["tzOffset"]  | (long)NTP_TIMEZONE_OFFSET;
    _dstOffset = doc["dstOffset"] | (long)NTP_DST_OFFSET;
    Serial.printf(DEBUG_TAG " Timezone loaded tz=%ld dst=%ld\n", _tzOffset, _dstOffset);
    return true;
}

bool Scheduler::ntpSynced() const {
    time_t now = time(nullptr);
    return now > 1700000000UL;   // past Nov 2023 = definitely synced
}

String Scheduler::currentTimeStr() const {
    time_t now = time(nullptr);
    struct tm tmbuf;
    struct tm* t = localtime_r(&now, &tmbuf);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
    return String(buf);
}

String Scheduler::currentDateStr() const {
    time_t now = time(nullptr);
    struct tm tmbuf;
    struct tm* t = localtime_r(&now, &tmbuf);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday);
    return String(buf);
}

// ── Lifecycle ─────────────────────────────────────────────────
void Scheduler::begin() {
    if (!_mux) _mux = xSemaphoreCreateRecursiveMutex();
    loadFromFile();
    loadTimezone();  // restore persisted tz/dst offsets before NTP starts
    // from wifi_manager or loop() when STA comes online.
    // We still call configTime() here so the timezone is set even if NTP
    // resolves later via SNTP background polling.
    if (wifiMgr.staConnected()) {
        _startNtp();
    } else {
        // Pre-configure timezone so SNTP daemon picks it up when WiFi connects
        configTime(_tzOffset, _dstOffset, NTP_SERVER1, NTP_SERVER2);
        Serial.println(DEBUG_TAG " NTP: scheduled for when STA connects");
    }
}

void Scheduler::loop() {
    unsigned long now = millis();

    // Use wifiMgr.staConnected() - cached, no WiFi.status() mutex per tick
    bool connected = wifiMgr.staConnected();

    // Start NTP as soon as STA connects (if not already started)
    if (!_ntpStarted && connected) {
        _startNtp();
    }

    // Re-sync NTP periodically (only when connected)
    if (_ntpStarted && connected &&
        (now - _lastNtpSync) >= NTP_SYNC_INTERVAL_MS) {
        _startNtp();
    }

    if ((now - _lastCheck) >= SCHEDULER_CHECK_INTERVAL_MS) {
        _lastCheck = now;
        if (ntpSynced()) {
            _checkAndFire();
            _checkAutoBackup();   // Feature 9
        }
    }
}

void Scheduler::_checkAndFire() {
    time_t t = time(nullptr);
    struct tm tmbuf;
    struct tm* tm = localtime_r(&t, &tmbuf);
    int curMinute = tm->tm_hour * 60 + tm->tm_min;
    int dotw      = tm->tm_wday; // 0=Sun

    // Fire at most once per minute window
    if (curMinute == _lastFiredMinute) return;

    // Collect entries to fire UNDER the lock (a copy), then release and fire
    // outside it. Holding _mux across _fireCb() would block web CRUD for the
    // whole transmit/broadcast/SD-log duration; copying keeps the critical
    // section to a vector walk. ScheduleEntry is copyable.
    std::vector<ScheduleEntry> toFire;
    {
        ScopedLock lk(_mux);
        if (!lk) return;   // couldn't acquire - skip this tick, retry next
        for (const auto& e : _entries) {
            if (!e.enabled) continue;
            if (e.hour != (uint8_t)tm->tm_hour) continue;
            if (e.minute != (uint8_t)tm->tm_min) continue;
            if (!(e.daysMask & (1 << dotw))) continue;
            toFire.push_back(e);
        }
    }

    for (const auto& e : toFire) {
        Serial.printf(DEBUG_TAG " Scheduler FIRE: entry %u '%s' btn=%u rpt=%u dly=%u macro='%s'\n",
                      e.id, e.name.c_str(), e.buttonId,
                      e.repeatCount, e.repeatDelay, e.macroFile.c_str());

        // Feature 8: if macroFile is set, queue the SD macro
        if (!e.macroFile.isEmpty() && sdMgr.isAvailable()) {
            sdMgr.log(String("Scheduler macro: ") + e.macroFile, SdLogLevel::INFO, "SCHED");
            sdMgr.queueMacro(e.macroFile);
        }

        if (_fireCb) _fireCb(e);
    }

    _lastFiredMinute = curMinute;
}

// ── CRUD ──────────────────────────────────────────────────────
uint32_t Scheduler::addEntry(const ScheduleEntry& e) {
    ScopedLock lk(_mux);
    if (_entries.size() >= MAX_SCHEDULES) return 0;
    ScheduleEntry copy = e;
    copy.id = _nextId++;
    _entries.push_back(copy);
    saveToFile();
    return copy.id;
}

bool Scheduler::updateEntry(const ScheduleEntry& e) {
    ScopedLock lk(_mux);
    for (auto& x : _entries) {
        if (x.id == e.id) { x = e; saveToFile(); return true; }
    }
    return false;
}

bool Scheduler::removeEntry(uint32_t id) {
    ScopedLock lk(_mux);
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (it->id == id) { _entries.erase(it); saveToFile(); return true; }
    }
    return false;
}

bool Scheduler::setEnabled(uint32_t id, bool en) {
    ScopedLock lk(_mux);
    for (auto& e : _entries) {
        if (e.id == id) { e.enabled = en; saveToFile(); return true; }
    }
    return false;
}

// ── Serialisation ─────────────────────────────────────────────
String Scheduler::toJson() const {
    ScopedLock lk(_mux);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : _entries) {
        JsonObject o = arr.add<JsonObject>();
        e.toJson(o);
    }
    String out; serializeJson(doc, out);
    return out;
}

bool Scheduler::loadFromFile() {
    if (!LittleFS.exists(SCHEDULES_FILE)) return false;
    File f = LittleFS.open(SCHEDULES_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err != DeserializationError::Ok || !doc.is<JsonArrayConst>()) return false;

    ScopedLock lk(_mux);
    _entries.clear();
    _nextId = 1;
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        ScheduleEntry e;
        if (e.fromJson(o)) {
            _entries.push_back(e);
            if (e.id >= _nextId) _nextId = e.id + 1;
        }
    }
    Serial.printf(DEBUG_TAG " Schedules loaded: %u\n", (unsigned)_entries.size());
    return true;
}

bool Scheduler::saveToFile() {
    File f = LittleFS.open(SCHEDULES_FILE, "w");
    if (!f) { Serial.println(DEBUG_TAG " ERROR: Cannot write schedules."); return false; }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : _entries) {
        JsonObject o = arr.add<JsonObject>();
        e.toJson(o);
    }
    size_t w = serializeJson(doc, f); f.close();
    return w > 0;
}

// ── Feature 6: exportToSD / importFromSD ─────────────────────
bool Scheduler::exportToSD(const String& tag) {
    if (!sdMgr.isAvailable()) {
        Serial.println(DEBUG_TAG " [exportToSD] SD not available");
        return false;
    }
    // Read /schedules.json from LittleFS
    if (!LittleFS.exists(SCHEDULES_FILE)) {
        Serial.println(DEBUG_TAG " [exportToSD] No schedules file to export");
        return false;
    }
    File src = LittleFS.open(SCHEDULES_FILE, "r");
    if (!src) return false;
    String content = src.readString();
    src.close();

    // Write to /sd/backups/<tag>/schedules.json
    String dstDir = String(SD_DIR_BACKUPS) + "/" + tag;
    sdMgr.makeDir(dstDir);
    String dstPath = dstDir + "/schedules.json";
    bool ok = sdMgr.safeWriteFile(dstPath, content);
    if (ok) {
        sdMgr.log(String("Schedules exported to ") + dstPath, SdLogLevel::INFO, "SCHED");
        Serial.printf(DEBUG_TAG " [exportToSD] Exported to %s\n", dstPath.c_str());
    } else {
        Serial.printf(DEBUG_TAG " [exportToSD] ERROR writing %s\n", dstPath.c_str());
    }
    return ok;
}

bool Scheduler::importFromSD(const String& tag) {
    if (!sdMgr.isAvailable()) {
        Serial.println(DEBUG_TAG " [importFromSD] SD not available");
        return false;
    }
    String srcPath = String(SD_DIR_BACKUPS) + "/" + tag + "/schedules.json";
    if (!sdMgr.exists(srcPath)) {
        Serial.printf(DEBUG_TAG " [importFromSD] Not found: %s\n", srcPath.c_str());
        return false;
    }
    String content = sdMgr.readTextFile(srcPath);
    if (content.isEmpty()) return false;

    // Write to LittleFS and reload
    File dst = LittleFS.open(SCHEDULES_FILE, "w");
    if (!dst) return false;
    dst.print(content);
    dst.close();

    bool ok = loadFromFile();
    if (ok) {
        sdMgr.log(String("Schedules imported from ") + srcPath, SdLogLevel::INFO, "SCHED");
        Serial.printf(DEBUG_TAG " [importFromSD] Imported from %s (%u entries)\n",
                      srcPath.c_str(), (unsigned)_entries.size());
    }
    return ok;
}

// ── Feature 7: savePreset / loadPreset / listPresets ─────────
bool Scheduler::savePreset(const String& name) {
    if (!sdMgr.isAvailable()) return false;
    if (!LittleFS.exists(SCHEDULES_FILE)) return false;
    File src = LittleFS.open(SCHEDULES_FILE, "r");
    if (!src) return false;
    String content = src.readString();
    src.close();

    String sdDir = String(SD_DIR_SCHEDULES);
    sdMgr.makeDir(sdDir);
    String dstPath = sdDir + "/" + name + ".json";
    bool ok = sdMgr.safeWriteFile(dstPath, content);
    if (ok) {
        sdMgr.log(String("Schedule preset saved: ") + name, SdLogLevel::INFO, "SCHED");
        Serial.printf(DEBUG_TAG " [savePreset] Saved '%s' to %s\n", name.c_str(), dstPath.c_str());
    }
    return ok;
}

bool Scheduler::loadPreset(const String& name) {
    if (!sdMgr.isAvailable()) return false;
    String srcPath = String(SD_DIR_SCHEDULES) + "/" + name + ".json";
    if (!sdMgr.exists(srcPath)) {
        Serial.printf(DEBUG_TAG " [loadPreset] Not found: %s\n", srcPath.c_str());
        return false;
    }
    String content = sdMgr.readTextFile(srcPath);
    if (content.isEmpty()) return false;

    File dst = LittleFS.open(SCHEDULES_FILE, "w");
    if (!dst) return false;
    dst.print(content);
    dst.close();

    bool ok = loadFromFile();
    if (ok) {
        sdMgr.log(String("Schedule preset loaded: ") + name, SdLogLevel::INFO, "SCHED");
        Serial.printf(DEBUG_TAG " [loadPreset] Loaded '%s' (%u entries)\n",
                      name.c_str(), (unsigned)_entries.size());
    }
    return ok;
}

std::vector<String> Scheduler::listPresets() const {
    std::vector<String> result;
    if (!sdMgr.isAvailable()) return result;
    auto entries = sdMgr.listDir(SD_DIR_SCHEDULES);
    for (const auto& e : entries) {
        if (e.isDir) continue;
        String n = e.name;
        if (n.endsWith(".json")) {
            result.push_back(n.substring(0, n.length() - 5));
        }
    }
    return result;
}

// ── Feature 9: enableAutoBackup / _checkAutoBackup ───────────
void Scheduler::enableAutoBackup(bool en, const String& cronExpr) {
    _autoBackupEn   = en;
    _autoBackupCron = cronExpr;
    // Parse simple "MIN HOUR * * *" cron expression
    // Only supports fixed hour/minute; other fields ignored
    _autoBackupHour   = 2;
    _autoBackupMinute = 0;
    if (!cronExpr.isEmpty()) {
        // Format: "MIN HOUR ..."
        int sp1 = cronExpr.indexOf(' ');
        if (sp1 > 0) {
            _autoBackupMinute = cronExpr.substring(0, sp1).toInt();
            int sp2 = cronExpr.indexOf(' ', sp1 + 1);
            String hourStr = (sp2 > 0)
                ? cronExpr.substring(sp1 + 1, sp2)
                : cronExpr.substring(sp1 + 1);
            _autoBackupHour = hourStr.toInt();
        }
    }
    Serial.printf(DEBUG_TAG " Auto-backup %s at %02d:%02d (cron: '%s')\n",
                  en ? "ENABLED" : "DISABLED",
                  _autoBackupHour, _autoBackupMinute, cronExpr.c_str());
}

void Scheduler::_checkAutoBackup() {
    if (!_autoBackupEn) return;
    if (!sdMgr.isAvailable()) return;

    time_t t = time(nullptr);
    struct tm tmbuf;
    struct tm* tm = localtime_r(&t, &tmbuf);
    int dayOfYear = tm->tm_yday;

    // Fire once per day at configured hour:minute
    if (tm->tm_hour != _autoBackupHour) return;
    if (tm->tm_min  != _autoBackupMinute) return;
    if (dayOfYear == _lastAutoBackupDay) return;  // already ran today

    _lastAutoBackupDay = dayOfYear;

    // Build tag: "auto_YYYY-MM-DD"
    char tag[32];
    snprintf(tag, sizeof(tag), "auto_%04d-%02d-%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    Serial.printf(DEBUG_TAG " Auto-backup firing: tag=%s\n", tag);
    sdMgr.log(String("Scheduled auto-backup: ") + tag, SdLogLevel::INFO, "SCHED");
    sdMgr.backupToSD(String(tag));
}
