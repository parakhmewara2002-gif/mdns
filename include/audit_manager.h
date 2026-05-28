#pragma once
// ============================================================
//  audit_manager.h  -  Audit Trail / Event Log System
//
//  Batch 1 Feature: Har action ka record
//    - Kisne kiya (module source)
//    - Kab kiya (timestamp)
//    - Kya kiya (event type + detail)
//
//  Storage: LittleFS /audit_log.json
//  Max entries: AUDIT_MAX_ENTRIES (circular buffer)
//
//  API Endpoints (registered in web_server.cpp):
//    GET  /api/v1/audit          - list entries (filter by type/limit)
//    POST /api/v1/audit/clear    - clear all logs
//    GET  /api/v1/audit/export   - download as JSON
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>  // MUTEX FIX: guard _entries vector mutations

#define AUDIT_LOG_FILE      "/audit_log.json"
#define AUDIT_MAX_ENTRIES   20      // was 50 — 20×154B = ~3KB vs 50×154B = ~7.5KB
#define AUDIT_MAX_DETAIL    128     // max chars in detail string

// ── Event source modules ──────────────────────────────────────
enum class AuditSource : uint8_t {
    IR_TX      = 0,   // IR signal transmitted
    IR_RX      = 1,   // IR signal received
    RFID       = 2,   // RFID card scanned
    NFC        = 3,   // NFC tag detected
    SCHEDULER  = 4,   // Scheduled event fired
    MACRO      = 5,   // Macro executed
    RULE       = 6,   // Automation rule triggered (Batch 2)
    WIFI       = 7,   // WiFi connect/disconnect
    SYSTEM     = 9,   // System restart / boot
    API        = 10,  // REST API call
    AUTH       = 11,  // Login attempt (Batch 3)
    UNKNOWN    = 255
};

struct AuditEntry {
    uint32_t    id;
    uint32_t    timestamp;      // Unix epoch (0 if NTP not synced)
    String      timeStr;        // "YYYY-MM-DD HH:MM:SS" or "uptime:Xs"
    AuditSource source;
    String      sourceStr;      // Human readable source name
    String      event;          // Short event name e.g. "IR_TRANSMIT"
    String      detail;         // Detail e.g. "Button: TV Power (id=5)"
    bool        success;        // Was the action successful?

    AuditEntry() : id(0), timestamp(0), source(AuditSource::UNKNOWN), success(true) {}
};

class AuditManager {
public:
    AuditManager();

    void begin();       // Load persisted log from LittleFS
    void loop();        // Periodic flush to LittleFS

    // ── Log an event ─────────────────────────────────────────
    void log(AuditSource source,
             const String& event,
             const String& detail,
             bool success = true);

    // Convenience wrappers
    void logIrTx    (const String& buttonName, uint32_t buttonId);
    void logIrRx    (const String& protocol,   const String& code);
    void logRfid    (const String& uid,        const String& cardName, bool known);
    void logNfc     (const String& uid,        const String& tagName,  bool known);
    void logScheduler(const String& entryName, uint32_t buttonId);
    void logMacro   (const String& macroName,  bool success);
    void logWifi    (const String& event,      const String& detail);
    void logOta     (const String& event,      bool success);
    void logSystem  (const String& event);
    void logApi     (const String& endpoint,   const String& method);

    // ── Query ────────────────────────────────────────────────
    // Get all entries (newest first)
    const std::vector<AuditEntry>& entries() const { return _entries; }

    // Get entries filtered by source (-1 = all), newest first, up to limit
    std::vector<AuditEntry> filter(int source, size_t limit = 50) const;

    // Serialize to JSON string
    String toJson(int sourceFilter = -1, size_t limit = 50) const;

    // Total entries logged (including rotated ones)
    uint32_t totalLogged() const { return _totalLogged; }

    // Clear all logs
    void clear();

    // Force save to LittleFS immediately
    void save() const;

    size_t size() const { return _entries.size(); }

    // ── SD Integration (features 36-38) ──────────────────────
    // Feature 36: overflow oldest entries to SD instead of dropping
    bool enableSdOverflow(bool en);
    void setSdOverflow(bool en) { _sdOverflow = en; }
    bool sdOverflowEnabled() const { return _sdOverflow; }

    // Feature 37: export all current entries to SD as JSON
    bool exportToSD(const String& filename = "");

    // Feature 38: mirror every new log entry to SD in real-time
    void setSdMirror(bool en) { _sdMirror = en; }
    bool sdMirrorEnabled() const { return _sdMirror; }

private:
    std::vector<AuditEntry> _entries;   // in-memory circular buffer (newest last)
    uint32_t    _nextId;
    uint32_t    _totalLogged;
    mutable bool          _dirty;       // needs save to flash
    mutable unsigned long _lastSave;    // millis() of last flash write

    // SD integration flags
    bool        _sdOverflow = false;    // Feature 36: write overflow to SD
    bool        _sdMirror   = false;    // Feature 38: mirror all entries to SD

    // MUTEX FIX: log() is called from the loop task, web handlers, the
    // WDT ping task, and indirectly from sdMgr.log() — all push_back on
    // _entries with no synchronisation. Eventually corrupts the vector
    // and crashes. _mux guards every mutator/iterator.
    SemaphoreHandle_t _mux = nullptr;

    void        _load();
    void        _addEntry(AuditEntry& e);    // caller must hold _mux
    String      _sourceToStr(AuditSource s) const;
    String      _buildTimeStr() const;
    uint32_t    _getTimestamp() const;
};

extern AuditManager auditMgr;
