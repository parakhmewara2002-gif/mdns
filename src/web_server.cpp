// ============================================================
//  web_server.cpp  -  Async HTTP + WebSocket + All API routes
//  v2.1.0 - smoothness fixes applied
// ============================================================
#include "web_server.h"
#include "ir_database.h"
#include "ir_transmitter.h"
#include "ir_receiver.h"
#include "wifi_manager.h"
#include "group_manager.h"
#include "scheduler.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "sd_manager.h"
#include "mic_module.h"
#include <esp_heap_caps.h>    // heap_caps_get_largest_free_block()
#include <esp_system.h>       // esp_get_minimum_free_heap_size()
#include "macro_manager.h"
#include "nfc_module.h"
#include "rfid_module.h"
#include "subghz_module.h"
#include "nrf24_module.h"
#include "system_module.h"
#include "audit_manager.h"   // Batch 1: Audit Trail
#include "auth_manager.h"
#include "wifi_pen_module.h"  // WiFi Penetration Module
#include "watchdog_manager.h" // Ultra Pro Watchdog - status fields
#include "ir_jammer.h"        // IR Jammer (Bruce port)
#include "ir_receiver.h"      // IR Receiver pause/resume
#include "beacon_spam.h"      // Beacon Spam (Bruce port)
#include "responder.h"        // LLMNR/NBNS Responder (Bruce port)
#include <ctime>          // time(), localtime_r() for SD backup timestamps

WebUI webUI;

// ── Helpers ───────────────────────────────────────────────────
// FIX: sendJson now uses AsyncResponseStream - writes directly to TCP send buffer.
// The old version built a String copy of 'json' and then copied it again into
// the response buffer (two allocations). AsyncResponseStream eliminates both.
void sendJson(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncResponseStream* r = req->beginResponseStream("application/json");
    r->setCode(code);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->print(json);
    req->send(r);
}

// Convenience overload: serialize a JsonDocument directly to stream - zero String alloc.
// Use this for all new handlers: sendJsonDoc(req, 200, doc)
void sendJsonDoc(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    AsyncResponseStream* r = req->beginResponseStream("application/json");
    r->setCode(code);
    r->addHeader("Access-Control-Allow-Origin", "*");
    serializeJson(doc, *r);   // writes directly to TCP buffer - no intermediate String
    req->send(r);
}

// Body accumulator (multi-chunk POST body support)
static String* getBodyBuf(AsyncWebServerRequest* req) {
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
static void freeBodyBuf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

// Macro to register a chunked-body POST handler.
// ESPAsyncWebServer sets t=0 for Transfer-Encoding: chunked requests
// (total length unknown). The correct end-of-body signal is (i + l >= t && t > 0)
// OR the "final" convention used by ESPAsyncWebServer: when the library has
// received the last chunk it calls the body handler one final time with
// (index + len == total) only when total > 0. For chunked encoding it calls
// with index=0, len=fullBody, total=0 on the only/last call.
// Solution: fire handler when (t == 0 && i == 0) [chunked, single callback]
// OR when (t > 0 && i + l >= t) [content-length known, final chunk].
#define POST_BODY(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (getBodyBuf(req)->length() + l > HTTP_MAX_BODY) { \
                freeBodyBuf(req); \
                sendJson(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            getBodyBuf(req)->concat((char*)d, l); \
            bool lastChunk = (t > 0) ? (i + l >= t) : (i == 0); \
            if (lastChunk) { \
                String* buf = getBodyBuf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                freeBodyBuf(req); \
            } \
        })

// ── Constructor / begin / loop ────────────────────────────────
WebUI::WebUI() : _server(HTTP_PORT), _ws(WS_PATH) {
    // FIX: create FreeRTOS mutex for WS queue - replaces portMUX spinlock
    _wsMutex = xSemaphoreCreateMutex();
    if (!_wsMutex) {
        Serial.println(DEBUG_TAG " FATAL: WS mutex creation failed");
    }
    // Serializes the actual textAll()/binaryAll() send calls across the
    // loop-task (text) and mic-task (binary) producers.
    _wsSendMutex = xSemaphoreCreateMutex();
    if (!_wsSendMutex) {
        Serial.println(DEBUG_TAG " FATAL: WS send mutex creation failed");
    }
}

void WebUI::begin() {
    setupWebSocket();
    setupApiRoutes();
    setupGroupRoutes();
    setupSchedulerRoutes();
    setupWifiRoutes();
    setupGpioRoutes();
    setupMacroRoutes();
    setupSdRoutes();
    // Hardware module routes: only register if at least one module is connected
    // When all 4 are absent (common), skips ~49 routes saving ~14KB heap
    if (nfcModule.isConnected() || rfidModule.isConnected() ||
        subGhzModule.isConnected() || nrf24Module.isConnected()) {
        setupModuleRoutes();
    }
    setupModuleToggleRoutes();
    setupRestApiV1Routes();
    setupAuditRoutes();
    setupDebugRoutes();
    // Mic routes: only if I2S mic is active (saves ~11 routes = ~3KB)
    if (micModule.i2sActive()) { setupMicRoutes(); setupMicPinsRoute(); }
    setupAuthRoutes();
    setupCaptivePortal();
    setupWatchdogRoutes();
    setupLogRoutes();
    // WiFi Pen routes: only if enabled (saves ~27 routes = ~7.5KB)
    if (wifiPen.enabled()) setupWpenRoutes();
    setupAcRoutes();
    setupSdExtRoutes();
    setupIrJammerRoutes();
    setupBeaconSpamRoutes();
    setupResponderRoutes();
    setupStaticRoutes();          // must be last - catch-all
    _server.begin();
    Serial.printf(DEBUG_TAG " HTTP server on port %d\n", HTTP_PORT);
}

void WebUI::loop() {
    // cleanupClients iterates all WS client slots - rate-limit to every 5s.
    // Disconnected clients are removed lazily; no correctness impact.
    unsigned long now = millis();
    if (now - _lastWsCleanup >= 5000UL) {
        _lastWsCleanup = now;
        _ws.cleanupClients(WS_MAX_CLIENTS);
    }
    _flushWsQueue();
    loopCaptivePortal();  // Batch 3: process DNS for captive portal
}

void WebUI::_flushWsQueue() {
    // FIX: If no clients, drain queue immediately to free heap - avoids
    // accumulating up to WS_QUEUE_MAX String objects nobody will receive.
    if (_ws.count() == 0) {
        if (_wsMutex && xSemaphoreTake(_wsMutex, 0) == pdTRUE) {
            while (!_wsQueue.empty()) _wsQueue.pop();
            xSemaphoreGive(_wsMutex);
        }
        return;
    }

    // Drain at most 8 messages per loop() call - doubled from 4 to reduce
    // queue backlog during IR auto-save bursts and multi-button scans.
    // Worst case: 8 × ~2ms textAll() = ~16ms per loop tick, still acceptable.
    int budget = 8;
    while (budget-- > 0) {
        String msg;
        {
            if (!_wsMutex || xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(2)) != pdTRUE)
                break;
            bool empty = _wsQueue.empty();
            if (!empty) {
                msg = std::move(_wsQueue.front());
                _wsQueue.pop();
            }
            xSemaphoreGive(_wsMutex);
            if (empty) break;
        }
        // Serialize the real send against broadcastBinary() (mic task, Core 0).
        if (_wsSendMutex && xSemaphoreTake(_wsSendMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            _ws.textAll(msg);
            xSemaphoreGive(_wsSendMutex);
        } else {
            _ws.textAll(msg);   // mutex unavailable (OOM at boot) - best effort
        }
    }
}

// FIX: _pushWsMessage uses FreeRTOS mutex instead of portENTER_CRITICAL.
// String copy (heap alloc) happens before lock - same as before.
// std::queue::push() can realloc its internal deque - safe only outside
// portENTER_CRITICAL because portENTER_CRITICAL disables interrupts and
// ESP-IDF heap_caps_malloc also takes a spinlock -> deadlock.
void WebUI::_pushWsMessage(const String& msg) {
    if (!_wsMutex) return;
    // Pre-copy outside the lock (malloc must never run inside the lock)
    String copy(msg);
    if (xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (_wsQueue.size() >= WS_QUEUE_MAX) _wsQueue.pop();  // drop oldest
    _wsQueue.push(std::move(copy));
    xSemaphoreGive(_wsMutex);
}

// ── WebSocket ─────────────────────────────────────────────────
void WebUI::setupWebSocket() {
    _ws.onEvent(onWsEvent);
    _server.addHandler(&_ws);
}

/*static*/
void WebUI::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len)
{
    (void)server;
    switch (type) {
        case WS_EVT_CONNECT:
            // FIX: evict stale clients before accepting new one - prevents
            // ghost-client heap accumulation from unreachable textAll() frames.
            server->cleanupClients(WS_MAX_CLIENTS);
            Serial.printf(DEBUG_TAG " WS #%u connected (active: %u)\n",
                          client->id(), server->count());
            client->text("{\"event\":\"connected\",\"version\":\"" FIRMWARE_VERSION "\"}");
            // Mark status dirty so full status is sent on next broadcast cycle
            webUI._statusDirty = true;
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf(DEBUG_TAG " WS #%u disconnected (remaining: %u)\n",
                          client->id(), server->count());
            // FIX: if no clients remain, mark dirty so next connect gets full state.
            // Queue drain of orphaned String objects happens inside _flushWsQueue()
            // which checks ws.count() == 0 and pops the entire queue.
            if (server->count() == 0) webUI._statusDirty = true;
            break;
        case WS_EVT_DATA: {
            AwsFrameInfo* fi = (AwsFrameInfo*)arg;
            if (fi->final && fi->index == 0 && fi->len == len && fi->opcode == WS_TEXT) {
                char buf[24] = {0};
                memcpy(buf, data, len < 23 ? len : 23);
                if (strcmp(buf, "ping") == 0)
                    client->text("{\"event\":\"pong\"}");
            }
            break;
        }
        case WS_EVT_ERROR:
            Serial.printf(DEBUG_TAG " WS #%u error\n", client->id());
            break;
        default: break;
    }
}

