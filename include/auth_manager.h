#pragma once
// ============================================================
//  auth_manager.h  -  Batch 3: Web Panel Authentication
//
//  Session-token based auth:
//    POST /api/v1/auth/login  -> returns token
//    All protected routes need: Authorization: Bearer <token>
//
//  Config: /auth_config.json
//  Default credentials: admin / irremote123
// ============================================================
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define AUTH_CFG_FILE       "/auth_config.json"
#define AUTH_TOKEN_TTL_MS   3600000UL   // 1 hour
#define AUTH_MAX_SESSIONS   4
#define AUTH_MAX_FAILURES   5
#define AUTH_LOCKOUT_MS     300000UL    // 5 min lockout
#define AUTH_TOKEN_LEN      32          // hex chars in token
#define AUTH_TAG            "[AUTH]"

struct AuthSession {
    String        token;
    unsigned long createdAt;
    String        clientHint;
};

class AuthManager {
public:
    AuthManager();
    void   begin();
    void   loop();   // expire old sessions

    // Login - returns token or empty string on failure
    String login(const String& username,
                 const String& password,
                 const String& clientHint = "");

    bool   logout       (const String& token);
    bool   validateToken(const String& token);

    // Extract Bearer token from request header
    static String extractBearer(AsyncWebServerRequest* req);

    // Middleware - returns true=authorized, false=rejected+401 sent
    bool   checkAuth(AsyncWebServerRequest* req);

    bool   loadConfig();
    bool   saveConfig();
    bool   changePassword(const String& oldPass, const String& newPass);

    bool   isAuthEnabled() const { return _enabled; }
    void   setAuthEnabled(bool en);
    bool   isFirstLogin()  const { return _firstLogin; }
    size_t sessionCount()  const { return _sessions.size(); }
    void   clearSessions();

    String statusJson() const;
    String configJson() const;

private:
    bool    _enabled;
    bool    _firstLogin;
    String  _username;
    String  _passwordHash;
    String  _defaultPass;      // S-01: MAC-derived default, cleared after loadConfig finds saved hash
    uint8_t _failCount;
    unsigned long _lockedUntil;

    std::vector<AuthSession> _sessions;
    // Guards _sessions: _cleanExpired() runs from loop() (Core 1) while
    // login/logout/validateToken run from web handlers (Core 0). Recursive
    // because validateToken() calls _cleanExpired() while it could itself be
    // called under a lock in future. Created in begin().
    SemaphoreHandle_t _sessionMux = nullptr;

    String _hashPassword (const String& pass) const;
    String _generateToken() const;
    void   _cleanExpired ();
};

extern AuthManager authMgr;
