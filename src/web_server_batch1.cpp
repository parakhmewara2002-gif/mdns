// ============================================================
//  web_server_batch1.cpp  -  Batch 1 Features
//
//  1. REST API v1  (/api/v1/*)
//     Clean endpoints for mobile apps / third-party integration
//
//  2. Audit Trail  (/api/v1/audit/*)
//     Log viewer, filter, export, clear
//
//  3. Debug Panel  (/api/v1/debug/*)
//     Live heap, CPU, uptime, module status
// ============================================================
#include "web_server.h"
#include "ir_database.h"
#include "ir_transmitter.h"
#include "ir_receiver.h"
#include "wifi_manager.h"
#include "scheduler.h"
#include "macro_manager.h"
#include "sd_manager.h"
#include "nfc_module.h"
#include "rfid_module.h"
#include "subghz_module.h"
#include "nrf24_module.h"
#include "system_module.h"
#include "audit_manager.h"
#include "auth_manager.h"
#include "group_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

// ── Shared helper (defined in web_server.cpp) ─────────────────
static void sendJsonV1(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    r->addHeader("Access-Control-Allow-Headers", "Content-Type");
    req->send(r);
}

// ── Body buffer helpers (same pattern as web_server.cpp) ──────
static String* _getV1Buf(AsyncWebServerRequest* req) {
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
static void _freeV1Buf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define V1_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_getV1Buf(req)->length() + l > HTTP_MAX_BODY) { \
                _freeV1Buf(req); \
                sendJsonV1(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _getV1Buf(req)->concat((char*)d, l); \
            bool last = (t > 0) ? (i + l >= t) : (i == 0); \
            if (last) { \
                String* buf = _getV1Buf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _freeV1Buf(req); \
            } \
        })

// ─────────────────────────────────────────────────────────────
// ══ SECTION 1: REST API v1 ROUTES ════════════════════════════
// ─────────────────────────────────────────────────────────────
void WebUI::setupRestApiV1Routes() {
    // OPTIONS preflight for CORS
    _server.on("/api/v1/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* r = req->beginResponse(200, "text/plain", "");
        r->addHeader("Access-Control-Allow-Origin", "*");
        r->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        r->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        req->send(r);
    });

    // ── Status ────────────────────────────────────────────────
    _server.on("/api/v1/status", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleV1Status(req); });

    // ── IR ────────────────────────────────────────────────────
    _server.on("/api/v1/ir/buttons", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleV1IrList(req); });

    V1_POST("/api/v1/ir/trigger",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l) {
            handleV1IrTrigger(r, d, l); });

    // ── Macros ────────────────────────────────────────────────
    _server.on("/api/v1/macro/list", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleV1MacroList(req); });

    V1_POST("/api/v1/macro/run",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l) {
            handleV1MacroRun(r, d, l); });

    // ── Schedules ─────────────────────────────────────────────
    _server.on("/api/v1/schedules", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleV1ScheduleList(req); });

    // ── RFID ──────────────────────────────────────────────────
    _server.on("/api/v1/rfid/log", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleV1RfidLog(req); });

    // ── System ────────────────────────────────────────────────
    _server.on("/api/v1/system/info", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleV1SystemInfo(req); });

    V1_POST("/api/v1/system/restart",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l) {
            handleV1SystemRestart(r, d, l); });

    Serial.println("[WEB] REST API v1 routes registered");
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/status
// Returns: full device status JSON
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1Status(AsyncWebServerRequest* req) {
    auditMgr.logApi("/api/v1/status", "GET");

    JsonDocument doc;
    doc["firmware"]     = FIRMWARE_VERSION;
    doc["uptime_s"]     = (uint32_t)(millis() / 1000);
    doc["heap_free"]    = ESP.getFreeHeap();
    doc["heap_min"]     = ESP.getMinFreeHeap();
    doc["cpu_mhz"]      = getCpuFrequencyMhz();
    doc["chip"]         = ESP.getChipModel();
    doc["flash_mb"]     = (uint32_t)(ESP.getFlashChipSize() >> 20);

    // WiFi
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["sta_connected"] = wifiMgr.staConnected();
    wifi["sta_ssid"]      = wifiMgr.staSSID();
    wifi["sta_ip"]        = wifiMgr.staIP();
    wifi["sta_rssi"]      = wifiMgr.staRSSI();
    wifi["ap_active"]     = wifiMgr.apActive();
    wifi["ap_ip"]         = wifiMgr.apIP();

    // Time
    JsonObject timeObj = doc["time"].to<JsonObject>();
    timeObj["ntp_synced"] = scheduler.ntpSynced();
    timeObj["time"]       = scheduler.ntpSynced() ? scheduler.currentTimeStr() : "";
    timeObj["date"]       = scheduler.ntpSynced() ? scheduler.currentDateStr() : "";

    // Storage
    JsonObject fs = doc["filesystem"].to<JsonObject>();
    fs["littlefs_total_kb"] = (uint32_t)(LittleFS.totalBytes() / 1024);
    fs["littlefs_used_kb"]  = (uint32_t)(LittleFS.usedBytes() / 1024);
    fs["sd_mounted"]        = sdMgr.isAvailable();

    // Modules
    JsonObject modules = doc["modules"].to<JsonObject>();
    modules["ir_buttons"]   = (uint32_t)irDB.buttons().size();
    modules["groups"]       = (uint32_t)groupMgr.size();
    modules["schedules"]    = (uint32_t)scheduler.size();
    modules["macro_running"]= macroMgr.isRunning();
    modules["macro_name"]   = macroMgr.runningName();

    // Audit
    doc["audit_total"] = auditMgr.totalLogged();

    String out; serializeJson(doc, out);
    sendJsonV1(req, 200, out);
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/ir/buttons[?group=id]
// Returns: list of IR buttons
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1IrList(AsyncWebServerRequest* req) {
    auditMgr.logApi("/api/v1/ir/buttons", "GET");

    const auto& btns = irDB.buttons();
    JsonDocument doc;
    doc["count"] = btns.size();
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (const auto& b : btns) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]       = b.id;
        o["name"]     = b.name;
        o["protocol"] = protocolName(b.protocol);
        o["groupId"]  = b.groupId;
        o["icon"]     = b.icon;
        o["color"]    = b.color;
    }
    String out; serializeJson(doc, out);
    sendJsonV1(req, 200, out);
}

