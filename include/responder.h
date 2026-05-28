#pragma once
// ============================================================
//  responder.h  -  LLMNR/NBNS/SMB Responder (ported from Bruce)
//  Captures Windows NTLMv2 hashes over WiFi
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct ResponderCapture {
    String protocol;   // "NBNS" or "LLMNR" or "SMB"
    String username;
    String domain;
    String queryName;
    String clientIp;
    String ntlmHash;   // NTLMv2 hash string for hashcat
    uint32_t timestamp = 0;
};

class Responder {
public:
    void   begin();
    void   loop();

    void   start(const String& hostname   = "FILESERVER",
                 const String& domain     = "WORKGROUP",
                 const String& dnsDomain  = "local");
    void   stop();
    bool   isActive() const { return _active; }

    const std::vector<ResponderCapture>& captures() const { return _captures; }
    void  clearCaptures();
    String capturesJson() const;
    String statusJson()   const;

private:
    bool      _active   = false;
    String    _hostname = "FILESERVER";
    String    _domain   = "WORKGROUP";
    String    _dns      = "local";
    uint32_t  _startMs  = 0;

    WiFiUDP   _nbnsUdp;
    WiFiUDP   _llmnrUdp;
    WiFiServer* _smbServer = nullptr;

    std::vector<ResponderCapture> _captures;
    // Guards _captures: _addCapture() runs from loop() (Core 1) while
    // capturesJson()/statusJson() iterate from web handlers (Core 0).
    SemaphoreHandle_t _capMux = nullptr;

    // NBNS (port 137)
    void _handleNbns();
    void _sendNbnsResponse(const IPAddress& dst, uint16_t txId,
                           const uint8_t* encodedName);

    // LLMNR (port 5355)
    void _handleLlmnr();
    void _sendLlmnrResponse(const IPAddress& dst, uint16_t txId,
                            const String& name);

    // SMB/NTLM (port 445)
    void _handleSmb();
    // Runs the (blocking, up-to-5s) SMB/NTLM conversation for one client in a
    // detached task so loop() is never stalled. Takes the client by value.
    void _smbConversation(WiFiClient client);
    void _buildNtlmType2(uint8_t* challenge, uint8_t* buf, uint16_t* len);

    void _encodeNetBiosName(const char* name, uint8_t out[32]);
    IPAddress _myIp() const;
    void _addCapture(const String& proto, const String& user,
                     const String& domain, const String& query,
                     const String& clientIp, const String& hash = "");
};

extern Responder responder;
