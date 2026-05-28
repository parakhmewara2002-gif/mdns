// ============================================================
//  sd_manager.cpp  -  Optional SD card subsystem
//  v2.0.0  |  Graceful fallback when SD absent
//  45 upgrades applied
// ============================================================
#include "sd_manager.h"
#include "ir_database.h"
#include "ir_transmitter.h"
#include "nfc_module.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <ctime>
#include <esp_partition.h>
#include <esp_spi_flash.h>

SdManager sdMgr;

// C-04 FIX: Global VSPI bus mutex - defined here, extern declared in sd_manager.h.
// Changed to RECURSIVE so internal SdManager methods that call each other do
// not self-deadlock (e.g. createTar calls _dirSizeImpl which both want the
// lock). NFC / NRF24 also share VSPI; any caller that touches SD.* should be
// holding this mutex.
SemaphoreHandle_t g_spi_vspi_mutex = nullptr;

// SD SPI MUTEX SWEEP: RAII wrapper. Constructed at the top of each public
// SdManager method that performs SD.* operations; takes the recursive bus
// mutex and gives it back on scope exit. Drops gracefully (no lock) if the
// mutex was never created (very early boot before begin() ran). Times out
// after 2 s so a stuck SD operation cannot wedge the rest of the firmware.
namespace {
class SdSpiLock {
public:
    SdSpiLock() : _ok(false) {
        if (g_spi_vspi_mutex)
            _ok = (xSemaphoreTakeRecursive(g_spi_vspi_mutex,
                                            pdMS_TO_TICKS(2000)) == pdTRUE);
    }
    ~SdSpiLock() {
        if (_ok && g_spi_vspi_mutex)
            xSemaphoreGiveRecursive(g_spi_vspi_mutex);
    }
    bool ok() const { return _ok; }
private:
    bool _ok;
};
}  // namespace

// ── Constructor ──────────────────────────────────────────────
SdManager::SdManager()
    : _mounted(false),
      _spiBegun(false),
      _lastProbeMs(0),
      _probeFailCount(0),
      _remountIntervalMs(SD_REMOUNT_MIN_MS),
      _lastRemountMs(0),
      _logMux(portMUX_INITIALIZER_UNLOCKED),
      _logLevel(SdLogLevel::INFO),
      _mountedFreq(0),
      _mountCount(0),
      _logRingHead(0),
      _logRingLen(0),
      _lastLogFlushMs(0),
      _macroRunning(false),
      _macroStepIdx(0),
      _macroNextMs(0),
      _macroRepeatIdx(0),
      _uploadOpen(false),
      _autoBackupEnabled(false),
      _autoBackupIntervalMs(0),
      _lastAutoBackupMs(0),
      _autoExportDirty(false),
      _recording(false),
      _recordStepCount(0),
      _writeQueue(nullptr)
{
    memset(_logRing, 0, sizeof(_logRing));
}

// ── begin ────────────────────────────────────────────────────
void SdManager::begin() {
    // C-04 FIX: create the global VSPI bus mutex (recursive — see header
    // comment above SdSpiLock).
    if (!g_spi_vspi_mutex) {
        g_spi_vspi_mutex = xSemaphoreCreateRecursiveMutex();
        if (!g_spi_vspi_mutex) {
            Serial.println("[SD] FATAL: could not create VSPI mutex");
        }
    }

    if (!_spiBegun) {
        if (!_sdSpi) _sdSpi = new SPIClass(VSPI);
        _sdSpi->begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
        _spiBegun = true;
    }

    // [#33] Create async write queue (depth=8, pointer queue)
    if (!_writeQueue) {
        _writeQueue = xQueueCreate(8, sizeof(SdWriteJob*));
    }

    // Feature 4/5: restore SD preferences from LittleFS
    if (LittleFS.exists("/sd_prefs.json")) {
        File pf = LittleFS.open("/sd_prefs.json", "r");
        if (pf) {
            JsonDocument pdoc;
            if (deserializeJson(pdoc, pf) == DeserializationError::Ok) {
                _autoRawDump = pdoc["autoRawDump"] | false;
                if (pdoc["defaultLib"].is<const char*>()) {
                    _defaultLib = pdoc["defaultLib"].as<String>();
                }
            }
            pf.close();
        }
    }

    _mount();
    // Prevent _probeSd() from firing immediately on first loop() tick.
    // Without this, loop() tick 1 tries SD.begin() 3x again right after begin().
    _lastProbeMs = millis();
}

// ── [#36] _mount with SPI frequency auto-tune ───────────────
bool SdManager::_mount() {
    // Use RAII guard (recursive — _mount can be called from inside another
    // already-locked method like remount-on-event-loop).
    bool tookMutex = false;
    if (g_spi_vspi_mutex) {
        tookMutex = (xSemaphoreTakeRecursive(g_spi_vspi_mutex,
                                              pdMS_TO_TICKS(100)) == pdTRUE);
    }

    SD.end();

    // [#36] Try 20 MHz, 10 MHz, 4 MHz in order
    static const uint32_t freqCandidates[] = { 20000000UL, 10000000UL, 4000000UL };
    bool ok = false;
    uint32_t usedFreq = SD_SPI_FREQ;
    for (size_t fi = 0; fi < 3; ++fi) {
        if (SD.begin(SD_CS_PIN, *_sdSpi, freqCandidates[fi])) {
            if (SD.cardType() != CARD_NONE) {
                usedFreq = freqCandidates[fi];
                ok = true;
                break;
            }
            SD.end();
        }
    }

    if (!ok) {
        _mounted = false;
        if (tookMutex) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
        return false;
    }

    if (tookMutex) xSemaphoreGiveRecursive(g_spi_vspi_mutex);

    _mounted           = true;
    _mountedFreq       = usedFreq;
    _probeFailCount    = 0;
    _remountIntervalMs = SD_REMOUNT_MIN_MS;
    ++_mountCount; // [#38]

    Serial.printf(DEBUG_TAG " [SD] Mounted OK  type=%s  size=%lluMB  free=%lluMB  freq=%luHz\n",
                  status().cardTypeStr.c_str(),
                  SD.totalBytes()  / (1024ULL * 1024ULL),
                  (SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL),
                  (unsigned long)usedFreq);

    _ensureDirectories();
    // [#42] emit mounted event
    _queueSdEvent("mounted");
    log("SD mounted - IR Remote started");
    return true;
}

// ── _unmount ─────────────────────────────────────────────────
void SdManager::_unmount() {
    if (_mounted) {
        _flushLogRing();
        SD.end();
        _mounted = false;
        // [#42] emit unmounted event
        _queueSdEvent("unmounted");
        Serial.println(DEBUG_TAG " [SD] Unmounted.");
    }
}

// ── _ensureDirectories ───────────────────────────────────────
void SdManager::_ensureDirectories() {
    const char* dirs[] = {
        SD_DIR_IR_LIBRARY, SD_DIR_BACKUPS,
        SD_DIR_MACROS,     SD_DIR_LOGS,    SD_DIR_ASSETS,
        SD_DIR_RAW_DUMPS,  SD_DIR_DEVICES,
        SD_DIR_PROFILES,   SD_DIR_SYNC,    // Features 48, 50
        nullptr
    };
    for (const char** d = dirs; *d; ++d) {
        if (!SD.exists(*d)) {
            SD.mkdir(*d);
            Serial.printf(DEBUG_TAG " [SD] Created dir: %s\n", *d);
        }
    }
    // Feature 49: check for boot config on SD and apply it
    _applyBootConfig();
}

// ── loop ─────────────────────────────────────────────────────
void SdManager::loop() {
    unsigned long now = millis();

    if (now - _lastProbeMs >= SD_PROBE_INTERVAL) {
        _probeSd();
        _lastProbeMs = now;
    }

    if (!_mounted) return;

    if (now - _lastLogFlushMs >= SD_LOG_FLUSH_MS) {
        _flushLogRing();
        _lastLogFlushMs = now;
    }

    if (_macroRunning) _tickMacro();

    // [#13] Auto-backup check
    _checkAutoBackup();
    // [#22] Auto-export check
    _checkAutoExport();
    // [#33] Drain async write queue
    _drainWriteQueue();
}

// ── _probeSd ─────────────────────────────────────────────────
void SdManager::_probeSd() {
    unsigned long now = millis();

    if (_mounted) {
        bool cardPresent = (SD.cardType() != CARD_NONE);
        if (!cardPresent) {
            ++_probeFailCount;
            if (_probeFailCount >= SD_PROBE_FAIL_DEBOUNCE) {
                Serial.printf(DEBUG_TAG " [SD] %u consecutive cardType=NONE - unmounting.\n",
                              (unsigned)_probeFailCount);
                _probeFailCount = 0;
                _unmount();
                _remountIntervalMs = SD_REMOUNT_MIN_MS;
                _lastRemountMs     = now;
            }
        } else {
            if (_probeFailCount > 0) _probeFailCount = 0;
        }
    } else {
        if ((now - _lastRemountMs) < _remountIntervalMs) return;
        _lastRemountMs = now;
        bool ok = _mount();
        if (!ok) {
            _remountIntervalMs = min(_remountIntervalMs * 2, SD_REMOUNT_MAX_MS);
            Serial.printf(DEBUG_TAG " [SD] Not present. Next retry in %lus.\n",
                          _remountIntervalMs / 1000UL);
        }
    }
}

// ── status ───────────────────────────────────────────────────
SdStatus SdManager::status() const {
    SdStatus s;
    s.mounted    = _mounted;
    s.totalBytes = _mounted ? SD.totalBytes() : 0;
    s.usedBytes  = _mounted ? SD.usedBytes()  : 0;
    s.cardType   = _mounted ? (uint8_t)SD.cardType() : 0;
    switch (s.cardType) {
        case CARD_MMC:     s.cardTypeStr = "MMC";     break;
        case CARD_SD:      s.cardTypeStr = "SD";      break;
        case CARD_SDHC:    s.cardTypeStr = "SDHC";    break;
        case CARD_UNKNOWN: s.cardTypeStr = "Unknown"; break;
        default:           s.cardTypeStr = "None";    break;
    }
    return s;
}

// ── [#43] statusJson ─────────────────────────────────────────
String SdManager::statusJson() const {
    SdStatus s = status();
    String out = "{";
    out += "\"mounted\":" + String(s.mounted ? "true" : "false") + ",";
    out += "\"totalBytes\":" + String((unsigned long long)s.totalBytes) + ",";
    out += "\"usedBytes\":"  + String((unsigned long long)s.usedBytes)  + ",";
    out += "\"cardType\":\""  + s.cardTypeStr + "\",";
    out += "\"mountedFreqHz\":" + String((unsigned long)_mountedFreq) + ",";
    out += "\"mountCount\":"    + String((unsigned long)_mountCount)  + ",";
    out += "\"macroRunning\":"  + String(_macroRunning ? "true" : "false") + ",";
    out += "\"macroStep\":"     + String((unsigned)_macroStepIdx) + ",";
    out += "\"macroTotal\":"    + String((unsigned)_macroSteps.size());
    out += "}";
    return out;
}

// ── _safePath ────────────────────────────────────────────────
bool SdManager::_safePath(const String& path) const {
    if (path.isEmpty())          return false;
    if (path.indexOf("..") >= 0) return false;
    if (!path.startsWith("/"))   return false;
    if (path.length() > 128)     return false;
    return true;
}

// ── _logTimestamp ────────────────────────────────────────────
String SdManager::_logTimestamp() const {
    time_t now = time(nullptr);
    if (now < 1700000000UL) {
        unsigned long s = millis() / 1000;
        char buf[24];
        snprintf(buf, sizeof(buf), "[up %02lu:%02lu:%02lu]",
                 s / 3600, (s % 3600) / 60, s % 60);
        return String(buf);
    }
    struct tm tmbuf, *t = localtime_r(&now, &tmbuf);
    char buf[24];
    snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d]",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    return String(buf);
}

