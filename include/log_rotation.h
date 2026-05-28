#pragma once
// ============================================================
//  log_rotation.h  -  Batch 4: Auto Log Rotation + CSV Export
//
//  Features:
//    - Audit log rotate karo jab AUDIT_MAX_ENTRIES se zyada ho
//    - SD card pe archive: /logs/audit_YYYY-MM-DD.json
//    - CSV export: /api/v1/audit/export.csv
//    - Old archived logs auto-delete (configurable days)
//    - SD daily log file rotation
//
//  API Endpoints:
//    GET  /api/v1/logs/list          - list archived logs
//    GET  /api/v1/logs/export.csv    - download audit as CSV
//    POST /api/v1/logs/rotate        - force rotate now
//    POST /api/v1/logs/config        - set retention days
//    GET  /api/v1/logs/config        - get config
// ============================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "audit_manager.h"

#define LOG_ROT_CFG_FILE    "/log_rot_config.json"
#define LOG_ARCHIVE_DIR     "/log_archive"
#define LOG_ROT_TAG         "[LOGROT]"
#define LOG_ROT_DEFAULT_DAYS  30     // keep logs for 30 days
#define LOG_ROT_CHECK_MS    3600000UL // check every hour

class LogRotationManager {
public:
    LogRotationManager();

    void begin();
    void loop();

    // Force rotate now - archive current audit log
    bool rotate();

    // Delete archived logs older than _retentionDays
    void pruneOldLogs();

    // Build CSV string from audit entries
    String auditToCsv(int sourceFilter = -1, size_t limit = 500) const;

    // Config
    uint8_t retentionDays() const { return _retentionDays; }
    void    setRetentionDays(uint8_t days);
    bool    loadConfig();
    bool    saveConfig();
    String  configJson() const;

    // List archived log files
    String  listArchivesJson() const;

private:
    uint8_t       _retentionDays;
    unsigned long _lastCheck;

    String _todayStr() const;
    bool   _archiveToCsv() const;
};

extern LogRotationManager logRotMgr;
