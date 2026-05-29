// ============================================================
//  main.cpp  -  IR Remote Web GUI  v5.2.0  |  ESP32-WROOM-32
//
//  v5.2.0 build (deep bug scan - smoothness & stability):
//    FIX Q-01 [CRITICAL]: IR TX queue shallow-copy crash
//              xQueueSend(sizeof(IrTxCommand)) does memcpy of IRButton
//              which contains String + vector<uint16_t> with heap ptrs.
//              Destructor on stack copy freed heap -> dangling ptrs ->
//              heap corruption / crash on dequeue. Fixed: pointer queue
//              (std::queue<IrTxCommand*> + mutex + binary semaphore notify).
//              IRButton deep-copied via C++ copy ctor before heap alloc.
//    FIX N-01: NFC readPassiveTargetID timeout 100ms -> 10ms in hw_poll task.
//              100ms blocked ALL hw_poll siblings 5× per tick (20ms period).
//    FIX R-01: RFID write delay(55ms) -> vTaskDelay(55ms) in hw_poll context.
//              delay() in hw_poll blocks NFC/SubGHz/NRF24 for 55ms + 5ms.
//    FIX R-02: NRF24 replay delay(2) -> vTaskDelay(2) in hw_poll context.
//    FIX S-01: hw_poll FreeRTOS task stack 4096 -> 6144.
//              NFC MIFARE block reads have deep I2C call stacks; 4096
//              caused silent stack overflow -> heap corruption.
//    FIX W-01: WDT_LOOP_MAX_MS 8000ms -> 500ms. 8s threshold too late
//              to catch real loop stalls; 500ms gives early warning.
//    FIX W-02: WDT tryMemoryCleanup() yield 5×1ms -> 20×10ms (200ms).
//              AsyncWebServer TCP flush requires multiple scheduler rounds;
//              original 5ms was rarely enough, triggering spurious reboots.
//    FIX W-03: wdt_ping task stack 4096 -> 6144. HTTPClient DNS+TCP+HTTP
//              parsing needs ~5.5KB; 4096 caused silent stack overflow.
//    FIX WS-01: broadcastMessage() direct _ws.textAll() -> _pushWsMessage().
//              textAll() not thread-safe from non-loop() tasks (rule callbacks,
// ============================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_log.h>
#include "task_manager.h"     // FreeRTOS task architecture
#include "config.h"
#include "gpio_config.h"
#include "ir_database.h"
#include "ir_receiver.h"
#include "ir_transmitter.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "group_manager.h"
#include "scheduler.h"
#include "sd_manager.h"
#include "macro_manager.h"
#include "nfc_module.h"
#include "rfid_module.h"
#include "subghz_module.h"
#include "nrf24_module.h"
#include "system_module.h"
#include "audit_manager.h"          // Batch 1: Audit Trail
#include "auth_manager.h"           // Batch 3: Authentication
#include "watchdog_manager.h"       // Batch 3: Self-Healing Watchdog
#include "log_rotation.h"           // Batch 4: Log Rotation + CSV Export
#include "mic_module.h"
#include "wifi_pen_module.h"        // WiFi Penetration Module
#include "ac_detector.h"            // Non-Contact AC Power Detector
#include "ir_jammer.h"              // IR Jammer (Bruce port)
#include "beacon_spam.h"            // Beacon Spam (Bruce port)
#include "responder.h"              // LLMNR/NBNS Responder (Bruce port)

static void initFilesystem();
static void ensureDefaultFiles();
static void onIRReceived(const IRButton& btn);
static void onScheduleFire(const ScheduleEntry& entry);
static void printBanner();

// FIX: removed static - web_server.cpp accesses this via extern for safe restart
portMUX_TYPE s_restartMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t s_restartAt = 0;