// ── Static / fallback routes ──────────────────────────────────
void WebUI::setupStaticRoutes() {
    // ── favicon - suppress "does not exist" log spam ──────────
    _server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (LittleFS.exists("/favicon.ico"))
            req->send(LittleFS, "/favicon.ico", "image/x-icon");
        else
            req->send(204);
    });



    // ── / and /index.html - serve gzip-compressed if available ──
    // index.html.gz is ~78% smaller (57KB vs 270KB), critical for LittleFS space.
    // All modern browsers support gzip Content-Encoding transparently.
    // Feature 42: if SD has an asset override for index.html.gz, serve it from SD.
    auto serveIndex = [](AsyncWebServerRequest* req) {
        uint32_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        Serial.printf("[WEB] serveIndex req - heap=%u maxBlock=%u\n",
                      ESP.getFreeHeap(), blk);
        if (blk < 3000) {
            req->send(503, "text/plain", "Low memory - please retry");
            return;
        }
        // Feature 42: SD asset override check
        if (sdMgr.hasAsset("index.html.gz")) {
            String sdPath = sdMgr.assetPath("index.html.gz");
            // RACE FIX: take the VSPI bus mutex before SD.open().
            // hw_poll task (Core 1) also accesses the SPI bus via nfc/rfid
            // modules. Direct SD.open() without the bus mutex can corrupt the
            // SPI transaction in progress, causing SD reads to return garbage
            // or the card to stop responding.
            extern SemaphoreHandle_t g_spi_vspi_mutex;
            bool _sdLocked = (g_spi_vspi_mutex &&
                xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(200)) == pdTRUE);
            File sdF = SD.open(sdPath, FILE_READ);
            if (_sdLocked) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
            if (sdF) {
                sdF.close();  // close probe handle; beginResponse opens its own handle
                AsyncWebServerResponse* r = req->beginResponse(
                    SD, sdPath, "text/html");
                r->addHeader("Content-Encoding", "gzip");
                r->addHeader("Vary", "Accept-Encoding");
                r->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
                req->send(r);
                return;
            }
        }
        if (LittleFS.exists("/index.html.gz")) {
            // Check if client accepts gzip (virtually all browsers do)
            String ae = req->header("Accept-Encoding");
            if (ae.indexOf("gzip") >= 0) {
                // Serve gzip-compressed file - browser decompresses transparently
                AsyncWebServerResponse* r = req->beginResponse(
                    LittleFS, "/index.html.gz", "text/html");
                r->addHeader("Content-Encoding", "gzip");
                r->addHeader("Vary", "Accept-Encoding");
                r->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
                r->addHeader("Pragma", "no-cache");
                req->send(r);
            } else {
                // Rare: client doesn't accept gzip - decompress in RAM and send
                // (only ~57KB to decompress; fits in ESP32 heap)
                File f = LittleFS.open("/index.html.gz", "r");
                if (f) {
                    f.close();
                }
                // Fallback: serve as-is with gzip header anyway
                // Modern ESP32 browsers always support gzip
                AsyncWebServerResponse* r = req->beginResponse(
                    LittleFS, "/index.html.gz", "text/html");
                r->addHeader("Content-Encoding", "gzip");
                r->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
                req->send(r);
            }
        } else if (LittleFS.exists("/index.html")) {
            AsyncWebServerResponse* r = req->beginResponse(
                LittleFS, "/index.html", "text/html");
            r->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
            r->addHeader("Pragma", "no-cache");
            req->send(r);
        } else {
            req->send(404, "text/plain", "index.html not found");
        }
    };
    _server.on("/", HTTP_GET, serveIndex);
    _server.on("/index.html", HTTP_GET, serveIndex);
    _server.on("/index.htm",  HTTP_GET, serveIndex);

    _server.serveStatic("/", LittleFS, "/")
           .setCacheControl("max-age=600")
           .setDefaultFile("index.html.gz");

    _server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_OPTIONS) {
            AsyncWebServerResponse* r = req->beginResponse(204);
            r->addHeader("Access-Control-Allow-Origin",  "*");
            r->addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
            r->addHeader("Access-Control-Allow-Headers", "Content-Type");
            req->send(r); return;
        }
        // Suppress common browser auto-requests
        const String& url = req->url();
        if (url == "/favicon.ico" || url == "/robots.txt" ||
            url == "/apple-touch-icon.png") {
            req->send(204); return;
        }
        sendJson(req, 404, "{\"error\":\"Not found\"}");
    });
}



// ── GPIO routes ───────────────────────────────────────────────
void WebUI::setupGpioRoutes() {
    _server.on("/api/gpio/pins", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetGpioPins(req); });

    POST_BODY("/api/gpio/pins",
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSetGpioPins(req, d, l);
        });

    _server.on("/api/gpio/available", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetPinList(req); });
}

// ── Core API routes ───────────────────────────────────────────
void WebUI::setupApiRoutes() {
    _server.on("/api/buttons", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetButtons(req); });

    POST_BODY("/api/buttons",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleAddButton(req, d, l); }));

    POST_BODY("/api/buttons/update",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleUpdateButton(req, d, l); }));

    _server.on("/api/buttons/delete", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleDeleteButton(req); });

    _server.on("/api/clear", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleClearButtons(req); });

    POST_BODY("/api/transmit",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleTransmit(req, d, l); }));

    // POST /api/v1/ir/pwm-test
    // Send a test RAW burst at a specified carrier frequency to verify
    // the emitter and PWM output are working correctly.
    // Body: { "freqKHz": 38, "emitterIdx": 0 }
    // Response: { "ok": true, "freqKHz": 38, "pulsesUs": [9000,4500,...] }
    POST_BODY("/api/v1/ir/pwm-test",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handlePwmTest(req, d, l); }));

    // GET /api/v1/ir/pwm-test?freq=38&duty=33
    // Convenience GET alias for the PWM test (same transmit logic as the POST version).
    // Query params: freq (kHz, default 38), duty (ignored - hardware uses 50%).
    _server.on("/api/v1/ir/pwm-test", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handlePwmTestGet(req); });

    // GET /api/v1/ir/pwm-info
    // Returns PWM capabilities: valid freq range, default, current emitter pins.
    _server.on("/api/v1/ir/pwm-info", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handlePwmInfo(req); });

    _server.on("/api/export", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleExport(req); });

    POST_BODY("/api/import",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleImport(req, d, l); }));

    _server.on("/api/config", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetConfig(req); });

    POST_BODY("/api/config",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleSetConfig(req, d, l); }));

    // GET /api/ping - dead-simple connectivity check, no auth/LittleFS needed
    _server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest* req) {
        String body = String("{\"ok\":true,\"firmware\":\"")
                    + FIRMWARE_VERSION
                    + "\",\"heap\":"
                    + String(ESP.getFreeHeap()) + "}";
        AsyncWebServerResponse* r = req->beginResponse(200, "application/json", body);
        r->addHeader("Access-Control-Allow-Origin", "*");
        req->send(r);
    });

    _server.on("/api/status", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetStatus(req); });

    _server.on("/api/restart", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleRestart(req); });

    // POST /api/wifi/reset — delete saved WiFi config and reboot.
    // Restores default AP SSID="IR-Remote" password="irremote123".
    _server.on("/api/wifi/reset", HTTP_POST,
        [this](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            LittleFS.remove(CFG_FILE);
            sendJson(req, 200, "{\"ok\":true,\"note\":\"WiFi config cleared - rebooting\"}");
            extern portMUX_TYPE s_restartMux;
            extern volatile uint32_t s_restartAt;
            taskENTER_CRITICAL(&s_restartMux);
            s_restartAt = (uint32_t)(millis() + 500);
            taskEXIT_CRITICAL(&s_restartMux);
        });

    // Fix 1: auto-save config routes
    _server.on("/api/autosave", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetAutoSave(req); });
    _server.on("/api/autosave", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleSetAutoSave(req); });

    // ── Backup & Restore routes ───────────────────────────────
    // POST /api/backup          -> create backup of current DB
    // GET  /api/backup          -> download the backup file
    // GET  /api/backup/status   -> check if backup exists + metadata
    // POST /api/restore         -> upload JSON, validate, backup, restore
    _server.on("/api/backup", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleBackupCreate(req); });

    _server.on("/api/backup", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleBackupDownload(req); });

    _server.on("/api/backup/status", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleBackupStatus(req); });

    POST_BODY("/api/restore",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleRestore(req, d, l); }));
}

// ── Group routes ──────────────────────────────────────────────
void WebUI::setupGroupRoutes() {
    _server.on("/api/groups", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetGroups(req); });

    POST_BODY("/api/groups",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleAddGroup(req, d, l); }));

    POST_BODY("/api/groups/update",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleUpdateGroup(req, d, l); }));

    _server.on("/api/groups/delete", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleDeleteGroup(req); });

    POST_BODY("/api/groups/reorder",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleReorderGroup(req, d, l); }));
}

// ── Scheduler routes ──────────────────────────────────────────
void WebUI::setupSchedulerRoutes() {
    _server.on("/api/schedules", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetSchedules(req); });

    POST_BODY("/api/schedules",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleAddSchedule(req, d, l); }));

    POST_BODY("/api/schedules/update",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleUpdateSchedule(req, d, l); }));

    _server.on("/api/schedules/delete", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleDeleteSchedule(req); });

    POST_BODY("/api/schedules/toggle",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleToggleSchedule(req, d, l); }));

    _server.on("/api/ntp/status", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleGetNtpStatus(req); });

    POST_BODY("/api/ntp/timezone",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l){
            handleSetTimezone(req, d, l); }));

    // ── Scheduler presets / export / import / autobackup ─────
    // GET  /api/scheduler/presets
    _server.on("/api/scheduler/presets", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            auto names = scheduler.listPresets();
            JsonDocument doc;
            JsonArray arr = doc["presets"].to<JsonArray>();
            for (const auto& n : names) arr.add(n);
            String out; serializeJson(doc, out);
            sendJson(req, 200, out);
        });

    // POST /api/scheduler/preset/save   body: {"name":"..."}
    POST_BODY("/api/scheduler/preset/save",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument body; deserializeJson(body, d, l);
            String name = body["name"] | "";
            name.trim();
            if (name.isEmpty()) { sendJson(req, 400, "{\"error\":\"Missing name\"}"); return; }
            bool ok = scheduler.savePreset(name);
            sendJson(req, ok ? 200 : 503, ok ? "{\"ok\":true}" : "{\"error\":\"SD unavailable\"}");
        }));

    // POST /api/scheduler/preset/load   body: {"name":"..."}
    POST_BODY("/api/scheduler/preset/load",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument body; deserializeJson(body, d, l);
            String name = body["name"] | "";
            name.trim();
            if (name.isEmpty()) { sendJson(req, 400, "{\"error\":\"Missing name\"}"); return; }
            bool ok = scheduler.loadPreset(name);
            sendJson(req, ok ? 200 : 404, ok ? "{\"ok\":true}" : "{\"error\":\"Preset not found\"}");
        }));

    // POST /api/scheduler/export   body: {"tag":"..."}  (tag optional — defaults to "default")
    POST_BODY("/api/scheduler/export",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument body; deserializeJson(body, d, l);
            String tag = body["tag"] | "default";
            tag.trim();
            bool ok = scheduler.exportToSD(tag);
            sendJson(req, ok ? 200 : 503, ok ? "{\"ok\":true}" : "{\"error\":\"Export failed\"}");
        }));

    // POST /api/scheduler/import   body: {"tag":"..."}
    POST_BODY("/api/scheduler/import",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument body; deserializeJson(body, d, l);
            String tag = body["tag"] | "default";
            tag.trim();
            bool ok = scheduler.importFromSD(tag);
            sendJson(req, ok ? 200 : 404, ok ? "{\"ok\":true}" : "{\"error\":\"Import failed\"}");
        }));

    // POST /api/scheduler/autobackup   body: {"en":true,"hour":3}
    POST_BODY("/api/scheduler/autobackup",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument body; deserializeJson(body, d, l);
            bool en   = body["en"] | false;
            int  hour = body["hour"] | 3;
            if (hour < 0 || hour > 23) hour = 3;
            String cron = String("0 ") + hour + " * * *";
            scheduler.enableAutoBackup(en, cron);
            sendJson(req, 200, String("{\"ok\":true,\"en\":") + (en?"true":"false") +
                     ",\"hour\":" + hour + "}");
        }));
}

// ── Wi-Fi scan routes ─────────────────────────────────────────
void WebUI::setupWifiRoutes() {
    _server.on("/api/wifi/scan", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleStartScan(req); });

    _server.on("/api/wifi/scan", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleScanResults(req); });
}

// ─────────────────────────────────────────────────────────────
//  Handler implementations
// ─────────────────────────────────────────────────────────────

// ── Buttons ───────────────────────────────────────────────────
void WebUI::handleGetButtons(AsyncWebServerRequest* req) {
    sendJson(req, 200, irDB.compactJson());
}

void WebUI::handleAddButton(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    IRButton btn;
    if (!btn.fromJson(doc.as<JsonObjectConst>()))
        { sendJson(req,400,"{\"error\":\"Invalid button data\"}"); return; }
    // Apply protocol repeat defaults if not supplied
    if (!doc["repeatCount"].is<int>()) {
        auto preset = defaultRepeatForProtocol(btn.protocol);
        btn.repeatCount = preset.count;
        btn.repeatDelay = preset.delayMs;
    }
    uint32_t id = irDB.add(btn);
    if (!id) { sendJson(req,500,"{\"error\":\"DB full\"}"); return; }
    JsonDocument r; r["ok"]=true; r["id"]=id;
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(r, out);
    sendJson(req,200,out);
}

void WebUI::handleUpdateButton(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    IRButton btn;
    if (!btn.fromJson(doc.as<JsonObjectConst>()) || !btn.id)
        { sendJson(req,400,"{\"error\":\"Invalid data\"}"); return; }
    if (!irDB.update(btn.id,btn))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    sendJson(req,200,"{\"ok\":true}");
}

void WebUI::handleClearButtons(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    irDB.clear();
    JsonDocument r; r["ok"]=true; r["buttons"]=0;
    sendJsonDoc(req,200,r);  // FIX: zero-copy stream serialize
}