// ── [#1][#4] log ─────────────────────────────────────────────
void SdManager::log(const String& message, SdLogLevel level) {
    log(message.c_str(), level);
}

void SdManager::log(const char* message, SdLogLevel level) {
    // [#1] Filter by configured minimum level
    if (level < _logLevel) return;
    if (!_mounted) return;

    const char* levelTag = "[INFO] ";
    if (level == SdLogLevel::WARN)  levelTag = "[WARN] ";
    if (level == SdLogLevel::ERROR) levelTag = "[ERROR] ";

    String line = _logTimestamp() + " " + levelTag + message + "\n";
    const char* lc = line.c_str();
    size_t len = line.length();
    if (len == 0) return;

    portENTER_CRITICAL(&_logMux);
    for (size_t i = 0; i < len; ++i) {
        _logRing[(_logRingHead + _logRingLen) % LOG_RING_SIZE] = lc[i];
        if (_logRingLen < LOG_RING_SIZE) {
            ++_logRingLen;
        } else {
            _logRingHead = (_logRingHead + 1) % LOG_RING_SIZE;
        }
    }
    portEXIT_CRITICAL(&_logMux);

    // [#3] Append to WebSocket pending log (max 2KB ring)
    if (_wsPendingLog.length() + len > WS_LOG_MAX) {
        // Drop oldest to make room
        size_t drop = (_wsPendingLog.length() + len) - WS_LOG_MAX;
        _wsPendingLog = _wsPendingLog.substring(drop);
    }
    _wsPendingLog += line;

    Serial.printf(DEBUG_TAG " [SD-LOG] %s", lc);
}

// [#4] Module-tagged overload
void SdManager::log(const String& msg, SdLogLevel level, const char* module) {
    if (level < _logLevel) return;
    String tagged = String("[") + module + "] " + msg;
    log(tagged, level);
}

// ── [#3] pollWsLog ───────────────────────────────────────────
String SdManager::pollWsLog() {
    String out = _wsPendingLog;
    _wsPendingLog = "";
    return out;
}

// ── [#5] streamLog ───────────────────────────────────────────
bool SdManager::streamLog(Print& out) const {
    SdSpiLock _l;
    if (!_mounted || !SD.exists(SD_LOG_FILE)) return false;
    File f = SD.open(SD_LOG_FILE, FILE_READ);
    if (!f) return false;
    uint8_t buf[256];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        if (n > 0) out.write(buf, n);
    }
    f.close();
    return true;
}

// ── [#2] _flushLogRing with log rotation ─────────────────────
void SdManager::_flushLogRing() {
    SdSpiLock _l;
    if (!_mounted) return;

    char buf[LOG_RING_SIZE];
    size_t len = 0;
    portENTER_CRITICAL(&_logMux);
    len = _logRingLen;
    size_t head = _logRingHead;
    if (len > 0) {
        for (size_t i = 0; i < len; ++i)
            buf[i] = _logRing[(head + i) % LOG_RING_SIZE];
        _logRingLen  = 0;
        _logRingHead = 0;
    }
    portEXIT_CRITICAL(&_logMux);
    if (len == 0) return;

    // [#2] Rotate log files: activity.log -> .1 -> .2 ... up to SD_LOG_ROTATE_MAX
    if (SD.exists(SD_LOG_FILE)) {
        File f = SD.open(SD_LOG_FILE, FILE_READ);
        if (f && f.size() >= SD_LOG_MAX_BYTES) {
            f.close();
            // Shift existing rotated files: .4 deleted, .3->.4, .2->.3, .1->.2
            for (int i = SD_LOG_ROTATE_MAX - 1; i >= 1; --i) {
                String older = String("/logs/activity.log.") + String(i);
                String newer = String("/logs/activity.log.") + String(i + 1);
                if (SD.exists(older)) {
                    if (SD.exists(newer)) SD.remove(newer);
                    SD.rename(older, newer);
                }
            }
            // Rename current -> .1
            if (SD.exists("/logs/activity.log.1"))
                SD.remove("/logs/activity.log.1");
            SD.rename(SD_LOG_FILE, "/logs/activity.log.1");
        } else if (f) {
            f.close();
        }
    }

    File f = SD.open(SD_LOG_FILE, FILE_APPEND);
    if (!f) {
        Serial.println(DEBUG_TAG " [SD] ERROR: cannot open log file for append.");
        return;
    }
    f.write((const uint8_t*)buf, len);
    f.close();
}

// ── tailLog ──────────────────────────────────────────────────
String SdManager::tailLog(uint16_t lines) const {
    SdSpiLock _l;
    if (!_mounted) return "";
    if (!SD.exists(SD_LOG_FILE)) return "";
    File f = SD.open(SD_LOG_FILE, FILE_READ);
    if (!f) return "";

    size_t fileSize = f.size();
    if (fileSize == 0) { f.close(); return ""; }

    const size_t SCAN_BUF = 4096;
    size_t scanSize = (fileSize < SCAN_BUF) ? fileSize : SCAN_BUF;
    f.seek(fileSize - scanSize);

    String chunk = "";
    chunk.reserve(scanSize);
    while (f.available()) chunk += (char)f.read();
    f.close();

    int found = 0;
    int pos   = (int)chunk.length() - 1;
    while (pos >= 0 && found <= (int)lines) {
        if (chunk[pos] == '\n') ++found;
        --pos;
    }
    if (pos < 0) pos = 0;
    else if (pos < (int)chunk.length() && chunk[pos] == '\n') ++pos;
    return chunk.substring(pos);
}

// ── [#45] tailLogFiltered ────────────────────────────────────
String SdManager::tailLogFiltered(uint16_t lines, SdLogLevel minLevel) const {
    SdSpiLock _l;
    String raw = tailLog(lines * 3); // fetch more lines to have enough after filtering
    if (raw.isEmpty()) return "";

    const char* tag = nullptr;
    if (minLevel == SdLogLevel::WARN)  tag = "[WARN]";
    if (minLevel == SdLogLevel::ERROR) tag = "[ERROR]";

    if (!tag) return raw; // INFO = no filter needed

    String out = "";
    int start = 0;
    int len   = (int)raw.length();
    uint16_t kept = 0;
    while (start < len && kept < lines) {
        int nl = raw.indexOf('\n', start);
        String line = (nl >= 0) ? raw.substring(start, nl + 1)
                                : raw.substring(start);
        // Accept if it contains the required tag or a higher-severity tag
        bool accept = false;
        if (minLevel <= SdLogLevel::INFO)  accept = true;
        else if (minLevel == SdLogLevel::WARN) {
            accept = (line.indexOf("[WARN]") >= 0 ||
                      line.indexOf("[ERROR]") >= 0);
        } else {
            accept = (line.indexOf("[ERROR]") >= 0);
        }
        if (accept) { out += line; ++kept; }
        if (nl < 0) break;
        start = nl + 1;
    }
    return out;
}

// ── listDir ──────────────────────────────────────────────────
std::vector<SdFileEntry> SdManager::listDir(const String& path) const {
    SdSpiLock _l;
    std::vector<SdFileEntry> result;
    if (!_mounted || !_safePath(path)) return result;

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return result;
    }

    String parent = path;
    if (!parent.endsWith("/")) parent += "/";
    if (parent == "//") parent = "/";

    File entry = dir.openNextFile();
    while (entry) {
        SdFileEntry fe;
        fe.name = String(entry.name());
        int lastSlash = fe.name.lastIndexOf('/');
        if (lastSlash >= 0) fe.name = fe.name.substring(lastSlash + 1);
        fe.isDir    = entry.isDirectory();
        fe.size     = entry.isDirectory() ? 0 : (size_t)entry.size();
        fe.modTime  = (uint32_t)entry.getLastWrite();
        fe.fullPath = (parent == "/") ? ("/" + fe.name) : (parent + fe.name);
        if (!fe.name.isEmpty() && fe.name != "." && fe.name != "..") {
            result.push_back(fe);
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return result;
}

// ── [#6] listDirRecursive ────────────────────────────────────
std::vector<SdFileEntry> SdManager::listDirRecursive(const String& path,
                                                      uint8_t maxDepth) const {
    std::vector<SdFileEntry> result;
    if (!_mounted || !_safePath(path)) return result;
    _listDirRecursiveImpl(path, 0, maxDepth, result);
    return result;
}

void SdManager::_listDirRecursiveImpl(const String& path, uint8_t depth,
                                       uint8_t maxDepth,
                                       std::vector<SdFileEntry>& out) const {
    auto entries = listDir(path);
    for (auto& e : entries) {
        out.push_back(e);
        if (e.isDir && depth < maxDepth) {
            _listDirRecursiveImpl(e.fullPath, depth + 1, maxDepth, out);
        }
    }
}

// ── [#39] listDirPaged ───────────────────────────────────────
SdPage SdManager::listDirPaged(const String& path,
                                uint16_t offset, uint16_t limit) const {
    SdSpiLock _l;
    SdPage page;
    page.total  = 0;
    page.offset = offset;
    auto all = listDir(path);
    page.total = (uint16_t)all.size();
    for (uint16_t i = offset; i < all.size() && page.entries.size() < limit; ++i) {
        page.entries.push_back(all[i]);
    }
    return page;
}

// ── [#40] listDirSorted ──────────────────────────────────────
std::vector<SdFileEntry> SdManager::listDirSorted(const String& path,
                                                    SdSort sort,
                                                    bool asc,
                                                    const String& extFilter) const {
    SdSpiLock _l;
    auto all = listDir(path);

    // Apply extension filter (empty = no filter)
    if (!extFilter.isEmpty()) {
        String lf = extFilter;
        lf.toLowerCase();
        std::vector<SdFileEntry> filtered;
        for (const auto& e : all) {
            String name = e.name;
            name.toLowerCase();
            if (name.endsWith(lf)) filtered.push_back(e);
        }
        all = filtered;
    }

    // Sort
    if (sort == SdSort::NAME) {
        std::sort(all.begin(), all.end(), [asc](const SdFileEntry& a, const SdFileEntry& b) {
            return asc ? (a.name < b.name) : (a.name > b.name);
        });
    } else if (sort == SdSort::SIZE) {
        std::sort(all.begin(), all.end(), [asc](const SdFileEntry& a, const SdFileEntry& b) {
            return asc ? (a.size < b.size) : (a.size > b.size);
        });
    } else { // DATE
        std::sort(all.begin(), all.end(), [asc](const SdFileEntry& a, const SdFileEntry& b) {
            return asc ? (a.modTime < b.modTime) : (a.modTime > b.modTime);
        });
    }
    return all;
}

// ── deleteFile ───────────────────────────────────────────────
bool SdManager::deleteFile(const String& path) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return false;
    if (!SD.exists(path)) return false;
    bool ok = SD.remove(path);
    if (ok) log(String("Deleted: ") + path);
    return ok;
}

// ── [#9] deleteFiles ─────────────────────────────────────────
uint16_t SdManager::deleteFiles(const std::vector<String>& paths) {
    uint16_t count = 0;
    for (const auto& p : paths) {
        if (deleteFile(p)) ++count;
    }
    return count;
}

// ── renameFile ───────────────────────────────────────────────
bool SdManager::renameFile(const String& from, const String& to) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(from) || !_safePath(to)) return false;
    bool ok = SD.rename(from, to);
    if (ok) log(String("Renamed: ") + from + " -> " + to);
    return ok;
}