static unsigned long s_lastStatusBroadcast = 0;
#define STATUS_BROADCAST_INTERVAL_MS 15000  // was 3s — frequent alloc/free fragments heap

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(300);
    printBanner();

    // Suppress VFS "does not exist" spam from ESPAsyncWebServer
    esp_log_level_set("vfs_api", ESP_LOG_ERROR);
    esp_log_level_set("vfs",     ESP_LOG_ERROR);

    // ── CPU frequency - lock at 240MHz for minimum IR timing jitter ─────
    // Without explicit setCpuFrequencyMhz(), ESP32 Arduino starts at 240MHz
    // but the watchdog's loadConfig() restores the saved perfMode later.
    // We set 240MHz here first so the WiFi/IR init path always runs at full
    // speed regardless of what perfMode was saved last session.
    setCpuFrequencyMhz(240);

    // ── LittleFS (always required) ────────────────────────────
    initFilesystem();

    // ── SD Card (optional - non-blocking if absent) ───────────
    // begin() returns immediately if no card is detected.
    // All existing features work unchanged without SD.
    sdMgr.begin();

    // ── IR Database ───────────────────────────────────────────
    irDB.begin();

    // ── Groups ────────────────────────────────────────────────
    groupMgr.begin();

    // ── Internal Macros (LittleFS - no SD needed) ─────────────
    macroMgr.onTransmit([](uint32_t buttonId) {
        IRButton btn = irDB.findById(buttonId);
        // FIX: use transmitAsync - macro steps call this from loop() context.
        // transmit() would block for the full TX duration per macro step.
        if (btn.id) irTransmitter.transmitAsync(btn);
    });
    macroMgr.begin();

    // ── New modules (Tasks 7-11) ─────────────────────────────
    nfcModule.begin();
    rfidModule.begin();
    subGhzModule.begin();
    nrf24Module.begin();
    sysModule.begin();
    wifiPen.begin();               // WiFi Pen Module
    micModule.begin();             // Mic: I2S streaming + SD recording

    // ── Audit Trail (Batch 1) ─────────────────────────────────
    auditMgr.begin();

    // ── Auth (Batch 3) ────────────────────────────────────────
    authMgr.begin();

    // ── Watchdog (Batch 3) ────────────────────────────────────
    wdtMgr.begin();
    // Re-apply saved perfMode CPU freq now that loadConfig() has run inside begin().
    // setup() forced 240MHz earlier for reliable init; this restores the user's
    // chosen mode (TURBO=240, NORMAL=160, POWER_SAVE=80) without an extra reboot.
    wdtMgr.setPerfMode(wdtMgr.getPerfMode());
    // ── Log Rotation (Batch 4) ────────────────────────────────
    logRotMgr.begin();

    // ── WiFi (AP+STA dual-mode with event handler) ────────────
    wifiMgr.begin();

    // ── AC Non-Contact Detector ───────────────────────────────
    acDetector.onAcDetected = []() {
        webUI.broadcastRaw("{\"event\":\"ac_detected\"}");
    };
    acDetector.onAcLost = []() {
        webUI.broadcastRaw("{\"event\":\"ac_lost\"}");
    };
    acDetector.begin();

    // ── Load persisted IR pin config ──────────────────────────
    IrPinConfig irPins;
    wifiMgr.loadIrPins(irPins);


    // ── IR Receiver ───────────────────────────────────────────
    irReceiver.onReceive(onIRReceived);
    irReceiver.begin(irPins.recvPin);

    // ── IR Transmitter ────────────────────────────────────────
    irTransmitter.begin(irPins);

    // ── IR Jammer ────────────────────────────────────────────
    irJammer.begin(irPins.emitPin[0]);

    // ── Beacon Spam ──────────────────────────────────────────
    beaconSpam.begin();

    // ── Responder ────────────────────────────────────────────
    responder.begin();

    webUI.begin();
    Serial.printf("[MEM] post-webUI heap=%u\n", ESP.getFreeHeap());
    webUI.startCaptivePortal();  // Batch 3: DNS redirect on AP mode

    // ── Scheduler ────────────────────────────────────────────
    scheduler.onFire(onScheduleFire);
    scheduler.begin();

    Serial.printf(DEBUG_TAG " Ready v%s  AP: http://%s\n",
                  FIRMWARE_VERSION, wifiMgr.apIP().c_str());
    Serial.printf(DEBUG_TAG " RX=GPIO%d  TX-active=%d  Groups=%u  Schedules=%u  Heap=%u\n",
                  irReceiver.activePin(),
                  irTransmitter.activeCount(),
                  (unsigned)groupMgr.size(),
                  (unsigned)scheduler.size(),
                  ESP.getFreeHeap());
    Serial.printf(DEBUG_TAG " SD: %s\n",
                  sdMgr.isAvailable() ? "MOUNTED" : "not present");

    if (sdMgr.isAvailable()) {
        SdStatus ss = sdMgr.status();
        Serial.printf(DEBUG_TAG " SD: %s  %lluMB total  %lluMB free\n",
                      ss.cardTypeStr.c_str(),
                      ss.totalBytes / (1024ULL * 1024ULL),
                      (ss.totalBytes - ss.usedBytes) / (1024ULL * 1024ULL));
    }

    // All modules initialised successfully - reset boot-failure counter.
    // This must be the LAST call in setup() so only a full clean boot clears it.
    wdtMgr.markBootSuccess();

    // ── Start FreeRTOS task architecture ─────────────────────
    // Must be AFTER all module begin() calls since tasks reference them.
    //   net_io (Core 0, priority 2): handles ALL blocking HTTPS + buzzer
    //   hw_poll (Core 1, priority 2): polls NFC/RFID/SubGHz/NRF24
    // loop() will no longer call those modules directly.
    taskMgr.begin();
    // Reset WDT loop timer AFTER taskMgr.begin() so the gap between
    // wdtMgr.begin() (mid-setup) and first loop() tick is not counted
    // as a stall — that gap is setup() finishing, not a runtime freeze.
    wdtMgr.resetLoopTimer();
}


