#pragma once
// ============================================================
//  sd_manager.h  -  Optional SD card subsystem
//  v2.0.0  |  ESP32-WROOM-32  |  VSPI bus
//
//  DESIGN PRINCIPLES
//  ─────────────────
//  1. ALWAYS OPTIONAL - if SD is absent or fails to mount,
//     every existing LittleFS/internal feature continues to
//     work exactly as before.  No crashes, no boot delays,
//     no broken UI.
//
//  2. HOT-PLUG AWARE - isAvailable() re-probes the bus on
//     every call (with a debounce timer) so insertion and
//     removal are handled gracefully at runtime.
//
//  3. RAM CONSERVATIVE - streaming I/O throughout; large
//     file transfers never buffer the whole file in RAM.
//     Log writes use a small fixed ring buffer, flushed
//     periodically from loop().
//
//
//  WIRING (VSPI - default SPI bus on ESP32)
//  ────────────────────────────────────────
//  SD Card │  ESP32 GPIO
//  ────────┼────────────────────────────────────
//  VCC     │  3.3 V  (use 3.3 V-compatible module; 5 V SD
//           │          modules must have level-shifter)
//  GND     │  GND
//  MOSI    │  GPIO 23  (SD_MOSI_PIN)
//  MISO    │  GPIO 19  (SD_MISO_PIN)
//  SCK     │  GPIO 18  (SD_SCK_PIN)
//  CS      │  GPIO  4  (SD_CS_PIN) - configurable
//
//  SD CARD FORMAT
//  ──────────────
//  • FAT32, up to 32 GB SDHC tested
//  • exFAT not supported by the ESP32 Arduino SD library
//  • Recommended: format with SD Card Formatter (sdcard.org)
//    or: mkfs.fat -F 32 /dev/sdX
//
//  DIRECTORY STRUCTURE CREATED ON FIRST MOUNT
//  ──────────────────────────────────────────
//  /sd/
//  ├── ir_library/      IR button JSON libraries
//  ├── backups/         Config + LittleFS snapshots
//  ├── macros/          IR macro scripts (.json)
//  ├── logs/            activity.log (rolling)
//  ├── assets/          Large HTML/CSS/JS served over HTTP
//  ├── raw_dumps/       Captured RAW IR timing arrays
//  └── devices/         TV/AC/device JSON profiles
//
//  MACRO SCRIPT FORMAT  (/sd/macros/*.json)
//  ─────────────────────────────────────────
//  {
//    "name": "TV On + HDMI1",
//    "steps": [
//      {"buttonId": 12, "delayAfterMs": 500},
//      {"buttonId": 15, "delayAfterMs": 200},
//      {"buttonId": 15, "delayAfterMs": 0}
//    ]
//  }
// ============================================================
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <functional>
#include "config.h"

// ── C-04 FIX: Global VSPI bus mutex ──────────────────────────────────────────
// SD card (sd_manager) uses VSPI. NFC / NRF24 / SubGHz are user-configurable
// and may also be set to VSPI. The hw_poll FreeRTOS task and the Arduino loop()
// task both access the VSPI bus concurrently - SPIClass is NOT thread-safe.
// This mutex serialises all VSPI transactions across all tasks.
//
// Usage: wrap every SPI operation with:
//   if (xSemaphoreTake(g_spi_vspi_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
//       /* SPI transaction */
//       xSemaphoreGive(g_spi_vspi_mutex);
//   }
//
// g_spi_vspi_mutex is created in SdManager::begin() so it is ready before any
// module uses VSPI. Modules on HSPI do NOT need this mutex.
// ─────────────────────────────────────────────────────────────────────────────
extern SemaphoreHandle_t g_spi_vspi_mutex;

// ── SD hardware pin defaults (overridable in config.h) ───────
#ifndef SD_CS_PIN
  #define SD_CS_PIN   4    // GPIO4  - safe, no boot-strap conflict
#endif
#ifndef SD_MOSI_PIN
  #define SD_MOSI_PIN 23   // VSPI MOSI
#endif
#ifndef SD_MISO_PIN
  #define SD_MISO_PIN 19   // VSPI MISO
#endif
#ifndef SD_SCK_PIN
  #define SD_SCK_PIN  18   // VSPI SCK
#endif
#ifndef SD_SPI_FREQ
  #define SD_SPI_FREQ 4000000UL   // 4 MHz - conservative; bump to 20 MHz if stable