// ── makeDir ──────────────────────────────────────────────────
bool SdManager::makeDir(const String& path) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return false;
    return SD.mkdir(path);
}

// ── exists ───────────────────────────────────────────────────
bool SdManager::exists(const String& path) const {
    SdSpiLock _l;
    if (!_mounted) return false;
    return SD.exists(path);
}

// ── [#10] dirSize ────────────────────────────────────────────
size_t SdManager::dirSize(const String& path) const {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return 0;
    return _dirSizeImpl(path);
}

size_t SdManager::_dirSizeImpl(const String& path) const {
    size_t total = 0;
    auto entries = listDir(path);
    for (const auto& e : entries) {
        if (e.isDir) {
            total += _dirSizeImpl(e.fullPath);
        } else {
            total += e.size;
        }
    }
    return total;
}

// ── [#11] findFiles ──────────────────────────────────────────
std::vector<SdFileEntry> SdManager::findFiles(const String& dir,
                                               const String& pattern) const {
    SdSpiLock _l;
    std::vector<SdFileEntry> result;
    if (!_mounted) return result;
    String lp = pattern;
    lp.toLowerCase();
    auto all = listDirRecursive(dir, 8);
    for (const auto& e : all) {
        if (e.isDir) continue;
        String name = e.name;
        name.toLowerCase();
        if (name.indexOf(lp) >= 0) {
            result.push_back(e);
        }
    }
    return result;
}

// ── [#12] createTar ──────────────────────────────────────────
// Writes a simple ustar TAR archive.  Skips if dir > 512 KB.
bool SdManager::createTar(const String& dir, const String& outPath) const {
    SdSpiLock _l;
    if (!_mounted || !_safePath(dir) || !_safePath(outPath)) return false;
    // Guard: refuse if dir too large
    if (_dirSizeImpl(dir) > 512UL * 1024UL) return false;

    File out = SD.open(outPath, FILE_WRITE);
    if (!out) return false;

    auto files = listDirRecursive(dir, 8);

    for (const auto& e : files) {
        if (e.isDir) continue;
        File src = SD.open(e.fullPath, FILE_READ);
        if (!src) continue;

        size_t fileSize = (size_t)src.size();
        String relPath  = e.fullPath;
        if (relPath.startsWith(dir)) relPath = relPath.substring(dir.length());
        if (relPath.startsWith("/"))  relPath = relPath.substring(1);

        // Build 512-byte ustar header
        uint8_t header[512];
        memset(header, 0, sizeof(header));

        // name (max 100 chars)
        strncpy((char*)header + 0, relPath.c_str(), 99);
        // mode
        snprintf((char*)header + 100, 8, "%07o", 0644);
        // uid / gid
        snprintf((char*)header + 108, 8, "%07o", 0);
        snprintf((char*)header + 116, 8, "%07o", 0);
        // size (11-char octal)
        snprintf((char*)header + 124, 12, "%011lo", (unsigned long)fileSize);
        // mtime
        snprintf((char*)header + 136, 12, "%011lo", (unsigned long)e.modTime);
        // type flag '0' = regular file
        header[156] = '0';
        // ustar magic
        memcpy((char*)header + 257, "ustar  ", 8);
        // checksum: sum of all header bytes (checksum field treated as spaces)
        memset(header + 148, ' ', 8);
        uint32_t chk = 0;
        for (int i = 0; i < 512; ++i) chk += header[i];
        snprintf((char*)header + 148, 8, "%06o", chk);
        header[155] = ' ';

        out.write(header, 512);

        // Write file data padded to 512-byte boundary
        uint8_t copyBuf[512];
        while (src.available()) {
            size_t n = src.read(copyBuf, sizeof(copyBuf));
            out.write(copyBuf, n);
        }
        // Pad to 512-byte boundary
        size_t rem = (512 - (fileSize % 512)) % 512;
        if (rem > 0) {
            memset(copyBuf, 0, rem);
            out.write(copyBuf, rem);
        }
        src.close();
    }

    // Two 512-byte zero blocks = end-of-archive
    uint8_t eof[1024];
    memset(eof, 0, sizeof(eof));
    out.write(eof, sizeof(eof));
    out.close();
    return true;
}

// ── getFileInfo ──────────────────────────────────────────────
SdFileEntry SdManager::getFileInfo(const String& path) const {
    SdSpiLock _l;
    SdFileEntry fe;
    fe.size    = 0;
    fe.isDir   = false;
    fe.modTime = 0;
    fe.fullPath = path;
    if (!_mounted || !_safePath(path)) return fe;
    File f = SD.open(path);
    if (!f) return fe;
    fe.isDir   = f.isDirectory();
    fe.size    = fe.isDir ? 0 : (size_t)f.size();
    fe.modTime = (uint32_t)f.getLastWrite();
    int sl = path.lastIndexOf('/');
    fe.name    = (sl >= 0) ? path.substring(sl + 1) : path;
    if (fe.name.isEmpty()) fe.name = "/";
    f.close();
    return fe;
}

// ── readTextFile ─────────────────────────────────────────────
String SdManager::readTextFile(const String& path, size_t maxBytes) const {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return "";
    File f = SD.open(path, FILE_READ);
    if (!f) return "";
    size_t fileSize = (size_t)f.size();
    bool truncated  = fileSize > maxBytes;
    size_t readSize = truncated ? maxBytes : fileSize;
    String out = "";
    out.reserve(min(readSize + 20, (size_t)8212));
    size_t read = 0;
    while (f.available() && read < readSize) {
        out += (char)f.read();
        ++read;
    }
    f.close();
    if (truncated) out += "\n[...truncated - showing first " +
                          String((unsigned)maxBytes) + " bytes of " +
                          String((unsigned)fileSize) + " total]";
    return out;
}

// ── [#8] previewFile ─────────────────────────────────────────
SdPreview SdManager::previewFile(const String& path, size_t maxBytes) const {
    SdSpiLock _l;
    SdPreview p;
    p.path     = path;
    p.fileSize = 0;
    p.truncated = false;
    if (!_mounted || !_safePath(path)) return p;
    File f = SD.open(path, FILE_READ);
    if (!f) return p;
    p.fileSize  = (size_t)f.size();
    p.truncated = p.fileSize > maxBytes;
    size_t readSize = p.truncated ? maxBytes : p.fileSize;
    p.content.reserve(min(readSize, (size_t)4096));
    size_t read = 0;
    while (f.available() && read < readSize) {
        p.content += (char)f.read();
        ++read;
    }
    f.close();
    return p;
}

// ── copyFileSd ───────────────────────────────────────────────
bool SdManager::copyFileSd(const String& src, const String& dst) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(src) || !_safePath(dst)) return false;
    if (src == dst) return false;
    if (SD.exists(dst)) {
        File existing = SD.open(dst);
        if (existing && existing.isDirectory()) { existing.close(); return false; }
        if (existing) existing.close();
    }
    bool ok = _copyFile(src, dst, true, true);
    if (ok) log(String("Copied: ") + src + " -> " + dst);
    return ok;
}

// ── moveFile ─────────────────────────────────────────────────
bool SdManager::moveFile(const String& src, const String& dst) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(src) || !_safePath(dst)) return false;
    if (src == dst) return false;
    if (SD.rename(src, dst)) {
        log(String("Moved (rename): ") + src + " -> " + dst);
        return true;
    }
    if (!copyFileSd(src, dst)) return false;
    if (!SD.remove(src)) {
        SD.remove(dst);
        return false;
    }
    log(String("Moved (copy+del): ") + src + " -> " + dst);
    return true;
}

// ── deleteRecursive ──────────────────────────────────────────
bool SdManager::deleteRecursive(const String& path) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return false;
    if (!SD.exists(path)) return false;
    bool ok = _deleteRecursiveImpl(path);
    if (ok) log(String("Deleted (recursive): ") + path);
    return ok;
}

bool SdManager::_deleteRecursiveImpl(const String& path) {
    File f = SD.open(path);
    if (!f) return false;
    if (!f.isDirectory()) {
        f.close();
        return SD.remove(path);
    }
    // FIX: child.name() may return basename only on some SD lib versions,
    // or full path with leading '/' on others. Strip to basename and rebuild
    // the full path from `path` so recursion targets the correct entry.
    String dirPath = path;
    if (dirPath.endsWith("/")) dirPath.remove(dirPath.length() - 1);
    File child = f.openNextFile();
    while (child) {
        String raw = String(child.name());
        child.close();
        int lastSlash = raw.lastIndexOf('/');
        String base = (lastSlash >= 0) ? raw.substring(lastSlash + 1) : raw;
        if (base.isEmpty() || base == "." || base == "..") {
            child = f.openNextFile();
            continue;
        }
        String childPath = dirPath + "/" + base;
        if (!_deleteRecursiveImpl(childPath)) {
            f.close();
            return false;
        }
        child = f.openNextFile();
    }
    f.close();
    return SD.rmdir(path);
}

// ── formatCard ───────────────────────────────────────────────
bool SdManager::formatCard() {
    SdSpiLock _l;
    if (!_mounted) return false;
    Serial.println(DEBUG_TAG " [SD] FORMAT REQUESTED - unmounting before format");
    _flushLogRing();
    SD.end();
    _mounted = false;

    if (!SD.begin(SD_CS_PIN, *_sdSpi, SD_SPI_FREQ)) {
        Serial.println(DEBUG_TAG " [SD] Format: SD.begin() failed");
        return false;
    }
    bool ok = false;
#if defined(SD_FORMAT_AVAILABLE)
    ok = SD.format();
#else
    Serial.println(DEBUG_TAG " [SD] format() not available in this framework version");
    SD.end();
    return false;
#endif
    if (ok) {
        Serial.println(DEBUG_TAG " [SD] Format complete - remounting");
        _mount();
    } else {
        Serial.println(DEBUG_TAG " [SD] Format failed");
        SD.end();
    }
    return ok;
}

// ── [#37] safeWriteFile ──────────────────────────────────────
bool SdManager::safeWriteFile(const String& path, const String& data) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return false;
    String tmpPath = path + ".tmp";
    File f = SD.open(tmpPath, FILE_WRITE);
    if (!f) return false;
    f.print(data);
    f.close();
    if (SD.exists(path)) SD.remove(path);
    return SD.rename(tmpPath, path);
}

// ── [#33] writeAsync ─────────────────────────────────────────
bool SdManager::writeAsync(const String& path, const String& data, bool append) {
    if (!_writeQueue) return false;
    SdWriteJob* job = new SdWriteJob{ path, data, append };
    if (xQueueSend(_writeQueue, &job, 0) != pdTRUE) {
        delete job;
        return false;
    }
    return true;
}

void SdManager::_drainWriteQueue() {
    SdSpiLock _l;
    if (!_writeQueue || !_mounted) return;
    SdWriteJob* job = nullptr;
    while (xQueueReceive(_writeQueue, &job, 0) == pdTRUE && job) {
        if (_safePath(job->path)) {
            File f = SD.open(job->path, job->append ? FILE_APPEND : FILE_WRITE);
            if (f) {
                f.print(job->data);
                f.close();
            }
        }
        delete job;
        job = nullptr;
    }
}

