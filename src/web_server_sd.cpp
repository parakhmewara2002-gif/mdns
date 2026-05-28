// ============================================================
//  web_server_sd.cpp  -  SD Card Extended HTTP Routes
//  Features 42-45:
//    42. HTML from SD (asset override)  - handled in web_server.cpp
//    43. HTTP file upload to SD
//    44. Streaming large file download
//    45. SD static file server (/sd/*)
//
//  Additional quick API routes:
//    GET  /api/sd/status        - SD status JSON
//    GET  /api/sd/usage         - disk usage breakdown
//    GET  /api/sd/ls            - directory listing
//    GET  /api/sd/preview       - file preview
//    DELETE /api/sd/file        - delete file
//    GET  /api/sd/log/tail      - tail log
//    GET  /api/sd/log/download  - stream full log
//    POST /api/sd/backup        - full backup (feature 46 hook)
//    POST /api/sd/restore       - restore from backup
//    GET  /api/sd/backups       - list backups
//    GET  /api/sd/benchmark     - SD speed benchmark
//    POST /api/sd/macro/queue   - queue a macro from SD
//    POST /api/sd/macro/stop    - stop running macro
// ============================================================
#include "web_server.h"
#include "sd_manager.h"
#include "auth_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

// ── Local JSON helper ─────────────────────────────────────────
static void _sendJsonSd(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    r->addHeader("Access-Control-Allow-Headers", "Content-Type");
    req->send(r);
}