// ─────────────────────────────────────────────────────────────
void loop() {
    taskENTER_CRITICAL(&s_restartMux); uint32_t _ra = s_restartAt; taskEXIT_CRITICAL(&s_restartMux);
    if (_ra != 0 && millis() >= _ra) {  // fire when future timestamp reached
        taskENTER_CRITICAL(&s_restartMux); s_restartAt = 0; taskEXIT_CRITICAL(&s_restartMux);
        ESP.restart();
    }
    irReceiver.loop();
    wifiMgr.loop();
    scheduler.loop();
    webUI.loop();
    irDB.loop();

    // SD loop: hot-plug probe, log flush, macro step tick
    sdMgr.loop();

    // Internal macro tick (LittleFS macros - no SD needed)
    macroMgr.loop();

    // ── Hardware polling moved to hw_poll task (Core 1, priority 2) ──────
    // nfcModule.loop(), rfidModule.loop(), subGhzModule.loop(),
    // nrf24Module.loop() - all removed from loop() and run in hw_poll task
    // at a fixed 20ms tick via vTaskDelayUntil(). This removes 4 SPI bus
    // polling calls from every loop() iteration, reducing worst-case loop
    // duration by ~15ms under heavy SPI activity.

    sysModule.loop();
    wifiPen.loop();                // WiFi Pen: timeout watchdog
    acDetector.loop();             // AC Detector: RMS sampling + threshold
    irJammer.loop();               // IR Jammer tick
    beaconSpam.loop();             // Beacon Spam tick
    responder.loop();              // LLMNR/NBNS Responder tick
    auditMgr.loop();   // Batch 1
    authMgr.loop();    // Batch 3: expire sessions
    wdtMgr.loop();     // Batch 3: watchdog feed + health check
    micModule.loop();  // Mic: SD recording tick
    // loopCaptivePortal() already called inside webUI.loop()
    logRotMgr.loop();  // Batch 4: auto log rotation


    if (millis() - s_lastStatusBroadcast >= STATUS_BROADCAST_INTERVAL_MS) {
        s_lastStatusBroadcast = millis();
        if (ESP.getFreeHeap() >= 20000) webUI.broadcastStatus();
    }

    static bool s_firstLoop = true;
    if (s_firstLoop) {
        s_firstLoop = false;
        Serial.printf("[MEM] post-firstLoop heap=%u\n", ESP.getFreeHeap());
    }

    yield();
}

// ─────────────────────────────────────────────────────────────
//  LittleFS init + default file creation
// ─────────────────────────────────────────────────────────────
static void initFilesystem() {
    if (!LittleFS.begin(true)) {
        Serial.println(DEBUG_TAG " FATAL: LittleFS mount+format failed.");
        return;
    }
    Serial.printf(DEBUG_TAG " LittleFS OK: total=%uKB  used=%uKB\n",
                  (unsigned)(LittleFS.totalBytes() / 1024),
                  (unsigned)(LittleFS.usedBytes()  / 1024));
    ensureDefaultFiles();
}

static void ensureFile(const char* path, const char* defaultContent) {
    if (LittleFS.exists(path)) return;
    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf(DEBUG_TAG " ERROR: Cannot create %s\n", path);
        return;
    }
    f.print(defaultContent);
    f.close();
    Serial.printf(DEBUG_TAG " Created default: %s\n", path);
}

static void ensureDefaultFiles() {
    ensureFile("/ir_database.json",  "{\"buttons\":[]}");
    ensureFile("/ir_pins.json",
        "{\"recvPin\":14,\"emitCount\":1,"
        "\"emit\":["
        "{\"pin\":27,\"enabled\":true},"
        "{\"pin\":26,\"enabled\":false},"
        "{\"pin\":25,\"enabled\":false},"
        "{\"pin\":33,\"enabled\":false}"
        "]}");
    ensureFile(CFG_FILE,
        "{\"apSSID\":\"IR-Remote\",\"apPass\":\"irremote123\","
        "\"apChannel\":1,\"apHidden\":false,"
        "\"staSSID\":\"\",\"staPass\":\"\",\"staEnabled\":false}");
    ensureFile("/groups.json",      "{\"groups\":[]}");
    ensureFile("/schedules.json",   "{\"schedules\":[]}");
    ensureFile("/ntp_config.json",  "{\"tzOffset\":19800,\"dstOffset\":0}");
    ensureFile(IR_AUTO_SAVE_FILE,   "{\"autoSave\":false}");
}