// ── CRC32 streaming helper ───────────────────────────────────
// FIX: _crc32() is one-shot (re-inits state on every call), so the
// previous chunked usage `runCrc = _crc32(buf+bi, 1)` for each byte
// overwrote the running value with the CRC of the single byte —
// verification "passed" only by accident. _crc32Stream lets callers
// chain across read chunks; pass 0xFFFFFFFFUL as the initial state
// and XOR with 0xFFFFFFFFUL after the final chunk.
static const uint32_t* _crc32Table();
uint32_t SdManager::_crc32Stream(uint32_t crc,
                                  const uint8_t* buf, size_t len) {
    const uint32_t* tbl = _crc32Table();
    for (size_t i = 0; i < len; ++i)
        crc = tbl[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// ── CRC32 lookup table (IEEE 802.3 polynomial, reversed) ─────
// Lifted into file scope so both _crc32() (one-shot) and the new
// _crc32Stream() (chained) share a single 1 KB table.
static const uint32_t* _crc32Table() {
    static const uint32_t table[256] = {
        0x00000000,0x77073096,0xee0e612c,0x990951ba,0x076dc419,0x706af48f,
        0xe963a535,0x9e6495a3,0x0edb8832,0x79dcb8a4,0xe0d5e91b,0x97d2d988,
        0x09b64c2b,0x7eb17cbf,0xe7b82d09,0x90bf1d9f,0x1db71064,0x6ab020f2,
        0xf3b97148,0x84be41de,0x1adad47d,0x6ddde4eb,0xf4d4b551,0x83d385c7,
        0x136c9856,0x646ba8c0,0xfd62f97a,0x8a65c9ec,0x14015c4f,0x63066cd9,
        0xfa0f3d63,0x8d080df5,0x3b6e20c8,0x4c69105e,0xd56041e4,0xa2677172,
        0x3c03e4d1,0x4b04d447,0xd20d85fd,0xa50ab56b,0x35b5a8fa,0x42b2986c,
        0xdbbbc9d6,0xacbcf940,0x32d86ce3,0x45df5c75,0xdcd60dcf,0xabd13d59,
        0x26d930ac,0x51de003a,0xc8d75180,0xbfd06116,0x21b4f6b5,0x56b3c423,
        0xcfba9599,0xb8bda50f,0x2802b89e,0x5f058808,0xc60cd9b2,0xb10be924,
        0x2f6f7c87,0x58684c11,0xc1611dab,0xb6662d3d,0x76dc4190,0x01db7106,
        0x98d220bc,0xefd5102a,0x71b18589,0x06b6b51f,0x9fbfe4a5,0xe8b8d433,
        0x7807c9a2,0x0f00f934,0x9609a88e,0xe10e9818,0x7f6a0dbb,0x086d3d2d,
        0x91646c97,0xe6635c01,0x6b6b51f4,0x1c6c6162,0x856530d8,0xf262004e,
        0x6c0695ed,0x1b01a57b,0x8208f4c1,0xf50fc457,0x65b0d9c6,0x12b7e950,
        0x8bbeb8ea,0xfcb9887c,0x62dd1d7f,0x15da2d49,0x8cd37cf3,0xfbd44c65,
        0x4db26158,0x3ab551ce,0xa3bc0074,0xd4bb30e2,0x4adfa541,0x3dd895d7,
        0xa4d1c46d,0xd3d6f4fb,0x4369e96a,0x346ed9fc,0xad678846,0xda60b8d0,
        0x44042d73,0x33031de5,0xaa0a4c5f,0xdd0d7cc9,0x5005713c,0x270241aa,
        0xbe0b1010,0xc90c2086,0x5768b525,0x206f85b3,0xb966d409,0xce61e49f,
        0x5edef90e,0x29d9c998,0xb0d09822,0xc7d7a8b4,0x59b33d17,0x2eb40d81,
        0xb7bd5c3b,0xc0ba6cad,0xedb88320,0x9abfb3b6,0x03b6e20c,0x74b1d29a,
        0xead54739,0x9dd277af,0x04db2615,0x73dc1683,0xe3630b12,0x94643b84,
        0x0d6d6a3e,0x7a6a5aa8,0xe40ecf0b,0x9309ff9d,0x0a00ae27,0x7d079eb1,
        0xf00f9344,0x8708a3d2,0x1e01f268,0x6906c2fe,0xf762575d,0x806567cb,
        0x196c3671,0x6e6b06e7,0xfed41b76,0x89d32be0,0x10da7a5a,0x67dd4acc,
        0xf9b9df6f,0x8ebeeff9,0x17b7be43,0x60b08ed5,0xd6d6a3e8,0xa1d1937e,
        0x38d8c2c4,0x4fdff252,0xd1bb67f1,0xa6bc5767,0x3fb506dd,0x48b2364b,
        0xd80d2bda,0xaf0a1b4c,0x36034af6,0x41047a60,0xdf60efc3,0xa8670955,
        0x316658ef,0x4661687b,0xb40bbe37,0xc30c8ea1,0x5a05df1b,0x2d02ef8d
    };
    return table;
}

// ── [#34] _crc32 ─────────────────────────────────────────────
uint32_t SdManager::_crc32(const uint8_t* buf, size_t len) {
    return _crc32Stream(0xFFFFFFFFUL, buf, len) ^ 0xFFFFFFFFUL;
}

// ── _copyFile ────────────────────────────────────────────────
bool SdManager::_copyFile(const String& src, const String& dst,
                           bool srcOnSD, bool dstOnSD) {
    uint32_t dummy;
    return _copyFileWithCrc(src, dst, srcOnSD, dstOnSD, dummy);
}

// ── [#34] _copyFileWithCrc ───────────────────────────────────
bool SdManager::_copyFileWithCrc(const String& src, const String& dst,
                                  bool srcOnSD, bool dstOnSD,
                                  uint32_t& crcOut) {
    File srcFile = srcOnSD ? SD.open(src, FILE_READ)
                           : LittleFS.open(src, "r");
    if (!srcFile) {
        Serial.printf(DEBUG_TAG " [SD] _copyFile: cannot open src %s\n", src.c_str());
        return false;
    }
    File dstFile = dstOnSD ? SD.open(dst, FILE_WRITE)
                           : LittleFS.open(dst, "w");
    if (!dstFile) {
        srcFile.close();
        Serial.printf(DEBUG_TAG " [SD] _copyFile: cannot open dst %s\n", dst.c_str());
        return false;
    }

    uint8_t copyBuf[512];
    // CRC FIX: chain real CRC32 state across read chunks. Previous code
    // had a placeholder ((crc>>8)^0) inner loop and also overwrote
    // crcOut with _crc32(copyBuf,n) per chunk (CRC of one chunk, not
    // the whole file) — manifest CRC stored on backup was always the
    // CRC of just the final chunk.
    uint32_t crc = 0xFFFFFFFFUL;
    size_t total = 0;
    bool writeFailed = false;
    while (srcFile.available()) {
        size_t n = srcFile.read(copyBuf, sizeof(copyBuf));
        if (n == 0) break;
        if (dstFile.write(copyBuf, n) != n) {
            writeFailed = true;
            break;
        }
        crc = _crc32Stream(crc, copyBuf, n);
        total += n;
    }
    srcFile.close();
    dstFile.close();
    if (writeFailed) {
        // Leave no partially-written destination behind.
        if (dstOnSD) SD.remove(dst); else LittleFS.remove(dst);
        return false;
    }
    crcOut = crc ^ 0xFFFFFFFFUL;

    Serial.printf(DEBUG_TAG " [SD] Copied %s -> %s (%u bytes, crc=0x%08lx)\n",
                  src.c_str(), dst.c_str(), (unsigned)total,
                  (unsigned long)crcOut);
    return true;
}

// ── exportIRLibrary ──────────────────────────────────────────
bool SdManager::exportIRLibrary(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String path = String(SD_DIR_IR_LIBRARY) + "/" + name + ".json";
    if (!_safePath(path)) return false;

    String json = irDB.exportJson();
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.print(json);
    f.close();
    log(String("IR library exported: ") + name + ".json  (" +
        String((unsigned)irDB.size()) + " buttons)");
    return true;
}

// ── importIRLibrary ──────────────────────────────────────────
bool SdManager::importIRLibrary(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String path = String(SD_DIR_IR_LIBRARY) + "/" + name + ".json";
    if (!_safePath(path) || !SD.exists(path)) return false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    String json = "";
    json.reserve(min((size_t)f.size(), (size_t)65536));
    while (f.available() && json.length() < 65536)
        json += (char)f.read();
    f.close();

    bool ok = irDB.importJson(json);
    if (ok) log(String("IR library imported: ") + name + ".json");
    return ok;
}

// ── listIRLibraries ──────────────────────────────────────────
std::vector<String> SdManager::listIRLibraries() const {
    SdSpiLock _l;
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_IR_LIBRARY);
    for (const auto& e : entries) {
        if (!e.isDir && e.name.endsWith(".json"))
            result.push_back(e.name);
    }
    return result;
}

// ── mergeIRLibrary ───────────────────────────────────────────
int SdManager::mergeIRLibrary(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return -1;
    String path = String(SD_DIR_IR_LIBRARY) + "/" + name;
    if (!path.endsWith(".json")) path += ".json";
    if (!_safePath(path) || !SD.exists(path)) return -1;

    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    String json = "";
    json.reserve(min((size_t)f.size(), (size_t)65536));
    while (f.available() && json.length() < 65536)
        json += (char)f.read();
    f.close();

    int added = irDB.mergeJson(json);
    if (added > 0)
        log(String("IR library merged: ") + name + " (+" + String(added) + " buttons)");
    return added;
}

// ── deleteIRLibrary ──────────────────────────────────────────
bool SdManager::deleteIRLibrary(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String path = String(SD_DIR_IR_LIBRARY) + "/" + name;
    if (!path.endsWith(".json")) path += ".json";
    if (!_safePath(path) || !SD.exists(path)) return false;
    bool ok = SD.remove(path);
    if (ok) log(String("IR library deleted: ") + name);
    return ok;
}

// ── renameIRLibrary ──────────────────────────────────────────
bool SdManager::renameIRLibrary(const String& oldName, const String& newName) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String oldPath = String(SD_DIR_IR_LIBRARY) + "/" + oldName;
    String newPath = String(SD_DIR_IR_LIBRARY) + "/" + newName;
    if (!oldPath.endsWith(".json")) oldPath += ".json";
    if (!newPath.endsWith(".json")) newPath += ".json";
    if (!_safePath(oldPath) || !_safePath(newPath)) return false;
    if (!SD.exists(oldPath) || SD.exists(newPath)) return false;
    bool ok = SD.rename(oldPath, newPath);
    if (ok) log(String("IR library renamed: ") + oldName + " -> " + newName);
    return ok;
}

// ── saveButtonToLibrary ──────────────────────────────────────
bool SdManager::saveButtonToLibrary(const String& libName, uint32_t buttonId) {
    SdSpiLock _l;
    if (!_mounted) return false;
    IRButton btn = irDB.findById(buttonId);
    if (btn.id == 0) return false;

    String path = String(SD_DIR_IR_LIBRARY) + "/" + libName;
    if (!path.endsWith(".json")) path += ".json";
    if (!_safePath(path)) return false;

    String existingJson = "";
    if (SD.exists(path)) {
        File f = SD.open(path, FILE_READ);
        if (f) {
            existingJson.reserve(min((size_t)f.size(), (size_t)65536));
            while (f.available() && existingJson.length() < 65536)
                existingJson += (char)f.read();
            f.close();
        }
    }

    JsonDocument doc;
    bool parsed = false;
    if (!existingJson.isEmpty()) {
        parsed = (deserializeJson(doc, existingJson) == DeserializationError::Ok
                  && doc["buttons"].is<JsonArray>());
    }
    if (!parsed) {
        doc.clear();
        doc["buttons"].to<JsonArray>();
    }

    JsonArray arr = doc["buttons"].as<JsonArray>();
    bool found = false;
    for (JsonObject obj : arr) {
        if (obj["name"] == btn.name) {
            btn.toJson(obj);
            found = true;
            break;
        }
    }
    if (!found) {
        JsonObject newObj = arr.add<JsonObject>();
        btn.toJson(newObj);
    }

    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    log(String("Button '") + btn.name + "' saved to library: " + libName);
    return true;
}