void WebUI::handleDeleteButton(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!req->hasParam("id"))
        { sendJson(req,400,"{\"error\":\"Missing id\"}"); return; }
    int raw = req->getParam("id")->value().toInt();
    if (raw<=0) { sendJson(req,400,"{\"error\":\"Invalid id\"}"); return; }
    if (!irDB.remove((uint32_t)raw))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    sendJson(req,200,"{\"ok\":true}");
}

void WebUI::handleTransmit(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    bool ok = false;
    if (doc["id"].is<int>() && doc["id"].as<int>()>0) {
        uint32_t id = doc["id"].as<uint32_t>();
        IRButton copy = irDB.findById(id);
        if (!copy.id) { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
        if (doc["repeatCount"].is<int>()) copy.repeatCount = doc["repeatCount"].as<uint8_t>();
        if (doc["repeatDelay"].is<int>()) copy.repeatDelay = doc["repeatDelay"].as<uint16_t>();
        if (doc["emitterIdx"].is<int>()) {
            // transmitOn is blocking but user-requested specific emitter - keep synchronous
            // so response reflects actual TX result
            ok = irTransmitter.transmitOn(doc["emitterIdx"].as<uint8_t>(), copy);
        } else {
            // FIX: use transmitAsync - this handler runs in AsyncTCP task on Core 0.
            // Blocking transmit() here would freeze all HTTP/WS handling during TX.
            ok = irTransmitter.transmitAsync(copy);
        }
    } else {
        IRButton btn;
        if (!doc["name"].is<const char*>()) doc["name"] = "_tx_test";
        if (!btn.fromJson(doc.as<JsonObjectConst>()))
            { sendJson(req,400,"{\"error\":\"Invalid data\"}"); return; }
        // FIX: async for ad-hoc raw TX too
        ok = irTransmitter.transmitAsync(btn);
    }
    sendJson(req, ok?200:500, ok?"{\"ok\":true}":"{\"error\":\"Transmit failed\"}");
}

// ── PWM / carrier frequency test ─────────────────────────────
// Sends a standard NEC-style mark/space RAW pattern at the requested
// carrier frequency. This verifies:
//   1. The IR emitter LED is wired correctly
//   2. The ledc PWM timer generates the right carrier
//   3. The freqKHz field in saved buttons will transmit correctly
//
// Standard test pattern: 9 ms mark + 4.5 ms space + 560 us mark (AGC burst).
// A receiver / oscilloscope on the RX pin will show the modulated carrier.
void WebUI::handlePwmTest(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }

    uint16_t freqKHz = doc["freqKHz"] | (uint16_t)IR_DEFAULT_FREQ_KHZ;
    // Clamp to valid range
    if (freqKHz < 20 || freqKHz > 60) freqKHz = IR_DEFAULT_FREQ_KHZ;

    uint8_t emitterIdx = doc["emitterIdx"] | (uint8_t)0;

    // Full NEC AGC burst: 9ms mark, 4.5ms space, 560us mark, 560us space.
    // 4 pulses satisfies transmitRaw() minimum length guard (len >= 4).
    // This fires the real ESP32 hardware ledc PWM at the requested freqKHz.
    // Use an oscilloscope or IR receiver on the TX pin to verify output.
    static const uint16_t testPulses[] = { 9000, 4500, 560, 560 };
    const size_t numPulses = sizeof(testPulses) / sizeof(testPulses[0]);

    // transmitRaw: pauses IR receiver, fires all active emitters via
    // IRremoteESP8266 sendRaw() which uses ESP32 ledc hardware PWM timer.
    // Returns false only if no emitters are configured.
    bool ok = irTransmitter.transmitRaw(testPulses, numPulses, freqKHz);

    // Build response
    JsonDocument resp;
    resp["ok"]          = ok;
    resp["freqKHz"]     = freqKHz;
    resp["freqHz"]      = (uint32_t)(freqKHz * 1000UL);
    resp["pulseCount"]  = (int)numPulses;
    resp["durationUs"]  = 9000 + 4500 + 560 + 560;  // total burst time
    JsonArray pulses    = resp["pulsesUs"].to<JsonArray>();
    for (size_t i = 0; i < numPulses; i++) pulses.add(testPulses[i]);
    resp["note"] = ok
        ? String("NEC AGC burst transmitted at ") + freqKHz +
          " kHz carrier via ESP32 ledc hardware PWM (" +
          (int)irTransmitter.activeCount() + " emitter(s) active)"
        : "No active emitters - configure TX GPIO in Settings -> GPIO first";
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(resp, out);
    sendJson(req, ok ? 200 : 500, out);
}

// ── GET /api/v1/ir/pwm-test?freq=38&duty=33 ──────────────────
// GET alias for the POST pwm-test handler.
// Reads freq (kHz) from query param; duty param is accepted but ignored
// (ESP32 ledc hardware uses fixed ~50% duty for IR which is optimal).
void WebUI::handlePwmTestGet(AsyncWebServerRequest* req) {
    uint16_t freqKHz = IR_DEFAULT_FREQ_KHZ;
    if (req->hasParam("freq")) {
        int f = req->getParam("freq")->value().toInt();
        if (f >= 20 && f <= 60) freqKHz = (uint16_t)f;
    }
    static const uint16_t testPulses[] = { 9000, 4500, 560, 560 };
    const size_t numPulses = sizeof(testPulses) / sizeof(testPulses[0]);
    bool ok = irTransmitter.transmitRaw(testPulses, numPulses, freqKHz);
    JsonDocument resp;
    resp["ok"]         = ok;
    resp["freqKHz"]    = freqKHz;
    resp["pulseCount"] = (int)numPulses;
    resp["durationUs"] = 9000 + 4500 + 560 + 560;
    resp["note"] = ok
        ? String("NEC AGC burst at ") + freqKHz + " kHz"
        : "No active emitters";
    String out;
    out.reserve(256);
    serializeJson(resp, out);
    sendJson(req, ok ? 200 : 500, out);
}

// ── PWM capabilities info ─────────────────────────────────────
// Returns valid carrier frequency range and current emitter config.
// UI uses this to build the frequency selector with correct bounds.
void WebUI::handlePwmInfo(AsyncWebServerRequest* req) {
    JsonDocument doc;
    // All values are real ESP32 hardware constants / runtime readings.
    // IRremoteESP8266 uses ESP32 ledc peripheral for carrier generation:
    //   - ledc channel per emitter (up to 8 channels on ESP32)
    //   - 8-bit duty resolution -> 128/255 = ~50% duty cycle for IR
    //   - Carrier frequency = freqKHz * 1000 Hz set via ledcSetup()
    doc["freqKhzDefault"]    = (uint16_t)IR_DEFAULT_FREQ_KHZ;  // from config.h = 38
    doc["freqKhzMin"]        = 20;   // ESP32 ledc lower bound for IR use
    doc["freqKhzMax"]        = 60;   // ESP32 ledc upper bound for IR use
    doc["freqKhzCommon"]     = "36, 38, 40, 56";
    doc["pwmResolutionBits"] = 8;    // IRremoteESP8266 ledcSetup() resolution
    doc["pwmDutyCycle"]      = 128;  // 128/255 ≈ 50% - standard for IR emitters
    doc["pwmDriver"]         = "ESP32 ledc hardware (no bitbanging)";
    doc["cpuFreqMHz"]        = (uint32_t)getCpuFrequencyMhz();   // real runtime value
    doc["emitterCount"]      = (int)irTransmitter.activeCount();  // real active count

    // Real per-emitter GPIO list from irTransmitter runtime state
    JsonArray emitters = doc["emitters"].to<JsonArray>();
    auto pins = irTransmitter.activePins();
    for (size_t i = 0; i < pins.size(); i++) {
        JsonObject e = emitters.add<JsonObject>();
        e["idx"]    = (int)i;
        e["gpio"]   = (int)pins[i];
        e["active"] = true;
        // ledc channel = i (IRremoteESP8266 assigns channel per sender index)
        e["ledcChannel"] = (int)i;
    }
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

void WebUI::handleExport(AsyncWebServerRequest* req) {
    String json = irDB.exportJson();
    AsyncWebServerResponse* r = req->beginResponse(200,"application/json",json);
    r->addHeader("Content-Disposition","attachment; filename=\"ir_database.json\"");
    r->addHeader("Access-Control-Allow-Origin","*");
    req->send(r);
}

void WebUI::handleImport(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    String json((char*)d,l);
    if (!irDB.importJson(json))
        { sendJson(req,400,"{\"error\":\"Import failed\"}"); return; }
    JsonDocument r; r["ok"]=true; r["buttons"]=(int)irDB.size();
    sendJsonDoc(req,200,r);  // FIX: zero-copy stream serialize
}

void WebUI::handleGetConfig(AsyncWebServerRequest* req) {
    const WiFiConfig& cfg = wifiMgr.config();
    JsonDocument doc;
    doc["apSSID"]    = cfg.apSSID;
    doc["apPass"]    = cfg.apPass;
    doc["apChannel"] = cfg.apChannel;
    doc["apHidden"]  = cfg.apHidden;
    doc["staSSID"]   = cfg.staSSID;
    doc["staPass"]   = cfg.staPass;
    doc["staEnabled"]= cfg.staEnabled;
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req,200,out);
}

void WebUI::handleSetConfig(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    WiFiConfig& cfg = wifiMgr.config();
    if (doc["apSSID"].is<const char*>())   cfg.apSSID     = doc["apSSID"].as<String>();
    if (doc["apPass"].is<const char*>())   cfg.apPass     = doc["apPass"].as<String>();
    if (doc["apChannel"].is<int>()) {
        int ch = doc["apChannel"].as<int>();
        cfg.apChannel = (ch>=1 && ch<=13) ? (uint8_t)ch : 1;
    }
    if (doc["apHidden"].is<bool>())        cfg.apHidden   = doc["apHidden"].as<bool>();
    if (doc["staSSID"].is<const char*>())  cfg.staSSID    = doc["staSSID"].as<String>();
    if (doc["staPass"].is<const char*>())  cfg.staPass    = doc["staPass"].as<String>();
    if (doc["staEnabled"].is<bool>())      cfg.staEnabled = doc["staEnabled"].as<bool>();

    if (!wifiMgr.saveConfig())
        { sendJson(req,500,"{\"error\":\"Flash write failed\"}"); return; }

    // Apply STA changes immediately; AP changes need a restart
    bool staChanged = doc["staSSID"].is<const char*>() || doc["staEnabled"].is<bool>() || doc["staPass"].is<const char*>();
    if (staChanged) {
        wifiMgr.applyStaConfig();
        sendJson(req,200,"{\"ok\":true,\"note\":\"STA reconnecting - AP stays up\"}");
    } else {
        sendJson(req,200,"{\"ok\":true,\"note\":\"AP settings saved - restart to apply\"}");
    }
}