#endif

// ── SD directory structure ───────────────────────────────────
#define SD_DIR_IR_LIBRARY  "/ir_library"
#define SD_DIR_BACKUPS     "/backups"
#define SD_DIR_MACROS      "/macros"
#define SD_DIR_LOGS        "/logs"
#define SD_DIR_ASSETS      "/assets"
#define SD_DIR_RAW_DUMPS   "/raw_dumps"
#define SD_DIR_DEVICES     "/devices"
#define SD_DIR_PROFILES    "/profiles"   // Feature 48: config profiles
#define SD_DIR_SYNC        "/sync"       // Feature 50: multi-device config sync

#define SD_LOG_FILE        "/logs/activity.log"

// ── Log limits ───────────────────────────────────────────────
#define SD_LOG_FLUSH_MS        10000UL       // flush log ring every 10 s
#define SD_LOG_MAX_BYTES       131072UL      // 128 KB - rotate log when exceeded
#define SD_LOG_ROTATE_MAX      5             // keep up to 5 rotated log files
#define SD_PROBE_INTERVAL      15000UL       // probe interval when mounted (15s)
#define SD_PROBE_FAIL_DEBOUNCE 3             // consecutive failures before unmount
#define SD_REMOUNT_MIN_MS      10000UL       // first remount retry delay (10s)
#define SD_REMOUNT_MAX_MS      60000UL       // max remount retry back-off (60s)
#define SD_MACRO_MAX_STEPS     64            // max steps in one macro

// ── [#1] Multi-level log severity ────────────────────────────
enum class SdLogLevel {
    INFO  = 0,
    WARN  = 1,
    ERROR = 2
};

// ── Macro step ───────────────────────────────────────────────
// [#25] Added condTagUid, condTimeAfter
// [#26] Added repeatCount, repeatDelayMs
// [#27] Added chainMacro
struct MacroStep {
    uint32_t buttonId      = 0;
    uint32_t delayAfterMs  = 100;
    // #25 conditions
    String   condTagUid;        // skip step if NFC tag UID doesn't match (empty = no check)
    String   condTimeAfter;     // skip step if current time is before this (ISO "HH:MM")
    // #26 repeat
    uint8_t  repeatCount   = 0; // 0 = execute once (no extra repeats)
    uint32_t repeatDelayMs = 0; // delay between repeats
    // #27 chaining - if non-empty and buttonId==0, queue this macro
    String   chainMacro;
};

// ── SD status snapshot (for /api/sd/status) ──────────────────
struct SdStatus {
    bool     mounted;
    uint64_t totalBytes;
    uint64_t usedBytes;
    uint8_t  cardType;     // CARD_NONE/MMC/SD/SDHC/UNKNOWN
    String   cardTypeStr;
};

// ── File entry (for /api/sd/ls) ──────────────────────────────
struct SdFileEntry {
    String   name;
    bool     isDir;
    size_t   size;
    uint32_t modTime;   // Unix timestamp (0 if unknown - FAT32 has limited time support)
    String   fullPath;  // Absolute path for convenience
};

// ── [#8] File preview ────────────────────────────────────────
struct SdPreview {
    String content;
    size_t fileSize;
    bool   truncated;
    String path;
};

// ── [#19] IR Library metadata ────────────────────────────────
struct IrLibMeta {
    String   name;
    String   device;
    String   author;
    uint16_t buttonCount;
    String   created;
};

// ── [#21] IR Library diff ────────────────────────────────────
struct IrLibDiff {
    std::vector<String> added;
    std::vector<String> removed;
    std::vector<String> changed;
};

// ── [#31] Device profile metadata ────────────────────────────
struct DeviceProfileMeta {
    String   name;
    String   brand;
    String   model;
    String   protocol;
    uint16_t buttonCount;
};

// ── [#33] Async write job ─────────────────────────────────────
struct SdWriteJob {
    String path;
    String data;
    bool   append;
};

// ── [#35] SD benchmark result ────────────────────────────────
struct SdBenchmark {
    uint32_t writeKBps;
    uint32_t readKBps;
    uint32_t fileSize;
};

// ── [#38] SD health stats ────────────────────────────────────
struct SdHealth {
    uint64_t totalBytes;
    uint64_t usedBytes;
    uint8_t  usedPct;
    String   cardType;
    uint32_t mountCount;
};

