// ============================================================
//  web_server_batch3.cpp  -  Batch 3
//  1. Auth Routes (/api/v1/auth/*)
//  2. Captive Portal (DNS + OS detection routes)
//  4. Watchdog routes
// ============================================================
#include "web_server.h"
#include "auth_manager.h"
#include "audit_manager.h"

#include "wifi_manager.h"
#include "watchdog_manager.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define DNS_PORT 53

static void sendB3(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    req->send(r);
}

static String* _getB3Buf(AsyncWebServerRequest* req) {
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
static void _freeB3Buf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define B3_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_getB3Buf(req)->length() + l > HTTP_MAX_BODY) { \
                _freeB3Buf(req); \
                sendB3(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _getB3Buf(req)->concat((char*)d, l); \
            bool last = (t > 0) ? (i + l >= t) : (i == 0); \
            if (last) { \
                String* buf = _getB3Buf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _freeB3Buf(req); \
            } \
        })

// ── Auth Routes ───────────────────────────────────────────────
void WebUI::setupAuthRoutes() {
    B3_POST("/api/v1/auth/login",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l) {
            handleAuthLogin(r, d, l); });
    B3_POST("/api/v1/auth/logout",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l) {
            handleAuthLogout(r, d, l); });
    B3_POST("/api/v1/auth/password",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l) {
            handleAuthPassword(r, d, l); });
    B3_POST("/api/v1/auth/config",
        [this](AsyncWebServerRequest* r, uint8_t* d, size_t l) {
            handleAuthConfig(r, d, l); });
    _server.on("/api/v1/auth/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            AsyncWebServerResponse* r = req->beginResponse(
                200, "application/json", authMgr.statusJson());
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });
    Serial.println("[WEB] Auth routes registered");
}

void WebUI::handleAuthLogin(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendB3(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    String user = body["username"] | (const char*)"";
    String pass = body["password"] | (const char*)"";
    if (user.isEmpty() || pass.isEmpty()) {
        sendB3(req, 400, "{\"error\":\"username and password required\"}"); return;
    }
    String hint  = req->client()->remoteIP().toString();
    String token = authMgr.login(user, pass, hint);
    if (token.isEmpty()) {
        sendB3(req, 401, "{\"error\":\"Invalid credentials or locked\"}"); return;
    }
    sendB3(req, 200,
        "{\"ok\":true,\"token\":\"" + token + "\""
        + ",\"ttl\":" + (AUTH_TOKEN_TTL_MS / 1000)
        + ",\"firstLogin\":" + (authMgr.isFirstLogin() ? "true" : "false") + "}");
}

void WebUI::handleAuthLogout(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    authMgr.logout(AuthManager::extractBearer(req));
    sendB3(req, 200, "{\"ok\":true}");
}

void WebUI::handleAuthPassword(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (!authMgr.checkAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendB3(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    String oldP = body["oldPassword"] | (const char*)"";
    String newP = body["newPassword"] | (const char*)"";
    if (oldP.isEmpty() || newP.isEmpty()) {
        sendB3(req, 400, "{\"error\":\"oldPassword and newPassword required\"}"); return;
    }
    if (newP.length() < 6) {
        sendB3(req, 400, "{\"error\":\"Min 6 characters\"}"); return;
    }
    if (!authMgr.changePassword(oldP, newP)) {
        sendB3(req, 401, "{\"error\":\"Wrong old password\"}"); return;
    }
    sendB3(req, 200, "{\"ok\":true,\"message\":\"Password changed. Login again.\"}");
}

void WebUI::handleAuthConfig(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    if (authMgr.isAuthEnabled() && !authMgr.checkAuth(req)) return;
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendB3(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    if (body["enabled"].is<bool>())
        authMgr.setAuthEnabled(body["enabled"].as<bool>());
    sendB3(req, 200,
        "{\"ok\":true,\"enabled\":"
        + String(authMgr.isAuthEnabled() ? "true" : "false") + "}");
}

// ── Captive Portal ────────────────────────────────────────────
void WebUI::setupCaptivePortal() {
    auto redir = [](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    };
    _server.on("/hotspot-detect.html", HTTP_GET, redir);
    _server.on("/generate_204",        HTTP_GET, redir);
    _server.on("/ncsi.txt",            HTTP_GET, redir);
    _server.on("/connecttest.txt",     HTTP_GET, redir);
    _server.on("/canonical.html",      HTTP_GET, redir);
    Serial.println("[WEB] Captive Portal routes registered");
}

void WebUI::startCaptivePortal() {
    IPAddress apIP(192, 168, 4, 1);
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(DNS_PORT, "*", apIP);
    _captiveActive = true;
    Serial.println("[WEB] Captive Portal DNS started");
}

void WebUI::stopCaptivePortal() {
    if (!_captiveActive) return;
    _dns.stop();
    _captiveActive = false;
    Serial.println("[WEB] Captive Portal DNS stopped");
}

void WebUI::loopCaptivePortal() {
    if (_captiveActive) _dns.processNextRequest();
}

// ── Watchdog Routes ───────────────────────────────────────────
void WebUI::setupWatchdogRoutes() {
    _server.on("/api/v1/watchdog/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            AsyncWebServerResponse* r = req->beginResponse(
                200, "application/json", wdtMgr.statusJson());
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    _server.on("/api/v1/watchdog/crashes", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            AsyncWebServerResponse* r = req->beginResponse(
                200, "application/json", wdtMgr.crashLogJson());
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    // POST /api/v1/watchdog/config - enable/disable HW watchdog, set heap threshold
    B3_POST("/api/v1/watchdog/config",
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument body;
            if (deserializeJson(body, d, l) != DeserializationError::Ok) {
                sendB3(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
            }
            if (body["hwEnabled"].is<bool>())
                wdtMgr.setHwEnabled(body["hwEnabled"].as<bool>());
            if (body["heapThreshold"].is<uint32_t>())
                wdtMgr.setHeapThreshold(body["heapThreshold"].as<uint32_t>());
            if (body["perfMode"].is<const char*>()) {
                String pm = body["perfMode"].as<String>();
                if (pm == "power_save") wdtMgr.setPerfMode(WdtPerfMode::POWER_SAVE);
                else if (pm == "turbo") wdtMgr.setPerfMode(WdtPerfMode::TURBO);
                else                   wdtMgr.setPerfMode(WdtPerfMode::NORMAL);
            }
            sendB3(req, 200,
                String("{\"ok\":true,\"hwEnabled\":") +
                (wdtMgr.hwEnabled() ? "true" : "false") +
                ",\"perfMode\":\"" + wdtMgr.perfModeStr() + "\"}");
        });

    // POST /api/v1/watchdog/clear-crashes - delete crash log
    _server.on("/api/v1/watchdog/clear-crashes", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            if (LittleFS.exists(WDT_CRASH_FILE)) LittleFS.remove(WDT_CRASH_FILE);
            if (LittleFS.exists(WDT_BOOT_CTR_FILE)) LittleFS.remove(WDT_BOOT_CTR_FILE);
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json",
                "{\"ok\":true,\"note\":\"Crash log and boot counter cleared\"}");
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    Serial.println("[WEB] Watchdog routes registered");
}
