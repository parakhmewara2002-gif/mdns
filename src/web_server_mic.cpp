// ============================================================
//  web_server_mic.cpp  -  Hybrid Mic API + WS stream
// ============================================================
#include "web_server.h"
#include "mic_module.h"
#include "auth_manager.h"
#include "audit_manager.h"
#include "sd_manager.h"
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>

extern AuthManager  authMgr;
extern AuditManager auditMgr;
extern WebUI        webUI;
// ── WS Audio stream task (Core 0) ────────────────────────────
static TaskHandle_t _micTask = nullptr;
static bool         _micStop = false;

static void micStreamTask(void*) {
    static uint8_t buf[MIC_STREAM_CHUNK + 8];
    while (!_micStop) {
        size_t n = micModule.readChunk(buf + 6, MIC_STREAM_CHUNK);
        if (n > 0) {
            buf[0]='M'; buf[1]='I'; buf[2]='C'; buf[3]='\0';
            buf[4]=micModule.volumeLevel();
            buf[5]=1; // always mono output from hybrid
            webUI.broadcastBinary(buf, n + 6);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    _micTask = nullptr;
    vTaskDelete(NULL);
}

// ── Routes ────────────────────────────────────────────────────
void WebUI::setupMicRoutes() {

    // GET /api/mic/status
    _server.on("/api/mic/status", HTTP_GET, [](AsyncWebServerRequest* req){
        sendJson(req, 200, micModule.statusJson());
    });

    // GET /api/mic/gpio
    _server.on("/api/mic/gpio", HTTP_GET, [](AsyncWebServerRequest* req){
        sendJson(req, 200, micModule.gpioJson());
    });

    // POST /api/mic/gpio - save full hybrid config
    _server.addHandler(new AsyncCallbackJsonWebHandler("/api/mic/gpio",
        [](AsyncWebServerRequest* req, JsonVariant& body){
            if (!authMgr.checkAuth(req)) return;

            MicConfig cfg;
            cfg.i2sEnabled  = body["i2sEnabled"]  | false;
            cfg.ws          = body["ws"]           | (uint8_t)25;
            cfg.sck         = body["sck"]          | (uint8_t)26;
            cfg.sd          = body["sd"]           | (uint8_t)33;
            cfg.stereo      = body["stereo"]       | false;
            cfg.sampleRate  = body["sampleRate"]   | (uint32_t)16000;
            cfg.mixMode     = (MicMixMode)(body["mixMode"] | (uint8_t)0);
            cfg.i2sWeight   = body["i2sWeight"]    | (uint8_t)60;
            cfg.masterGain  = body["masterGain"]   | (uint8_t)6;

            // Parse ADC mics array
            JsonArray adcArr = body["adc"].as<JsonArray>();
            for (uint8_t i = 0; i < MIC_ADC_MAX && i < adcArr.size(); i++) {
                cfg.adc[i].pin     = adcArr[i]["pin"]     | (uint8_t)0;
                cfg.adc[i].gain    = adcArr[i]["gain"]    | (uint8_t)6;
                cfg.adc[i].enabled = adcArr[i]["enabled"] | false;
            }

            // Pin conflict check for I2S pins
            static const uint8_t BAD[] = {1,3,4,6,7,8,9,10,11,12,14,15,18,19,20,23,24,27};
            if (cfg.i2sEnabled) {
                for (uint8_t b : BAD) {
                    if (cfg.ws==b||cfg.sck==b||cfg.sd==b) {
                        sendJson(req,409,
                            "{\"error\":\"I2S pin conflict: GPIO "+String(b)+"\"}");
                        return;
                    }
                }
            }
            // ADC pin check - must be GPIO 32-39
            static const uint8_t VALID_ADC[] = {32,33,34,35,36,39};
            for (uint8_t i = 0; i < MIC_ADC_MAX; i++) {
                if (!cfg.adc[i].enabled) continue;
                bool ok = false;
                for (auto v : VALID_ADC) if (cfg.adc[i].pin==v) { ok=true; break; }
                if (!ok) {
                    sendJson(req,409,
                        "{\"error\":\"ADC mic "+String(i+1)+": GPIO "+
                        String(cfg.adc[i].pin)+" not valid (use 32-39)\"}");
                    return;
                }
            }

            bool ok = micModule.applyConfig(cfg);
            auditMgr.logSystem("MIC_CONFIG_SAVED");
            if (ok)
                sendJson(req,200,
                    "{\"ok\":true,\"source\":\""+micModule.activeSourceStr()+"\"}");
            else
                sendJson(req,500,"{\"error\":\"Init failed - check wiring\"}");
        }));

    // POST /api/mic/stream/start
    _server.on("/api/mic/stream/start", HTTP_POST,
        [](AsyncWebServerRequest* req){
            if (!authMgr.checkAuth(req)) return;
            if (micModule.isStreaming()){
                sendJson(req,200,"{\"ok\":true,\"already\":true}"); return;
            }
            if (!micModule.startStream()){
                sendJson(req,500,"{\"error\":\"No mic source ready - configure GPIO first\"}");
                return;
            }
            _micStop = false;
            if (!_micTask)
                xTaskCreatePinnedToCore(micStreamTask,"mic_stream",
                    6144,nullptr,1,&_micTask,0);
            sendJson(req,200,
                "{\"ok\":true,\"source\":\""+micModule.activeSourceStr()+"\"}");
        });

    // POST /api/mic/stream/stop
    _server.on("/api/mic/stream/stop", HTTP_POST,
        [](AsyncWebServerRequest* req){
            if (!authMgr.checkAuth(req)) return;
            _micStop = true;
            micModule.stopStream();
            sendJson(req,200,"{\"ok\":true}");
        });

    // POST /api/mic/record/start?name=optional  (name also accepted in JSON body)
    _server.on("/api/mic/record/start", HTTP_POST,
        [](AsyncWebServerRequest* req){},   // completion handler (body arrives below)
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
            if (!authMgr.checkAuth(req)) return;
            if (micModule.isRecording()){
                sendJson(req,409,"{\"error\":\"Already recording\"}"); return;
            }
            // Accept name from URL query string first, then JSON body
            String name = req->hasParam("name") ? req->getParam("name")->value() : "";
            if (name.isEmpty() && len > 0) {
                JsonDocument bd;
                if (!deserializeJson(bd, data, len))
                    name = bd["name"] | "";
            }
            if (!micModule.startRecording(name)){
                sendJson(req,500,"{\"error\":\"Cannot open file - insert SD or free space\"}");
                return;
            }
            auditMgr.logSystem("MIC_REC_START");
            sendJson(req,200,
                "{\"ok\":true"
                ",\"file\":\""+micModule.recordingName()+"\""
                ",\"onSd\":"+(micModule.isRecordingOnSd()?"true":"false")+"}");
        });

    // POST /api/mic/record/stop
    _server.on("/api/mic/record/stop", HTTP_POST,
        [](AsyncWebServerRequest* req){
            if (!authMgr.checkAuth(req)) return;
            String   name  = micModule.recordingName();
            bool     onSd  = micModule.isRecordingOnSd();
            uint32_t bytes = micModule.recordingBytes();
            uint32_t secs  = micModule.recordingSecs();
            micModule.stopRecording();
            auditMgr.logSystem("MIC_REC_STOP");
            sendJson(req,200,
                "{\"ok\":true"
                ",\"file\":\""+name+"\""
                ",\"bytes\":"+String(bytes)+
                ",\"secs\":"+String(secs)+
                ",\"onSd\":"+(onSd?"true":"false")+"}");
        });

    // GET /api/mic/recordings
    _server.on("/api/mic/recordings", HTTP_GET,
        [](AsyncWebServerRequest* req){
            sendJson(req,200, micModule.listRecordingsJson());
        });

    // GET /api/mic/recordings/download?name=rec_123.wav
    _server.on("/api/mic/recordings/download", HTTP_GET,
        [](AsyncWebServerRequest* req){
            if (!authMgr.checkAuth(req)) return;
            if (!req->hasParam("name")){
                sendJson(req,400,"{\"error\":\"Missing name\"}"); return;
            }
            String name = req->getParam("name")->value();
            int sl = name.lastIndexOf('/');
            if (sl>=0) name = name.substring(sl+1);
            if (!name.endsWith(".wav")&&!name.endsWith(".WAV")){
                sendJson(req,400,"{\"error\":\"Only .wav files\"}"); return;
            }
            // SD first
            if (sdMgr.isAvailable()) {
                String p = "/recordings/"+name;
                if (sdMgr.exists(p)) {
                    File f = sdMgr.openForRead(p);
                    if (f) {
                        size_t sz = f.size();
                        auto* r = req->beginResponse("audio/wav", sz,
                            [f](uint8_t* buf,size_t maxLen,size_t) mutable->size_t{
                                return f.read(buf,maxLen);
                            });
                        r->addHeader("Content-Disposition",
                            "attachment; filename=\""+name+"\"");
                        r->addHeader("Access-Control-Allow-Origin","*");
                        req->send(r); return;
                    }
                }
            }
            // LittleFS fallback
            String lp = String(MIC_REC_DIR)+"/"+name;
            if (!LittleFS.exists(lp)){
                sendJson(req,404,"{\"error\":\"Not found\"}"); return;
            }
            auto* r = req->beginResponse(LittleFS,lp,"audio/wav");
            r->addHeader("Content-Disposition",
                "attachment; filename=\""+name+"\"");
            r->addHeader("Access-Control-Allow-Origin","*");
            req->send(r);
        });

    // DELETE /api/mic/recordings/delete?name=
    _server.on("/api/mic/recordings/delete", HTTP_DELETE,
        [](AsyncWebServerRequest* req){
            if (!authMgr.checkAuth(req)) return;
            if (!req->hasParam("name")){
                sendJson(req,400,"{\"error\":\"Missing name\"}"); return;
            }
            bool ok = micModule.deleteRecording(req->getParam("name")->value());
            sendJson(req, ok?200:404, ok?"{\"ok\":true}":"{\"error\":\"Not found\"}");
        });

    Serial.println("[WEB] Mic routes ready");
}

// Called from setupMicRoutes() - but defined separately for clarity

// ── /api/mic/pins - real-time GPIO conflict map ───────────────
void WebUI::setupMicPinsRoute() {
    _server.on("/api/mic/pins", HTTP_GET, [](AsyncWebServerRequest* req) {

        // ── Static system pins - always occupied ──────────────
        struct StaticPin { uint8_t gpio; const char* mod; const char* lbl; };
        static const StaticPin SYS[] = {
            {1,  "SYSTEM", "USB TX"},
            {3,  "SYSTEM", "USB RX"},
            {6,  "SYSTEM", "Flash"},
            {7,  "SYSTEM", "Flash"},
            {8,  "SYSTEM", "Flash"},
            {9,  "SYSTEM", "Flash"},
            {10, "SYSTEM", "Flash"},
            {11, "SYSTEM", "Flash"},
            {12, "SYSTEM", "Boot"},
            {15, "SYSTEM", "Boot"},
            {4,  "SD",     "SD CS"},
            {18, "SD",     "SD SCK"},
            {19, "SD",     "SD MISO"},
            {23, "SD",     "SD MOSI"},
            {14, "IR",     "IR RX"},
            {27, "IR",     "IR TX"},
        };
        static const uint8_t SYS_LEN = sizeof(SYS) / sizeof(SYS[0]);

        // ── Non-existent pins on WROOM-32 ─────────────────────
        static const uint8_t SKIP[] = {
            6,7,8,9,10,11,  // flash
            20,24            // not bonded
        };
        auto isSkip = [](uint8_t g) -> bool {
            for (uint8_t i = 0; i < 8; i++)
                if (SKIP[i] == g) return true;
            return false;
        };

        // ── ADC1 capable (work with WiFi ON) ──────────────────
        static const uint8_t ADC1[] = {32,33,34,35,36,39};
        auto isADC1 = [](uint8_t g) -> bool {
            for (uint8_t a : ADC1) if (a == g) return true;
            return false;
        };

        // ── Build occupied map ─────────────────────────────────
        bool     occ[40] = {};
        uint8_t  modIdx[40] = {};  // index into mod/lbl arrays
        // 0=FREE, 1=SYSTEM/IR/SD, 2=NFC, 3=NRF24, 4=SubGHz, 5=RFID

        struct ConflictEntry { uint8_t gpio; uint8_t src; const char* lbl; };
        static ConflictEntry dyn[20];
        uint8_t dynCount = 0;

        // Mark static
        for (uint8_t i = 0; i < SYS_LEN; i++) {
            uint8_t g = SYS[i].gpio;
            if (g < 40) { occ[g] = true; modIdx[g] = i; }
        }

        // ── Dynamic module configs ─────────────────────────────
        auto markPin = [&](uint8_t g, const char* lbl) {
            if (g == 0 || g >= 40) return;
            if (!occ[g] && dynCount < 20) {
                dyn[dynCount++] = {g, 0, lbl};
            }
            occ[g] = true;
        };

        // NFC
        {
            File f = LittleFS.open("/nfc_gpio.json","r");
            if (f) {
                JsonDocument d; deserializeJson(d,f); f.close();
                if (d["enabled"] | false) {
                    markPin(d["sda"]|21, "NFC SDA");
                    markPin(d["scl"]|22, "NFC SCL");
                    uint8_t irq = d["irq"]|0;
                    if (irq) markPin(irq, "NFC IRQ");
                }
            }
        }
        // NRF24
        {
            File f = LittleFS.open("/nrf24_gpio.json","r");
            if (f) {
                JsonDocument d; deserializeJson(d,f); f.close();
                if (d["enabled"] | false) {
                    markPin(d["ce"] |16, "NRF24 CE");
                    markPin(d["csn"]|17, "NRF24 CSN");
                    markPin(d["miso"]|26,"NRF24 MISO");
                }
            }
        }
        // SubGHz
        {
            File f = LittleFS.open("/subghz_gpio.json","r");
            if (f) {
                JsonDocument d; deserializeJson(d,f); f.close();
                if (d["enabled"] | false) {
                    markPin(d["cs"]  |32, "SubGHz CS");
                    markPin(d["gdo0"]|34, "SubGHz GDO0");
                    markPin(d["gdo2"]|35, "SubGHz GDO2");
                    markPin(d["miso"]|26, "SubGHz MISO");
                }
            }
        }
        // RFID
        {
            File f = LittleFS.open("/rfid_gpio.json","r");
            if (f) {
                JsonDocument d; deserializeJson(d,f); f.close();
                if (d["enabled"] | false) {
                    markPin(d["dataPin"]|36, "RFID DATA");
                    markPin(d["clkPin"] |39, "RFID CLK");
                }
            }
        }

        // ── Build label lookup for dynamic pins ───────────────
        // We need per-pin labels - use a flat lookup
        const char* dynLabel[40] = {};
        for (uint8_t i = 0; i < dynCount; i++)
            dynLabel[dyn[i].gpio] = dyn[i].lbl;

        // Module name from static table
        auto getMod = [&](uint8_t g) -> const char* {
            for (uint8_t i = 0; i < SYS_LEN; i++)
                if (SYS[i].gpio == g) return SYS[i].mod;
            if (dynLabel[g]) {
                // Extract module prefix from label
                if (strncmp(dynLabel[g],"NFC",3)==0)    return "NFC";
                if (strncmp(dynLabel[g],"NRF",3)==0)    return "NRF24";
                if (strncmp(dynLabel[g],"Sub",3)==0)    return "SubGHz";
                if (strncmp(dynLabel[g],"RFID",4)==0)   return "RFID";
            }
            return "USED";
        };
        auto getLbl = [&](uint8_t g) -> const char* {
            if (dynLabel[g]) return dynLabel[g];
            for (uint8_t i = 0; i < SYS_LEN; i++)
                if (SYS[i].gpio == g) return SYS[i].lbl;
            return "Used";
        };

        // ── Build JSON ─────────────────────────────────────────
        // Use JsonDocument for safe serialization (handles escaping)
        JsonDocument doc;
        JsonArray arr = doc["pins"].to<JsonArray>();

        for (uint8_t g = 0; g < 40; g++) {
            if (isSkip(g)) continue;
            bool free = !occ[g];
            bool adc  = isADC1(g);
            bool adcOk = free && adc;

            JsonObject pin = arr.add<JsonObject>();
            pin["gpio"]  = g;
            pin["free"]  = free;
            pin["adcOk"] = adcOk;
            pin["adc"]   = adc;  // whether pin is ADC1 capable (even if occupied)

            if (free) {
                pin["module"] = "FREE";
                pin["label"]  = adc ? "Free (ADC1)" : "Free";
            } else {
                pin["module"] = getMod(g);
                pin["label"]  = getLbl(g);
            }
        }

        String out;
        serializeJson(doc, out);
        sendJson(req, 200, out);
    });
}