// ── Per-request body buffer (mirrors pattern in web_server_modules.cpp) ──
// AsyncWebServer delivers POST bodies in chunks. We accumulate them into a
// String hung off req->_tempObject so the handler sees one contiguous buffer.
static String* _getBodyBufSd(AsyncWebServerRequest* req) {
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
static void _freeBodyBufSd(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define SD_POST_BODY(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_getBodyBufSd(req)->length() + l > HTTP_MAX_BODY) { \
                _freeBodyBufSd(req); \
                _sendJsonSd(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _getBodyBufSd(req)->concat((char*)d, l); \
            bool lastChunk = (t > 0) ? (i + l >= t) : (i == 0); \
            if (lastChunk) { \
                String* buf = _getBodyBufSd(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _freeBodyBufSd(req); \
            } \
        })

// ── Content-type helper ───────────────────────────────────────
static String _mimeForPath(const String& path) {
    if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
    if (path.endsWith(".css"))    return "text/css";
    if (path.endsWith(".js"))     return "application/javascript";
    if (path.endsWith(".json"))   return "application/json";
    if (path.endsWith(".png"))    return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".ico"))    return "image/x-icon";
    if (path.endsWith(".svg"))    return "image/svg+xml";
    if (path.endsWith(".gz"))     return "application/gzip";
    if (path.endsWith(".txt"))    return "text/plain";
    if (path.endsWith(".xml"))    return "text/xml";
    return "application/octet-stream";
}

// ─────────────────────────────────────────────────────────────
//  WebUI::setupSdExtRoutes  - called from WebUI::begin()
// ─────────────────────────────────────────────────────────────
void WebUI::setupSdExtRoutes() {

    // ── Feature 43: HTTP file upload to SD ────────────────────
    // POST /api/sd/upload?path=<target-path>
    // Uses ESPAsyncWebServer multipart upload callbacks.
    // Note: a simpler route at /api/sd/upload already exists in setupSdRoutes()
    // for multipart upload.  This handler adds a query-param-based variant
    // so tools like curl can target a specific path directly.
    _server.on("/api/sd/upload2", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            _sendJsonSd(req, 200, "{\"ok\":true}");
        },
        [](AsyncWebServerRequest* req, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final) {
            if (!sdMgr.isAvailable()) {
                req->send(503, "application/json", "{\"error\":\"SD not available\"}");
                return;
            }
            String path;
            if (req->hasParam("path")) {
                path = req->getParam("path")->value();
            } else {
                path = String("/uploads/") + filename;
            }
            if (index == 0) {
                sdMgr.beginUpload(path);
            }
            if (len > 0) {
                sdMgr.writeUploadChunk(data, len);
            }
            if (final) {
                sdMgr.endUpload();
            }
        });

    // ── Feature 44: Streaming large file download ──────────────
    // GET /api/sd/download2?path=<file-path>
    // Streams file from SD with proper Content-Disposition.
    // Note: /api/sd/download already exists; this is the extended streaming version.
    _server.on("/api/sd/download2", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}");
                return;
            }
            if (!req->hasParam("path")) {
                _sendJsonSd(req, 400, "{\"error\":\"Missing path param\"}");
                return;
            }
            String path = req->getParam("path")->value();
            if (!sdMgr.exists(path)) {
                _sendJsonSd(req, 404, "{\"error\":\"File not found\"}");
                return;
            }

            // Extract filename from path for Content-Disposition
            String fname = path;
            int slash = fname.lastIndexOf('/');
            if (slash >= 0) fname = fname.substring(slash + 1);

            String mime = _mimeForPath(path);
            AsyncWebServerResponse* r = req->beginResponse(
                SD, path, mime);
            r->addHeader("Content-Disposition",
                         String("attachment; filename=\"") + fname + "\"");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    // ── Feature 45: SD static file server ─────────────────────
    // GET /sd/*  ->  serve /assets/<subpath> from SD
    _server.on("/sd/*", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                req->send(503, "text/plain", "SD not available");
                return;
            }
            // Extract subpath after /sd/
            String subpath = req->url().substring(3); // remove "/sd"
            // Guard against empty subpath
            if (subpath.length() == 0) subpath = "/";
            String sdPath = String(SD_DIR_ASSETS) + subpath;

            if (!sdMgr.exists(sdPath)) {
                req->send(404, "text/plain", "Not found");
                return;
            }
            String mime = _mimeForPath(sdPath);
            AsyncWebServerResponse* r = req->beginResponse(SD, sdPath, mime);
            r->addHeader("Cache-Control", "max-age=600");
            r->addHeader("Access-Control-Allow-Origin", "*");
            // If .gz file, add encoding header
            if (sdPath.endsWith(".gz")) {
                r->addHeader("Content-Encoding", "gzip");
            }
            req->send(r);
        });

    // ── Quick API routes ───────────────────────────────────────

    // GET /api/sd/usage
    _server.on("/api/sd/usage", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            _sendJsonSd(req, 200, sdMgr.usageJson());
        });

    // GET /api/sd/preview?path=...
    _server.on("/api/sd/preview", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            if (!req->hasParam("path")) {
                _sendJsonSd(req, 400, "{\"error\":\"Missing path\"}"); return;
            }
            String path = req->getParam("path")->value();
            SdPreview pv = sdMgr.previewFile(path);
            JsonDocument doc;
            doc["path"]      = pv.path;
            doc["size"]      = (unsigned long)pv.fileSize;
            doc["truncated"] = pv.truncated;
            doc["content"]   = pv.content;
            String out; serializeJson(doc, out);
            _sendJsonSd(req, 200, out);
        });

    // DELETE /api/sd/file?path=...
    _server.on("/api/sd/file", HTTP_DELETE,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            if (!req->hasParam("path")) {
                _sendJsonSd(req, 400, "{\"error\":\"Missing path\"}"); return;
            }
            String path = req->getParam("path")->value();
            bool ok = sdMgr.deleteFile(path);
            _sendJsonSd(req, ok ? 200 : 404,
                        ok ? "{\"ok\":true}" : "{\"error\":\"Delete failed\"}");
        });

    // GET /api/sd/log/tail?lines=50
    _server.on("/api/sd/log/tail", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            uint16_t lines = 50;
            if (req->hasParam("lines")) {
                lines = (uint16_t)req->getParam("lines")->value().toInt();
            }
            String tail = sdMgr.tailLog(lines);
            JsonDocument doc;
            doc["ok"]   = true;
            doc["lines"] = lines;
            doc["log"]   = tail;
            String out; serializeJson(doc, out);
            _sendJsonSd(req, 200, out);
        });

    // GET /api/sd/log/download - stream entire log file
    _server.on("/api/sd/log/download", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            String logPath = SD_LOG_FILE;
            if (!sdMgr.exists(logPath)) {
                _sendJsonSd(req, 404, "{\"error\":\"Log file not found\"}"); return;
            }
            AsyncWebServerResponse* r = req->beginResponse(
                SD, logPath, "text/plain");
            r->addHeader("Content-Disposition", "attachment; filename=\"activity.log\"");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    // POST /api/sd/backup?tag=...
    _server.on("/api/sd/backup2", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            String tag;
            if (req->hasParam("tag")) {
                tag = req->getParam("tag")->value();
            } else {
                // Auto-generate tag from millis
                char buf[24]; snprintf(buf, sizeof(buf), "backup_%lu", (unsigned long)millis());
                tag = String(buf);
            }
            bool ok = sdMgr.backupToSD(tag);
            JsonDocument doc;
            doc["ok"]  = ok;
            doc["tag"] = tag;
            String out; serializeJson(doc, out);
            _sendJsonSd(req, ok ? 200 : 500, out);
        });

    // POST /api/sd/restore2?tag=...
    _server.on("/api/sd/restore2", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            if (!req->hasParam("tag")) {
                _sendJsonSd(req, 400, "{\"error\":\"Missing tag\"}"); return;
            }
            String tag = req->getParam("tag")->value();
            bool ok = sdMgr.restoreFromSD(tag);
            _sendJsonSd(req, ok ? 200 : 500,
                        ok ? "{\"ok\":true}" : "{\"error\":\"Restore failed\"}");
        });

    // GET /api/sd/benchmark
    _server.on("/api/sd/benchmark", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            SdBenchmark bm = sdMgr.benchmark();
            JsonDocument doc;
            doc["ok"]         = true;
            doc["writeKBps"]  = bm.writeKBps;
            doc["readKBps"]   = bm.readKBps;
            doc["fileSizeB"]  = bm.fileSize;
            String out; serializeJson(doc, out);
            _sendJsonSd(req, 200, out);
        });

    // POST /api/sd/macro/queue?file=...
    _server.on("/api/sd/macro/queue", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            if (!req->hasParam("file")) {
                _sendJsonSd(req, 400, "{\"error\":\"Missing file param\"}"); return;
            }
            String file = req->getParam("file")->value();
            bool ok = sdMgr.queueMacro(file);
            _sendJsonSd(req, ok ? 200 : 400,
                        ok ? "{\"ok\":true}" : "{\"error\":\"Queue macro failed\"}");
        });

    // POST /api/sd/macro/stop2
    _server.on("/api/sd/macro/stop2", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            sdMgr.stopMacro();
            _sendJsonSd(req, 200, "{\"ok\":true}");
        });

    // ── Feature 46: Full backup (all LittleFS JSON files) ─────
    // POST /api/sd/backup/full?tag=...
    _server.on("/api/sd/backup/full", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            String tag;
            if (req->hasParam("tag")) {
                tag = req->getParam("tag")->value();
            } else {
                char buf[28];
                snprintf(buf, sizeof(buf), "full_%lu", (unsigned long)millis());
                tag = String(buf);
            }
            bool ok = sdMgr.fullBackup(tag);
            JsonDocument doc;
            doc["ok"]  = ok;
            doc["tag"] = tag;
            String out; serializeJson(doc, out);
            _sendJsonSd(req, ok ? 200 : 500, out);
        });

    // ── Feature 47: Factory reset with SD restore ─────────────
    // POST /api/sd/factory_reset
    _server.on("/api/sd/factory_reset", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            _sendJsonSd(req, 200, "{\"ok\":true,\"msg\":\"Rebooting with SD restore\"}");
            vTaskDelay(pdMS_TO_TICKS(500));
            sdMgr.factoryResetWithRestore();
        });

    // ── Feature 48: Config profiles ───────────────────────────
    // GET /api/sd/profiles - list all saved config profiles
    _server.on("/api/sd/profiles", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            auto profiles = sdMgr.listConfigProfiles();
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            for (const auto& p : profiles) arr.add(p);
            String out; serializeJson(doc, out);
            _sendJsonSd(req, 200, out);
        });

    // POST /api/sd/profiles  body: {action:"save"|"load", name:"..."}
    // Action-style dispatcher used by the built-in HTML UI.
    // (The split /save and /load endpoints below remain for query-param callers.)
    SD_POST_BODY("/api/sd/profiles",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            JsonDocument body;
            if (deserializeJson(body, d, l) != DeserializationError::Ok) {
                _sendJsonSd(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
            }
            String action = body["action"] | "";
            String name   = body["name"]   | "";
            if (name.isEmpty()) {
                _sendJsonSd(req, 400, "{\"error\":\"Missing name\"}"); return;
            }
            bool ok;
            if (action == "save") {
                ok = sdMgr.saveConfigProfile(name);
                _sendJsonSd(req, ok ? 200 : 500,
                    ok ? "{\"ok\":true}" : "{\"error\":\"Save profile failed\"}");
            } else if (action == "load") {
                ok = sdMgr.loadConfigProfile(name);
                _sendJsonSd(req, ok ? 200 : 500,
                    ok ? "{\"ok\":true}" : "{\"error\":\"Load profile failed\"}");
            } else {
                _sendJsonSd(req, 400,
                    "{\"error\":\"Unknown action — use 'save' or 'load'\"}");
            }
        });

    // POST /api/sd/profiles/save?name=...
    _server.on("/api/sd/profiles/save", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            if (!req->hasParam("name")) {
                _sendJsonSd(req, 400, "{\"error\":\"Missing name\"}"); return;
            }
            String name = req->getParam("name")->value();
            bool ok = sdMgr.saveConfigProfile(name);
            _sendJsonSd(req, ok ? 200 : 500,
                        ok ? "{\"ok\":true}" : "{\"error\":\"Save profile failed\"}");
        });

    // POST /api/sd/profiles/load?name=...
    _server.on("/api/sd/profiles/load", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            if (!req->hasParam("name")) {
                _sendJsonSd(req, 400, "{\"error\":\"Missing name\"}"); return;
            }
            String name = req->getParam("name")->value();
            bool ok = sdMgr.loadConfigProfile(name);
            _sendJsonSd(req, ok ? 200 : 500,
                        ok ? "{\"ok\":true}" : "{\"error\":\"Load profile failed\"}");
        });

    // ── Feature 50: Multi-device config sync ──────────────────
    // POST /api/sd/sync/export?device=...
    _server.on("/api/sd/sync/export", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            String dev = req->hasParam("device")
                         ? req->getParam("device")->value()
                         : String("device1");
            bool ok = sdMgr.exportConfigForSync(dev);
            _sendJsonSd(req, ok ? 200 : 500,
                        ok ? "{\"ok\":true}" : "{\"error\":\"Export sync failed\"}");
        });

    // POST /api/sd/sync/import?device=...
    _server.on("/api/sd/sync/import", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            String dev = req->hasParam("device")
                         ? req->getParam("device")->value()
                         : String("device1");
            bool ok = sdMgr.importConfigFromSync(dev);
            _sendJsonSd(req, ok ? 200 : 500,
                        ok ? "{\"ok\":true}" : "{\"error\":\"Import sync failed\"}");
        });

    // GET /api/sd/sync/devices - list all synced device configs
    _server.on("/api/sd/sync/devices", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            auto devs = sdMgr.listSyncedDevices();
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            for (const auto& d : devs) arr.add(d);
            String out; serializeJson(doc, out);
            _sendJsonSd(req, 200, out);
        });

    // ── SD health stats ───────────────────────────────────────
    // GET /api/sd/health
    _server.on("/api/sd/health", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            SdHealth h = sdMgr.healthStats();
            JsonDocument doc;
            doc["ok"]          = true;
            doc["totalBytes"]  = (unsigned long long)h.totalBytes;
            doc["usedBytes"]   = (unsigned long long)h.usedBytes;
            doc["usedPct"]     = h.usedPct;
            doc["cardType"]    = h.cardType;
            doc["mountCount"]  = h.mountCount;
            String out; serializeJson(doc, out);
            _sendJsonSd(req, 200, out);
        });

    // ── SD TAR archive ────────────────────────────────────────
    // POST /api/sd/archive   body: {"dir":"/backups","out":"/archive.tar"}
    SD_POST_BODY("/api/sd/archive",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            if (!sdMgr.isAvailable()) {
                _sendJsonSd(req, 503, "{\"error\":\"SD not available\"}"); return;
            }
            JsonDocument body;
            if (deserializeJson(body, d, l) != DeserializationError::Ok) {
                _sendJsonSd(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
            }
            String dir = body["dir"] | "/backups";
            String out = body["out"] | "/archive.tar";
            bool ok = sdMgr.createTar(dir, out);
            _sendJsonSd(req, ok ? 200 : 500,
                ok ? "{\"ok\":true}" : "{\"error\":\"Archive failed\"}");
        });
}