// ── [#39] Paginated dir listing ──────────────────────────────
struct SdPage {
    std::vector<SdFileEntry> entries;
    uint16_t total;
    uint16_t offset;
};

// ── [#40] Sort order ─────────────────────────────────────────
enum class SdSort {
    NAME,
    SIZE,
    DATE
};

// ─────────────────────────────────────────────────────────────
class SdManager {
public:
    SdManager();

    // ── Lifecycle ─────────────────────────────────────────────
    void begin();
    void loop();

    // ── Status ────────────────────────────────────────────────
    bool     isAvailable() const { return _mounted; }
    SdStatus status()      const;
    // [#43] JSON status including freq, mount count, macro state
    String   statusJson()  const;

    // ── [#1] Multi-level Logging ──────────────────────────────
    // Thread-safe via portMUX.  Queues into a small ring buffer
    // and flushes to SD asynchronously from loop().
    void log(const String& message,
             SdLogLevel level = SdLogLevel::INFO);
    void log(const char* message,
             SdLogLevel level = SdLogLevel::INFO);
    // [#4] Module-tagged overload
    void log(const String& msg, SdLogLevel level, const char* module);

    // [#1] Set minimum log level filter (messages below are dropped)
    void setLogLevel(SdLogLevel level) { _logLevel = level; }

    // [#3] Drain and return pending WebSocket log text
    String pollWsLog();

    // [#5] Stream entire log file to any Print object
    bool streamLog(Print& out) const;

    // Return last N lines of the log file as a String.
    String  tailLog(uint16_t lines = 50) const;
    // [#45] tail with severity filter
    String  tailLogFiltered(uint16_t lines, SdLogLevel minLevel) const;

    // ── File Manager ──────────────────────────────────────────
    std::vector<SdFileEntry> listDir(const String& path) const;
    // [#6] Recursive listing
    std::vector<SdFileEntry> listDirRecursive(const String& path,
                                              uint8_t maxDepth = 3) const;
    // [#39] Paginated listing
    SdPage listDirPaged(const String& path,
                        uint16_t offset, uint16_t limit) const;
    // [#40] Sorted + filtered listing
    std::vector<SdFileEntry> listDirSorted(const String& path,
                                           SdSort sort,
                                           bool asc,
                                           const String& extFilter) const;

    bool     deleteFile(const String& path);
    bool     deleteRecursive(const String& path);
    // [#9] Bulk delete
    uint16_t deleteFiles(const std::vector<String>& paths);

    bool     renameFile(const String& from, const String& to);
    bool     copyFileSd(const String& src, const String& dst);
    bool     moveFile(const String& src, const String& dst);
    bool     makeDir(const String& path);
    bool     exists(const String& path) const;
    SdFileEntry  getFileInfo(const String& path) const;
    String   readTextFile(const String& path, size_t maxBytes = 8192) const;
    // [#8] Preview endpoint
    SdPreview previewFile(const String& path, size_t maxBytes = 4096) const;
    // [#10] Directory size
    size_t   dirSize(const String& path) const;
    // [#11] File search
    std::vector<SdFileEntry> findFiles(const String& dir,
                                       const String& pattern) const;
    // [#12] Simple TAR archive (no ZIP - too heavy for ESP32 RAM)
    bool     createTar(const String& dir, const String& outPath) const;
    bool     formatCard();
    // [#37] Power-safe atomic write
    bool     safeWriteFile(const String& path, const String& data);
    // [#33] Async queued write
    bool     writeAsync(const String& path, const String& data,
                        bool append = false);

    // ── [#41] Disk usage breakdown ────────────────────────────
    String usageJson() const;

    // ── [#44] JSON helpers ────────────────────────────────────
    String fileEntryToJson(const SdFileEntry& e) const;
    String fileListToJson(const std::vector<SdFileEntry>& v) const;