// ── [#19] getLibraryMeta ─────────────────────────────────────
IrLibMeta SdManager::getLibraryMeta(const String& name) const {
    SdSpiLock _l;
    IrLibMeta meta;
    meta.buttonCount = 0;
    if (!_mounted) return meta;
    String path = String(SD_DIR_IR_LIBRARY) + "/" + name;
    if (!path.endsWith(".json")) path += ".json";
    if (!_safePath(path) || !SD.exists(path)) return meta;

    File f = SD.open(path, FILE_READ);
    if (!f) return meta;
    // Read just the first 2KB for meta extraction (avoid loading full file)
    String json = "";
    json.reserve(2048);
    while (f.available() && json.length() < 2048)
        json += (char)f.read();
    f.close();

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return meta;
    meta.name        = doc["name"]    | name;
    meta.device      = doc["device"]  | "";
    meta.author      = doc["author"]  | "";
    meta.created     = doc["created"] | "";
    if (doc["buttons"].is<JsonArray>())
        meta.buttonCount = (uint16_t)doc["buttons"].as<JsonArrayConst>().size();
    return meta;
}

// ── [#20] searchLibrary ──────────────────────────────────────
std::vector<String> SdManager::searchLibrary(const String& libName,
                                               const String& query) const {
    SdSpiLock _l;
    std::vector<String> result;
    if (!_mounted) return result;
    String path = String(SD_DIR_IR_LIBRARY) + "/" + libName;
    if (!path.endsWith(".json")) path += ".json";
    if (!_safePath(path) || !SD.exists(path)) return result;

    File f = SD.open(path, FILE_READ);
    if (!f) return result;
    String json = "";
    json.reserve(min((size_t)f.size(), (size_t)65536));
    while (f.available() && json.length() < 65536)
        json += (char)f.read();
    f.close();

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return result;
    if (!doc["buttons"].is<JsonArray>()) return result;

    String lq = query;
    lq.toLowerCase();
    for (JsonObjectConst btn : doc["buttons"].as<JsonArrayConst>()) {
        String bname = btn["name"] | "";
        String lower = bname;
        lower.toLowerCase();
        if (lower.indexOf(lq) >= 0)
            result.push_back(bname);
    }
    return result;
}

// ── [#21] diffLibraries ──────────────────────────────────────
IrLibDiff SdManager::diffLibraries(const String& nameA,
                                    const String& nameB) const {
    IrLibDiff diff;
    if (!_mounted) return diff;

    auto loadNames = [&](const String& n) -> std::vector<std::pair<String,String>> {
        std::vector<std::pair<String,String>> entries; // name -> serialised for comparison
        String path = String(SD_DIR_IR_LIBRARY) + "/" + n;
        if (!path.endsWith(".json")) path += ".json";
        if (!_safePath(path) || !SD.exists(path)) return entries;
        File f = SD.open(path, FILE_READ);
        if (!f) return entries;
        String json = "";
        json.reserve(min((size_t)f.size(), (size_t)65536));
        while (f.available() && json.length() < 65536) json += (char)f.read();
        f.close();
        JsonDocument doc;
        if (deserializeJson(doc, json) != DeserializationError::Ok) return entries;
        if (!doc["buttons"].is<JsonArray>()) return entries;
        for (JsonObjectConst btn : doc["buttons"].as<JsonArrayConst>()) {
            String bname = btn["name"] | "";
            String serial = "";
            serializeJson(btn, serial);
            entries.push_back({bname, serial});
        }
        return entries;
    };

    auto listA = loadNames(nameA);
    auto listB = loadNames(nameB);

    // Build lookup maps
    std::vector<String> namesA, namesB;
    for (const auto& p : listA) namesA.push_back(p.first);
    for (const auto& p : listB) namesB.push_back(p.first);

    // Added: in B but not A
    for (const auto& p : listB) {
        bool found = false;
        for (const auto& a : listA) if (a.first == p.first) { found = true; break; }
        if (!found) diff.added.push_back(p.first);
    }
    // Removed: in A but not B
    for (const auto& p : listA) {
        bool found = false;
        for (const auto& b : listB) if (b.first == p.first) { found = true; break; }
        if (!found) diff.removed.push_back(p.first);
    }
    // Changed: in both but different serialised form
    for (const auto& pa : listA) {
        for (const auto& pb : listB) {
            if (pa.first == pb.first && pa.second != pb.second) {
                diff.changed.push_back(pa.first);
                break;
            }
        }
    }
    return diff;
}

// ── [#22] setAutoExportLib / _checkAutoExport ────────────────
void SdManager::setAutoExportLib(const String& libName) {
    _autoExportLib   = libName;
    _autoExportDirty = false;
}

void SdManager::_checkAutoExport() {
    if (_autoExportLib.isEmpty()) return;
    // Export when irDB is dirty (method exists) OR when explicitly marked dirty
    bool dbDirty = irDB.isDirty();
    if (!dbDirty && !_autoExportDirty) return;
    exportIRLibrary(_autoExportLib);
    _autoExportDirty = false;
}

// ── [#23] exportIRLibraryVersioned ───────────────────────────
bool SdManager::exportIRLibraryVersioned(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return false;

    // Build timestamped filename
    time_t now = time(nullptr);
    char tsBuf[20];
    if (now > 1700000000UL) {
        struct tm tmbuf, *t = localtime_r(&now, &tmbuf);
        snprintf(tsBuf, sizeof(tsBuf), "%04d%02d%02d%02d%02d%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    } else {
        snprintf(tsBuf, sizeof(tsBuf), "%010lu", (unsigned long)millis());
    }
    String vPath = String(SD_DIR_IR_LIBRARY) + "/" + name + "_" + tsBuf + ".json";
    if (!_safePath(vPath)) return false;

    String json = irDB.exportJson();
    File f = SD.open(vPath, FILE_WRITE);
    if (!f) return false;
    f.print(json);
    f.close();

    // Keep only 3 most recent versioned files for this name prefix
    String prefix = name + "_";
    auto entries = listDir(SD_DIR_IR_LIBRARY);
    std::vector<String> versions;
    for (const auto& e : entries) {
        if (!e.isDir && e.name.startsWith(prefix) && e.name.endsWith(".json")) {
            versions.push_back(e.name);
        }
    }
    std::sort(versions.begin(), versions.end());
    // Delete oldest if more than 3
    while (versions.size() > 3) {
        String del = String(SD_DIR_IR_LIBRARY) + "/" + versions[0];
        SD.remove(del);
        versions.erase(versions.begin());
    }

    log(String("IR library versioned export: ") + vPath);
    return true;
}

// ── [#24] importFlipperIR ────────────────────────────────────
int SdManager::importFlipperIR(const String& path) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path) || !SD.exists(path)) return -1;
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;

    int added = 0;
    String curName, curType, curProtocol;
    uint64_t curAddress = 0, curCommand = 0;
    std::vector<uint16_t> rawData;
    bool inEntry = false;

    // commitEntry defined as local lambda capturing locals
    // (called at each "name:" line and at end of file)
    auto commitEntry = [&]() {
        if (!inEntry || curName.isEmpty()) return;
        IRButton btn;
        btn.name = curName;
        if (curType == "parsed") {
            // Flipper "parsed" entries have address + command; encode into code field
            // code = (address << 16) | command  (NEC-style packing)
            btn.protocol = protocolFromString(curProtocol.c_str());
            if (btn.protocol == IRProtocol::UNKNOWN)
                btn.protocol = IRProtocol::NEC; // fallback
            btn.code = ((uint64_t)curAddress << 16) | (curCommand & 0xFFFF);
            btn.bits = 32;
        } else if (curType == "raw") {
            btn.rawData  = rawData;
            btn.protocol = IRProtocol::RAW;
        } else {
            inEntry = false;
            return;
        }
        if (!btn.name.isEmpty()) {
            irDB.add(btn);
            ++added;
        }
        curName = ""; curType = ""; curProtocol = "";
        curAddress = 0; curCommand = 0;
        rawData.clear();
        inEntry = false;
    };

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.isEmpty() || line.startsWith("#")) continue;

        int colonPos = line.indexOf(':');
        if (colonPos < 0) continue;
        String key = line.substring(0, colonPos);
        String val = line.substring(colonPos + 1);
        key.trim(); val.trim();

        if (key == "name") {
            commitEntry(); // save previous entry if any
            curName  = val;
            inEntry  = true;
        } else if (key == "type") {
            curType  = val;
        } else if (key == "protocol") {
            curProtocol = val;
        } else if (key == "address") {
            curAddress = (uint64_t)strtoull(val.c_str(), nullptr, 0);
        } else if (key == "command") {
            curCommand = (uint64_t)strtoull(val.c_str(), nullptr, 0);
        } else if (key == "data") {
            // Space-separated timing values
            rawData.clear();
            int pos2 = 0;
            while (pos2 < (int)val.length()) {
                int sp = val.indexOf(' ', pos2);
                String tok = (sp >= 0) ? val.substring(pos2, sp) : val.substring(pos2);
                tok.trim();
                if (!tok.isEmpty()) rawData.push_back((uint16_t)tok.toInt());
                if (sp < 0) break;
                pos2 = sp + 1;
            }
        }
    }
    commitEntry(); // last entry
    f.close();

    if (added > 0) log(String("Flipper IR imported: ") + path + " (+" + String(added) + ")");
    return added;
}

// ── Feature 3: bulkImportIRLibraries ─────────────────────────
SdManager::SdImportResult SdManager::bulkImportIRLibraries() {
    SdSpiLock _l;
    SdImportResult result{0, 0, {}};
    if (!_mounted) {
        result.errors.push_back("SD not available");
        return result;
    }
    auto entries = listDir(SD_DIR_IR_LIBRARY);
    for (const auto& e : entries) {
        if (e.isDir) continue;
        String name = e.name;
        if (!name.endsWith(".json")) continue;
        // Strip the .json extension to get the library name
        String libName = name.substring(0, name.length() - 5);
        int added = mergeIRLibrary(libName);
        result.filesProcessed++;
        if (added < 0) {
            result.errors.push_back(String("Failed to merge library: ") + libName);
            log(String("bulkImport: failed to merge ") + libName, SdLogLevel::WARN, "SD_BULK");
        } else {
            result.buttonsAdded += (uint16_t)added;
            log(String("bulkImport: merged ") + libName + " (+" + added + " buttons)", SdLogLevel::INFO, "SD_BULK");
        }
    }
    log(String("bulkImportIRLibraries complete: files=") + result.filesProcessed +
        " added=" + result.buttonsAdded, SdLogLevel::INFO, "SD_BULK");
    return result;
}

// ── Feature 4: setAutoRawDump ─────────────────────────────────
void SdManager::setAutoRawDump(bool en) {
    _autoRawDump = en;
    // Persist preference to LittleFS /sd_prefs.json
    File f = LittleFS.open("/sd_prefs.json", "r");
    JsonDocument doc;
    if (f) {
        deserializeJson(doc, f);
        f.close();
    }
    doc["autoRawDump"] = en;
    f = LittleFS.open("/sd_prefs.json", "w");
    if (f) {
        serializeJson(doc, f);
        f.close();
    }
    log(String("autoRawDump ") + (en ? "enabled" : "disabled"), SdLogLevel::INFO, "SD_PREFS");
}

// ── Feature 5: setDefaultLib ──────────────────────────────────
void SdManager::setDefaultLib(const String& name) {
    _defaultLib = name;
    log(String("Default IR library set to: ") + name, SdLogLevel::INFO, "SD");
}

