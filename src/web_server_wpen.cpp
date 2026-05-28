// ============================================================
//  web_server_wpen.cpp  —  WiFi Pen Module REST routes  v4.0
//
//  POST /api/wpen/enable              {"enabled":true/false}
//  POST /api/wpen/scan                trigger scan
//  GET  /api/wpen/aps                 scanned AP list JSON
//  GET  /api/wpen/aps.csv             scanned AP list CSV download
//  POST /api/wpen/attack/start        {"type":0-7,"method":0-4,"apIdx":N,"timeout":30}
//  POST /api/wpen/attack/stop
//  POST /api/wpen/attack/reset
//  GET  /api/wpen/status              state JSON
//  GET  /api/wpen/capture/pcap        download .pcap
//  GET  /api/wpen/capture/hccapx      download .hccapx
//  GET  /api/wpen/capture/pmkid       download hashcat pmkid txt
//  GET  /api/wpen/capture/hcx22000    download hcxtools .22000 format
//  POST /api/wpen/hop                 {"enabled":true/false}
//  GET  /api/wpen/clients             detected client STA list JSON
//  GET  /api/wpen/probes              probe-sniffer list JSON
//  POST /api/wpen/target_client       {"mac":"aa:bb:cc:dd:ee:ff"}
//  DELETE /api/wpen/target_client     clear target client
//  POST /api/wpen/auto_ch_lock        {"enabled":true/false}
//  GET  /api/wpen/hashcat_cmd         {"cmd":"..."}
//  POST /api/wpen/capture/save_sd     save PCAP to SD card
//  GET  /api/wpen/log                 attack log JSON array
//  POST /api/wpen/targets/save        persist AP list to LittleFS
//  POST /api/wpen/targets/load        restore AP list from LittleFS
//  POST /api/wpen/multi_targets       {"targets":[0,1,2]}
//  GET  /api/wpen/auto_detect?apIdx=N {"type":N,"typeName":"pmkid"}
//  POST /api/wpen/deauth_rate         {"pps":5}
//  GET  /api/wpen/captive_pass        {"password":"..."}
//  POST /api/wpen/wps_probe           {"apIdx":N,"timeout":3000}
//  GET  /api/wpen/channel_util        channel utilization JSON
//  GET  /api/wpen/hidden_aps          hidden AP fingerprint JSON
//  GET  /api/wpen/mesh_aps            mesh/repeater detection JSON
//  GET  /api/wpen/vendor_ies          vendor IE list JSON
//  GET  /api/wpen/rssi_hist?apIdx=N   RSSI history JSON
//  GET  /api/wpen/profiles            attack profiles list JSON
//  POST /api/wpen/profiles/apply      {"profileIdx":0,"apIdx":N}
// ============================================================
#include "web_server.h"
#include "wifi_pen_module.h"
#include "auth_manager.h"
#include <ArduinoJson.h>

extern AuthManager authMgr;