void WebUI::handleGetStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;

    // ── Memory Dashboard (FIX: use IDF APIs for accuracy) ────
    uint32_t freeHeap  = esp_get_free_heap_size();
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t minEver   = esp_get_minimum_free_heap_size();
    uint32_t maxBlock  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint8_t  fragPct   = (freeHeap > 0) ? (uint8_t)(100u - (maxBlock * 100u / freeHeap)) : 0;

    doc["freeHeap"]      = freeHeap;
    doc["totalHeap"]     = totalHeap;
    doc["minFreeHeap"]   = minEver;       // lowest heap ever (proxy for worst fragmentation)
    doc["maxAllocBlock"] = maxBlock;      // largest single allocatable block right now
    doc["heapFrag"]      = fragPct;       // fragmentation % - 0=perfect, >40=degraded
    doc["heapUsed"]      = totalHeap - freeHeap;
    doc["heapUsedPct"]   = (uint8_t)((totalHeap - freeHeap) * 100u / totalHeap);

    // Flash / LittleFS
    doc["flashSize"]     = (uint32_t)(ESP.getFlashChipSize() / 1024);

    // CPU
    doc["cpuFreqMHz"]    = getCpuFrequencyMhz();
    doc["coreId"]        = xPortGetCoreID();

    // Task watermarks (stack health monitoring)
    // uxTaskGetStackHighWaterMark returns words remaining - multiply by 4 for bytes
    doc["loopStackFree"] = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * 4);

    // Wi-Fi
    doc["apIP"]          = wifiMgr.apIP();
    doc["apActive"]      = wifiMgr.apActive();
    doc["staIP"]         = wifiMgr.staIP();
    doc["staConnected"]  = wifiMgr.staConnected();
    doc["staRSSI"]       = wifiMgr.staRSSI();
    doc["staSSID"]       = wifiMgr.staSSID();
    doc["staStatus"]     = wifiMgr.staStatus();
    doc["apClients"]     = (int)WiFi.softAPgetStationNum();
    doc["mdnsHostname"]  = wifiMgr.mdnsActive()
                           ? String(MDNS_HOSTNAME) + ".local"
                           : String("");
    // System
    doc["uptime"]        = (uint32_t)(millis() / 1000);
    doc["firmware"]      = FIRMWARE_VERSION;
    doc["chip"]          = ESP.getChipModel();
    doc["chipRev"]       = (int)ESP.getChipRevision();
    // IR
    doc["buttons"]       = (int)irDB.size();
    doc["groups"]        = (int)groupMgr.size();
    doc["schedules"]     = (int)scheduler.size();
    doc["recvPin"]       = irReceiver.activePin();
    doc["emitters"]      = irTransmitter.activeCount();
    // NTP
    doc["ntpSynced"]     = scheduler.ntpSynced();
    doc["currentTime"]   = scheduler.ntpSynced() ? scheduler.currentTimeStr() : "N/A";
    doc["currentDate"]   = scheduler.ntpSynced() ? scheduler.currentDateStr() : "N/A";
    // Fix 1 + Fix 3: auto-save state and RAW count
    doc["autoSave"]      = irDB.autoSaveEnabled();
    doc["rawButtons"]    = (int)irDB.rawCount();
    doc["maxRawButtons"] = MAX_RAW_BUTTONS;
    // SD card status
    doc["sdMounted"]     = sdMgr.isAvailable();
    if (sdMgr.isAvailable()) {
        SdStatus ss = sdMgr.status();
        doc["sdCardType"]  = ss.cardTypeStr;
        doc["sdTotalKB"]   = (uint32_t)(ss.totalBytes / 1024);
        doc["sdUsedKB"]    = (uint32_t)(ss.usedBytes  / 1024);
        doc["sdMacro"]     = sdMgr.isMacroRunning();
    }
    // Internal macro status (v2.2.0)
    doc["macroRunning"]  = macroMgr.isRunning();
    doc["macroName"]     = macroMgr.runningName();
    doc["macroCount"]    = (int)macroMgr.list().size();
    doc["sketchSizeKB"]  = (uint32_t)(ESP.getSketchSize() / 1024);
    // Watchdog / system health (real sensor readings)
    doc["cpuTempC"]      = (float)((int)(wdtMgr.cpuTemperature() * 10)) / 10.0f;
    doc["heapMin"]       = wdtMgr.minHeapSeen();
    doc["internetOk"]    = wdtMgr.internetReachable();
    doc["safeMode"]      = wdtMgr.isSafeMode();
    doc["bootFailCount"] = (int)wdtMgr.bootFailCount();
    doc["perfMode"]      = wdtMgr.perfModeStr();

    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req,200,out);
}

void WebUI::handleRestart(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    sendJson(req, 200, "{\"ok\":true}");
    // FIX: protect s_restartAt write with spinlock (same mux as main.cpp)
    extern portMUX_TYPE s_restartMux;
    extern volatile uint32_t s_restartAt;
    taskENTER_CRITICAL(&s_restartMux);
    s_restartAt = (uint32_t)(millis() + 400);
    taskEXIT_CRITICAL(&s_restartMux);
}

// ── Groups ────────────────────────────────────────────────────
void WebUI::handleGetGroups(AsyncWebServerRequest* req) {
    sendJson(req, 200, groupMgr.toJson());
}

void WebUI::handleAddGroup(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    String name = doc["name"] | (const char*)"";
    String icon = doc["icon"] | (const char*)"📺";
    uint32_t id = groupMgr.add(name, icon);
    if (!id) { sendJson(req,400,"{\"error\":\"Invalid name or group limit reached\"}"); return; }
    JsonDocument r; r["ok"]=true; r["id"]=id;
    sendJsonDoc(req,200,r);  // FIX: zero-copy stream serialize
}

void WebUI::handleUpdateGroup(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    uint32_t id = doc["id"] | (uint32_t)0;
    String name = doc["name"] | (const char*)"";
    String icon = doc["icon"] | (const char*)"📺";
    if (!id || !groupMgr.update(id, name, icon))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    sendJson(req,200,"{\"ok\":true}");
}

void WebUI::handleDeleteGroup(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!req->hasParam("id"))
        { sendJson(req,400,"{\"error\":\"Missing id\"}"); return; }
    uint32_t id = (uint32_t)req->getParam("id")->value().toInt();
    if (!groupMgr.remove(id))
        { sendJson(req,400,"{\"error\":\"Cannot remove last group or not found\"}"); return; }

    // Reassign buttons in the deleted group to ungrouped (groupId=0).
    // Use compactJson snapshot to avoid holding a raw reference to the
    // internal vector across irDB.update() calls (which can reallocate it).
    std::vector<uint32_t> toFix;
    {
        // Collect IDs via findById-safe iteration: take a snapshot first.
        // irDB.buttons() returns a const ref - safe to read here since all
        // DB mutations happen on the same async-TCP task, but we still
        // collect IDs before calling update() to avoid iterator invalidation.
        const auto& all = irDB.buttons();
        for (const auto& b : all)
            if (b.groupId == id) toFix.push_back(b.id);
    }
    for (uint32_t bid : toFix) {
        IRButton copy = irDB.findById(bid);
        if (copy.id) { copy.groupId = 0; irDB.update(copy.id, copy); }
    }
    if (!toFix.empty())
        Serial.printf(DEBUG_TAG " Group %u deleted: %u button(s) ungrouped\n",
                      id, (unsigned)toFix.size());

    sendJson(req,200,"{\"ok\":true}");
}

void WebUI::handleReorderGroup(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    // Frontend sends {order: [id1, id2, ...]} — full ordered array of group IDs
    if (doc["order"].is<JsonArrayConst>()) {
        std::vector<uint32_t> ids;
        for (JsonVariantConst v : doc["order"].as<JsonArrayConst>())
            ids.push_back(v.as<uint32_t>());
        if (!groupMgr.reorderAll(ids))
            { sendJson(req,400,"{\"error\":\"Reorder failed\"}"); return; }
        sendJson(req,200,"{\"ok\":true}");
        return;
    }
    // Legacy single-item form: {id, order}
    uint32_t id  = doc["id"]    | (uint32_t)0;
    uint8_t  ord = doc["order"] | (uint8_t)0;
    if (!groupMgr.reorder(id, ord))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    sendJson(req,200,"{\"ok\":true}");
}

// ── Scheduler ────────────────────────────────────────────────
void WebUI::handleGetSchedules(AsyncWebServerRequest* req) {
    sendJson(req, 200, scheduler.toJson());
}

void WebUI::handleAddSchedule(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    ScheduleEntry e;
    if (!e.fromJson(doc.as<JsonObjectConst>()))
        { sendJson(req,400,"{\"error\":\"Invalid schedule (buttonId required)\"}"); return; }
    uint32_t id = scheduler.addEntry(e);
    if (!id) { sendJson(req,400,"{\"error\":\"Schedule limit reached\"}"); return; }
    JsonDocument r; r["ok"]=true; r["id"]=id;
    sendJsonDoc(req,200,r);  // FIX: zero-copy stream serialize
}

void WebUI::handleUpdateSchedule(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    ScheduleEntry e;
    if (!e.fromJson(doc.as<JsonObjectConst>()) || !e.id)
        { sendJson(req,400,"{\"error\":\"Invalid data\"}"); return; }
    if (!scheduler.updateEntry(e))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    sendJson(req,200,"{\"ok\":true}");
}

void WebUI::handleDeleteSchedule(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!req->hasParam("id"))
        { sendJson(req,400,"{\"error\":\"Missing id\"}"); return; }
    uint32_t id = (uint32_t)req->getParam("id")->value().toInt();
    if (!scheduler.removeEntry(id))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    sendJson(req,200,"{\"ok\":true}");
}

void WebUI::handleToggleSchedule(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    uint32_t id  = doc["id"]      | (uint32_t)0;
    bool     en  = doc["enabled"] | true;
    if (!scheduler.setEnabled(id, en))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    sendJson(req,200,"{\"ok\":true}");
}

void WebUI::handleGetNtpStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["synced"]   = scheduler.ntpSynced();
    doc["time"]     = scheduler.ntpSynced() ? scheduler.currentTimeStr() : "";
    doc["date"]     = scheduler.ntpSynced() ? scheduler.currentDateStr() : "";
    doc["tzOffset"] = scheduler.tzOffsetSec();
    doc["dstOffset"]= scheduler.dstOffsetSec();
    sendJsonDoc(req,200,doc);  // FIX: zero-copy stream serialize
}

void WebUI::handleSetTimezone(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }
    long tz  = doc["tzOffset"]  | 0L;
    long dst = doc["dstOffset"] | 0L;
    scheduler.setTimezone(tz, dst);
    sendJson(req,200,"{\"ok\":true}");
}

// ── Wi-Fi scan ────────────────────────────────────────────────
void WebUI::handleStartScan(AsyncWebServerRequest* req) {
    wifiMgr.startScan();
    sendJson(req,200,"{\"ok\":true,\"note\":\"Scan started\"}");
}

void WebUI::handleScanResults(AsyncWebServerRequest* req) {
    sendJson(req,200, wifiMgr.scanResultsJson());
}

// ── Fix 1: Auto-save handlers ────────────────────────────────
void WebUI::handleGetAutoSave(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["autoSave"]      = irDB.autoSaveEnabled();
    doc["rawButtons"]    = (int)irDB.rawCount();
    doc["maxRawButtons"] = MAX_RAW_BUTTONS;
    doc["lazyDelayMs"]   = (uint32_t)DB_LAZY_SAVE_MS;
    doc["dirty"]         = irDB.isDirty();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

void WebUI::handleSetAutoSave(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!req->hasParam("enabled")) {
        sendJson(req, 400, "{\"error\":\"Missing 'enabled' param - use ?enabled=true or ?enabled=false\"}");
        return;
    }
    String val = req->getParam("enabled")->value();
    bool en = (val == "1" || val == "true");
    irDB.setAutoSave(en);
    JsonDocument doc;
    doc["ok"]       = true;
    doc["autoSave"] = irDB.autoSaveEnabled();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── Backup & Restore handlers ────────────────────────────────

// POST /api/backup
// Creates /ir_database_backup.json from the current live DB.
void WebUI::handleBackupCreate(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!irDB.backup()) {
        sendJson(req, 500, "{\"error\":\"Backup write failed\"}");
        return;
    }
    JsonDocument doc;
    doc["ok"]      = true;
    doc["file"]    = DB_BACKUP_FILE;
    doc["buttons"] = (int)irDB.size();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// GET /api/backup
// Downloads the backup file as an attachment.
void WebUI::handleBackupDownload(AsyncWebServerRequest* req) {
    if (!irDB.hasBackup()) {
        sendJson(req, 404, "{\"error\":\"No backup found - POST /api/backup first\"}");
        return;
    }
    // Stream directly from LittleFS - no String copy in RAM
    AsyncWebServerResponse* r = req->beginResponse(
        LittleFS, DB_BACKUP_FILE, "application/json");
    r->addHeader("Content-Disposition",
                 "attachment; filename=\"ir_database_backup.json\"");
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

// GET /api/backup/status
// Returns metadata: exists, size, button count estimate.
void WebUI::handleBackupStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    bool exists = irDB.hasBackup();
    doc["exists"] = exists;
    if (exists) {
        File f = LittleFS.open(DB_BACKUP_FILE, "r");
        if (f) {
            doc["sizeBytes"] = (uint32_t)f.size();
            f.close();
        }
    }
    doc["liveButtons"]  = (int)irDB.size();
    doc["maxButtons"]   = MAX_BUTTONS;
    doc["maxRawButtons"]= MAX_RAW_BUTTONS;
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// POST /api/restore  (body: raw JSON of ir_database.json format)
// Full pipeline: validate -> backup current DB -> atomic swap.
void WebUI::handleRestore(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    // Size guard - reject oversized bodies before any processing
    if (l == 0) {
        sendJson(req, 400, "{\"error\":\"Empty body\"}");
        return;
    }
    if (l > DB_RESTORE_MAX_BYTES) {
        sendJson(req, 413, "{\"error\":\"File too large for restore\"}");
        return;
    }

    String json(reinterpret_cast<const char*>(d), l);

    // Full restore: validate -> backup -> atomic importJson
    IRDatabase::RestoreResult res = irDB.restore(json);

    JsonDocument doc;
    doc["ok"]       = res.ok;
    doc["accepted"] = res.accepted;
    doc["rejected"] = res.rejected;
    if (!res.error.isEmpty()) doc["note"] = res.error;

    if (res.ok) {
        doc["buttons"] = (int)irDB.size();
        String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
        serializeJson(doc, out);
        sendJson(req, 200, out);
    } else {
        String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
        serializeJson(doc, out);
        sendJson(req, 400, out);
    }
}

// ── Broadcasts ────────────────────────────────────────────────
void WebUI::broadcastIREvent(const IRButton& btn) {
    if (_ws.count() == 0) return;   // fast exit - no clients
    JsonDocument doc;
    doc["event"]    = "ir_received";
    doc["protocol"] = protocolName(btn.protocol);
    doc["bits"]     = btn.bits;
    doc["name"]     = btn.name;
    doc["freqKHz"]  = btn.freqKHz;
    char hex[20];
    snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)btn.code);
    doc["code"] = hex;
    if (btn.protocol == IRProtocol::RAW && !btn.rawData.empty()) {
        JsonArray a = doc["rawData"].to<JsonArray>();
        for (uint16_t v : btn.rawData) a.add(v);
    }
    // FIX: reserve expected size to avoid String realloc during serialization.
    // IR event JSON is ~120 bytes for decoded, ~800 bytes for RAW (50 samples).
    String msg;
    msg.reserve(btn.rawData.empty() ? 128 : 896);
    serializeJson(doc, msg);
    _pushWsMessage(msg);
}