// ─────────────────────────────────────────────────────────────
static void onIRReceived(const IRButton& btn) {
    webUI.broadcastIREvent(btn);
    auditMgr.logIrRx(protocolName(btn.protocol), String(btn.code, HEX));  // Batch 1

    if (!irDB.autoSaveEnabled()) return;

    IRButton copy = btn;
    uint32_t savedId = irDB.autoSaveReceived(copy);

    if (savedId) {
        Serial.printf(DEBUG_TAG " [AutoSave] Saved '%s' as id=%u\n",
                      copy.name.c_str(), savedId);
        webUI.broadcastMessage(String("Auto-saved: ") + copy.name);

        // Mirror auto-saved signal to SD raw dump if SD available
        if (sdMgr.isAvailable() && btn.protocol == IRProtocol::RAW &&
            !btn.rawData.empty()) {
            sdMgr.saveRawDump(copy.name, btn.rawData.data(),
                              btn.rawData.size(), btn.freqKHz);
        }
    } else {
        if (btn.protocol != IRProtocol::RAW) {
            // hasMatchingButton() holds _mux internally — safe to call from
            // loop() (Core 1) while web add/delete mutates _buttons (Core 0).
            if (irDB.hasMatchingButton(btn.protocol, btn.code)) {
                Serial.printf(DEBUG_TAG " [AutoSave] Duplicate skipped (guarded)\n");
            }
        }
    }
}

static void onScheduleFire(const ScheduleEntry& entry) {
    IRButton copy = irDB.findById(entry.buttonId);
    if (!copy.id) {
        Serial.printf(DEBUG_TAG " Scheduler: button %u not found\n", entry.buttonId);
        return;
    }

    // Honour schedule-level repeatCount (overrides button default if > 1)
    uint8_t  fireCount = (entry.repeatCount > 1) ? entry.repeatCount : copy.repeatCount;
    uint16_t fireDelay = (entry.repeatDelay > 0) ? entry.repeatDelay : copy.repeatDelay;

    Serial.printf(DEBUG_TAG " Scheduler TX: btn=%u '%s'  fires=%u  delay=%ums\n",
                  copy.id, copy.name.c_str(), fireCount, fireDelay);

    // FIX: was calling irTransmitter.transmit() + delay() in loop() context.
    // With fireCount=3 and fireDelay=200ms this blocked loop() for ~650ms -
    // starving IR receiver, WebSocket flush, WDT feed, and all other modules.
    //
    // Fix: post each fire as a separate IrTxCommand to the TX queue.
    // The dedicated ir_tx FreeRTOS task (priority 5) executes them sequentially.
    // loop() returns immediately and continues servicing other modules.
    //
    // repeatDelay between fires: bake it into the button copy so the TX task
    // applies it between the outer repeat iterations inside doTransmit().
    copy.repeatCount = fireCount;
    copy.repeatDelay = fireDelay;
    irTransmitter.transmitAsync(copy);   // non-blocking - posts to ir_tx queue

    // FIX: send structured WS event 'scheduled_tx' so the GUI toast fires correctly.
    // Previously used broadcastMessage() which sends generic 'message' event;
    // the JS switch already has case 'scheduled_tx' wired to a named toast.
    //
    // RACE FIX: use ArduinoJson to escape the button name properly (it may
    // contain '"' or '\' which would inject into the raw JSON string, causing
    // malformed JSON on the WebSocket clients). The old 128-byte char buf was
    // also too small for a max-length button name (96 chars + 56 overhead = 152).
    {
        JsonDocument _schedDoc;
        _schedDoc["event"]    = "scheduled_tx";
        _schedDoc["name"]     = copy.name;
        _schedDoc["buttonId"] = entry.buttonId;
        String _schedStr;
        _schedStr.reserve(192);
        serializeJson(_schedDoc, _schedStr);
        webUI.broadcastRaw(_schedStr);
    }
    auditMgr.logScheduler(entry.name, entry.buttonId);
    if (sdMgr.isAvailable())
        sdMgr.log(String("Scheduler TX: ") + copy.name);
}

static void printBanner() {
    Serial.println(F("\n"
        "╔══════════════════════════════════════════════╗\n"
        "║   IR Remote Web GUI                          ║\n"
        "║   ESP32-WROOM-32  .  Full Feature Build      ║\n"
        "║   NFC . RFID . SubGHz . NRF24 . System       ║\n"
        "╚══════════════════════════════════════════════╝"));
    Serial.printf("Version: %s\n", FIRMWARE_VERSION);
    Serial.printf("Chip: %s rev%d %uMHz  Flash:%uMB  Heap:%u\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  getCpuFrequencyMhz(),
                  (unsigned)(ESP.getFlashChipSize() >> 20),
                  ESP.getFreeHeap());
}