    // ── IR Library ────────────────────────────────────────────
    bool     exportIRLibrary(const String& name);
    bool     importIRLibrary(const String& name);
    int      mergeIRLibrary(const String& name);
    bool     deleteIRLibrary(const String& name);
    bool     renameIRLibrary(const String& oldName, const String& newName);
    bool     saveButtonToLibrary(const String& libName, uint32_t buttonId);
    // Feature 5: default library name helpers
    String   defaultLibName() const { return _defaultLib; }
    void     setDefaultLib(const String& name);
    std::vector<String> listIRLibraries() const;
    // [#19] Library metadata
    IrLibMeta getLibraryMeta(const String& name) const;
    // [#20] Search within a library
    std::vector<String> searchLibrary(const String& libName,
                                      const String& query) const;
    // [#21] Diff two libraries
    IrLibDiff diffLibraries(const String& nameA, const String& nameB) const;
    // [#22] Auto-export hook
    void setAutoExportLib(const String& libName);
    void markExportDirty() { _autoExportDirty = true; }
    // [#23] Versioned export
    bool exportIRLibraryVersioned(const String& name);
    // [#24] Import Flipper IR format
    int  importFlipperIR(const String& path);

    // Feature 3: Bulk import all IR library JSON files from /sd/ir_library/
    struct SdImportResult {
        uint16_t filesProcessed;
        uint16_t buttonsAdded;
        std::vector<String> errors;
    };
    SdImportResult bulkImportIRLibraries();

    // ── Raw IR Dump ───────────────────────────────────────────
    bool     saveRawDump(const String& name,
                         const uint16_t* data, size_t len,
                         uint16_t freqKHz = 38);
    // Feature 4: Auto-save RAW dump on receive
    bool     autoRawDumpEnabled() const { return _autoRawDump; }
    void     setAutoRawDump(bool en);

    // ── Config Backup / Restore ───────────────────────────────
    bool     backupToSD(const String& tag);
    bool     restoreFromSD(const String& tag);
    std::vector<String> listBackups() const;
    // [#13] Auto-backup scheduling
    void setAutoBackup(bool en, uint32_t intervalMs);
    // [#15] Delta backup
    bool backupDeltaToSD(const String& tag);
    // [#16] LittleFS raw image backup
    bool backupLittleFSImage(const String& tag);
    // [#17] Prune old backups
    void pruneBackups(uint8_t maxKeep);
    // [#18] Dry-run restore
    std::vector<String> restoreDryRun(const String& tag) const;

    // ── Macros ────────────────────────────────────────────────
    bool     queueMacro(const String& filename);
    bool     isMacroRunning() const { return _macroRunning; }
    std::vector<String> listMacros() const;
    // [#29] Stop running macro
    void     stopMacro();
    // [#28] Poll macro WebSocket status JSON
    String   pollMacroStatus();
    // [#30] Macro recorder
    void     startRecord(const String& filename);
    void     recordStep(uint32_t buttonId);
    void     stopRecord();

    // ── Asset serving ─────────────────────────────────────────
    String   assetPath(const String& name) const;
    bool     hasAsset(const String& name)  const;

    // ── Device profiles ───────────────────────────────────────
    std::vector<String> listDeviceProfiles() const;
    String   readDeviceProfile(const String& name) const;
    bool     saveDeviceProfile(const String& name, const String& json);
    bool     deleteDeviceProfile(const String& name);
    int      importDeviceProfileButtons(const String& name);
    // [#31] Profile metadata
    DeviceProfileMeta getProfileMeta(const String& name) const;
    // [#32] Profile share URL (stub)
    String   profileShareUrl(const String& name) const {
        return String("http://ir-remote.local/api/sd/devices/") + name;
    }

    // ── Streaming helpers for web server ─────────────────────
    File     openForRead(const String& path) const;
    File     openForWrite(const String& path);
    bool     beginUpload(const String& path);
    bool     writeUploadChunk(const uint8_t* data, size_t len);
    bool     endUpload();
    void     abortUpload();
    // [#7] Upload path accessor
    String   uploadPath() const { return _uploadPath; }

    // ── [#35] SD benchmark ───────────────────────────────────
    SdBenchmark benchmark(size_t testBytes = 16384);

    // ── [#38] Health stats ───────────────────────────────────
    SdHealth healthStats() const;

    // ── [#42] SD event polling ───────────────────────────────
    String pollSdEvents();

    // ── Features 46-50: Advanced config management ────────────

    // Feature 46: Full settings backup (all LittleFS JSON files)
    bool fullBackup(const String& tag);

    // Feature 47: Factory reset with SD restore
    bool factoryResetWithRestore();