void WebUI::broadcastMessage(const String& text) {
    if (_ws.count() == 0) return;
    // FIX: Route through _pushWsMessage() instead of calling _ws.textAll() directly.
    // textAll() is not thread-safe from non-loop() contexts (e.g., rule engine callbacks
    // firing from hw_poll task or other task contexts.
    // _pushWsMessage uses a mutex-protected queue and is safe from any task.
    //
    // JSON ESCAPE FIX: use ArduinoJson to serialize the message string so that
    // button names containing '"' or '\' (e.g. Auto-saved: My "TV" remote) do not
    // inject into the JSON frame, producing malformed JSON on WebSocket clients.
    JsonDocument _msgDoc;
    _msgDoc["event"]   = "message";
    _msgDoc["message"] = text;
    String _msgStr;
    _msgStr.reserve(text.length() + 40);
    serializeJson(_msgDoc, _msgStr);
    _pushWsMessage(_msgStr);
}



void WebUI::broadcastBinary(const uint8_t* data, size_t len) {
    if (_ws.count() == 0 || !data || len == 0) return;
    // Use the SEND mutex (not the queue mutex) so this is mutually exclusive
    // with _flushWsQueue()'s textAll() on the other core.
    if (!_wsSendMutex || xSemaphoreTake(_wsSendMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    _ws.binaryAll(const_cast<uint8_t*>(data), len);
    xSemaphoreGive(_wsSendMutex);
}

// Send pre-serialized JSON directly to all WS clients via the safe push queue.
// Use for one-off structured events (e.g. scheduled_tx) without JsonDocument overhead.
void WebUI::broadcastRaw(const char* json) {
    if (!json || _ws.count() == 0) return;
    _pushWsMessage(String(json));
}

void WebUI::broadcastStatus() {
    // FIX-1: skip entirely if no clients - avoids JsonDocument + String alloc
    // for nobody. Queue drain already happens in _flushWsQueue() when count==0.
    if (_ws.count() == 0) return;

    // FIX-3: heap guard - skip this tick if free heap is dangerously low.
    // JsonDocument's internal pool plus serializeJson() can push us over the
    // edge under fragmentation pressure, leading to a silently truncated
    // status frame or an alloc failure mid-build. Skipping is safe: the next
    // 5s tick will retry, and _statusDirty preserves the "needs send" bit.
    if (ESP.getFreeHeap() < 16384 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 8192) {
        _statusDirty = true; return;
    }

    // FIX-2: delta suppression - only serialize and broadcast when something
    // actually changed. Saves ~15 function calls + JsonDocument every 5s.
    bool staConn  = wifiMgr.staConnected();
    int32_t rssi  = wifiMgr.staRSSI();
    bool ntpOk    = scheduler.ntpSynced();
    bool sdMnt    = sdMgr.isAvailable();
    bool macroRun = macroMgr.isRunning();
    uint32_t heap = ESP.getFreeHeap();

    bool changed = _statusDirty
        || (staConn  != _lastStatus.staConnected)
        || (rssi     != _lastStatus.rssi)
        || (ntpOk    != _lastStatus.ntpSynced)
        || (sdMnt    != _lastStatus.sdMounted)
        || (macroRun != _lastStatus.macroRunning)
        || (heap     <  _lastStatus.heap - 4096)   // only if heap dropped >4KB
        || (heap     >  _lastStatus.heap + 4096);

    if (!changed) return;

    _lastStatus = { staConn, rssi, ntpOk, sdMnt, macroRun, heap };
    _statusDirty = false;

    // FIX: use IDF APIs - more reliable than Arduino's ESP.getFreeHeap()
    uint32_t maxBlock   = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t minEver    = esp_get_minimum_free_heap_size();
    uint8_t  fragPct    = (heap > 0) ? (uint8_t)(100u - (maxBlock * 100u / heap)) : 0;

    JsonDocument doc;
    doc["event"]        = "status";
    doc["heap"]         = heap;
    doc["heapMin"]      = minEver;       // lowest heap ever seen since boot
    doc["heapBlock"]    = maxBlock;      // largest contiguous free block
    doc["heapFrag"]     = fragPct;       // fragmentation % (0=perfect, 100=fully fragmented)
    doc["uptime"]       = (uint32_t)(millis()/1000);
    doc["staConnected"] = staConn;
    doc["staRSSI"]      = rssi;
    doc["staSSID"]      = wifiMgr.staSSID();
    doc["staStatus"]    = wifiMgr.staStatus();
    doc["staIP"]        = wifiMgr.staIP();
    doc["apActive"]     = wifiMgr.apActive();
    doc["apIP"]         = wifiMgr.apIP();
    doc["mdnsHostname"] = wifiMgr.mdnsActive()
                          ? String(MDNS_HOSTNAME) + ".local"
                          : String("");
    doc["ntpSynced"]    = ntpOk;
    doc["time"]         = ntpOk ? scheduler.currentTimeStr() : "";
    doc["sdMounted"]    = sdMnt;
    doc["sdMacro"]      = sdMgr.isMacroRunning();
    doc["macroRunning"] = macroRun;
    doc["macroName"]    = macroMgr.runningName();
    doc["macroStep"]    = macroMgr.runStep();
    doc["macroTotal"]   = macroMgr.runTotal();
    // P-03 FIX: serialize directly into a static char buffer - zero heap allocation.
    // broadcastStatus() is called up to 20x/minute; using String alloc/free 864K
    // times per 30 days was the leading cause of heap fragmentation on this device.
    // Static buffer is safe: _pushWsMessage() copies into the WS queue String,
    // and broadcastStatus() is only ever called from loop() (single task on Core 1).
    static char statusBuf[640];
    size_t n = serializeJson(doc, statusBuf, sizeof(statusBuf));
    if (n > 0 && n < sizeof(statusBuf)) {
        _pushWsMessage(String(statusBuf));
    }
}

// Push a pre-serialized JSON string to the WS queue without wrapping.
// Safe to call from any FreeRTOS task - uses the same mutex-protected queue.
void WebUI::broadcastRaw(const String& json) {
    _pushWsMessage(json);
}

// ── GPIO handlers ─────────────────────────────────────────────
void WebUI::handleGetGpioPins(AsyncWebServerRequest* req) {
    IrPinConfig pins;
    wifiMgr.loadIrPins(pins);
    JsonDocument doc;
    doc["recvPin"]   = irReceiver.activePin();
    doc["emitCount"] = irTransmitter.activeCount();
    JsonArray ea = doc["emitters"].to<JsonArray>();
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        JsonObject o = ea.add<JsonObject>();
        o["idx"]     = i;
        o["pin"]     = pins.emitPin[i];
        o["enabled"] = pins.emitEnabled[i];
    }
    sendJsonDoc(req,200,doc);  // FIX: zero-copy stream serialize
}

void WebUI::handleSetGpioPins(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc,d,l)!=DeserializationError::Ok)
        { sendJson(req,400,"{\"error\":\"JSON parse failed\"}"); return; }

    IrPinConfig pins;
    wifiMgr.loadIrPins(pins);
    IrPinConfig proposed = pins;

    JsonDocument respDoc;
    JsonArray warnings = respDoc["warnings"].to<JsonArray>();
    bool anyError = false;
    bool changed  = false;

    if (doc["recvPin"].is<int>()) {
        uint8_t pin = doc["recvPin"].as<uint8_t>();
        uint8_t excl[IR_MAX_EMITTERS]; uint8_t exclN=0;
        for (uint8_t i=0;i<IR_MAX_EMITTERS;++i)
            if (proposed.emitEnabled[i]) excl[exclN++]=proposed.emitPin[i];
        PinStatus st = validateRxPin(pin, excl, exclN);
        if (st!=PinStatus::OK && st!=PinStatus::OK_RX_ONLY) {
            warnings.add(String("RX GPIO")+pin+" rejected: "+pinStatusMsg(st));
            anyError=true;
        } else if (pin != proposed.recvPin) {
            proposed.recvPin=pin; changed=true;
        }
    }

    if (doc["emitters"].is<JsonArrayConst>()) {
        for (JsonObjectConst o : doc["emitters"].as<JsonArrayConst>()) {
            uint8_t idx = o["idx"] | (uint8_t)255;
            if (idx>=IR_MAX_EMITTERS) continue;
            bool    enabled = o["enabled"] | proposed.emitEnabled[idx];
            uint8_t pin     = o["pin"]     | proposed.emitPin[idx];
            if (enabled) {
                uint8_t excl[IR_MAX_EMITTERS+1]; uint8_t exclN=0;
                excl[exclN++]=proposed.recvPin;
                for (uint8_t j=0;j<IR_MAX_EMITTERS;++j)
                    if (j!=idx && proposed.emitEnabled[j]) excl[exclN++]=proposed.emitPin[j];
                PinStatus st = validateTxPin(pin,excl,exclN);
                if (st!=PinStatus::OK) {
                    warnings.add(String("Emitter[")+idx+"] GPIO"+pin+" rejected: "+pinStatusMsg(st));
                    enabled=false;
                }
            }
            if (pin!=proposed.emitPin[idx]||enabled!=proposed.emitEnabled[idx]) {
                proposed.emitPin[idx]=pin; proposed.emitEnabled[idx]=enabled; changed=true;
            }
        }
        if (doc["emitCount"].is<int>()) {
            uint8_t ec=doc["emitCount"].as<uint8_t>();
            proposed.emitCount=min(ec,(uint8_t)IR_MAX_EMITTERS);
        }
    }

    // Apply all changes together - outside the emitters block so a
    // RX-only change (no "emitters" key) is also committed.
    if (changed && !anyError) {
        pins = proposed;

        // Apply new RX pin to the live receiver
        if (pins.recvPin != irReceiver.activePin()) {
            if (!irReceiver.changePin(pins.recvPin)) {
                warnings.add(String("RX GPIO") + pins.recvPin + " changePin failed at runtime");
            }
        }

        // Reconfigure TX emitters
        irTransmitter.reconfigure(pins);
    }

    if (!wifiMgr.saveIrPins(pins))
        warnings.add("Warning: pin config could not be saved to flash");

    respDoc["ok"]            = !anyError;
    respDoc["activeRecvPin"] = irReceiver.activePin();
    respDoc["activeEmitters"]= irTransmitter.activeCount();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(respDoc, out);
    sendJson(req, anyError?207:200, out);
}