// ── saveRawDump ───────────────────────────────────────────────
bool SdManager::saveRawDump(const String& name, const uint16_t* data,
                              size_t len, uint16_t freqKHz) {
    SdSpiLock _l;
    if (!_mounted || !data || len == 0) return false;
    String path = String(SD_DIR_RAW_DUMPS) + "/" + name + ".csv";
    if (!_safePath(path)) return false;

    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.printf("# RAW IR dump: %s  freq=%u kHz  samples=%u\n",
             name.c_str(), (unsigned)freqKHz, (unsigned)len);
    f.print("idx,us\n");
    for (size_t i = 0; i < len; ++i)
        f.printf("%u,%u\n", (unsigned)i, (unsigned)data[i]);
    f.close();
    log(String("Raw IR dump saved: ") + name + ".csv");
    return true;
}

// ── [#14][#34] backupToSD with manifest and CRC ──────────────
bool SdManager::backupToSD(const String& tag) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;
    if (!_safePath(dir)) return false;
    SD.mkdir(dir);

    const char* files[] = {
        "/ir_database.json", "/config.json", "/groups.json",
        "/schedules.json",   "/ntp_config.json", "/ir_autosave.json",
        "/ir_pins.json",     nullptr
    };

    // [#14] Build manifest JSON
    JsonDocument manifest;
    manifest["tag"]       = tag;
    manifest["timestamp"] = (unsigned long)time(nullptr);
    JsonArray fileArr = manifest["files"].to<JsonArray>();

    uint8_t copied = 0;
    for (const char** fp = files; *fp; ++fp) {
        if (!LittleFS.exists(*fp)) continue;
        String dst = dir + *fp;
        uint32_t crc = 0;
        if (_copyFileWithCrc(*fp, dst, false, true, crc)) {
            ++copied;
            // [#14] Add to manifest
            JsonObject fobj = fileArr.add<JsonObject>();
            fobj["name"] = String(*fp);
            // Get size
            File sf = LittleFS.open(*fp, "r");
            fobj["size"] = sf ? (unsigned long)sf.size() : 0UL;
            if (sf) sf.close();
            // [#34] CRC32
            fobj["crc32"] = (unsigned long)crc;
        }
    }

    // Write manifest
    String manifestPath = dir + "/manifest.json";
    File mf = SD.open(manifestPath, FILE_WRITE);
    if (mf) {
        serializeJson(manifest, mf);
        mf.close();
    }

    log(String("Backup created: ") + tag + " (" + String(copied) + " files)");
    return (copied > 0);
}

// ── [#34] restoreFromSD with CRC verification ────────────────
bool SdManager::restoreFromSD(const String& tag) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;
    if (!_safePath(dir) || !SD.exists(dir)) return false;

    // Load manifest if available
    String manifestPath = dir + "/manifest.json";
    JsonDocument manifest;
    bool hasManifest = false;
    if (SD.exists(manifestPath)) {
        File mf = SD.open(manifestPath, FILE_READ);
        if (mf) {
            String mjs = "";
            mjs.reserve(min((size_t)mf.size(), (size_t)4096));
            while (mf.available() && mjs.length() < 4096) mjs += (char)mf.read();
            mf.close();
            hasManifest = (deserializeJson(manifest, mjs) == DeserializationError::Ok);
        }
    }

    auto entries = listDir(dir);
    uint8_t restored = 0;
    for (const auto& e : entries) {
        if (e.isDir || e.name == "manifest.json" || e.name == "littlefs.bin") continue;
        String src = dir + "/" + e.name;
        String dst = "/" + e.name;

        // [#34] Verify CRC before restore if manifest available
        if (hasManifest && manifest["files"].is<JsonArray>()) {
            uint32_t storedCrc = 0;
            bool foundInManifest = false;
            for (JsonObjectConst fobj : manifest["files"].as<JsonArrayConst>()) {
                String mname = fobj["name"] | "";
                if (mname == dst || mname == ("/" + e.name)) {
                    storedCrc = fobj["crc32"] | (uint32_t)0;
                    foundInManifest = true;
                    break;
                }
            }
            if (foundInManifest && storedCrc != 0) {
                // CRC FIX: stream the file once with chained CRC32 state.
                // Previously the inner loop did `runCrc = _crc32(buf+bi,1)`
                // (re-init on every byte → always CRC of the last byte) and
                // then re-opened the file to load the WHOLE thing into a
                // std::vector<uint8_t> just to recompute the CRC. Both
                // passes were broken AND the duplicate read could OOM on
                // multi-KB JSON.
                File sf = SD.open(src, FILE_READ);
                if (sf) {
                    uint8_t buf[512];
                    uint32_t runCrc = 0xFFFFFFFFUL;
                    while (sf.available()) {
                        size_t n = sf.read(buf, sizeof(buf));
                        runCrc = _crc32Stream(runCrc, buf, n);
                    }
                    uint32_t fileCrc = runCrc ^ 0xFFFFFFFFUL;
                    sf.close();
                    if (fileCrc != storedCrc) {
                        log(String("CRC mismatch on restore: ") + e.name +
                            " (expected " + String(storedCrc) + " got " + String(fileCrc) + ")",
                            SdLogLevel::ERROR);
                        continue; // skip this file
                    }
                }
            }
        }

        if (_copyFile(src, dst, true, false)) ++restored;
    }
    log(String("Restored from SD: ") + tag + " (" + String(restored) + " files)");
    return (restored > 0);
}

// ── listBackups ──────────────────────────────────────────────
std::vector<String> SdManager::listBackups() const {
    SdSpiLock _l;
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_BACKUPS);
    for (const auto& e : entries) {
        if (e.isDir) result.push_back(e.name);
    }
    return result;
}

// ── [#13] setAutoBackup / _checkAutoBackup ───────────────────
void SdManager::setAutoBackup(bool en, uint32_t intervalMs) {
    _autoBackupEnabled    = en;
    _autoBackupIntervalMs = intervalMs;
    _lastAutoBackupMs     = millis();
}

void SdManager::_checkAutoBackup() {
    if (!_autoBackupEnabled || _autoBackupIntervalMs == 0) return;
    unsigned long now = millis();
    if ((now - _lastAutoBackupMs) >= _autoBackupIntervalMs) {
        _lastAutoBackupMs = now;
        // Build tag from timestamp
        time_t ts = time(nullptr);
        char tag[24];
        if (ts > 1700000000UL) {
            struct tm tmbuf, *t = localtime_r(&ts, &tmbuf);
            snprintf(tag, sizeof(tag), "auto_%04d%02d%02d%02d%02d%02d",
                     t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                     t->tm_hour, t->tm_min, t->tm_sec);
        } else {
            snprintf(tag, sizeof(tag), "auto_%010lu", (unsigned long)millis());
        }
        backupToSD(String(tag));
    }
}

// ── [#15] backupDeltaToSD ────────────────────────────────────
bool SdManager::backupDeltaToSD(const String& tag) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;
    if (!_safePath(dir)) return false;
    SD.mkdir(dir);

    const char* files[] = {
        "/ir_database.json", "/config.json", "/groups.json",
        "/schedules.json",   "/ntp_config.json", "/ir_autosave.json",
        "/ir_pins.json",     nullptr
    };

    uint8_t copied = 0;
    for (const char** fp = files; *fp; ++fp) {
        if (!LittleFS.exists(*fp)) continue;
        String dst = dir + *fp;

        // Get source size
        File srcF = LittleFS.open(*fp, "r");
        size_t srcSize = srcF ? (size_t)srcF.size() : 0;
        if (srcF) srcF.close();

        // Get existing backup size (if any)
        size_t dstSize = 0;
        if (SD.exists(dst)) {
            File dstF = SD.open(dst, FILE_READ);
            dstSize = dstF ? (size_t)dstF.size() : 0;
            if (dstF) dstF.close();
        }

        // Only copy if size differs (delta check)
        if (srcSize != dstSize) {
            uint32_t dummy;
            if (_copyFileWithCrc(*fp, dst, false, true, dummy)) ++copied;
        }
    }
    log(String("Delta backup: ") + tag + " (" + String(copied) + " files updated)");
    return (copied > 0);
}

// ── [#16] backupLittleFSImage ────────────────────────────────
bool SdManager::backupLittleFSImage(const String& tag) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;
    if (!_safePath(dir)) return false;
    SD.mkdir(dir);

    String imgPath = dir + "/littlefs.bin";

    // Locate the SPIFFS/LittleFS partition
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        nullptr);
    if (!it) {
        log("backupLittleFSImage: SPIFFS partition not found", SdLogLevel::ERROR);
        return false;
    }
    const esp_partition_t* part = esp_partition_get(it);
    esp_partition_iterator_release(it);
    if (!part) return false;

    File out = SD.open(imgPath, FILE_WRITE);
    if (!out) return false;

    const size_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    size_t offset = 0;
    bool ok = true;

    while (offset < part->size) {
        size_t toRead = min(CHUNK, part->size - offset);
        esp_err_t err = esp_partition_read(part, offset, buf, toRead);
        if (err != ESP_OK) {
            log(String("backupLittleFSImage: read error at offset ") + String((unsigned)offset),
                SdLogLevel::ERROR);
            ok = false;
            break;
        }
        out.write(buf, toRead);
        offset += toRead;
    }
    out.close();

    if (ok) {
        log(String("LittleFS image backed up: ") + imgPath +
            " (" + String((unsigned)(part->size / 1024)) + " KB)");
    }
    return ok;
}

// ── [#17] pruneBackups ───────────────────────────────────────
void SdManager::pruneBackups(uint8_t maxKeep) {
    SdSpiLock _l;
    if (!_mounted) return;
    auto entries = listDir(SD_DIR_BACKUPS);
    std::vector<String> dirs;
    for (const auto& e : entries) {
        if (e.isDir) dirs.push_back(e.name);
    }
    std::sort(dirs.begin(), dirs.end()); // oldest (lexically smallest) first
    while (dirs.size() > (size_t)maxKeep) {
        String delPath = String(SD_DIR_BACKUPS) + "/" + dirs[0];
        _deleteRecursiveImpl(delPath);
        log(String("Pruned old backup: ") + dirs[0]);
        dirs.erase(dirs.begin());
    }
}

// ── [#18] restoreDryRun ──────────────────────────────────────
std::vector<String> SdManager::restoreDryRun(const String& tag) const {
    SdSpiLock _l;
    std::vector<String> overwritten;
    if (!_mounted) return overwritten;
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;
    if (!_safePath(dir) || !SD.exists(dir)) return overwritten;

    auto entries = listDir(dir);
    for (const auto& e : entries) {
        if (e.isDir || e.name == "manifest.json" || e.name == "littlefs.bin") continue;
        String dst = "/" + e.name;
        if (LittleFS.exists(dst)) overwritten.push_back(dst);
    }
    return overwritten;
}

// ── queueMacro ───────────────────────────────────────────────
bool SdManager::queueMacro(const String& filename) {
    SdSpiLock _l;
    if (!_mounted) return false;
    if (_macroRunning) return false;

    String path = String(SD_DIR_MACROS) + "/" + filename;
    if (!_safePath(path) || !SD.exists(path)) return false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    String json = "";
    json.reserve(min((size_t)f.size(), (size_t)4096));
    while (f.available() && json.length() < 4096) json += (char)f.read();
    f.close();

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

    JsonArrayConst steps = doc["steps"].as<JsonArrayConst>();
    _macroSteps.clear();
    uint8_t count = 0;
    for (JsonObjectConst s : steps) {
        if (count >= SD_MACRO_MAX_STEPS) break;
        MacroStep ms;
        ms.buttonId      = s["buttonId"]     | (uint32_t)0;
        ms.delayAfterMs  = s["delayAfterMs"] | (uint32_t)100;
        // [#25] conditions
        ms.condTagUid    = s["condTagUid"]   | String("");
        ms.condTimeAfter = s["condTimeAfter"]| String("");
        // [#26] repeat
        ms.repeatCount   = s["repeatCount"]  | (uint8_t)0;
        ms.repeatDelayMs = s["repeatDelayMs"]| (uint32_t)0;
        // [#27] chaining
        ms.chainMacro    = s["chainMacro"]   | String("");

        if (ms.buttonId > 0 || !ms.chainMacro.isEmpty()) {
            _macroSteps.push_back(ms);
            ++count;
        }
    }

    if (_macroSteps.empty()) return false;

    _macroStepIdx  = 0;
    _macroRepeatIdx = 0;
    _macroNextMs   = millis();
    _macroRunning  = true;

    log(String("Macro queued: ") + filename + " (" + String(count) + " steps)");
    return true;
}