static void _wpenJson(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

// BODY-BUFFER FIX: the prior WPEN_POST had NO buffering — handler ran
// once per TCP chunk with only that chunk's bytes (and would call
// req->send each time, producing multiple responses per request).
// Use the same getBodyBuf/freeBodyBuf pattern as web_server_modules.cpp.
static String* _wpenGetBodyBuf(AsyncWebServerRequest* req) {
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
static void _wpenFreeBodyBuf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define WPEN_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_wpenGetBodyBuf(req)->length() + l > HTTP_MAX_BODY) { \
                _wpenFreeBodyBuf(req); \
                _wpenJson(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _wpenGetBodyBuf(req)->concat((char*)d, l); \
            bool lastChunk = (t > 0) ? (i + l >= t) : (i == 0); \
            if (lastChunk) { \
                String* buf = _wpenGetBodyBuf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _wpenFreeBodyBuf(req); \
            } \
        })

void WebUI::setupWpenRoutes() {

    // ── Enable / disable ──────────────────────────────────────
    WPEN_POST("/api/wpen/enable",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            wifiPen.setEnabled(doc["enabled"] | false);
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Scan ──────────────────────────────────────────────────
    _server.on("/api/wpen/scan", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (!wifiPen.enabled()) {
                _wpenJson(req, 403, "{\"error\":\"module disabled\"}"); return;
            }
            bool ok = wifiPen.scan();
            _wpenJson(req, ok ? 200 : 409,
                      ok ? "{\"ok\":true}" : "{\"error\":\"attack running\"}");
        }
    );

    // ── AP list (JSON) ────────────────────────────────────────
    _server.on("/api/wpen/aps", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: gate scan results
            _wpenJson(req, 200, wifiPen.apListJson());
        }
    );

    // ── AP list (CSV download) — Feature 17 ──────────────────
    _server.on("/api/wpen/aps.csv", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: scan CSV download
            String csv = wifiPen.apListCsv();
            AsyncWebServerResponse* r = req->beginResponse(200, "text/csv", csv);
            r->addHeader("Content-Disposition",
                         "attachment; filename=\"aps.csv\"");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        }
    );

    // ── Attack start ──────────────────────────────────────────
    WPEN_POST("/api/wpen/attack/start",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            if (!wifiPen.enabled()) {
                _wpenJson(req, 403, "{\"error\":\"module disabled\"}"); return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            auto type   = (WpenAttackType)(doc["type"]   | 0);
            auto method = (WpenMethod)    (doc["method"] | 0);
            uint8_t idx = doc["apIdx"]   | 0;
            uint8_t tout= doc["timeout"] | 30;
            bool ok = wifiPen.startAttack(type, method, idx, tout);
            _wpenJson(req, ok ? 200 : 409,
                      ok ? "{\"ok\":true}" : "{\"error\":\"could not start\"}");
        }
    );

    // ── Attack stop ───────────────────────────────────────────
    _server.on("/api/wpen/attack/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            wifiPen.stopAttack();
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Attack reset ──────────────────────────────────────────
    _server.on("/api/wpen/attack/reset", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            wifiPen.resetAttack();
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Status ────────────────────────────────────────────────
    _server.on("/api/wpen/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _wpenJson(req, 200, wifiPen.statusJson());
        }
    );

    // ── Download PCAP ─────────────────────────────────────────
    _server.on("/api/wpen/capture/pcap", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: pcap with handshakes
            if (!wifiPen.hasPcap()) {
                _wpenJson(req, 404, "{\"error\":\"no pcap\"}"); return;
            }
            AsyncWebServerResponse* r = req->beginResponse_P(
                200, "application/octet-stream",
                wifiPen.pcapBuf(), wifiPen.pcapSize()
            );
            r->addHeader("Content-Disposition",
                         "attachment; filename=\"capture.pcap\"");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        }
    );

    // ── Download HCCAPX ───────────────────────────────────────
    _server.on("/api/wpen/capture/hccapx", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            if (!wifiPen.hasHccapx()) {
                _wpenJson(req, 404, "{\"error\":\"no hccapx\"}"); return;
            }
            AsyncWebServerResponse* r = req->beginResponse_P(
                200, "application/octet-stream",
                (const uint8_t*)wifiPen.hccapx(), sizeof(hccapx_t)
            );
            r->addHeader("Content-Disposition",
                         "attachment; filename=\"capture.hccapx\"");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        }
    );

    // ── Download PMKID (hashcat legacy format) ────────────────
    _server.on("/api/wpen/capture/pmkid", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            String pmkid = wifiPen.pmkidHashcat();
            if (!pmkid.length()) {
                _wpenJson(req, 404, "{\"error\":\"no pmkid\"}"); return;
            }
            AsyncWebServerResponse* r = req->beginResponse(
                200, "text/plain", pmkid
            );
            r->addHeader("Content-Disposition",
                         "attachment; filename=\"pmkid.txt\"");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        }
    );

    // ── Download PMKID hcxtools .22000 format — Feature 16 ───
    _server.on("/api/wpen/capture/hcx22000", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            String data = wifiPen.pmkidHcx22000();
            if (!data.length()) {
                _wpenJson(req, 404, "{\"error\":\"no pmkid\"}"); return;
            }
            AsyncWebServerResponse* r = req->beginResponse(
                200, "text/plain", data
            );
            r->addHeader("Content-Disposition",
                         "attachment; filename=\"pmkid.22000\"");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        }
    );

    // ── Channel hop toggle ────────────────────────────────────
    WPEN_POST("/api/wpen/hop",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            wifiPen.setHop(doc["enabled"] | false);
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Client list ───────────────────────────────────────────
    _server.on("/api/wpen/clients", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: client MAC list
            _wpenJson(req, 200, wifiPen.clientListJson());
        }
    );

    // ── Probe sniffer list ────────────────────────────────────
    _server.on("/api/wpen/probes", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: probed SSIDs leak
            _wpenJson(req, 200, wifiPen.probeListJson());
        }
    );

    // ── Set target client (directed deauth) ───────────────────
    WPEN_POST("/api/wpen/target_client",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            const char* macStr = doc["mac"] | "";
            if (!macStr || strlen(macStr) < 17) {
                _wpenJson(req, 400, "{\"error\":\"invalid mac\"}"); return;
            }
            unsigned int b[6] = {};
            int parsed = sscanf(macStr, "%x:%x:%x:%x:%x:%x",
                                &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]);
            if (parsed != 6) {
                _wpenJson(req, 400, "{\"error\":\"invalid mac format\"}"); return;
            }
            uint8_t mac[6];
            for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
            wifiPen.setTargetClient(mac);
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Clear target client ───────────────────────────────────
    _server.on("/api/wpen/target_client", HTTP_DELETE,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            wifiPen.clearTargetClient();
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Auto channel lock ─────────────────────────────────────
    WPEN_POST("/api/wpen/auto_ch_lock",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            wifiPen.setAutoChannelLock(doc["enabled"] | false);
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Hashcat command ───────────────────────────────────────
    _server.on("/api/wpen/hashcat_cmd", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            String cmd = wifiPen.hashcatCmd();
            String json = "{\"cmd\":\"" + cmd + "\"}";
            _wpenJson(req, 200, json);
        }
    );

    // ── Save PCAP to SD ───────────────────────────────────────
    _server.on("/api/wpen/capture/save_sd", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            bool ok = wifiPen.savePcapToSd("/wpen/capture.pcap");
            if (ok) {
                _wpenJson(req, 200, "{\"ok\":true,\"path\":\"/wpen/capture.pcap\"}");
            } else {
                _wpenJson(req, 500, "{\"error\":\"save failed — check SD and pcap data\"}");
            }
        }
    );

    // ── Attack log ────────────────────────────────────────────
    _server.on("/api/wpen/log", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: attack log leak
            _wpenJson(req, 200, wifiPen.attackLogJson());
        }
    );

    // ── Save targets to LittleFS ──────────────────────────────
    _server.on("/api/wpen/targets/save", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            wifiPen.saveTargets();
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Load targets from LittleFS ────────────────────────────
    _server.on("/api/wpen/targets/load", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            wifiPen.loadTargets();
            _wpenJson(req, 200,
                String("{\"ok\":true,\"apCount\":") + String(wifiPen.apCount()) + "}");
        }
    );

    // ── Multi-target attack — Feature 2 ──────────────────────
    WPEN_POST("/api/wpen/multi_targets",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            JsonArray arr = doc["targets"];
            if (arr.isNull()) {
                _wpenJson(req, 400, "{\"error\":\"missing targets array\"}"); return;
            }
            uint8_t idxList[8];
            uint8_t count = 0;
            for (JsonVariant v : arr) {
                if (count >= 8) break;
                idxList[count++] = (uint8_t)(v | 0);
            }
            wifiPen.setMultiTargets(idxList, count);
            _wpenJson(req, 200,
                String("{\"ok\":true,\"count\":") + String(count) + "}");
        }
    );

    // ── WPA2/WPA3 auto-detect — Feature 3 ────────────────────
    _server.on("/api/wpen/auto_detect", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            uint8_t apIdx = 0;
            if (req->hasParam("apIdx")) {
                apIdx = (uint8_t)req->getParam("apIdx")->value().toInt();
            }
            WpenAttackType t = wifiPen.autoDetectAttackType(apIdx);
            const char* name = (t == WpenAttackType::PMKID) ? "pmkid" : "handshake";
            String json = "{\"type\":" + String((uint8_t)t)
                        + ",\"typeName\":\"" + String(name) + "\"}";
            _wpenJson(req, 200, json);
        }
    );

    // ── Rate-limited deauth — Feature 5 ──────────────────────
    WPEN_POST("/api/wpen/deauth_rate",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            uint8_t pps = doc["pps"] | 10;
            wifiPen.setDeauthRate(pps);
            _wpenJson(req, 200, "{\"ok\":true}");
        }
    );

    // ── Captive portal password — Feature 6 ──────────────────
    _server.on("/api/wpen/captive_pass", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX: captured passwords!
            String pw = wifiPen.captivePassword();
            // Basic JSON string escaping
            pw.replace("\\", "\\\\");
            pw.replace("\"", "\\\"");
            _wpenJson(req, 200, "{\"password\":\"" + pw + "\"}");
        }
    );

    // ── WPS probe — Feature 9 ─────────────────────────────────
    WPEN_POST("/api/wpen/wps_probe",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            uint8_t  apIdx   = doc["apIdx"]   | 0;
            uint32_t timeout = doc["timeout"] | 3000;
            bool result = wifiPen.probeForWps(apIdx, timeout);
            _wpenJson(req, 200,
                String("{\"wps\":") + (result ? "true" : "false") + "}");
        }
    );

    // ── Channel utilization — Feature 10 ─────────────────────
    _server.on("/api/wpen/channel_util", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _wpenJson(req, 200, wifiPen.channelUtilJson());
        }
    );

    // ── Hidden AP fingerprint — Feature 11 ───────────────────
    _server.on("/api/wpen/hidden_aps", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _wpenJson(req, 200, wifiPen.hiddenApJson());
        }
    );

    // ── Mesh/repeater detection — Feature 12 ─────────────────
    _server.on("/api/wpen/mesh_aps", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _wpenJson(req, 200, wifiPen.meshApJson());
        }
    );

    // ── Vendor IE list — Feature 13 ──────────────────────────
    _server.on("/api/wpen/vendor_ies", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _wpenJson(req, 200, wifiPen.vendorIEJson());
        }
    );

    // ── RSSI history — Feature 14 ────────────────────────────
    _server.on("/api/wpen/rssi_hist", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            uint8_t apIdx = 0;
            if (req->hasParam("apIdx")) {
                apIdx = (uint8_t)req->getParam("apIdx")->value().toInt();
            }
            _wpenJson(req, 200, wifiPen.rssiHistJson(apIdx));
        }
    );

    // ── Attack profiles list — Feature 15 ────────────────────
    _server.on("/api/wpen/profiles", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;  // AUTH FIX
            _wpenJson(req, 200, wifiPen.profileListJson());
        }
    );

    // ── Apply attack profile — Feature 15 ────────────────────
    WPEN_POST("/api/wpen/profiles/apply",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            if (!wifiPen.enabled()) {
                _wpenJson(req, 403, "{\"error\":\"module disabled\"}"); return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, d, l)) {
                _wpenJson(req, 400, "{\"error\":\"bad json\"}"); return;
            }
            uint8_t profileIdx = doc["profileIdx"] | 0;
            uint8_t apIdx      = doc["apIdx"]       | 0;
            bool ok = wifiPen.applyProfile(profileIdx, apIdx);
            _wpenJson(req, ok ? 200 : 409,
                      ok ? "{\"ok\":true}" : "{\"error\":\"could not apply profile\"}");
        }
    );
}