void WebUI::handleGetPinList(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray pins = doc["pins"].to<JsonArray>();
    struct PinMeta { uint8_t gpio; const char* label; bool txOk; bool rxOk; const char* note; };
    static const PinMeta META[] = {
        {4,  "GPIO4",  true,  true,  "General purpose"},
        {13, "GPIO13", true,  true,  "General purpose"},
        {14, "GPIO14", true,  true,  "Default RX - PWM at boot"},
        {16, "GPIO16", true,  true,  "General purpose"},
        {17, "GPIO17", true,  true,  "General purpose"},
        {18, "GPIO18", true,  true,  "SPI CLK (free if SPI unused)"},
        {19, "GPIO19", true,  true,  "SPI MISO (free if SPI unused)"},
        {21, "GPIO21", true,  true,  "I2C SDA (free if I2C unused)"},
        {22, "GPIO22", true,  true,  "I2C SCL (free if I2C unused)"},
        {23, "GPIO23", true,  true,  "SPI MOSI (free if SPI unused)"},
        {25, "GPIO25", true,  true,  "DAC1 - general purpose"},
        {26, "GPIO26", true,  true,  "DAC2 - general purpose"},
        {27, "GPIO27", true,  true,  "Default TX - general purpose"},
        {32, "GPIO32", true,  true,  "General purpose"},
        {33, "GPIO33", true,  true,  "General purpose"},
        {34, "GPIO34", false, true,  "Input-only - RX only"},
        {35, "GPIO35", false, true,  "Input-only - RX only"},
        {36, "GPIO36", false, true,  "Input-only (SVP) - RX only"},
        {39, "GPIO39", false, true,  "Input-only (SVN) - RX only"},
    };
    uint8_t curRx = irReceiver.activePin();
    for (const auto& m : META) {
        JsonObject o = pins.add<JsonObject>();
        o["gpio"]    = m.gpio;
        o["label"]   = m.label;
        o["txOk"]    = m.txOk;
        o["rxOk"]    = m.rxOk;
        o["note"]    = m.note;
        o["inUseRx"] = (m.gpio == curRx);
        bool inUseTx = false;
        for (uint8_t i=0;i<IR_MAX_EMITTERS;++i)
            if (irTransmitter.emitterPin(i)==m.gpio) { inUseTx=true; break; }
        o["inUseTx"] = inUseTx;
    }
    doc["maxEmitters"]  = IR_MAX_EMITTERS;
    doc["maxReceivers"] = 1;
    sendJsonDoc(req,200,doc);  // FIX: zero-copy stream serialize
}

// ============================================================
//  SD Card API Routes
//  All routes return {"error":"SD not available"} gracefully
//  when no SD card is inserted - no crashes, no broken UI.
// ============================================================

// ── Inline helpers ────────────────────────────────────────────
static void sdNotAvail(AsyncWebServerRequest* req) {
    sendJson(req, 503, "{\"error\":\"SD not available\"}");
}

// ── Route Registration ────────────────────────────────────────
// ── Internal LittleFS Macro routes (v2.2.0) ───────────────────
void WebUI::setupMacroRoutes() {
    _server.on("/api/macros", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleMacroList(req); });
    _server.on("/api/macro", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleMacroRead(req); });
    POST_BODY("/api/macro",
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l)
            { handleMacroSave(req, d, l); });
    _server.on("/api/macro/delete", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleMacroDelete(req); });
    _server.on("/api/macro/run", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleMacroRun(req); });
    _server.on("/api/macro/abort", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleMacroAbort(req); });
    _server.on("/api/macro/status", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleMacroStatus(req); });
}

// GET /api/macros - list all internal macros
void WebUI::handleMacroList(AsyncWebServerRequest* req) {
    auto list = macroMgr.list();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& m : list) {
        JsonObject o = arr.add<JsonObject>();
        o["name"]      = m.name;
        o["label"]     = m.label;
        o["stepCount"] = m.stepCount;
    }
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// GET /api/macro?name=x - read macro JSON
void WebUI::handleMacroRead(AsyncWebServerRequest* req) {
    if (!req->hasParam("name"))
        { sendJson(req,400,"{\"error\":\"Missing name\"}"); return; }
    String name = req->getParam("name")->value();
    String label;
    std::vector<MacroInternalStep> steps;
    if (!macroMgr.load(name, label, steps))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    JsonDocument doc;
    doc["name"] = label;
    JsonArray arr = doc["steps"].to<JsonArray>();
    for (const auto& s : steps) {
        JsonObject o = arr.add<JsonObject>();
        o["buttonId"]     = s.buttonId;
        o["delayAfterMs"] = s.delayAfterMs;
    }
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// POST /api/macro?name=x  body=JSON - save macro
void WebUI::handleMacroSave(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!req->hasParam("name"))
        { sendJson(req,400,"{\"error\":\"Missing name param\"}"); return; }
    String name = req->getParam("name")->value();
    String err = macroMgr.save(name, d, l);
    if (!err.isEmpty()) {
        JsonDocument r; r["error"] = err;
        String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
        serializeJson(r, out);
        sendJson(req, 400, out);
        return;
    }
    JsonDocument r; r["ok"] = true; r["name"] = name;
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// GET /api/macro/delete?name=x
void WebUI::handleMacroDelete(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!req->hasParam("name"))
        { sendJson(req,400,"{\"error\":\"Missing name\"}"); return; }
    String name = req->getParam("name")->value();
    if (!macroMgr.remove(name))
        { sendJson(req,404,"{\"error\":\"Not found\"}"); return; }
    sendJson(req, 200, "{\"ok\":true}");
}

// GET /api/macro/run?name=x
void WebUI::handleMacroRun(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!req->hasParam("name"))
        { sendJson(req,400,"{\"error\":\"Missing name\"}"); return; }
    if (macroMgr.isRunning())
        { sendJson(req,409,"{\"error\":\"Macro already running\"}"); return; }
    String name = req->getParam("name")->value();
    if (!macroMgr.run(name))
        { sendJson(req,404,"{\"error\":\"Not found or invalid\"}"); return; }
    JsonDocument r;
    r["ok"]   = true; r["name"] = name;
    r["steps"]= macroMgr.runTotal();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// POST /api/macro/abort
void WebUI::handleMacroAbort(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    macroMgr.abort();
    sendJson(req, 200, "{\"ok\":true}");
}

// GET /api/macro/status
void WebUI::handleMacroStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["running"]  = macroMgr.isRunning();
    doc["name"]     = macroMgr.runningName();
    doc["step"]     = macroMgr.runStep();
    doc["total"]    = macroMgr.runTotal();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

void WebUI::setupSdRoutes() {
    // Status
    _server.on("/api/sd/status", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdStatus(req); });

    // File manager
    _server.on("/api/sd/ls", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdList(req); });

    _server.on("/api/sd/delete", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleSdDelete(req); });

    POST_BODY("/api/sd/rename",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdRename(req, d, l); }));

    POST_BODY("/api/sd/mkdir",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdMkdir(req, d, l); }));

    _server.on("/api/sd/download", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdDownload(req); });

    // SD upload (multipart file upload to SD)
    _server.on("/api/sd/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200, "{\"ok\":true}");
        },
        [this](AsyncWebServerRequest* req, const String& fn,
               size_t idx, uint8_t* data, size_t len, bool final) {
            handleSdUpload(req, fn, idx, data, len, final);
        });

    // Log
    _server.on("/api/sd/log", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdLog(req); });


    // Backup / Restore
    // SD encrypt removed — not implemented

    POST_BODY("/api/sd/backup",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdBackup(req, d, l); }));

    POST_BODY("/api/sd/restore",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdRestore(req, d, l); }));

    _server.on("/api/sd/backups", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdBackupList(req); });

    // Macros
    _server.on("/api/sd/macros", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdMacroList(req); });

    _server.on("/api/sd/macro/run", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdMacroRun(req); });

    _server.on("/api/sd/macro/status", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdMacroStatus(req); });

    // IR Library
    _server.on("/api/sd/irlibrary", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdIRLibList(req); });

    POST_BODY("/api/sd/irlibrary/export",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdIRLibExport(req, d, l); }));

    POST_BODY("/api/sd/irlibrary/import",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdIRLibImport(req, d, l); }));

    POST_BODY("/api/sd/irlibrary/merge",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdIRLibMerge(req, d, l); }));

    POST_BODY("/api/sd/irlibrary/delete",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdIRLibDelete(req, d, l); }));

    POST_BODY("/api/sd/irlibrary/rename",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdIRLibRename(req, d, l); }));

    POST_BODY("/api/sd/irlibrary/savebtn",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdIRLibSaveBtn(req, d, l); }));

    // Device profiles
    _server.on("/api/sd/devices", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdDeviceList(req); });

    _server.on("/api/sd/device", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdDeviceRead(req); });

    POST_BODY("/api/sd/device/save",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdDeviceSave(req, d, l); }));

    POST_BODY("/api/sd/device/delete",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdDeviceDelete(req, d, l); }));

    POST_BODY("/api/sd/device/import",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdDeviceImport(req, d, l); }));

    // ── Advanced File Manager routes ──────────────────────────
    POST_BODY("/api/sd/copy",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdCopy(req, d, l); }));

    POST_BODY("/api/sd/move",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdMove(req, d, l); }));

    _server.on("/api/sd/rmrf", HTTP_POST,
        [this](AsyncWebServerRequest* req) { handleSdDeleteRecursive(req); });

    _server.on("/api/sd/info", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdFileInfo(req); });

    _server.on("/api/sd/read", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleSdReadText(req); });

    POST_BODY("/api/sd/format",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleSdFormat(req, d, l); }));
}

