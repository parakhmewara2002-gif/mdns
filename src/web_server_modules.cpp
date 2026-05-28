// ============================================================
//  web_server_modules.cpp  -  API routes for Tasks 7-11
//  NFC, RFID, Sub-GHz, NRF24, System modules
// ============================================================
#include "web_server.h"
#include <new>            // std::nothrow for guarded task-arg allocation
#include "nfc_module.h"
#include "rfid_module.h"
#include "subghz_module.h"
#include "nrf24_module.h"
#include "system_module.h"
#include "wifi_manager.h"
#include "ir_transmitter.h"
#include "ir_receiver.h"
#include "auth_manager.h"
#include "audit_manager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#include <map>
extern AuditManager auditMgr;
extern AuthManager  authMgr;

// Local helper - mirrors the pattern in web_server.cpp
static void _sendJson(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

static String* _getBodyBuf(AsyncWebServerRequest* req) {
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
static void _freeBodyBuf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define POST_BODY_MOD(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_getBodyBuf(req)->length() + l > HTTP_MAX_BODY) { \
                _freeBodyBuf(req); \
                _sendJson(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _getBodyBuf(req)->concat((char*)d, l); \
            bool lastChunk = (t > 0) ? (i + l >= t) : (i == 0); \
            if (lastChunk) { \
                String* buf = _getBodyBuf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _freeBodyBuf(req); \
            } \
        })

// ============================================================
//  WebUI::setupModuleRoutes()  - called from WebUI::begin()
// ============================================================
void WebUI::setupModuleRoutes() {

    // ── NFC ─────────────────────────────────────────────────
    // GET /api/nfc/tags
    _server.on("/api/nfc/tags", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, nfcModule.tagsToJson());
    });

    // GET /api/nfc/read  - poll for result
    _server.on("/api/nfc/read", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (nfcModule.isReading()) {
            _sendJson(req, 200, "{\"ok\":true,\"reading\":true}");
            return;
        }
        if (nfcModule.tagAvailable()) {
            NfcTag t = nfcModule.lastTag();
            nfcModule.clearTag();
            // FIX: use ArduinoJson to avoid JSON injection if uid/type
            // contain quotes or backslashes (e.g. from malformed tags)
            JsonDocument nfcDoc;
            nfcDoc["ok"]   = true;
            nfcDoc["uid"]  = t.uid;
            nfcDoc["type"] = t.type;
            nfcDoc["atqa"] = t.atqa;
            nfcDoc["sak"]  = t.sak;
            JsonArray blkArr = nfcDoc["blocks"].to<JsonArray>();
            for (const auto& blk : t.blocks) blkArr.add(blk);
            String out; serializeJson(nfcDoc, out);
            _sendJson(req, 200, out);
        } else {
            // Start a new read if nothing pending
            nfcModule.startRead();
            _sendJson(req, 200, "{\"ok\":true,\"reading\":true}");
        }
    });

    // POST /api/nfc/save
    POST_BODY_MOD("/api/nfc/save", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc;
        if (deserializeJson(doc, d, l)) {
            _sendJson(req, 400, "{\"error\":\"JSON parse failed\"}"); return;
        }
        NfcTag tag;
        tag.name = doc["name"] | "Tag";
        tag.uid  = doc["uid"]  | "";
        tag.type = doc["type"] | "Unknown";
        tag.atqa = doc["atqa"] | "";
        tag.sak  = doc["sak"]  | "";
        for (const char* b : doc["blocks"].as<JsonArray>())
            tag.blocks.push_back(String(b));
        uint32_t id = nfcModule.saveTag(tag);
        _sendJson(req, 200, id ? "{\"ok\":true,\"id\":" + String(id) + "}" :
                                  "{\"error\":\"Tag storage full\"}");
    });

    // POST /api/nfc/delete  body: {id:N}
    POST_BODY_MOD("/api/nfc/delete", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc;
        if (deserializeJson(doc, d, l)) { _sendJson(req, 400, "{\"error\":\"JSON\"}\n"); return; }
        uint32_t id = doc["id"] | 0u;
        bool ok = nfcModule.deleteTag(id);
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"Not found\"}");
    });

    // POST /api/nfc/emulate  body: {id:N}
    POST_BODY_MOD("/api/nfc/emulate", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc;
        deserializeJson(doc, d, l);
        uint32_t id = doc["id"] | 0u;
        bool ok = nfcModule.startEmulate(id);
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"Tag not found\"}");
    });

    // POST /api/nfc/emulate/stop
    _server.on("/api/nfc/emulate/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nfcModule.stopEmulate();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    // ── NFC SD integration ───────────────────────────────────

    // POST /api/nfc/backup   body: {"tag":"mybackup"}
    POST_BODY_MOD("/api/nfc/backup", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String tag = doc["tag"] | "";
        if (tag.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing tag\"}"); return; }
        bool ok = nfcModule.backupToSD(tag);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Backup failed\"}");
    });

    // POST /api/nfc/restore   body: {"tag":"mybackup"}
    POST_BODY_MOD("/api/nfc/restore", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String tag = doc["tag"] | "";
        if (tag.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing tag\"}"); return; }
        bool ok = nfcModule.restoreFromSD(tag);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Restore failed\"}");
    });

    // POST /api/nfc/export/all   no body needed
    _server.on("/api/nfc/export/all", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            bool ok = nfcModule.exportAllFlipperToSD();
            _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Export failed\"}");
        });

    // POST /api/nfc/sd/import   body: {"filename":"tag.nfc"}
    POST_BODY_MOD("/api/nfc/sd/import", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String filename = doc["filename"] | "";
        if (filename.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing filename\"}"); return; }
        bool ok = nfcModule.importTagFromSD(filename);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Import failed\"}");
    });

    // GET /api/nfc/sd/tags   list tags saved on SD
    _server.on("/api/nfc/sd/tags", HTTP_GET, [](AsyncWebServerRequest* req) {
        auto tags = nfcModule.listSdTags();
        String out = "[";
        for (size_t i = 0; i < tags.size(); i++) {
            if (i) out += ",";
            out += "\"" + tags[i] + "\"";
        }
        out += "]";
        _sendJson(req, 200, "{\"ok\":true,\"tags\":" + out + "}");
    });

    // POST /api/nfc/clone  body: tag JSON
    POST_BODY_MOD("/api/nfc/clone", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        bool ok = nfcModule.writeClone();
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"No source\"}");
    });

    // POST /api/nfc/dict/start  body: {keyb: true/false}
    _server.on("/api/nfc/dict/start", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            // keyb param: default true (try both Key A and Key B)
            bool keyB = true;
            if (req->hasParam("keyb", true))
                keyB = req->getParam("keyb", true)->value() != "false";
            nfcModule.startDictAttack(keyB);
            _sendJson(req, 200, "{\"ok\":true}");
        });

    // POST /api/nfc/dict/stop
    _server.on("/api/nfc/dict/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nfcModule.stopDictAttack();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    // GET /api/nfc/dict/progress - lightweight progress polling
    _server.on("/api/nfc/dict/progress", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, nfcModule.dictProgressJson());
    });

    // GET /api/nfc/dict/poll — removed 2026-05-23: unused by HTML; clients
    // should call /api/nfc/dict/data (richer payload) and /api/nfc/dict/progress.

    // GET /api/nfc/dict/data - returns {lines:[...], done:bool, found:N} for UI
    _server.on("/api/nfc/dict/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        String raw = nfcModule.pollDictResult();
        bool done = !nfcModule.isDictRunning();
        uint8_t foundInBatch = 0;
        // Split raw into lines array
        String linesJson = "[";
        int start = 0; bool first = true;
        for (int i = 0; i <= (int)raw.length(); i++) {
            if (i == (int)raw.length() || raw[i] == '\n') {
                String line = raw.substring(start, i);
                line.trim();
                if (line.length() > 0) {
                    if (!first) linesJson += ",";
                    String display = line;
                    // Format: "S3:KeyA:AABBCCDDEEFF"
                    if (line.indexOf(":Key") >= 0) {
                        display = "✅ FOUND " + line;
                        foundInBatch++;
                    } else if (line.startsWith("DONE:")) {
                        display = "🏁 Complete - " + line.substring(5) + " keys cracked";
                    }
                    display.replace("\\", "\\\\");
                    display.replace("\"", "\\\"");
                    linesJson += "\"" + display + "\"";
                    first = false;
                }
                start = i + 1;
            }
        }
        linesJson += "]";
        _sendJson(req, 200, "{\"ok\":true,\"lines\":" + linesJson +
                  ",\"done\":" + (done ? "true" : "false") +
                  ",\"foundInBatch\":" + String(foundInBatch) +
                  ",\"running\":" + (nfcModule.isDictRunning() ? "true" : "false") + "}");
    });

    // GET /api/nfc/export?id=N  - download .nfc content
    _server.on("/api/nfc/export", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("id")) { _sendJson(req, 400, "{\"error\":\"Missing id\"}"); return; }
        uint32_t id = req->getParam("id")->value().toInt();
        String content = nfcModule.exportFlipperNfc(id);
        if (content.isEmpty()) { _sendJson(req, 404, "{\"error\":\"Not found\"}"); return; }
        AsyncWebServerResponse* r = req->beginResponse(200, "text/plain", content);
        r->addHeader("Content-Disposition", "attachment; filename=\"tag.nfc\"");
        req->send(r);
    });

    // POST /api/nfc/import  body: {name:"x", content:"..."}
    POST_BODY_MOD("/api/nfc/import", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc;
        if (deserializeJson(doc, d, l)) { _sendJson(req, 400, "{\"error\":\"JSON\"}"); return; }
        NfcTag tag;
        tag.name = doc["name"] | "Imported";
        String content = doc["content"] | "";
        if (!nfcModule.importFlipperNfc(content, tag)) {
            _sendJson(req, 400, "{\"error\":\"Parse failed\"}"); return;
        }
        tag.name = doc["name"] | "Imported";
        uint32_t id = nfcModule.saveTag(tag);
        _sendJson(req, 200, id ? "{\"ok\":true,\"id\":" + String(id) + "}" :
                                  "{\"error\":\"Storage full\"}");
    });

    // GET /api/nfc/parse?id=N
    _server.on("/api/nfc/parse", HTTP_GET, [](AsyncWebServerRequest* req) {
        NfcTag t = nfcModule.lastTag();
        String result = nfcModule.parseTag(t);
        JsonDocument doc;
        doc["ok"]     = true;
        doc["result"] = result;
        String out; serializeJson(doc, out);
        _sendJson(req, 200, out);
    });

    // Chameleon removed — not implemented

    // ── RFID ────────────────────────────────────────────────
    _server.on("/api/rfid/cards", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, rfidModule.cardsToJson());
    });

    _server.on("/api/rfid/read", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (rfidModule.isReading()) {
            _sendJson(req, 200, "{\"ok\":true,\"reading\":true}"); return;
        }
        if (rfidModule.cardAvailable()) {
            RfidCard c = rfidModule.lastCard();
            rfidModule.clearCard();
            JsonDocument doc;
            doc["ok"]   = true;
            doc["uid"]  = c.uid;
            doc["type"] = c.type;
            String out; serializeJson(doc, out);
            _sendJson(req, 200, out);
        } else {
            rfidModule.startRead();
            _sendJson(req, 200, "{\"ok\":true,\"reading\":true}");
        }
    });

    POST_BODY_MOD("/api/rfid/save", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc;
        if (deserializeJson(doc, d, l)) { _sendJson(req, 400, "{\"error\":\"JSON\"}"); return; }
        RfidCard card;
        card.name = doc["name"] | "Card";
        card.uid  = doc["uid"]  | "";
        card.type = doc["type"] | "";
        uint32_t id = rfidModule.saveCard(card);
        _sendJson(req, 200, id ? "{\"ok\":true,\"id\":" + String(id) + "}" :
                                  "{\"error\":\"Storage full\"}");
    });

    POST_BODY_MOD("/api/rfid/delete", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        bool ok = rfidModule.deleteCard(doc["id"] | 0u);
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"Not found\"}");
    });

    POST_BODY_MOD("/api/rfid/write", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String uid = doc["uid"] | "";
        // C-01 FIX: use writeCardAsync() - writeCard() blocks 300-500ms with
        // raw delay() calls. Calling it here (AsyncWebServer task context) would
        // stall the HTTPS server for the full write duration. The async version
        // spawns a dedicated task and returns immediately; the card write proceeds
        // in the background. HTTP 200 is returned as "queued", not "confirmed".
        bool queued = rfidModule.writeCardAsync(uid);
        _sendJson(req, 200, queued ? "{\"ok\":true,\"async\":true}" : "{\"error\":\"Write failed to queue\"}");
    });

    POST_BODY_MOD("/api/rfid/emulate", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        bool ok = rfidModule.startEmulate(doc["id"] | 0u);
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"Card not found\"}");
    });

    _server.on("/api/rfid/emulate/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            rfidModule.stopEmulate();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    // ── RFID Allowlist ──────────────────────────────────────
    // GET  /api/rfid/allowlist           — list all allowed UIDs
    _server.on("/api/rfid/allowlist", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            _sendJson(req, 200, rfidModule.allowlistToJson());
        });

    // POST /api/rfid/allowlist/add       — { "uid": "AABBCCDD" }
    POST_BODY_MOD("/api/rfid/allowlist/add",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            String uid = doc["uid"] | "";
            uid.trim();
            if (uid.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing uid\"}"); return; }
            rfidModule.addToAllowlist(uid);
            rfidModule.saveAllowlistToSD();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    // POST /api/rfid/allowlist/remove    — { "uid": "AABBCCDD" }
    POST_BODY_MOD("/api/rfid/allowlist/remove",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            String uid = doc["uid"] | "";
            uid.trim();
            if (uid.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing uid\"}"); return; }
            rfidModule.removeFromAllowlist(uid);
            rfidModule.saveAllowlistToSD();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    // POST /api/rfid/allowlist/save   — persist allowlist to SD
    _server.on("/api/rfid/allowlist/save", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            bool ok = rfidModule.saveAllowlistToSD();
            _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Save failed\"}");
        });

    // POST /api/rfid/allowlist/load   — reload allowlist from SD
    _server.on("/api/rfid/allowlist/load", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            bool ok = rfidModule.loadAllowlistFromSD();
            _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Load failed\"}");
        });

    // POST /api/rfid/backup    body: {"tag":"..."}
    POST_BODY_MOD("/api/rfid/backup", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String tag = doc["tag"] | "";
        if (tag.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing tag\"}"); return; }
        bool ok = rfidModule.backupToSD(tag);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Backup failed\"}");
    });

    // POST /api/rfid/restore   body: {"tag":"..."}
    POST_BODY_MOD("/api/rfid/restore", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String tag = doc["tag"] | "";
        if (tag.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing tag\"}"); return; }
        bool ok = rfidModule.restoreFromSD(tag);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Restore failed\"}");
    });

    // POST /api/rfid/allowlist/enable   body: {"enabled": true/false}
    POST_BODY_MOD("/api/rfid/allowlist/enable", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        rfidModule.setAllowlistEnabled(doc["enabled"] | false);
        _sendJson(req, 200, "{\"ok\":true}");
    });

    // ── RFID Macro Mappings ─────────────────────────────────
    // GET  /api/rfid/macros              — list all uid→macro mappings
    _server.on("/api/rfid/macros", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            _sendJson(req, 200, rfidModule.macroMappingsToJson());
        });

    // POST /api/rfid/macros/set          — { "uid": "AABBCCDD", "macro": "filename.json" }
    POST_BODY_MOD("/api/rfid/macros/set",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            String uid   = doc["uid"]   | "";
            String macro = doc["macro"] | "";
            uid.trim(); macro.trim();
            if (uid.isEmpty() || macro.isEmpty()) {
                _sendJson(req, 400, "{\"error\":\"Missing uid or macro\"}"); return;
            }
            rfidModule.setMacroMapping(uid, macro);
            rfidModule.saveMacroMappings();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    // POST /api/rfid/macros/remove       — { "uid": "AABBCCDD" }
    POST_BODY_MOD("/api/rfid/macros/remove",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc; deserializeJson(doc, d, l);
            String uid = doc["uid"] | "";
            uid.trim();
            if (uid.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing uid\"}"); return; }
            rfidModule.removeMacroMapping(uid);
            rfidModule.saveMacroMappings();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    // ── Sub-GHz ─────────────────────────────────────────────
    _server.on("/api/subghz/signals", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, subGhzModule.signalsToJson());
    });

    POST_BODY_MOD("/api/subghz/capture/start", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        float freq = doc["freq"] | 433.92f;
        subGhzModule.startCapture(freq);
        _sendJson(req, 200, "{\"ok\":true}");
    });

    _server.on("/api/subghz/capture/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            subGhzModule.stopCapture();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    _server.on("/api/subghz/capture/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        String data = subGhzModule.pollCaptured();
        // Return as JSON packets array
        String out = "{\"ok\":true,\"packets\":[";
        int pos = 0; bool first = true;
        while (pos < (int)data.length()) {
            int nl = data.indexOf('\n', pos);
            if (nl < 0) nl = data.length();
            String line = data.substring(pos, nl);
            if (line.length()) {
                if (!first) out += ",";
                out += line;
                first = false;
            }
            pos = nl + 1;
        }
        out += "]}";
        _sendJson(req, 200, out);
    });

    POST_BODY_MOD("/api/subghz/save", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        SubGhzSignal sig;
        sig.name     = doc["name"]    | "Signal";
        sig.freqMhz  = doc["freq"]    | 433.92f;
        sig.protocol = doc["protocol"]| "";
        sig.captured = doc["captured"]| "";
        uint32_t id = subGhzModule.saveSignal(sig);
        _sendJson(req, 200, id ? "{\"ok\":true,\"id\":" + String(id) + "}" :
                                  "{\"error\":\"Storage full\"}");
    });

    POST_BODY_MOD("/api/subghz/replay", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        uint32_t id = doc["id"] | 0u;
        if (id == 0) { _sendJson(req, 400, "{\"error\":\"Missing id\"}"); return; }
        _sendJson(req, 202, "{\"ok\":true,\"queued\":true}");
        uint32_t* arg = new (std::nothrow) uint32_t(id);
        if (!arg) return;   // OOM - response already sent; nothing leaked
        if (xTaskCreate([](void* p) {
            uint32_t sid = *reinterpret_cast<uint32_t*>(p);
            delete reinterpret_cast<uint32_t*>(p);
            subGhzModule.replaySignal(sid);
            vTaskDelete(NULL);
        }, "subghz_replay", 4096, arg, 1, nullptr) != pdPASS) {
            delete arg;     // spawn failed (low heap) - don't leak the arg
        }
    });

    POST_BODY_MOD("/api/subghz/delete", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        bool ok = subGhzModule.deleteSignal(doc["id"] | 0u);
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"Not found\"}");
    });

    POST_BODY_MOD("/api/subghz/rename", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        bool ok = subGhzModule.renameSignal(doc["id"] | 0u, doc["name"] | "");
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"Not found\"}");
    });

    // ── SubGHz SD integration ────────────────────────────────

    // POST /api/subghz/backup   no body needed
    _server.on("/api/subghz/backup", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            bool ok = subGhzModule.backupSignalsToSD();
            _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Backup failed\"}");
        });

    // POST /api/subghz/restore   no body needed
    _server.on("/api/subghz/restore", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            bool ok = subGhzModule.restoreSignalsFromSD();
            _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Restore failed\"}");
        });

    // GET  /api/subghz/sd/signals  — list CSV/JSON files saved on SD
    _server.on("/api/subghz/sd/signals", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!authMgr.checkAuth(req)) return;
        auto files = subGhzModule.listSdSignals();
        JsonDocument doc;
        JsonArray arr = doc["signals"].to<JsonArray>();
        for (const auto& f : files) arr.add(f);
        String out; serializeJson(doc, out);
        _sendJson(req, 200, out);
    });

    // POST /api/subghz/sd/replay   body: {"filename":"signal.sub"}
    POST_BODY_MOD("/api/subghz/sd/replay", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String filename = doc["filename"] | "";
        if (filename.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing filename\"}"); return; }
        _sendJson(req, 202, "{\"ok\":true,\"queued\":true}");
        String* arg = new (std::nothrow) String(filename);
        if (!arg) return;   // OOM - response already sent; nothing leaked
        if (xTaskCreate([](void* p) {
            String fname = *reinterpret_cast<String*>(p);
            delete reinterpret_cast<String*>(p);
            subGhzModule.replayFromSD(fname);
            vTaskDelete(NULL);
        }, "subghz_sd_replay", 4096, arg, 1, nullptr) != pdPASS) {
            delete arg;     // spawn failed (low heap) - don't leak the String
        }
    });

    // POST /api/subghz/sdlog   body: {"enabled": true/false}
    POST_BODY_MOD("/api/subghz/sdlog", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        subGhzModule.setSdLog(doc["enabled"] | false);
        _sendJson(req, 200, "{\"ok\":true,\"sdlog\":" +
                  String(subGhzModule.sdLogEnabled() ? "true" : "false") + "}");
    });

    // GET /api/subghz/modulation   — get current modulation
    _server.on("/api/subghz/modulation", HTTP_GET, [](AsyncWebServerRequest* req) {
        SubGhzModulation m = subGhzModule.getModulation();
        String name = SubGhzModule::modulationName(m);
        _sendJson(req, 200, "{\"ok\":true,\"modulation\":" + String((int)m) +
                  ",\"name\":\"" + name + "\"}");
    });

    // POST /api/subghz/modulation   body: {"modulation": 0-3}
    POST_BODY_MOD("/api/subghz/modulation", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        SubGhzModulation mod = (SubGhzModulation)(doc["modulation"] | 0);
        bool ok = subGhzModule.setModulation(mod);
        _sendJson(req, ok ? 200 : 400, ok ? "{\"ok\":true}" : "{\"error\":\"Set modulation failed\"}");
    });

    // GET /api/subghz/presets   — list built-in modulation presets
    _server.on("/api/subghz/presets", HTTP_GET, [](AsyncWebServerRequest* req) {
        uint8_t count = 0;
        const SubGhzPreset* presets = SubGhzModule::builtinPresets(count);
        String out = "[";
        for (uint8_t i = 0; i < count; i++) {
            if (i) out += ",";
            out += "{\"name\":\"";
            out += presets[i].name;
            out += "\",\"mod\":" + String((int)presets[i].mod);
            out += ",\"freqMhz\":" + String(presets[i].freqMhz);
            out += "}";
        }
        out += "]";
        _sendJson(req, 200, "{\"ok\":true,\"presets\":" + out + "}");
    });

    // ── SubGHz Brute Force (Bruce port) ──────────────────────
    // POST /api/subghz/bruteforce/start
    POST_BODY_MOD("/api/subghz/bruteforce/start", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        float    freq  = doc["freq"]      | 433.92f;
        String   mod   = doc["mod"]       | "AM270";
        uint32_t start = doc["startCode"] | 0;
        uint32_t end   = doc["endCode"]   | 255;
        uint8_t  bits  = doc["bits"]      | 24;
        uint32_t delay = doc["delayMs"]   | 100;
        bool ok = subGhzModule.startBruteForce(freq, mod, start, end, bits, delay);
        _sendJson(req, ok ? 200 : 503,
            String("{\"ok\":") + (ok?"true":"false") + ",\"total\":" +
            String(end-start+1) + "}");
    });

    _server.on("/api/subghz/bruteforce/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            subGhzModule.stopBruteForce();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    _server.on("/api/subghz/bruteforce/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            _sendJson(req, 200, subGhzModule.bruteStatusJson());
        });

    // ── nRF24 SD integration ─────────────────────────────────

    // POST /api/nrf24/backup   body: {"tag":"..."}
    POST_BODY_MOD("/api/nrf24/backup", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String tag = doc["tag"] | "";
        if (tag.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing tag\"}"); return; }
        bool ok = nrf24Module.backupConfigToSD(tag);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Backup failed\"}");
    });

    // POST /api/nrf24/sdlog    body: {"enabled": true/false}
    POST_BODY_MOD("/api/nrf24/sdlog", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        nrf24Module.setSdLog(doc["enabled"] | false);
        _sendJson(req, 200, "{\"ok\":true,\"sdlog\":" +
                  String(nrf24Module.sdLogEnabled() ? "true" : "false") + "}");
    });

    POST_BODY_MOD("/api/nrf24/rc", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String dir = doc["direction"] | "S";
        uint8_t speed = doc["speed"] | 50;
        nrf24Module.sendRcCommand(dir[0], speed);
        _sendJson(req, 200, "{\"ok\":true}");
    });

    POST_BODY_MOD("/api/nrf24/scan/start", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        nrf24Module.startScan();
        _sendJson(req, 200, "{\"ok\":true}");
    });

    _server.on("/api/nrf24/scan/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nrf24Module.stopScan();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    _server.on("/api/nrf24/scan/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        const uint8_t* ch = nrf24Module.scanData();
        String out = "{\"ok\":true,\"channels\":[";
        for (int i = 0; i < NRF24_CHANNELS; i++) {
            if (i) out += ",";
            out += String(ch[i]);
        }
        out += "]}";
        _sendJson(req, 200, out);
    });

    POST_BODY_MOD("/api/nrf24/sniff/start", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        nrf24Module.startSniff();
        _sendJson(req, 200, "{\"ok\":true}");
    });

    _server.on("/api/nrf24/sniff/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nrf24Module.stopSniff();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    _server.on("/api/nrf24/sniff/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        String data = nrf24Module.pollSniffPacket();
        String out = "{\"ok\":true,\"packets\":[";
        int pos = 0; bool first = true;
        while (pos < (int)data.length()) {
            int nl = data.indexOf('\n', pos);
            if (nl < 0) nl = data.length();
            String line = data.substring(pos, nl);
            if (line.length()) {
                if (!first) out += ",";
                out += line;
                first = false;
            }
            pos = nl + 1;
        }
        out += "]}";
        _sendJson(req, 200, out);
    });

    POST_BODY_MOD("/api/nrf24/replay", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        bool ok = nrf24Module.replayPackets();
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"No captured packets\"}");
    });

    POST_BODY_MOD("/api/nrf24/replay/start", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        nrf24Module.startReplayCapture();
        _sendJson(req, 200, "{\"ok\":true}");
    });

    _server.on("/api/nrf24/replay/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nrf24Module.stopReplayCapture();
            _sendJson(req, 200, "{\"ok\":true,\"count\":" +
                      String(nrf24Module.replayPacketCount()) + "}");
        });

    // ── Jammer ───────────────────────────────────────────────
    // POST /api/nrf24/jammer/start  body: {"mode":0}  (0=Full,1=WiFi,2=BLE,3=BLE_ADV,4=BT,5=USB,6=Video,7=RC,8=Zigbee,9=Drone)
    POST_BODY_MOD("/api/nrf24/jammer/start", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        int mode = doc["mode"] | 0;
        if (mode < 0 || mode >= NRF_JAM_MODE_COUNT) mode = 0;
        nrf24Module.startJammer((NrfJamMode)mode);
        _sendJson(req, 200, nrf24Module.jammerStatusJson());
    });

    _server.on("/api/nrf24/jammer/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nrf24Module.stopJammer();
            _sendJson(req, 200, nrf24Module.jammerStatusJson());
        });

    _server.on("/api/nrf24/jammer/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            _sendJson(req, 200, nrf24Module.jammerStatusJson());
        });

    // ── Spectrum ─────────────────────────────────────────────
    // GET  /api/nrf24/spectrum/data   → [0,12,45,...] 80 values
    _server.on("/api/nrf24/spectrum/start", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nrf24Module.startSpectrum();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    _server.on("/api/nrf24/spectrum/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nrf24Module.stopSpectrum();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    _server.on("/api/nrf24/spectrum/data", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            _sendJson(req, 200, nrf24Module.spectrumJson());
        });

    // ── Mousejack ────────────────────────────────────────────
    // POST /api/nrf24/mousejack/scan/start
    // GET  /api/nrf24/mousejack/targets
    // POST /api/nrf24/mousejack/inject  body: {"target":0,"text":"hello"}
    _server.on("/api/nrf24/mousejack/scan/start", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nrf24Module.startMousejackScan();
            _sendJson(req, 200, "{\"ok\":true}");
        });

    _server.on("/api/nrf24/mousejack/scan/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            nrf24Module.stopMousejackScan();
            _sendJson(req, 200, nrf24Module.mousejackTargetsJson());
        });

    _server.on("/api/nrf24/mousejack/targets", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            _sendJson(req, 200, nrf24Module.mousejackTargetsJson());
        });

    POST_BODY_MOD("/api/nrf24/mousejack/inject", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        uint8_t targetIdx = doc["target"] | 0;
        String text = doc["text"] | "";
        bool ok = nrf24Module.mousejackInject(targetIdx, text);
        _sendJson(req, ok ? 200 : 500, String("{\"ok\":") + (ok?"true":"false") + "}");
    });

    // ── System Module ────────────────────────────────────────
    _server.on("/api/system/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, sysModule.getStatusJson());
    });


    POST_BODY_MOD("/api/system/led", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        LedConfig cfg;
        cfg.type       = (LedType)(doc["type"]       | 0);
        cfg.mode       = (LedMode)(doc["mode"]       | 0);
        cfg.dataPin    = doc["dataPin"]    | (uint8_t)2;
        cfg.numLeds    = doc["numLeds"]    | (uint8_t)8;
        cfg.r          = doc["r"]          | (uint8_t)255;
        cfg.g          = doc["g"]          | (uint8_t)0;
        cfg.b          = doc["b"]          | (uint8_t)128;
        cfg.brightness = doc["brightness"] | (uint8_t)128;
        sysModule.setLedMode(cfg);
        _sendJson(req, 200, "{\"ok\":true}");
    });

    POST_BODY_MOD("/api/system/timezone", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String tz = doc["timezone"] | "UTC";
        sysModule.setTimezone(tz);
        _sendJson(req, 200, "{\"ok\":true}");
    });

    POST_BODY_MOD("/api/ghostlink", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        JsonDocument doc; deserializeJson(doc, d, l);
        sysModule.setGhostLink(doc["enabled"] | false);
        _sendJson(req, 200, "{\"ok\":true}");
    });

    _server.on("/api/system/gpio/overview", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, sysModule.gpioOverviewJson());
    });

    // GET /api/gpio/conflicts - check all module pin assignments for conflicts
    _server.on("/api/gpio/conflicts", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Collect all active pin assignments
        std::map<uint8_t, String> pinMap;
        auto reg = [&](uint8_t pin, const String& label) {
            if (pin == 0 || pin == 255) return;
            if (pinMap.count(pin)) pinMap[pin] += " + " + label;
            else                   pinMap[pin] = label;
        };
        // Boot-critical pins
        const uint8_t BOOT_PINS[] = {0,1,2,3,5,6,7,8,9,10,11,12,15};

        // IR pins
        IrPinConfig irp; wifiMgr.loadIrPins(irp);
        reg(irp.recvPin, "IR-RX");
        for (uint8_t i = 0; i < irp.emitCount && i < IR_MAX_EMITTERS; i++)
            if (irp.emitEnabled[i]) reg(irp.emitPin[i], String("IR-TX")+i);

        // SD
        reg(SD_CS_PIN,   "SD-CS");
        reg(SD_SCK_PIN,  "SD-SCK");
        reg(SD_MOSI_PIN, "SD-MOSI");
        reg(SD_MISO_PIN, "SD-MISO");

        // NRF24
        Nrf24GpioConfig nrfc = nrf24Module.loadGpioConfig();
        reg(nrfc.ce,   "NRF24-CE");
        reg(nrfc.csn,  "NRF24-CSN");
        reg(nrfc.sck,  "NRF24-SCK");
        reg(nrfc.mosi, "NRF24-MOSI");
        reg(nrfc.miso, "NRF24-MISO");
        if (nrfc.irq) reg(nrfc.irq, "NRF24-IRQ");

        // SubGHz
        SubGhzGpioConfig sgc = subGhzModule.loadGpioConfig();
        reg(sgc.cs,   "SubGHz-CS");
        reg(sgc.gdo0, "SubGHz-GDO0");
        reg(sgc.gdo2, "SubGHz-GDO2");
        reg(sgc.sck,  "SubGHz-SCK");
        reg(sgc.mosi, "SubGHz-MOSI");
        reg(sgc.miso, "SubGHz-MISO");

        // NFC
        NfcGpioConfig nfcc = nfcModule.loadGpioConfig();
        if (nfcc.iface == NfcIface::I2C) {
            reg(nfcc.sda, "NFC-SDA"); reg(nfcc.scl, "NFC-SCL");
            reg(nfcc.irq, "NFC-IRQ"); reg(nfcc.reset, "NFC-RESET");
        } else if (nfcc.iface == NfcIface::SPI) {
            reg(nfcc.ss,   "NFC-SS");   reg(nfcc.sck,  "NFC-SCK");
            reg(nfcc.mosi, "NFC-MOSI"); reg(nfcc.miso, "NFC-MISO");
            reg(nfcc.irq,  "NFC-IRQ");  reg(nfcc.reset,"NFC-RESET");
        }

        // RFID
        RfidGpioConfig rfidC = rfidModule.loadGpioConfig();
        reg(rfidC.dataPin,  "RFID-DATA");
        reg(rfidC.clkPin,   "RFID-CLK");
        if (rfidC.powerPin) reg(rfidC.powerPin, "RFID-PWR");

        // Build response
        String out = "{\"conflicts\":[],\"bootPins\":[],\"pinMap\":{";
        bool first = true;
        std::vector<String> conflicts, bootWarns;
        for (auto& kv : pinMap) {
            if (!first) out += ",";
            out += "\"" + String(kv.first) + "\":\"" + kv.second + "\"";
            first = false;
            if (kv.second.indexOf('+') >= 0)
                conflicts.push_back("{\"pin\":" + String(kv.first) + ",\"modules\":\"" + kv.second + "\"}");
            for (uint8_t bp : BOOT_PINS) {
                if (kv.first == bp)
                    bootWarns.push_back("{\"pin\":" + String(kv.first) + ",\"modules\":\"" + kv.second + "\"}");
            }
        }
        out += "},\"conflicts\":[";
        for (size_t i = 0; i < conflicts.size(); i++) { if(i) out+=","; out+=conflicts[i]; }
        out += "],\"bootPins\":[";
        for (size_t i = 0; i < bootWarns.size(); i++) { if(i) out+=","; out+=bootWarns[i]; }
        out += "],\"ok\":" + String(conflicts.empty() ? "true" : "false") + "}";
        _sendJson(req, 200, out);
    });

    _server.on("/api/system/schedule", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, sysModule.scheduleTasksToJson());
    });

    POST_BODY_MOD("/api/system/schedule/add", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        SysScheduleTask task;
        task.name    = doc["name"]   | "Task";
        task.time    = doc["time"]   | "08:00";
        task.action  = doc["action"] | "reboot";
        task.enabled = doc["enabled"]| true;
        uint32_t id = sysModule.addScheduleTask(task);
        _sendJson(req, 200, "{\"ok\":true,\"id\":" + String(id) + "}");
    });

    POST_BODY_MOD("/api/system/schedule/delete", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        bool ok = sysModule.deleteScheduleTask(doc["id"] | 0u);
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"Not found\"}");
    });

    POST_BODY_MOD("/api/system/schedule/toggle", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        bool ok = sysModule.toggleScheduleTask(doc["id"] | 0u, doc["enabled"] | true);
        _sendJson(req, 200, ok ? "{\"ok\":true}" : "{\"error\":\"Not found\"}");
    });

    POST_BODY_MOD("/api/system/reboot", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        _sendJson(req, 200, "{\"ok\":true}");
        // Deferred restart - lets HTTP response transmit before reset
        extern portMUX_TYPE s_restartMux;
        extern volatile uint32_t s_restartAt;
        taskENTER_CRITICAL(&s_restartMux);
        s_restartAt = (uint32_t)(millis() + 1200);
        taskEXIT_CRITICAL(&s_restartMux);
    });

    POST_BODY_MOD("/api/system/reset", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        _sendJson(req, 200, "{\"ok\":true}");
        // Small yield to allow response to send before erasing storage
        vTaskDelay(pdMS_TO_TICKS(50));
        // Erase NVS + LittleFS then restart
        LittleFS.format();
        ESP.restart();
    });
    // ═══════════════════════════════════════════════════════════
    // Module Status + GPIO endpoints
    // ═══════════════════════════════════════════════════════════

    // ── NRF24 ────────────────────────────────────────────────
    _server.on("/api/nrf24/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, nrf24Module.statusJson());
    });
    _server.on("/api/nrf24/gpio", HTTP_GET, [](AsyncWebServerRequest* req) {
        Nrf24GpioConfig c = nrf24Module.loadGpioConfig();
        JsonDocument doc;
        doc["moduleType"] = (int)c.moduleType;
        doc["ce"]  = c.ce;  doc["csn"]  = c.csn;
        doc["sck"] = c.sck; doc["mosi"] = c.mosi;
        doc["miso"]= c.miso;doc["irq"]  = c.irq;
        doc["spiBus"]   = c.spiBus;
        doc["dataRate"] = (int)c.dataRate;
        doc["paLevel"]  = (int)c.paLevel;
        String out; serializeJson(doc, out);
        _sendJson(req, 200, out);
    });
    POST_BODY_MOD("/api/nrf24/gpio", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        JsonDocument doc;
        if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
            _sendJson(req, 400, "{\"error\":\"JSON\"}"); return;
        }
        Nrf24GpioConfig cfg;

        // module type: UI sends "nrf24"/"nrf24pa"/"nrf24sma"
        if (doc["module"].is<const char*>()) {
            String m = doc["module"].as<String>();
            if      (m == "nrf24pa")  cfg.moduleType = Nrf24Module_t::NRF24L01_PA_LNA;
            else if (m == "nrf24sma") cfg.moduleType = Nrf24Module_t::NRF24L01_SMA;
            else                      cfg.moduleType = Nrf24Module_t::NRF24L01;
        } else {
            cfg.moduleType = (Nrf24Module_t)(doc["moduleType"] | 0);
        }

        // Pin numbers - UI sends strings from gpio-sel dropdowns
        auto toPin = [&](const char* key, uint8_t def) -> uint8_t {
            if (doc[key].is<int>())         return doc[key].as<uint8_t>();
            if (doc[key].is<const char*>()) {
                String sv = doc[key].as<String>();
                if (sv.length() > 0) return (uint8_t)sv.toInt();
            }
            return def;
        };
        cfg.ce   = toPin("ce",   16);
        cfg.csn  = toPin("csn",  17);
        cfg.sck  = toPin("sck",  18);
        cfg.mosi = toPin("mosi", 23);
        cfg.miso = toPin("miso", 19);
        cfg.irq  = toPin("irq",  0);

        // spiBus: UI sends "vspi"/"hspi" string
        if (doc["spiBus"].is<const char*>()) {
            String bus = doc["spiBus"].as<String>();
            cfg.spiBus = (bus == "hspi" || bus == "1") ? 1 : 0;
        } else {
            cfg.spiBus = doc["spiBus"] | (uint8_t)0;
        }

        // dataRate: UI sends "250k"/"1m"/"2m" string
        if (doc["dataRate"].is<const char*>()) {
            String dr = doc["dataRate"].as<String>();
            if      (dr == "250k") cfg.dataRate = Nrf24DataRate::RATE_250K;
            else if (dr == "2m")   cfg.dataRate = Nrf24DataRate::RATE_2M;
            else                   cfg.dataRate = Nrf24DataRate::RATE_1M;
        } else {
            cfg.dataRate = (Nrf24DataRate)(doc["dataRate"] | 1);
        }

        cfg.paLevel = (Nrf24PaLevel)(doc["paLevel"] | 2);

        Serial.printf("[NRF24] GPIO save: CE=%u CSN=%u SCK=%u MOSI=%u MISO=%u bus=%s\n",
                      cfg.ce, cfg.csn, cfg.sck, cfg.mosi, cfg.miso,
                      cfg.spiBus==1?"HSPI":"VSPI");

        nrf24Module.reinit(cfg);

        // Return connection result so UI can update banner immediately
        String status = nrf24Module.statusJson();
        _sendJson(req, 200, "{\"ok\":true,\"status\":" + status + "}");
    });
    POST_BODY_MOD("/api/nrf24/config", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        JsonDocument doc;
        if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
            _sendJson(req, 400, "{\"error\":\"JSON\"}"); return;
        }
        if (doc["channel"].is<uint8_t>())
            nrf24Module.setChannel(doc["channel"].as<uint8_t>());
        if (doc["dataRate"].is<int>())
            nrf24Module.setDataRate((Nrf24DataRate)doc["dataRate"].as<int>());
        _sendJson(req, 200, "{\"ok\":true}");
    });

    // ── SubGHz ───────────────────────────────────────────────
    _server.on("/api/subghz/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, subGhzModule.statusJson());
    });
    _server.on("/api/subghz/gpio", HTTP_GET, [](AsyncWebServerRequest* req) {
        SubGhzGpioConfig c = subGhzModule.loadGpioConfig();
        JsonDocument doc;
        doc["gdo0"] = c.gdo0; doc["gdo2"] = c.gdo2;
        doc["cs"]   = c.cs;   doc["sck"]  = c.sck;
        doc["mosi"] = c.mosi; doc["miso"] = c.miso;
        doc["spiBus"] = c.spiBus;
        String out; serializeJson(doc, out);
        _sendJson(req, 200, out);
    });
    POST_BODY_MOD("/api/subghz/gpio", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        JsonDocument doc;
        if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
            _sendJson(req, 400, "{\"error\":\"JSON\"}"); return;
        }
        // Helper: parse pin from string or int
        auto toPin = [&](const char* key, uint8_t def) -> uint8_t {
            if (doc[key].is<int>())         return doc[key].as<uint8_t>();
            if (doc[key].is<const char*>()) { String sv=doc[key].as<String>(); if(sv.length()>0)return(uint8_t)sv.toInt(); }
            return def;
        };
        SubGhzGpioConfig cfg;
        cfg.gdo0   = toPin("gdo0", 34);
        cfg.gdo2   = toPin("gdo2", 35);
        cfg.cs     = toPin("cs",   32);
        cfg.sck    = toPin("sck",  18);
        cfg.mosi   = toPin("mosi", 23);
        cfg.miso   = toPin("miso", 19);
        // spiBus: UI sends "vspi"/"hspi" string
        if (doc["spiBus"].is<const char*>()) {
            String bus = doc["spiBus"].as<String>();
            cfg.spiBus = (bus=="hspi"||bus=="1") ? 1 : 0;
        } else { cfg.spiBus = doc["spiBus"] | (uint8_t)0; }
        Serial.printf("[SUBGHZ] GPIO save: GDO0=%u CS=%u SCK=%u MOSI=%u MISO=%u bus=%s\n",
                      cfg.gdo0, cfg.cs, cfg.sck, cfg.mosi, cfg.miso, cfg.spiBus==1?"HSPI":"VSPI");
        subGhzModule.reinit(cfg);
        String status = subGhzModule.statusJson();
        _sendJson(req, 200, "{\"ok\":true,\"status\":" + status + "}");
    });

    // ── RFID ─────────────────────────────────────────────────
    _server.on("/api/rfid/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, rfidModule.statusJson());
    });
    _server.on("/api/rfid/gpio", HTTP_GET, [](AsyncWebServerRequest* req) {
        RfidGpioConfig c = rfidModule.loadGpioConfig();
        JsonDocument doc;
        doc["dataPin"]  = c.dataPin;
        doc["clkPin"]   = c.clkPin;
        doc["powerPin"] = c.powerPin;
        String out; serializeJson(doc, out);
        _sendJson(req, 200, out);
    });
    POST_BODY_MOD("/api/rfid/gpio", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        JsonDocument doc;
        if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
            _sendJson(req, 400, "{\"error\":\"JSON\"}"); return;
        }
        auto toPin = [&](const char* key, uint8_t def) -> uint8_t {
            if (doc[key].is<int>())         return doc[key].as<uint8_t>();
            if (doc[key].is<const char*>()) { String sv=doc[key].as<String>(); if(sv.length()>0)return(uint8_t)sv.toInt(); }
            return def;
        };
        RfidGpioConfig cfg;
        // UI sends: data, clk, mod, pwr  (NOT dataPin/clkPin/powerPin)
        cfg.dataPin  = toPin("data",  toPin("dataPin",  36));
        cfg.clkPin   = toPin("clk",   toPin("clkPin",   39));
        cfg.powerPin = toPin("pwr",   toPin("powerPin", 26));
        Serial.printf("[RFID] GPIO save: DATA=%u CLK=%u PWR=%u\n",
                      cfg.dataPin, cfg.clkPin, cfg.powerPin);
        rfidModule.reinit(cfg);
        String status = rfidModule.statusJson();
        _sendJson(req, 200, "{\"ok\":true,\"status\":" + status + "}");
    });

    // ── NFC ──────────────────────────────────────────────────
    _server.on("/api/nfc/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendJson(req, 200, nfcModule.statusJson());
    });
    _server.on("/api/nfc/gpio", HTTP_GET, [](AsyncWebServerRequest* req) {
        NfcGpioConfig c = nfcModule.loadGpioConfig();
        JsonDocument doc;
        doc["iface"]  = (int)c.iface;
        doc["spiBus"] = c.spiBus;
        doc["sda"]    = c.sda;   doc["scl"]   = c.scl;
        doc["ss"]     = c.ss;    doc["irq"]   = c.irq;
        doc["reset"]  = c.reset;
        doc["sck"]    = c.sck;   doc["mosi"]  = c.mosi;
        doc["miso"]   = c.miso;
        String out; serializeJson(doc, out);
        _sendJson(req, 200, out);
    });
        POST_BODY_MOD("/api/nfc/gpio", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        JsonDocument doc;
        if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
            _sendJson(req, 400, "{\"error\":\"JSON\"}"); return;
        }
        auto toPin = [&](const char* key, uint8_t def) -> uint8_t {
            if (doc[key].is<int>())         return doc[key].as<uint8_t>();
            if (doc[key].is<const char*>()) { String sv=doc[key].as<String>(); if(sv.length()>0)return(uint8_t)sv.toInt(); }
            return def;
        };
        NfcGpioConfig cfg;
        // iface: UI sends "i2c"/"spi"/"uart" string
        if (doc["iface"].is<const char*>()) {
            String iface = doc["iface"].as<String>();
            if      (iface=="spi")  cfg.iface = NfcIface::SPI;
            else if (iface=="uart") cfg.iface = NfcIface::UART;
            else                    cfg.iface = NfcIface::I2C;
        } else { cfg.iface = (NfcIface)(doc["iface"] | 0); }
        // spiBus: UI sends "vspi"/"hspi" string
        if (doc["spiBus"].is<const char*>()) {
            String bus = doc["spiBus"].as<String>();
            cfg.spiBus = (bus=="hspi"||bus=="1") ? 1 : 0;
        } else { cfg.spiBus = doc["spiBus"] | (uint8_t)0; }
        // I2C pins
        cfg.sda   = toPin("sda",   21);
        cfg.scl   = toPin("scl",   22);
        cfg.irq   = toPin("irq",   13);
        // UI sends "rsto" for reset pin
        cfg.reset = toPin("rsto",  toPin("reset", 33));
        // SPI pins
        cfg.ss    = toPin("ss",    13);
        cfg.sck   = toPin("sck",   18);
        cfg.mosi  = toPin("mosi",  23);
        cfg.miso  = toPin("miso",  19);
        // SPI IRQ/RSTO (separate fields in UI: nfc-irq-spi, nfc-rsto-spi)
        if (doc["irqSpi"].is<const char*>()||doc["irqSpi"].is<int>())
            cfg.irq   = toPin("irqSpi", cfg.irq);
        if (doc["rstoSpi"].is<const char*>()||doc["rstoSpi"].is<int>())
            cfg.reset = toPin("rstoSpi", cfg.reset);
        // UART pins
        cfg.tx    = toPin("tx",    17);
        cfg.rx    = toPin("rx",    16);
        Serial.printf("[NFC] GPIO save: iface=%d SDA=%u SCL=%u IRQ=%u RESET=%u\n",
                      (int)cfg.iface, cfg.sda, cfg.scl, cfg.irq, cfg.reset);
        nfcModule.saveGpioConfig(cfg);
        nfcModule.reinit();
        String status = nfcModule.statusJson();
        _sendJson(req, 200, "{\"ok\":true,\"status\":" + status + "}");
    });

    // ── LED ──────────────────────────────────────────────────
    _server.on("/api/led/gpio", HTTP_GET, [](AsyncWebServerRequest* req) {
        LedConfig c = sysModule.loadLedConfig();
        JsonDocument doc;
        doc["type"]       = (int)c.type;
        doc["mode"]       = (int)c.mode;
        doc["dataPin"]    = c.dataPin;
        doc["numLeds"]    = c.numLeds;
        doc["r"]          = c.r;
        doc["g"]          = c.g;
        doc["b"]          = c.b;
        doc["brightness"] = c.brightness;
        String out; serializeJson(doc, out);
        _sendJson(req, 200, out);
    });
    POST_BODY_MOD("/api/led/gpio", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        JsonDocument doc;
        if (deserializeJson(doc, d, l) != DeserializationError::Ok) {
            _sendJson(req, 400, "{\"error\":\"JSON\"}"); return;
        }
        LedConfig cfg;
        cfg.type       = (LedType)(doc["type"]       | 0);
        cfg.mode       = (LedMode)(doc["mode"]       | 0);
        cfg.dataPin    = doc["dataPin"]    | (uint8_t)2;
        cfg.numLeds    = doc["numLeds"]    | (uint8_t)8;
        cfg.r          = doc["r"]          | (uint8_t)255;
        cfg.g          = doc["g"]          | (uint8_t)0;
        cfg.b          = doc["b"]          | (uint8_t)128;
        cfg.brightness = doc["brightness"] | (uint8_t)128;
        sysModule.setLedMode(cfg);
        _sendJson(req, 200, "{\"ok\":true}");
    });

    // ── Hardware summary ─────────────────────────────────────
    _server.on("/api/system/hardware", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"nrf24\":%s,\"subghz\":%s,\"rfid\":%s,"
                 "\"nfc\":%s,\"led\":%s}",
                 nrf24Module.isConnected()  ? "true":"false",
                 subGhzModule.isConnected() ? "true":"false",
                 rfidModule.isConnected()   ? "true":"false",
                 nfcModule.isConnected()    ? "true":"false",
                 sysModule.isLedActive()    ? "true":"false");
        _sendJson(req, 200, String(buf));
    });

    // ── System LED presets & diagnostics (SD integration) ────

    // GET /api/system/led/presets   — list saved LED presets on SD
    _server.on("/api/system/led/presets", HTTP_GET, [](AsyncWebServerRequest* req) {
        auto presets = sysModule.listLedPresets();
        String out = "[";
        for (size_t i = 0; i < presets.size(); i++) {
            if (i) out += ",";
            out += "\"" + presets[i] + "\"";
        }
        out += "]";
        _sendJson(req, 200, "{\"ok\":true,\"presets\":" + out + "}");
    });

    // POST /api/system/led/preset/save   body: {"name":"..."}
    POST_BODY_MOD("/api/system/led/preset/save", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String name = doc["name"] | "";
        if (name.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing name\"}"); return; }
        bool ok = sysModule.saveLedPreset(name);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Save preset failed\"}");
    });

    // POST /api/system/led/preset/load   body: {"name":"..."}
    POST_BODY_MOD("/api/system/led/preset/load", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String name = doc["name"] | "";
        if (name.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing name\"}"); return; }
        bool ok = sysModule.loadLedPreset(name);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Load preset failed\"}");
    });

    // POST /api/system/led/backup   body: {"tag":"..."}
    POST_BODY_MOD("/api/system/led/backup", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String tag = doc["tag"] | "";
        if (tag.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing tag\"}"); return; }
        bool ok = sysModule.backupLedConfigToSD(tag);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"LED backup failed\"}");
    });

    // POST /api/system/led/restore   body: {"tag":"..."}
    POST_BODY_MOD("/api/system/led/restore", [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
        if (!authMgr.checkAuth(req)) return;
        JsonDocument doc; deserializeJson(doc, d, l);
        String tag = doc["tag"] | "";
        if (tag.isEmpty()) { _sendJson(req, 400, "{\"error\":\"Missing tag\"}"); return; }
        bool ok = sysModule.restoreLedConfigFromSD(tag);
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"LED restore failed\"}");
    });

    // GET /api/system/diagnostics   — dump diagnostics snapshot to SD and return status
    _server.on("/api/system/diagnostics", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool ok = sysModule.dumpDiagnosticsToSD();
        _sendJson(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"Diagnostics dump failed\"}");
    });
}