// ── [#25][#26][#27][#28] _tickMacro ──────────────────────────
void SdManager::_tickMacro() {
    if (!_macroRunning) return;
    if (millis() < _macroNextMs) return;

    if (_macroStepIdx >= _macroSteps.size()) {
        _macroRunning = false;
        log("Macro completed.");
        _macroWsStatus = "{\"done\":true}";
        return;
    }

    const MacroStep& step = _macroSteps[_macroStepIdx];

    // [#25] Condition: NFC tag UID check
    if (!step.condTagUid.isEmpty()) {
        String lastUid = nfcModule.lastTag().uid;
        if (lastUid != step.condTagUid) {
            Serial.printf(DEBUG_TAG " [SD-Macro] Step %u: condTagUid mismatch - skipped\n",
                          (unsigned)_macroStepIdx);
            ++_macroStepIdx;
            _macroRepeatIdx = 0;
            _macroNextMs = millis();
            return;
        }
    }

    // [#25] Condition: time-after check (format "HH:MM")
    if (!step.condTimeAfter.isEmpty()) {
        time_t now = time(nullptr);
        struct tm tmbuf, *t = localtime_r(&now, &tmbuf);
        int curMins = t->tm_hour * 60 + t->tm_min;
        int idx = step.condTimeAfter.indexOf(':');
        if (idx > 0) {
            int h = step.condTimeAfter.substring(0, idx).toInt();
            int m = step.condTimeAfter.substring(idx + 1).toInt();
            int condMins = h * 60 + m;
            if (curMins < condMins) {
                Serial.printf(DEBUG_TAG " [SD-Macro] Step %u: condTimeAfter not met - skipped\n",
                              (unsigned)_macroStepIdx);
                ++_macroStepIdx;
                _macroRepeatIdx = 0;
                _macroNextMs = millis();
                return;
            }
        }
    }

    // [#27] Macro chaining: if chainMacro non-empty and buttonId==0
    if (!step.chainMacro.isEmpty() && step.buttonId == 0) {
        log(String("Macro chain -> ") + step.chainMacro);
        ++_macroStepIdx;
        _macroRepeatIdx = 0;
        _macroRunning = false; // will be re-queued below
        queueMacro(step.chainMacro);
        return;
    }

    // Execute button
    IRButton btn = irDB.findById(step.buttonId);
    if (btn.id) {
        irTransmitter.transmitAsync(btn);
        Serial.printf(DEBUG_TAG " [SD-Macro] Step %u rep %u: TX btn=%u '%s'\n",
                      (unsigned)_macroStepIdx, (unsigned)_macroRepeatIdx,
                      btn.id, btn.name.c_str());
    }

    // [#28] Update WebSocket macro status
    _macroWsStatus = "{\"step\":" + String((unsigned)_macroStepIdx) +
                     ",\"total\":" + String((unsigned)_macroSteps.size()) +
                     ",\"button\":\"" + (btn.id ? btn.name : String("?")) + "\"}";

    // [#26] Repeat logic
    if (_macroRepeatIdx < step.repeatCount) {
        ++_macroRepeatIdx;
        _macroNextMs = millis() + (step.repeatDelayMs > 0 ? step.repeatDelayMs : step.delayAfterMs);
    } else {
        _macroRepeatIdx = 0;
        ++_macroStepIdx;
        _macroNextMs = millis() + step.delayAfterMs;
    }

    if (_macroStepIdx >= _macroSteps.size()) {
        _macroRunning = false;
        log("Macro completed.");
        _macroWsStatus = "{\"done\":true}";
    }
}

// ── listMacros ───────────────────────────────────────────────
std::vector<String> SdManager::listMacros() const {
    SdSpiLock _l;
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_MACROS);
    for (const auto& e : entries) {
        if (!e.isDir && e.name.endsWith(".json"))
            result.push_back(e.name);
    }
    return result;
}

// ── [#29] stopMacro ──────────────────────────────────────────
void SdManager::stopMacro() {
    _macroRunning = false;
    _macroSteps.clear();
    _macroStepIdx   = 0;
    _macroRepeatIdx = 0;
    _macroWsStatus  = "{\"stopped\":true}";
    log("Macro stopped by user.");
}

// ── [#28] pollMacroStatus ────────────────────────────────────
String SdManager::pollMacroStatus() {
    String out = _macroWsStatus;
    _macroWsStatus = "";
    return out;
}

// ── [#30] Macro recorder ─────────────────────────────────────
void SdManager::startRecord(const String& filename) {
    if (!_mounted) return;
    if (_recording) stopRecord();
    String path = String(SD_DIR_MACROS) + "/" + filename;
    if (!path.endsWith(".json")) path += ".json";
    if (!_safePath(path)) return;
    _recordFilename  = filename;
    _recordStepCount = 0;
    _recordFile = SD.open(path, FILE_WRITE);
    if (_recordFile) {
        _recordFile.print("{\"name\":\"");
        _recordFile.print(filename);
        _recordFile.print("\",\"steps\":[");
        _recording = true;
        log(String("Macro recording started: ") + filename);
    }
}

void SdManager::recordStep(uint32_t buttonId) {
    if (!_recording || !_recordFile) return;
    if (_recordStepCount > 0) _recordFile.print(",");
    _recordFile.printf("{\"buttonId\":%lu,\"delayAfterMs\":200}",
                       (unsigned long)buttonId);
    ++_recordStepCount;
}

void SdManager::stopRecord() {
    if (!_recordFile) { _recording = false; return; }
    _recordFile.print("]}");
    _recordFile.close();
    _recording = false;
    log(String("Macro recording stopped: ") + _recordFilename +
        " (" + String(_recordStepCount) + " steps)");
}

// ── assetPath / hasAsset ─────────────────────────────────────
String SdManager::assetPath(const String& name) const {
    SdSpiLock _l;
    if (!_mounted) return "";
    String path = String(SD_DIR_ASSETS) + "/" + name;
    if (!_safePath(path)) return "";
    return SD.exists(path) ? path : "";
}

bool SdManager::hasAsset(const String& name) const {
    SdSpiLock _l;
    return !assetPath(name).isEmpty();
}

// ── listDeviceProfiles ───────────────────────────────────────
std::vector<String> SdManager::listDeviceProfiles() const {
    SdSpiLock _l;
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_DEVICES);
    for (const auto& e : entries) {
        if (!e.isDir && e.name.endsWith(".json"))
            result.push_back(e.name);
    }
    return result;
}

// ── readDeviceProfile ────────────────────────────────────────
String SdManager::readDeviceProfile(const String& name) const {
    SdSpiLock _l;
    if (!_mounted) return "";
    String path = String(SD_DIR_DEVICES) + "/" + name;
    if (!_safePath(path) || !SD.exists(path)) return "";
    File f = SD.open(path, FILE_READ);
    if (!f) return "";
    String out = "";
    out.reserve(min((size_t)f.size(), (size_t)16384));
    while (f.available() && out.length() < 16384) out += (char)f.read();
    f.close();
    return out;
}

// ── saveDeviceProfile ────────────────────────────────────────
bool SdManager::saveDeviceProfile(const String& name, const String& json) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String safeName = name;
    safeName.replace("/", "_"); safeName.replace(" ", "_");
    if (!safeName.endsWith(".json")) safeName += ".json";
    String path = String(SD_DIR_DEVICES) + "/" + safeName;
    if (!_safePath(path)) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.print(json);
    f.close();
    log(String("Device profile saved: ") + safeName);
    return true;
}

// ── deleteDeviceProfile ──────────────────────────────────────
bool SdManager::deleteDeviceProfile(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return false;
    String path = String(SD_DIR_DEVICES) + "/" + name;
    if (!path.endsWith(".json")) path += ".json";
    if (!_safePath(path) || !SD.exists(path)) return false;
    bool ok = SD.remove(path);
    if (ok) log(String("Device profile deleted: ") + name);
    return ok;
}

// ── importDeviceProfileButtons ───────────────────────────────
int SdManager::importDeviceProfileButtons(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return -1;
    String profile = readDeviceProfile(name);
    if (profile.isEmpty()) return -1;
    int added = irDB.mergeJson(profile);
    if (added > 0)
        log(String("Device profile buttons imported: ") + name +
            " (+" + String(added) + ")");
    return added;
}

// ── [#31] getProfileMeta ─────────────────────────────────────
DeviceProfileMeta SdManager::getProfileMeta(const String& name) const {
    SdSpiLock _l;
    DeviceProfileMeta meta;
    meta.buttonCount = 0;
    if (!_mounted) return meta;
    String profile = readDeviceProfile(name);
    if (profile.isEmpty()) return meta;

    JsonDocument doc;
    if (deserializeJson(doc, profile) != DeserializationError::Ok) return meta;
    meta.name        = doc["name"]     | name;
    meta.brand       = doc["brand"]    | "";
    meta.model       = doc["model"]    | "";
    meta.protocol    = doc["protocol"] | "";
    if (doc["buttons"].is<JsonArray>())
        meta.buttonCount = (uint16_t)doc["buttons"].as<JsonArrayConst>().size();
    return meta;
}

// ── [#35] benchmark ──────────────────────────────────────────
SdBenchmark SdManager::benchmark(size_t testBytes) {
    SdSpiLock _l;
    SdBenchmark result = {0, 0, (uint32_t)testBytes};
    if (!_mounted) return result;

    const String testPath = "/bench_tmp.bin";
    uint8_t buf[512];
    memset(buf, 0xAB, sizeof(buf));

    // Write test
    unsigned long t0 = millis();
    File wf = SD.open(testPath, FILE_WRITE);
    if (wf) {
        size_t written = 0;
        while (written < testBytes) {
            size_t n = min(sizeof(buf), testBytes - written);
            wf.write(buf, n);
            written += n;
        }
        wf.close();
    }
    unsigned long writeMs = millis() - t0;
    if (writeMs > 0) result.writeKBps = (uint32_t)((testBytes / 1024UL) * 1000UL / writeMs);

    // Read test
    t0 = millis();
    File rf = SD.open(testPath, FILE_READ);
    if (rf) {
        while (rf.available()) rf.read(buf, sizeof(buf));
        rf.close();
    }
    unsigned long readMs = millis() - t0;
    if (readMs > 0) result.readKBps = (uint32_t)((testBytes / 1024UL) * 1000UL / readMs);

    SD.remove(testPath);
    log(String("Benchmark: write=") + String(result.writeKBps) +
        " KB/s  read=" + String(result.readKBps) + " KB/s");
    return result;
}

// ── [#38] healthStats ────────────────────────────────────────
SdHealth SdManager::healthStats() const {
    SdSpiLock _l;
    SdHealth h;
    h.totalBytes  = _mounted ? SD.totalBytes() : 0;
    h.usedBytes   = _mounted ? SD.usedBytes()  : 0;
    h.usedPct     = (h.totalBytes > 0) ? (uint8_t)(h.usedBytes * 100 / h.totalBytes) : 0;
    h.cardType    = status().cardTypeStr;
    h.mountCount  = _mountCount;
    return h;
}