// ── /api/sd/status ───────────────────────────────────────────
void WebUI::handleSdStatus(AsyncWebServerRequest* req) {
    SdStatus s = sdMgr.status();
    JsonDocument doc;
    doc["mounted"]    = s.mounted;
    doc["cardType"]   = s.cardTypeStr;
    doc["totalKB"]    = (uint32_t)(s.totalBytes / 1024);
    doc["usedKB"]     = (uint32_t)(s.usedBytes  / 1024);
    doc["freeKB"]     = (uint32_t)((s.totalBytes - s.usedBytes) / 1024);
    doc["macroRunning"] = sdMgr.isMacroRunning();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/ls ────────────────────────────────────────────────
void WebUI::handleSdList(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    String path = req->hasParam("path") ? req->getParam("path")->value() : "/";
    auto entries = sdMgr.listDir(path);
    JsonDocument doc;
    doc["path"] = path;
    JsonArray arr = doc["files"].to<JsonArray>();
    for (const auto& e : entries) {
        JsonObject o = arr.add<JsonObject>();
        o["name"]  = e.name;
        o["isDir"] = e.isDir;
        o["size"]  = (uint32_t)e.size;
    }
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/delete ────────────────────────────────────────────
void WebUI::handleSdDelete(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    if (!req->hasParam("path")) {
        sendJson(req, 400, "{\"error\":\"Missing path\"}"); return;
    }
    String path = req->getParam("path")->value();
    if (!sdMgr.deleteFile(path)) {
        sendJson(req, 404, "{\"error\":\"Delete failed or not found\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/rename ────────────────────────────────────────────
void WebUI::handleSdRename(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String from = doc["from"] | (const char*)"";
    String to   = doc["to"]   | (const char*)"";
    if (from.isEmpty() || to.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing from/to\"}"); return;
    }
    if (!sdMgr.renameFile(from, to)) {
        sendJson(req, 500, "{\"error\":\"Rename failed\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/mkdir ─────────────────────────────────────────────
void WebUI::handleSdMkdir(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String path = doc["path"] | (const char*)"";
    if (path.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing path\"}"); return;
    }
    if (!sdMgr.makeDir(path)) {
        sendJson(req, 500, "{\"error\":\"mkdir failed\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/download ─────────────────────────────────────────
// Streams SD file using beginResponse(Stream&, ...) which is the
// correct API for ESPAsyncWebServer 3.3.12 (mathieucarbou fork).
// beginChunkedResponse() does NOT exist in this version.
//
// A File is heap-allocated so it outlives the handler stack frame.
// ESPAsyncWebServer streams it asynchronously and calls onDisconnect
// (or the response destructor) when done - we delete the File there.
void WebUI::handleSdDownload(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    if (!req->hasParam("path")) {
        sendJson(req, 400, "{\"error\":\"Missing path\"}"); return;
    }
    String path = req->getParam("path")->value();
    if (!sdMgr.exists(path)) {
        sendJson(req, 404, "{\"error\":\"File not found\"}"); return;
    }

    // Heap-allocate File so it stays open after this stack frame exits.
    // ESPAsyncWebServer streams it from the async task; we clean up on disconnect.
    // RACE FIX: take VSPI bus mutex before SD.open() - same guard used by SdManager.
    extern SemaphoreHandle_t g_spi_vspi_mutex;
    bool _sdLocked = (g_spi_vspi_mutex &&
        xSemaphoreTakeRecursive(g_spi_vspi_mutex, pdMS_TO_TICKS(200)) == pdTRUE);
    File* fp = new File(SD.open(path.c_str(), FILE_READ));
    if (_sdLocked) xSemaphoreGiveRecursive(g_spi_vspi_mutex);
    if (!fp || !*fp) {
        delete fp;
        sendJson(req, 500, "{\"error\":\"Cannot open file\"}"); return;
    }

    size_t fileSize = fp->size();

    // Determine MIME type
    String mime = "application/octet-stream";
    if      (path.endsWith(".json"))                          mime = "application/json";
    else if (path.endsWith(".html"))                          mime = "text/html";
    else if (path.endsWith(".css"))                           mime = "text/css";
    else if (path.endsWith(".js"))                            mime = "application/javascript";
    else if (path.endsWith(".txt") || path.endsWith(".log")
          || path.endsWith(".csv"))                           mime = "text/plain";

    // Extract filename for Content-Disposition
    String fname = path;
    int slash = fname.lastIndexOf('/');
    if (slash >= 0) fname = fname.substring(slash + 1);

    // M-01 FIX: use a shared_ptr-style atomic flag so both onDisconnect and
    // the response-complete path close the File exactly once, regardless of
    // which fires first. ESPAsyncWebServer guarantees onDisconnect fires on TCP
    // close, but does NOT guarantee it fires after a normal HTTP/1.1 keep-alive
    // transfer completes without a disconnect. Without this, the File handle
    // leaks until the next TCP close - SD supports only ~5 concurrent handles.
    auto* closed = new (std::nothrow) volatile bool(false);
    auto closeOnce = [fp, closed]() {
        // Simple flag - only one path should fire, but guard just in case.
        if (closed && !*closed) {
            *closed = true;
            if (fp && *fp) fp->close();
            delete fp;
            delete const_cast<volatile bool*>(closed);
        }
    };

    // beginResponse(Stream&, contentType, size) - correct API for ESPAsyncWebServer 3.x
    // Passes the File by reference; the server reads it asynchronously.
    AsyncWebServerResponse* r = req->beginResponse(*fp, mime, fileSize);
    r->addHeader("Content-Disposition",
                 String("attachment; filename=\"") + fname + "\"");
    r->addHeader("Access-Control-Allow-Origin", "*");

    req->onDisconnect(closeOnce);
    req->send(r);
}

// ── /api/sd/upload ────────────────────────────────────────────
void WebUI::handleSdUpload(AsyncWebServerRequest* req, const String& filename,
                            size_t index, uint8_t* data, size_t len, bool final)
{
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) return;

    // Determine destination path
    String destDir = "/";
    if (req->hasParam("path")) destDir = req->getParam("path")->value();
    if (!destDir.endsWith("/")) destDir += "/";

    // Strip any path from filename - store in destDir only
    String fname = filename;
    int slash = fname.lastIndexOf('/');
    if (slash >= 0) fname = fname.substring(slash + 1);
    String destPath = destDir + fname;

    if (index == 0) {
        // First chunk - open file
        if (!sdMgr.beginUpload(destPath)) {
            Serial.printf(DEBUG_TAG " [SD-Upload] Cannot open: %s\n", destPath.c_str());
            return;
        }
        Serial.printf(DEBUG_TAG " [SD-Upload] Uploading to: %s\n", destPath.c_str());
    }

    if (!sdMgr.writeUploadChunk(data, len)) {
        Serial.println(DEBUG_TAG " [SD-Upload] Write error");
        sdMgr.abortUpload();
        return;
    }

    if (final) {
        sdMgr.endUpload();
        Serial.printf(DEBUG_TAG " [SD-Upload] Done: %s\n", destPath.c_str());
    }
}

// ── /api/sd/log ───────────────────────────────────────────────
void WebUI::handleSdLog(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    uint16_t lines = 50;
    if (req->hasParam("lines"))
        lines = (uint16_t)constrain(req->getParam("lines")->value().toInt(), 1, 500);
    String logText = sdMgr.tailLog(lines);
    AsyncWebServerResponse* r = req->beginResponse(200, "text/plain", logText);
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

// ── /api/sd/backup ────────────────────────────────────────────
void WebUI::handleSdBackup(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    String tag = "manual";
    if (l > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, d, l) == DeserializationError::Ok)
            tag = doc["tag"] | (const char*)"manual";
    }
    // Append timestamp to tag for uniqueness
    tag.replace("/", "_"); tag.replace(" ", "_");
    time_t now = time(nullptr);
    if (now > 1700000000UL) {
        struct tm tmbuf, *t = localtime_r(&now, &tmbuf);
        char ts[20];
        snprintf(ts, sizeof(ts), "_%04d%02d%02d_%02d%02d",
                 t->tm_year+1900, t->tm_mon+1, t->tm_mday,
                 t->tm_hour, t->tm_min);
        tag += ts;
    }
    bool ok = sdMgr.backupToSD(tag);
    if (!ok) { sendJson(req, 500, "{\"error\":\"Backup failed\"}"); return; }
    JsonDocument r;
    r["ok"]  = true;
    r["tag"] = tag;
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// ── /api/sd/restore ───────────────────────────────────────────
void WebUI::handleSdRestore(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String tag = doc["tag"] | (const char*)"";
    if (tag.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing tag\"}"); return;
    }
    bool ok = sdMgr.restoreFromSD(tag);
    if (!ok) { sendJson(req, 500, "{\"error\":\"Restore failed\"}"); return; }
    sendJson(req, 200, "{\"ok\":true,\"note\":\"Restart recommended to reload config\"}");
}

// ── /api/sd/backups ───────────────────────────────────────────
void WebUI::handleSdBackupList(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    auto entries = sdMgr.listDir("/backups");
    JsonDocument doc;
    JsonArray arr = doc["backups"].to<JsonArray>();
    for (const auto& e : entries) {
        if (!e.isDir) continue;
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = e.name;
        obj["size"] = (unsigned long)e.size;
        // Format modTime as date string if available
        if (e.modTime > 0) {
            char buf[20];
            struct tm* tm_info = localtime((const time_t*)&e.modTime);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_info);
            obj["date"] = buf;
        } else {
            obj["date"] = "";
        }
    }
    String out;
    out.reserve(512);
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/macro/run ─────────────────────────────────────────
void WebUI::handleSdMacroRun(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    if (!req->hasParam("file")) {
        sendJson(req, 400, "{\"error\":\"Missing file param\"}"); return;
    }
    if (sdMgr.isMacroRunning()) {
        sendJson(req, 409, "{\"error\":\"Macro already running\"}"); return;
    }
    String fname = req->getParam("file")->value();
    if (!sdMgr.queueMacro(fname)) {
        sendJson(req, 404, "{\"error\":\"Macro not found or invalid\"}"); return;
    }
    sendJson(req, 200, String("{\"ok\":true,\"file\":\"") + fname + "\"}");
}

// ── /api/sd/macro/status ──────────────────────────────────────
void WebUI::handleSdMacroStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["running"] = sdMgr.isMacroRunning();
    doc["sdAvailable"] = sdMgr.isAvailable();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/macros ────────────────────────────────────────────
void WebUI::handleSdMacroList(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    auto list = sdMgr.listMacros();
    JsonDocument doc;
    JsonArray arr = doc["macros"].to<JsonArray>();
    for (const auto& m : list) arr.add(m);
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/irlibrary/export ──────────────────────────────────
void WebUI::handleSdIRLibExport(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    String name = "library";
    if (l > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, d, l) == DeserializationError::Ok)
            name = doc["name"] | (const char*)"library";
    }
    name.replace("/", "_"); name.replace(" ", "_");
    if (!sdMgr.exportIRLibrary(name)) {
        sendJson(req, 500, "{\"error\":\"Export failed\"}"); return;
    }
    JsonDocument r;
    r["ok"]      = true;
    r["name"]    = name;
    r["buttons"] = (int)irDB.size();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// ── /api/sd/irlibrary/import ──────────────────────────────────
void WebUI::handleSdIRLibImport(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String name = doc["name"] | (const char*)"";
    if (name.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing name\"}"); return;
    }
    if (!sdMgr.importIRLibrary(name)) {
        sendJson(req, 404, "{\"error\":\"Library not found or invalid\"}"); return;
    }
    JsonDocument r;
    r["ok"]      = true;
    r["buttons"] = (int)irDB.size();
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// ── /api/sd/irlibrary ─────────────────────────────────────────
void WebUI::handleSdIRLibList(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    auto list = sdMgr.listIRLibraries();
    JsonDocument doc;
    JsonArray arr = doc["libraries"].to<JsonArray>();
    for (const auto& f : list) arr.add(f);
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/devices ───────────────────────────────────────────
void WebUI::handleSdDeviceList(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    auto list = sdMgr.listDeviceProfiles();
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& d : list) arr.add(d);
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/device ────────────────────────────────────────────
void WebUI::handleSdDeviceRead(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    if (!req->hasParam("name")) {
        sendJson(req, 400, "{\"error\":\"Missing name\"}"); return;
    }
    String profile = sdMgr.readDeviceProfile(req->getParam("name")->value());
    if (profile.isEmpty()) {
        sendJson(req, 404, "{\"error\":\"Profile not found\"}"); return;
    }
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", profile);
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

// ── /api/sd/irlibrary/merge ───────────────────────────────────
// Body: {"name":"my_library"}
// Merges library into current DB — keeps existing buttons, adds new ones.
void WebUI::handleSdIRLibMerge(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String name = doc["name"] | (const char*)"";
    if (name.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing name\"}"); return;
    }
    int added = sdMgr.mergeIRLibrary(name);
    if (added < 0) {
        sendJson(req, 404, "{\"error\":\"Library not found or invalid\"}"); return;
    }
    JsonDocument r;
    r["ok"]      = true;
    r["added"]   = added;
    r["total"]   = (int)irDB.size();
    String out; out.reserve(128);
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// ── /api/sd/irlibrary/delete ──────────────────────────────────
// Body: {"name":"my_library"} or {"name":"my_library.json"}
void WebUI::handleSdIRLibDelete(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String name = doc["name"] | (const char*)"";
    if (name.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing name\"}"); return;
    }
    if (!sdMgr.deleteIRLibrary(name)) {
        sendJson(req, 404, "{\"error\":\"Library not found\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/irlibrary/rename ──────────────────────────────────
// Body: {"from":"old_name","to":"new_name"}
void WebUI::handleSdIRLibRename(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String from = doc["from"] | (const char*)"";
    String to   = doc["to"]   | (const char*)"";
    if (from.isEmpty() || to.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing from/to\"}"); return;
    }
    if (!sdMgr.renameIRLibrary(from, to)) {
        sendJson(req, 409, "{\"error\":\"Rename failed — source missing or dest exists\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/irlibrary/savebtn ─────────────────────────────────
// Body: {"buttonId":42,"library":"my_library"}
// Saves a single button from irDB into the named SD library (upsert by name).
void WebUI::handleSdIRLibSaveBtn(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    uint32_t buttonId = doc["buttonId"] | (uint32_t)0;
    String   library  = doc["library"]  | (const char*)"default";
    if (buttonId == 0) {
        sendJson(req, 400, "{\"error\":\"Missing buttonId\"}"); return;
    }
    if (!sdMgr.saveButtonToLibrary(library, buttonId)) {
        sendJson(req, 404, "{\"error\":\"Button not found or SD write failed\"}"); return;
    }
    JsonDocument r;
    r["ok"]      = true;
    r["library"] = library;
    r["buttonId"] = buttonId;
    String out; out.reserve(128);
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// ── /api/sd/device/save ───────────────────────────────────────
// Body: {"name":"samsung_tv","profile":{...full JSON...}}
// Creates or overwrites a device profile on SD.
void WebUI::handleSdDeviceSave(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String name = doc["name"] | (const char*)"";
    if (name.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing name\"}"); return;
    }
    // Accept either {"profile":{...}} or raw buttons array at root
    String profileJson;
    if (doc["profile"].is<JsonObject>()) {
        serializeJson(doc["profile"], profileJson);
    } else {
        // Whole body is the profile
        serializeJson(doc, profileJson);
    }
    if (!sdMgr.saveDeviceProfile(name, profileJson)) {
        sendJson(req, 500, "{\"error\":\"SD write failed\"}"); return;
    }
    JsonDocument r;
    r["ok"]   = true;
    r["name"] = name;
    String out; out.reserve(128);
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// ── /api/sd/device/delete ─────────────────────────────────────
// Body: {"name":"samsung_tv"}
void WebUI::handleSdDeviceDelete(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String name = doc["name"] | (const char*)"";
    if (name.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing name\"}"); return;
    }
    if (!sdMgr.deleteDeviceProfile(name)) {
        sendJson(req, 404, "{\"error\":\"Profile not found\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/device/import ─────────────────────────────────────
// Body: {"name":"samsung_tv"}
// Merges the device profile's buttons into irDB (keeps existing, adds new).
void WebUI::handleSdDeviceImport(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String name = doc["name"] | (const char*)"";
    if (name.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing name\"}"); return;
    }
    int added = sdMgr.importDeviceProfileButtons(name);
    if (added < 0) {
        sendJson(req, 404, "{\"error\":\"Profile not found or invalid\"}"); return;
    }
    JsonDocument r;
    r["ok"]    = true;
    r["added"] = added;
    r["total"] = (int)irDB.size();
    String out; out.reserve(128);
    serializeJson(r, out);
    sendJson(req, 200, out);
}

// ── /api/sd/copy ──────────────────────────────────────────────
// Body: {"src":"/path/file.txt","dst":"/path/copy.txt"}
void WebUI::handleSdCopy(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String src = doc["src"] | (const char*)"";
    String dst = doc["dst"] | (const char*)"";
    if (src.isEmpty() || dst.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing src/dst\"}"); return;
    }
    if (!sdMgr.exists(src)) {
        sendJson(req, 404, "{\"error\":\"Source not found\"}"); return;
    }
    if (!sdMgr.copyFileSd(src, dst)) {
        sendJson(req, 500, "{\"error\":\"Copy failed\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/move ──────────────────────────────────────────────
// Body: {"src":"/path/file.txt","dst":"/other/file.txt"}
void WebUI::handleSdMove(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String src = doc["src"] | (const char*)"";
    String dst = doc["dst"] | (const char*)"";
    if (src.isEmpty() || dst.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Missing src/dst\"}"); return;
    }
    if (!sdMgr.exists(src)) {
        sendJson(req, 404, "{\"error\":\"Source not found\"}"); return;
    }
    if (!sdMgr.moveFile(src, dst)) {
        sendJson(req, 500, "{\"error\":\"Move failed\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/rmrf ──────────────────────────────────────────────
// ?path=/dir  - deletes file or entire directory tree
void WebUI::handleSdDeleteRecursive(AsyncWebServerRequest* req) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    if (!req->hasParam("path")) {
        sendJson(req, 400, "{\"error\":\"Missing path\"}"); return;
    }
    String path = req->getParam("path")->value();
    // Safety: refuse to delete root
    if (path == "/" || path.isEmpty()) {
        sendJson(req, 400, "{\"error\":\"Cannot delete root directory\"}"); return;
    }
    if (!sdMgr.exists(path)) {
        sendJson(req, 404, "{\"error\":\"Path not found\"}"); return;
    }
    if (!sdMgr.deleteRecursive(path)) {
        sendJson(req, 500, "{\"error\":\"Delete failed\"}"); return;
    }
    sendJson(req, 200, "{\"ok\":true}");
}

// ── /api/sd/info ──────────────────────────────────────────────
// ?path=/file  - returns metadata (size, modTime, isDir)
void WebUI::handleSdFileInfo(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    if (!req->hasParam("path")) {
        sendJson(req, 400, "{\"error\":\"Missing path\"}"); return;
    }
    String path = req->getParam("path")->value();
    if (!sdMgr.exists(path)) {
        sendJson(req, 404, "{\"error\":\"Not found\"}"); return;
    }
    SdFileEntry info = sdMgr.getFileInfo(path);
    JsonDocument doc;
    doc["name"]     = info.name;
    doc["path"]     = path;
    doc["isDir"]    = info.isDir;
    doc["size"]     = (uint32_t)info.size;
    doc["modTime"]  = (uint32_t)info.modTime;
    // Format modTime as human-readable if valid (FAT32 epoch starts 1980)
    if (info.modTime > 315532800UL) {  // > 1980-01-01
        struct tm tmbuf;
        time_t t = (time_t)info.modTime;
        localtime_r(&t, &tmbuf);
        char tbuf[24];
        snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d",
                 tmbuf.tm_year+1900, tmbuf.tm_mon+1, tmbuf.tm_mday,
                 tmbuf.tm_hour, tmbuf.tm_min);
        doc["modTimeStr"] = tbuf;
    } else {
        doc["modTimeStr"] = "";
    }
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/read ──────────────────────────────────────────────
// ?path=/file[&max=8192]  - read text file for preview (max 8 KB)
// Only allows text-safe extensions to prevent binary garbage in UI.
void WebUI::handleSdReadText(AsyncWebServerRequest* req) {
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    if (!req->hasParam("path")) {
        sendJson(req, 400, "{\"error\":\"Missing path\"}"); return;
    }
    String path = req->getParam("path")->value();

    // Whitelist: only readable text extensions
    static const char* READABLE[] = {
        ".txt", ".log", ".json", ".csv", ".cfg", ".ini",
        ".md",  ".htm", ".html", ".xml", ".yaml", ".yml",
        ".sh",  ".py",  ".js",   ".css", ".conf", nullptr
    };
    bool allowed = false;
    String lower = path; lower.toLowerCase();
    for (const char** ext = READABLE; *ext; ++ext) {
        if (lower.endsWith(*ext)) { allowed = true; break; }
    }
    if (!allowed) {
        sendJson(req, 415, "{\"error\":\"File type not previewable\"}"); return;
    }
    if (!sdMgr.exists(path)) {
        sendJson(req, 404, "{\"error\":\"File not found\"}"); return;
    }

    size_t maxBytes = 8192;
    if (req->hasParam("max")) {
        int m = req->getParam("max")->value().toInt();
        if (m > 0 && m <= 32768) maxBytes = (size_t)m;
    }

    String content = sdMgr.readTextFile(path, maxBytes);
    JsonDocument doc;
    doc["path"]    = path;
    doc["content"] = content;
    doc["size"]    = (uint32_t)sdMgr.getFileInfo(path).size;
    String out;
    out.reserve(512);  // FIX: pre-alloc avoids realloc during serialize
    serializeJson(doc, out);
    sendJson(req, 200, out);
}

// ── /api/sd/format ────────────────────────────────────────────
// Body: {"confirm":"FORMAT_SD_CARD"}  - must include exact passphrase
void WebUI::handleSdFormat(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    if (!sdMgr.isAvailable()) { sdNotAvail(req); return; }
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
        sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
    }
    String confirm = doc["confirm"] | (const char*)"";
    // Require exact passphrase to prevent accidental format
    if (confirm != "FORMAT_SD_CARD") {
        sendJson(req, 400, "{\"error\":\"Missing confirmation. Send confirm:\\\"FORMAT_SD_CARD\\\"\"}"); return;
    }
    Serial.println(DEBUG_TAG " [SD] Format triggered by user via Web UI");
    bool ok = sdMgr.formatCard();
    if (!ok) {
        sendJson(req, 500,
            "{\"error\":\"Format not supported in this firmware build. "
            "Use SD Card Formatter (https://www.sdcard.org/downloads/formatter/) on a PC.\"}");
        return;
    }
    sendJson(req, 200, "{\"ok\":true,\"note\":\"Card formatted and remounted\"}");
}

// ── IR Jammer routes ────────────────────────────────────────────────────────
// POST /api/ir/jammer/start   body: {"mode":0,"freqIdx":3,"density":5}
// POST /api/ir/jammer/stop
// GET  /api/ir/jammer/status
void WebUI::setupIrJammerRoutes() {
    POST_BODY("/api/ir/jammer/start",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            uint8_t mode    = doc["mode"]    | 0;
            uint8_t freqIdx = doc["freqIdx"] | 3;
            uint8_t density = doc["density"] | 5;
            if (mode >= JAM_MODE_COUNT) mode = 0;
            irJammer.start((JamMode)mode, freqIdx, density);
            sendJson(req, 200, irJammer.statusJson());
        }));

    _server.on("/api/ir/jammer/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            irJammer.stop();
            sendJson(req, 200, irJammer.statusJson());
        });

    _server.on("/api/ir/jammer/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200, irJammer.statusJson());
        });

    // POST /api/ir/receiver/pause   — pause IR receiver (10s countdown done)
    _server.on("/api/ir/receiver/pause", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            irReceiver.pause();
            sendJson(req, 200, "{\"ok\":true,\"paused\":true}");
        });

    // POST /api/ir/receiver/resume  — resume IR receiver
    _server.on("/api/ir/receiver/resume", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            irReceiver.resume();
            sendJson(req, 200, "{\"ok\":true,\"paused\":false}");
        });

    // GET /api/ir/receiver/status
    _server.on("/api/ir/receiver/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200,
                String("{\"paused\":") +
                (irReceiver.isPaused() ? "true" : "false") + "}");
        });
}

// ── Beacon Spam routes ──────────────────────────────────────────────────────
// POST /api/beacon/start   {"mode":0,"channel":0}  mode: 0=random,1=list,2=clone
// POST /api/beacon/stop
// POST /api/beacon/ssids   {"ssids":["SSID1","SSID2",...]}
// GET  /api/beacon/status
void WebUI::setupBeaconSpamRoutes() {
    POST_BODY("/api/beacon/start",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            uint8_t mode    = doc["mode"]    | 0;
            uint8_t channel = doc["channel"] | 0;
            beaconSpam.start((BeaconMode)(mode % 3), channel);
            sendJson(req, 200, beaconSpam.statusJson());
        }));

    _server.on("/api/beacon/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            beaconSpam.stop();
            sendJson(req, 200, beaconSpam.statusJson());
        });

    POST_BODY("/api/beacon/ssids",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            beaconSpam.clearSsids();
            JsonArray arr = doc["ssids"].as<JsonArray>();
            for (JsonVariant v : arr)
                beaconSpam.addSsid(v.as<String>());
            sendJson(req, 200, "{\"ok\":true,\"count\":" +
                     String(arr.size()) + "}");
        }));

    _server.on("/api/beacon/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200, beaconSpam.statusJson());
        });
}

// ── Responder routes ────────────────────────────────────────────────────────
// POST /api/responder/start   {"hostname":"FILESERVER","domain":"WORKGROUP"}
// POST /api/responder/stop
// GET  /api/responder/status
// GET  /api/responder/captures
// POST /api/responder/captures/clear
void WebUI::setupResponderRoutes() {
    POST_BODY("/api/responder/start",
        ([](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            String host   = doc["hostname"] | "FILESERVER";
            String domain = doc["domain"]   | "WORKGROUP";
            String dns    = doc["dns"]      | "local";
            responder.start(host, domain, dns);
            sendJson(req, 200, responder.statusJson());
        }));

    _server.on("/api/responder/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            responder.stop();
            sendJson(req, 200, responder.statusJson());
        });

    _server.on("/api/responder/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200, responder.statusJson());
        });

    _server.on("/api/responder/captures", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            sendJson(req, 200, responder.capturesJson());
        });

    _server.on("/api/responder/captures/clear", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            responder.clearCaptures();
            sendJson(req, 200, "{\"ok\":true}");
        });
}