// ============================================================
//  Module Enable/Disable Toggle API
// ============================================================

// GET /api/modules/status - returns enabled state of all modules
// POST /api/modules/toggle - body: {module:"nfc"|"rfid"|"nrf24"|"subghz", enabled:bool}
void WebUI::setupModuleToggleRoutes() {

    _server.on("/api/modules/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            String j = "{";
            j += "\"nfc\":"    + String(nfcModule.isEnabled()    ? "true":"false") + ",";
            j += "\"nfcHw\":"  + String(nfcModule.isConnected()  ? "true":"false") + ",";
            j += "\"rfid\":"   + String(rfidModule.isEnabled()   ? "true":"false") + ",";
            j += "\"rfidHw\":" + String(rfidModule.isConnected() ? "true":"false") + ",";
            j += "\"nrf24\":"  + String(nrf24Module.isEnabled()  ? "true":"false") + ",";
            j += "\"nrf24Hw\":"+ String(nrf24Module.isConnected()? "true":"false") + ",";
            j += "\"subghz\":" + String(subGhzModule.isEnabled() ? "true":"false") + ",";
            j += "\"subghzHw\":"+ String(subGhzModule.isConnected()? "true":"false");
            j += "}";
            _sendJson(req, 200, j);
        });

    _server.addHandler(new AsyncCallbackJsonWebHandler("/api/modules/toggle",
        [](AsyncWebServerRequest* req, JsonVariant& body) {
            if (!authMgr.checkAuth(req)) return;
            String mod = body["module"] | "";
            bool   en  = body["enabled"] | false;

            if (mod != "nfc" && mod != "rfid" &&
                mod != "nrf24" && mod != "subghz") {
                _sendJson(req, 400, "{\"error\":\"Unknown module\"}");
                return;
            }

            // ── Respond IMMEDIATELY - no blocking ─────────────
            // Hardware toggle runs on a one-shot background task
            // so the HTTP response is sent before any SPI/I2C ops
            auditMgr.logSystem(("MODULE_" + mod + (en?"_ON":"_OFF")).c_str());
            _sendJson(req, 200,
                "{\"ok\":true,\"module\":\"" + mod +
                "\",\"enabled\":" + (en ? "true" : "false") + "}");

            // ── Background task - hardware safe toggle ─────────
            struct ToggleArgs { char mod[12]; bool en; };
            ToggleArgs* args = new ToggleArgs;
            mod.toCharArray(args->mod, sizeof(args->mod));
            args->en = en;

            xTaskCreate([](void* p) {
                ToggleArgs* a = static_cast<ToggleArgs*>(p);
                vTaskDelay(pdMS_TO_TICKS(80));  // let HTTP send first
                String m(a->mod);
                if      (m == "nfc")    nfcModule.setEnabled(a->en);
                else if (m == "rfid")   rfidModule.setEnabled(a->en);
                else if (m == "nrf24")  nrf24Module.setEnabled(a->en);
                else if (m == "subghz") subGhzModule.setEnabled(a->en);
                Serial.printf("[MOD] %s -> %s\n", a->mod, a->en ? "ON" : "OFF");
                delete a;
                vTaskDelete(NULL);
            }, "mod_toggle", 8192, args, 1, nullptr);
        }));
}
