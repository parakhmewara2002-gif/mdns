// ============================================================
//  web_server_batch4.cpp  -  Batch 4
//  1. Log Rotation + CSV Export  (/api/v1/logs/*)
//  2. Audit CSV download
// ============================================================
#include "web_server.h"
#include "auth_manager.h"
#include "audit_manager.h"
#include "log_rotation.h"
#include <ArduinoJson.h>

static void sendB4(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    req->send(r);
}

static String* _getB4Buf(AsyncWebServerRequest* req) {
    if (!req->_tempObject) {
        req->_tempObject = new String();
        req->onDisconnect([req]() {
            if (req->_tempObject) {
                delete reinterpret_cast<String*>(req->_tempObject);
                req->_tempObject = nullptr;
            }
        });
    }
    return reinterpret_cast<String*>(req->_tempObject);
}
static void _freeB4Buf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define B4_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_getB4Buf(req)->length() + l > HTTP_MAX_BODY) { \
                _freeB4Buf(req); \
                sendB4(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _getB4Buf(req)->concat((char*)d, l); \
            bool last = (t > 0) ? (i + l >= t) : (i == 0); \
            if (last) { \
                String* buf = _getB4Buf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _freeB4Buf(req); \
            } \
        })

void WebUI::setupLogRoutes() {
    // GET /api/v1/logs/list - archived logs list
    _server.on("/api/v1/logs/list", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            AsyncWebServerResponse* r = req->beginResponse(
                200, "application/json", logRotMgr.listArchivesJson());
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    // GET /api/v1/logs/export.csv - download audit as CSV
    _server.on("/api/v1/logs/export.csv", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleLogExportCsv(req); });

    // GET /api/v1/audit/export.csv — removed 2026-05-23: dead alias.
    // Use /api/v1/logs/export.csv (same handler).

    // POST /api/v1/logs/rotate - force rotate now
    _server.on("/api/v1/logs/rotate", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            bool ok = logRotMgr.rotate();
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Rotate failed\"}");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    // GET /api/v1/logs/config
    _server.on("/api/v1/logs/config", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            AsyncWebServerResponse* r = req->beginResponse(
                200, "application/json", logRotMgr.configJson());
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    // POST /api/v1/logs/config
    B4_POST("/api/v1/logs/config",
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleLogConfig(req, d, l); });

    Serial.println("[WEB] Log Rotation routes registered");
}

// GET /api/v1/logs/export.csv[?source=RFID&limit=500]
void WebUI::handleLogExportCsv(AsyncWebServerRequest* req) {
    // FIX: auth check was missing - anyone could download full audit log
    if (!authMgr.checkAuth(req)) return;
    int    srcFilter = -1;
    size_t limit     = 500;

    if (req->hasParam("source")) {
        String src = req->getParam("source")->value();
        src.toUpperCase();
        if      (src == "RFID")      srcFilter = (int)AuditSource::RFID;
        else if (src == "IR_TX")     srcFilter = (int)AuditSource::IR_TX;
        else if (src == "IR_RX")     srcFilter = (int)AuditSource::IR_RX;
        else if (src == "SYSTEM")    srcFilter = (int)AuditSource::SYSTEM;
        else if (src == "RULE")      srcFilter = (int)AuditSource::RULE;
        else if (src == "SCHEDULER") srcFilter = (int)AuditSource::SCHEDULER;
    }
    if (req->hasParam("limit")) {
        int l = req->getParam("limit")->value().toInt();
        if (l > 0 && l <= 1000) limit = (size_t)l;
    }

    String csv = logRotMgr.auditToCsv(srcFilter, limit);
    AsyncWebServerResponse* r = req->beginResponse(200, "text/csv", csv);
    r->addHeader("Content-Disposition", "attachment; filename=\"audit_log.csv\"");
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

// POST /api/v1/logs/config
void WebUI::handleLogConfig(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendB4(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    if (body["retentionDays"].is<uint8_t>()) {
        uint8_t days = body["retentionDays"].as<uint8_t>();
        logRotMgr.setRetentionDays(days);
    }
    sendB4(req, 200, "{\"ok\":true," + logRotMgr.configJson().substring(1));
}