// ─────────────────────────────────────────────────────────────
// POST /api/v1/ir/trigger
// Body: {"id": 5} or {"name": "TV Power"} or {"id":5,"repeat":3}
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1IrTrigger(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendJsonV1(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }

    IRButton btn;
    bool found = false;

    if (body["id"].is<uint32_t>()) {
        btn = irDB.findById(body["id"].as<uint32_t>());
        found = btn.id != 0;
    } else if (body["name"].is<const char*>()) {
        String name = body["name"].as<String>();
        for (const auto& b : irDB.buttons()) {
            if (b.name.equalsIgnoreCase(name)) { btn = b; found = true; break; }
        }
    }

    if (!found) {
        sendJsonV1(req, 404, "{\"error\":\"Button not found\"}"); return;
    }

    uint8_t repeat = body["repeat"] | (uint8_t)1;
    if (repeat < 1)  repeat = 1;
    if (repeat > 10) repeat = 10;

    // FIX: was calling transmit() + delay() in a loop inside the AsyncTCP
    // task handler. Each transmit() = 50-150ms block. With repeat=10 this
    // blocked Core 0 for up to 1.5 seconds, freezing all HTTP/WS processing.
    // Fix: set repeatCount on the button and post a single async command.
    btn.repeatCount = repeat;
    irTransmitter.transmitAsync(btn);

    auditMgr.logIrTx(btn.name, btn.id);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["id"]      = btn.id;
    resp["name"]    = btn.name;
    resp["repeat"]  = repeat;
    String out; serializeJson(resp, out);
    sendJsonV1(req, 200, out);
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/macro/list
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1MacroList(AsyncWebServerRequest* req) {
    auditMgr.logApi("/api/v1/macro/list", "GET");
    auto list = macroMgr.list();
    JsonDocument doc;
    doc["count"] = list.size();
    JsonArray arr = doc["macros"].to<JsonArray>();
    for (const auto& m : list) {
        JsonObject o = arr.add<JsonObject>();
        o["name"]   = m.name;
        o["label"]  = m.label;
        o["steps"]  = m.stepCount;
    }
    String out; serializeJson(doc, out);
    sendJsonV1(req, 200, out);
}

// ─────────────────────────────────────────────────────────────
// POST /api/v1/macro/run
// Body: {"name": "night_mode"}
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1MacroRun(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendJsonV1(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    String name = body["name"] | (const char*)"";
    if (name.isEmpty()) {
        sendJsonV1(req, 400, "{\"error\":\"Missing name\"}"); return;
    }
    bool ok = macroMgr.run(name);
    if (!ok) {
        sendJsonV1(req, 409, "{\"error\":\"Macro not found or already running\"}"); return;
    }
    auditMgr.logMacro(name, true);
    sendJsonV1(req, 200, "{\"ok\":true,\"running\":\"" + name + "\"}");
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/schedules
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1ScheduleList(AsyncWebServerRequest* req) {
    auditMgr.logApi("/api/v1/schedules", "GET");
    sendJsonV1(req, 200, scheduler.toJson());
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/rfid/log[?limit=20]
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1RfidLog(AsyncWebServerRequest* req) {
    auditMgr.logApi("/api/v1/rfid/log", "GET");
    size_t limit = 20;
    if (req->hasParam("limit")) {
        int l = req->getParam("limit")->value().toInt();
        if (l > 0 && l <= 100) limit = (size_t)l;
    }
    // Pull RFID entries from audit log
    String out = auditMgr.toJson((int)AuditSource::RFID, limit);
    sendJsonV1(req, 200, out);
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/system/info
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1SystemInfo(AsyncWebServerRequest* req) {
    auditMgr.logApi("/api/v1/system/info", "GET");
    JsonDocument doc;
    doc["firmware"]       = FIRMWARE_VERSION;
    doc["chip_model"]     = ESP.getChipModel();
    doc["chip_rev"]       = ESP.getChipRevision();
    doc["cpu_mhz"]        = getCpuFrequencyMhz();
    doc["flash_size_mb"]  = (uint32_t)(ESP.getFlashChipSize() >> 20);
    doc["heap_free"]      = ESP.getFreeHeap();
    doc["heap_total"]     = ESP.getHeapSize();
    doc["heap_min_free"]  = ESP.getMinFreeHeap();
    doc["psram_size"]     = ESP.getPsramSize();
    doc["uptime_s"]       = (uint32_t)(millis() / 1000);
    doc["sdk_version"]    = ESP.getSdkVersion();
    doc["littlefs_total"] = (uint32_t)LittleFS.totalBytes();
    doc["littlefs_used"]  = (uint32_t)LittleFS.usedBytes();
    String out; serializeJson(doc, out);
    sendJsonV1(req, 200, out);
}

// ─────────────────────────────────────────────────────────────
// POST /api/v1/system/restart
// Body: {"confirm": true}
// ─────────────────────────────────────────────────────────────
void WebUI::handleV1SystemRestart(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendJsonV1(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    if (!body["confirm"].as<bool>()) {
        sendJsonV1(req, 400, "{\"error\":\"Send confirm:true to restart\"}"); return;
    }
    auditMgr.logSystem("RESTART_REQUESTED");
    auditMgr.save();
    sendJsonV1(req, 200, "{\"ok\":true,\"message\":\"Restarting in 1s\"}");
    // Use deferred restart so HTTP response fully transmits before reset
    extern portMUX_TYPE s_restartMux;
    extern volatile uint32_t s_restartAt;
    taskENTER_CRITICAL(&s_restartMux);
    s_restartAt = (uint32_t)(millis() + 1200);
    taskEXIT_CRITICAL(&s_restartMux);
}

// ─────────────────────────────────────────────────────────────
// ══ SECTION 2: AUDIT TRAIL ROUTES ════════════════════════════
// ─────────────────────────────────────────────────────────────
void WebUI::setupAuditRoutes() {
    // GET /api/v1/audit[?source=RFID&limit=50]
    _server.on("/api/v1/audit", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleAuditGet(req); });

    // GET /api/v1/audit/export  - download full log as JSON file
    _server.on("/api/v1/audit/export", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleAuditExport(req); });

    // POST /api/v1/audit/clear
    V1_POST("/api/v1/audit/clear",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l) {
            handleAuditClear(r, d, l); });

    // POST /api/audit/sdmirror   body: {"en": true/false}
    V1_POST("/api/audit/sdmirror",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            bool en = doc["en"] | false;
            auditMgr.setSdMirror(en);
            sendJson(req, 200, String("{\"ok\":true,\"sdMirror\":") + (en?"true":"false") + "}");
        });

    // POST /api/audit/sdoverflow  body: {"en": true/false}
    V1_POST("/api/audit/sdoverflow",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            bool en = doc["en"] | false;
            auditMgr.setSdOverflow(en);
            sendJson(req, 200, String("{\"ok\":true,\"sdOverflow\":") + (en?"true":"false") + "}");
        });

    Serial.println("[WEB] Audit Trail routes registered");
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/audit[?source=RFID&limit=50]
// ─────────────────────────────────────────────────────────────
void WebUI::handleAuditGet(AsyncWebServerRequest* req) {
    size_t limit = 50;
    int srcFilter = -1;  // -1 = all

    if (req->hasParam("limit")) {
        int l = req->getParam("limit")->value().toInt();
        if (l > 0 && l <= 200) limit = (size_t)l;
    }

    // Filter by source name
    if (req->hasParam("source")) {
        String src = req->getParam("source")->value();
        src.toUpperCase();
        if      (src == "IR_TX")     srcFilter = (int)AuditSource::IR_TX;
        else if (src == "IR_RX")     srcFilter = (int)AuditSource::IR_RX;
        else if (src == "RFID")      srcFilter = (int)AuditSource::RFID;
        else if (src == "NFC")       srcFilter = (int)AuditSource::NFC;
        else if (src == "SCHEDULER") srcFilter = (int)AuditSource::SCHEDULER;
        else if (src == "MACRO")     srcFilter = (int)AuditSource::MACRO;
        else if (src == "WIFI")      srcFilter = (int)AuditSource::WIFI;
        else if (src == "SYSTEM")    srcFilter = (int)AuditSource::SYSTEM;
        else if (src == "API")       srcFilter = (int)AuditSource::API;
    }

    sendJsonV1(req, 200, auditMgr.toJson(srcFilter, limit));
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/audit/export  - triggers browser download
// ─────────────────────────────────────────────────────────────
void WebUI::handleAuditExport(AsyncWebServerRequest* req) {
    String json = auditMgr.toJson(-1, AUDIT_MAX_ENTRIES);
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", json);
    r->addHeader("Content-Disposition", "attachment; filename=\"audit_log.json\"");
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

// ─────────────────────────────────────────────────────────────
// POST /api/v1/audit/clear
// Body: {"confirm": true}
// ─────────────────────────────────────────────────────────────
void WebUI::handleAuditClear(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendJsonV1(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    if (!body["confirm"].as<bool>()) {
        sendJsonV1(req, 400, "{\"error\":\"Send confirm:true\"}"); return;
    }
    auditMgr.clear();
    sendJsonV1(req, 200, "{\"ok\":true,\"message\":\"Audit log cleared\"}");
}

// ─────────────────────────────────────────────────────────────
// ══ SECTION 3: DEBUG PANEL ROUTES ════════════════════════════
// ─────────────────────────────────────────────────────────────
void WebUI::setupDebugRoutes() {
    // GET /api/v1/debug/stats  - live heap, CPU, uptime
    _server.on("/api/v1/debug/stats", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleDebugStats(req); });

    // GET /api/v1/debug/modules  - module enable/status
    _server.on("/api/v1/debug/modules", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleDebugModules(req); });

    Serial.println("[WEB] Debug Panel routes registered");
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/debug/stats
// ─────────────────────────────────────────────────────────────
void WebUI::handleDebugStats(AsyncWebServerRequest* req) {
    JsonDocument doc;

    // Memory
    JsonObject mem = doc["memory"].to<JsonObject>();
    mem["heap_free"]     = ESP.getFreeHeap();
    mem["heap_total"]    = ESP.getHeapSize();
    mem["heap_min_free"] = ESP.getMinFreeHeap();
    uint32_t heapTotal = ESP.getHeapSize();
    mem["heap_used_pct"] = heapTotal > 0
        ? (uint8_t)(100 - (ESP.getFreeHeap() * 100 / heapTotal))
        : (uint8_t)0;
    mem["psram_free"]    = ESP.getFreePsram();

    // CPU
    JsonObject cpu = doc["cpu"].to<JsonObject>();
    cpu["freq_mhz"]  = getCpuFrequencyMhz();
    cpu["chip"]      = ESP.getChipModel();
    cpu["cores"]     = 2;
    cpu["sdk"]       = ESP.getSdkVersion();

    // Runtime
    JsonObject rt = doc["runtime"].to<JsonObject>();
    uint32_t upSec = millis() / 1000;
    rt["uptime_s"]   = upSec;
    rt["uptime_str"] = String(upSec / 3600) + "h "
                     + String((upSec % 3600) / 60) + "m "
                     + String(upSec % 60) + "s";

    // Flash / FS
    JsonObject fs = doc["filesystem"].to<JsonObject>();
    fs["flash_size_mb"]  = (uint32_t)(ESP.getFlashChipSize() >> 20);
    fs["lfs_total_kb"]   = (uint32_t)(LittleFS.totalBytes() / 1024);
    fs["lfs_used_kb"]    = (uint32_t)(LittleFS.usedBytes() / 1024);
    fs["lfs_free_kb"]    = (uint32_t)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);
    fs["sd_mounted"]     = sdMgr.isAvailable();

    // WiFi
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["sta_connected"] = wifiMgr.staConnected();
    wifi["sta_rssi"]      = wifiMgr.staRSSI();
    wifi["sta_ip"]        = wifiMgr.staIP();
    wifi["ap_ip"]         = wifiMgr.apIP();

    // Audit
    doc["audit_total"] = auditMgr.totalLogged();
    doc["audit_stored"]= (uint32_t)auditMgr.size();

    String out; serializeJson(doc, out);
    sendJsonV1(req, 200, out);
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/debug/modules
// Returns status of all hardware modules
// ─────────────────────────────────────────────────────────────
void WebUI::handleDebugModules(AsyncWebServerRequest* req) {
    JsonDocument doc;

    // IR
    JsonObject ir = doc["ir"].to<JsonObject>();
    ir["rx_pin"]       = irReceiver.activePin();
    ir["tx_count"]     = irTransmitter.activeCount();
    ir["buttons_total"]= (uint32_t)irDB.buttons().size();
    ir["autosave"]     = irDB.autoSaveEnabled();

    // NFC
    JsonObject nfc = doc["nfc"].to<JsonObject>();
    nfc["reading"]   = nfcModule.isReading();
    nfc["emulating"] = nfcModule.isEmulating();

    // RFID
    JsonObject rfid = doc["rfid"].to<JsonObject>();
    rfid["reading"]   = rfidModule.isReading();
    rfid["emulating"] = rfidModule.isEmulating();

    // SubGHz
    JsonObject subghz = doc["subghz"].to<JsonObject>();
    subghz["capturing"] = subGhzModule.isCapturing();

    // NRF24
    JsonObject nrf24 = doc["nrf24"].to<JsonObject>();
    nrf24["scanning"] = nrf24Module.isScanning();
    nrf24["sniffing"] = nrf24Module.isSniffing();

    // Scheduler
    JsonObject sched = doc["scheduler"].to<JsonObject>();
    sched["entries"]    = (uint32_t)scheduler.size();
    sched["ntp_synced"] = scheduler.ntpSynced();
    sched["time"]       = scheduler.ntpSynced() ? scheduler.currentTimeStr() : "N/A";

    // Macros
    JsonObject macro = doc["macro"].to<JsonObject>();
    macro["running"]    = macroMgr.isRunning();
    macro["name"]       = macroMgr.runningName();
    macro["step"]       = macroMgr.runStep();
    macro["total_steps"]= macroMgr.runTotal();

    // Groups
    doc["groups_total"]    = (uint32_t)groupMgr.size();
    doc["firmware"]        = FIRMWARE_VERSION;

    String out; serializeJson(doc, out);
    sendJsonV1(req, 200, out);
}