    // Feature 48: Config profiles on SD
    bool saveConfigProfile(const String& name);
    bool loadConfigProfile(const String& name);
    std::vector<String> listConfigProfiles() const;

    // Feature 50: Multi-device config sync
    bool exportConfigForSync(const String& deviceName);
    bool importConfigFromSync(const String& deviceName);
    std::vector<String> listSyncedDevices() const;

private:
    bool          _mounted;
    bool          _spiBegun;
    SPIClass*     _sdSpi = nullptr;
    unsigned long _lastProbeMs;
    uint8_t       _probeFailCount;
    unsigned long _remountIntervalMs;
    unsigned long _lastRemountMs;
    portMUX_TYPE  _logMux;

    // [#1] Log level filter
    SdLogLevel    _logLevel;

    // [#36] Mounted SPI frequency
    uint32_t      _mountedFreq;

    // [#38] Mount counter
    uint32_t      _mountCount;

    // Log ring buffer (fits in DRAM, not IRAM)
    static constexpr size_t LOG_RING_SIZE = 1024;
    char          _logRing[LOG_RING_SIZE];
    size_t        _logRingHead;
    size_t        _logRingLen;
    unsigned long _lastLogFlushMs;

    // [#3] WebSocket pending log text (ring, max 2KB)
    static constexpr size_t WS_LOG_MAX = 2048;
    String        _wsPendingLog;

    // [#42] SD event queue
    String        _sdEventQueue;

    // Macro execution state
    bool              _macroRunning;
    std::vector<MacroStep> _macroSteps;
    size_t            _macroStepIdx;
    unsigned long     _macroNextMs;
    // [#26] Repeat tracking
    uint8_t           _macroRepeatIdx;
    // [#28] Macro WS status
    String            _macroWsStatus;

    // Upload state
    File   _uploadFile;
    bool   _uploadOpen;
    // [#7] Upload path
    String _uploadPath;

    // [#13] Auto-backup state
    bool          _autoBackupEnabled;
    uint32_t      _autoBackupIntervalMs;
    unsigned long _lastAutoBackupMs;

    // [#22] Auto-export state
    String        _autoExportLib;
    bool          _autoExportDirty;

    // [#30] Macro recorder state
    bool          _recording;
    File          _recordFile;
    String        _recordFilename;
    uint32_t      _recordStepCount;

    // [#33] Async write queue
    QueueHandle_t _writeQueue;

    // Feature 4: auto raw dump preference
    bool          _autoRawDump = false;

    // Feature 5: default library name
    String        _defaultLib = "default";

    // Private helpers
    bool   _mount();
    void   _unmount();
    void   _ensureDirectories();
    void   _applyBootConfig();    // Feature 49: apply /boot_config.json on SD mount
    void   _flushLogRing();
    void   _tickMacro();
    void   _probeSd();
    bool   _safePath(const String& path) const;
    String _logTimestamp()               const;
    bool   _copyFile(const String& src, const String& dst,
                     bool srcOnSD, bool dstOnSD);
    bool   _copyFileWithCrc(const String& src, const String& dst,
                            bool srcOnSD, bool dstOnSD, uint32_t& crcOut);
    bool   _deleteRecursiveImpl(const String& path);
    void   _listDirRecursiveImpl(const String& path, uint8_t depth,
                                 uint8_t maxDepth,
                                 std::vector<SdFileEntry>& out) const;
    size_t _dirSizeImpl(const String& path) const;
    // [#13] Auto-backup check from loop()
    void   _checkAutoBackup();
    // [#22] Auto-export check from loop()
    void   _checkAutoExport();
    // [#33] Drain async write queue
    void   _drainWriteQueue();
    // [#34] Software CRC32 (one-shot — re-inits state every call).
    static uint32_t _crc32(const uint8_t* buf, size_t len);
    // CRC32 STREAM FIX: chainable variant. Pass 0xFFFFFFFFUL as crc on
    // the first chunk, feed buf/len, and XOR result with 0xFFFFFFFFUL
    // after the final chunk. Required for files > one buffer; the
    // previous code mis-used _crc32 per chunk, overwriting the running
    // state with the CRC of a single chunk so verification was a no-op.
    static uint32_t _crc32Stream(uint32_t crc,
                                  const uint8_t* buf, size_t len);
    // [#42] Queue an SD event string
    void   _queueSdEvent(const char* event);
};

extern SdManager sdMgr;