// ── [#41] usageJson ──────────────────────────────────────────
String SdManager::usageJson() const {
    SdSpiLock _l;
    uint64_t total = _mounted ? SD.totalBytes() : 0;
    uint64_t used  = _mounted ? SD.usedBytes()  : 0;
    uint64_t free_ = total - used;

    String out = "{";
    out += "\"total\":"   + String((unsigned long long)total) + ",";
    out += "\"used\":"    + String((unsigned long long)used)  + ",";
    out += "\"free\":"    + String((unsigned long long)free_) + ",";
    out += "\"dirs\":{";
    out += "\"ir_library\":"  + String((unsigned long long)(_mounted ? _dirSizeImpl(SD_DIR_IR_LIBRARY) : 0)) + ",";
    out += "\"backups\":"     + String((unsigned long long)(_mounted ? _dirSizeImpl(SD_DIR_BACKUPS)    : 0)) + ",";
    out += "\"macros\":"      + String((unsigned long long)(_mounted ? _dirSizeImpl(SD_DIR_MACROS)     : 0)) + ",";
    out += "\"logs\":"        + String((unsigned long long)(_mounted ? _dirSizeImpl(SD_DIR_LOGS)       : 0)) + ",";
    out += "\"raw_dumps\":"   + String((unsigned long long)(_mounted ? _dirSizeImpl(SD_DIR_RAW_DUMPS)  : 0)) + ",";
    out += "\"devices\":"     + String((unsigned long long)(_mounted ? _dirSizeImpl(SD_DIR_DEVICES)    : 0));
    out += "}}";
    return out;
}

// ── [#42] SD event queue ─────────────────────────────────────
void SdManager::_queueSdEvent(const char* event) {
    if (!_sdEventQueue.isEmpty()) _sdEventQueue += ",";
    _sdEventQueue += String(event);
}

String SdManager::pollSdEvents() {
    String out = _sdEventQueue;
    _sdEventQueue = "";
    return out;
}

// ── [#44] JSON helpers ────────────────────────────────────────
String SdManager::fileEntryToJson(const SdFileEntry& e) const {
    String out = "{";
    out += "\"name\":\"" + e.name + "\",";
    out += "\"isDir\":"  + String(e.isDir ? "true" : "false") + ",";
    out += "\"size\":"   + String((unsigned long)e.size) + ",";
    out += "\"modTime\":" + String((unsigned long)e.modTime) + ",";
    out += "\"fullPath\":\"" + e.fullPath + "\"";
    out += "}";
    return out;
}

String SdManager::fileListToJson(const std::vector<SdFileEntry>& v) const {
    String out = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ",";
        out += fileEntryToJson(v[i]);
    }
    out += "]";
    return out;
}

// ── openForRead / openForWrite ───────────────────────────────
File SdManager::openForRead(const String& path) const {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return File();
    return SD.open(path, FILE_READ);
}

File SdManager::openForWrite(const String& path) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return File();
    return SD.open(path, FILE_WRITE);
}

// ── [#7] Upload helpers ───────────────────────────────────────
bool SdManager::beginUpload(const String& path) {
    SdSpiLock _l;
    if (!_mounted || !_safePath(path)) return false;
    if (_uploadOpen) abortUpload();
    _uploadPath = path; // [#7] store path
    _uploadFile = SD.open(path, FILE_WRITE);
    _uploadOpen = (bool)_uploadFile;
    return _uploadOpen;
}

bool SdManager::writeUploadChunk(const uint8_t* data, size_t len) {
    SdSpiLock _l;
    if (!_uploadOpen || !_uploadFile) return false;
    return _uploadFile.write(data, len) == len;
}

bool SdManager::endUpload() {
    SdSpiLock _l;
    if (!_uploadOpen) return false;
    size_t sz = _uploadFile.size();
    _uploadFile.close();
    _uploadOpen = false;
    log(String("Upload complete: ") + _uploadPath +
        " (" + String((unsigned)sz) + " bytes)");
    return true;
}

void SdManager::abortUpload() {
    SdSpiLock _l;
    if (_uploadFile) _uploadFile.close();
    _uploadOpen = false;
}

// ─────────────────────────────────────────────────────────────
//  Feature 46: fullBackup - enumerate ALL LittleFS .json files
// ─────────────────────────────────────────────────────────────
bool SdManager::fullBackup(const String& tag) {
    SdSpiLock _l;
    if (!_mounted) return false;

    // First do the standard backup (covers known config files + manifest)
    bool ok = backupToSD(tag);

    // Then additionally copy every .json file found in LittleFS root
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;

    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return ok;
    }

    File f = root.openNextFile();
    while (f) {
        String fname = String(f.name());
        // Only copy .json files not already covered by standard backup
        if (!f.isDirectory() && fname.endsWith(".json")) {
            String srcPath = String("/") + fname;
            String dstPath = dir + "/" + fname;
            // Copy if destination doesn't already exist (standard backup handles known ones)
            if (!SD.exists(dstPath)) {
                _copyFile(srcPath, dstPath, false, true);
            }
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();

    log(String("fullBackup complete: tag=") + tag, SdLogLevel::INFO, "SD_BACKUP");
    return ok;
}

// ─────────────────────────────────────────────────────────────
//  Feature 47: factoryResetWithRestore
// ─────────────────────────────────────────────────────────────
bool SdManager::factoryResetWithRestore() {
    SdSpiLock _l;
    if (!_mounted) return false;

    auto backups = listBackups();
    if (backups.empty()) {
        log("factoryResetWithRestore: no backups found", SdLogLevel::WARN, "SD_BACKUP");
        return false;
    }

    // Sort descending to get the newest (backups are timestamp-named)
    std::sort(backups.begin(), backups.end(),
              [](const String& a, const String& b) { return a > b; });

    String newest = backups.front();
    log(String("factoryResetWithRestore: restoring from ") + newest,
        SdLogLevel::INFO, "SD_BACKUP");

    bool ok = restoreFromSD(newest);
    if (ok) {
        log("factoryResetWithRestore: restore OK, restarting...",
            SdLogLevel::INFO, "SD_BACKUP");
        delay(500);
        ESP.restart();
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────
//  Feature 48: Config profiles on SD
// ─────────────────────────────────────────────────────────────
bool SdManager::saveConfigProfile(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return false;

    String dir = String(SD_DIR_PROFILES) + "/" + name;
    if (!SD.exists(SD_DIR_PROFILES)) SD.mkdir(SD_DIR_PROFILES);
    if (!SD.exists(dir))             SD.mkdir(dir);

    uint8_t copied = 0;
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return false;
    }

    File f = root.openNextFile();
    while (f) {
        String fname = String(f.name());
        if (!f.isDirectory() && fname.endsWith(".json")) {
            String srcPath = String("/") + fname;
            String dstPath = dir + "/" + fname;
            if (_copyFile(srcPath, dstPath, false, true)) ++copied;
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();

    log(String("saveConfigProfile: ") + name + " (" + copied + " files)",
        SdLogLevel::INFO, "SD_PROFILE");
    return (copied > 0);
}

bool SdManager::loadConfigProfile(const String& name) {
    SdSpiLock _l;
    if (!_mounted) return false;

    String dir = String(SD_DIR_PROFILES) + "/" + name;
    if (!SD.exists(dir)) return false;

    auto entries = listDir(dir);
    uint8_t restored = 0;
    for (const auto& e : entries) {
        if (e.isDir || !e.name.endsWith(".json")) continue;
        String src = dir + "/" + e.name;
        String dst = "/" + e.name;
        if (_copyFile(src, dst, true, false)) ++restored;
    }

    log(String("loadConfigProfile: ") + name + " (" + restored + " files)",
        SdLogLevel::INFO, "SD_PROFILE");
    return (restored > 0);
}

std::vector<String> SdManager::listConfigProfiles() const {
    SdSpiLock _l;
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_PROFILES);
    for (const auto& e : entries) {
        if (e.isDir) result.push_back(e.name);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────
//  Feature 49: _applyBootConfig - reads /boot_config.json from SD
//  and writes matching LittleFS config files.
//  Called from _ensureDirectories() after first mount.
// ─────────────────────────────────────────────────────────────
void SdManager::_applyBootConfig() {
    if (!_mounted) return;

    const char* bootCfgPath = "/boot_config.json";
    if (!SD.exists(bootCfgPath)) return;

    File f = SD.open(bootCfgPath, FILE_READ);
    if (!f) return;

    // Read file content (limit to 16KB)
    String content;
    content.reserve(min((size_t)f.size(), (size_t)16384));
    while (f.available() && content.length() < 16384) {
        content += (char)f.read();
    }
    f.close();

    JsonDocument doc;
    if (deserializeJson(doc, content) != DeserializationError::Ok) {
        Serial.println("[SD] Boot config parse failed");
        return;
    }

    uint8_t applied = 0;
    for (JsonPair kv : doc.as<JsonObject>()) {
        String filename = kv.key().c_str();
        // Ensure path starts with /
        String path = filename.startsWith("/") ? filename : "/" + filename;
        String data = kv.value().as<String>();
        if (data.length() == 0) continue;

        File wf = LittleFS.open(path, "w");
        if (wf) {
            wf.print(data);
            wf.close();
            ++applied;
            Serial.printf("[SD] Boot config applied: %s\n", path.c_str());
        }
    }
    Serial.printf("[SD] Boot config applied: %u files written\n", (unsigned)applied);
    log(String("[SD] Boot config applied: ") + applied + " files",
        SdLogLevel::INFO, "SD_BOOT");
}

// ─────────────────────────────────────────────────────────────
//  Feature 50: Multi-device config sync
// ─────────────────────────────────────────────────────────────
bool SdManager::exportConfigForSync(const String& deviceName) {
    SdSpiLock _l;
    if (!_mounted) return false;

    String dir = String(SD_DIR_SYNC) + "/" + deviceName;
    if (!SD.exists(SD_DIR_SYNC)) SD.mkdir(SD_DIR_SYNC);
    if (!SD.exists(dir))         SD.mkdir(dir);

    uint8_t copied = 0;
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return false;
    }

    File f = root.openNextFile();
    while (f) {
        String fname = String(f.name());
        if (!f.isDirectory() && fname.endsWith(".json")) {
            String srcPath = String("/") + fname;
            String dstPath = dir + "/" + fname;
            if (_copyFile(srcPath, dstPath, false, true)) ++copied;
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();

    log(String("exportConfigForSync: device=") + deviceName +
        " (" + copied + " files)", SdLogLevel::INFO, "SD_SYNC");
    return (copied > 0);
}

bool SdManager::importConfigFromSync(const String& deviceName) {
    SdSpiLock _l;
    if (!_mounted) return false;

    String dir = String(SD_DIR_SYNC) + "/" + deviceName;
    if (!SD.exists(dir)) return false;

    auto entries = listDir(dir);
    uint8_t restored = 0;
    for (const auto& e : entries) {
        if (e.isDir || !e.name.endsWith(".json")) continue;
        String src = dir + "/" + e.name;
        String dst = "/" + e.name;
        if (_copyFile(src, dst, true, false)) ++restored;
    }

    log(String("importConfigFromSync: device=") + deviceName +
        " (" + restored + " files)", SdLogLevel::INFO, "SD_SYNC");
    return (restored > 0);
}

std::vector<String> SdManager::listSyncedDevices() const {
    SdSpiLock _l;
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_SYNC);
    for (const auto& e : entries) {
        if (e.isDir) result.push_back(e.name);
    }
    return result;
}
