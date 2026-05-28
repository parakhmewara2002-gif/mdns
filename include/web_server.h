#pragma once
// ============================================================
//  web_server.h  -  IR Remote Web GUI  v5.0.0
//  All batches combined - clean, no duplicates
// ============================================================
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <queue>
#include "ir_button.h"
#include "config.h"
#include "gpio_config.h"

// Shared JSON response helpers (defined in web_server.cpp, used by all batch files)
void sendJson(AsyncWebServerRequest* req, int code, const String& json);
void sendJsonDoc(AsyncWebServerRequest* req, int code, const JsonDocument& doc);

class WebUI {
public:
    WebUI();
    void begin();
    void loop();

    // ── Broadcasts (thread-safe via queue) ────────────────────
    void broadcastIREvent    (const IRButton& btn);
    void broadcastMessage    (const String& msg);
    void broadcastStatus     ();
    void broadcastBinary     (const uint8_t* data, size_t len);
    // Send pre-serialized JSON string directly to all WS clients
    void broadcastRaw        (const char* json);
    // Raw JSON string pushed directly to WS queue - used by background tasks
    void broadcastRaw        (const String& json);

    // ── Captive Portal (public - called from main.cpp) ────────
    void startCaptivePortal();
    void stopCaptivePortal ();
    void loopCaptivePortal ();

private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;

    // FIX: FreeRTOS mutex replaces portMUX spinlock.
    // portENTER_CRITICAL disables interrupts on both cores - unsafe when
    // std::queue::push() triggers a realloc() inside the critical section.
    // xSemaphoreCreateMutex() is safe: malloc never runs with IRQs disabled.
    SemaphoreHandle_t  _wsMutex = nullptr;
    // _wsSendMutex serializes the actual AsyncWebSocket send calls
    // (_ws.textAll / _ws.binaryAll). _wsMutex only guards the _wsQueue deque;
    // it is released before textAll() runs, so without this second mutex the
    // loop-task text path and the mic-task binary path can call into
    // AsyncWebSocket concurrently from two cores -> client-queue corruption.
    SemaphoreHandle_t  _wsSendMutex = nullptr;
    std::queue<String> _wsQueue;

    // Last-broadcast status snapshot for delta suppression
    struct StatusSnap {
        bool     staConnected = false;
        int32_t  rssi         = 0;
        bool     ntpSynced    = false;
        bool     sdMounted    = false;
        bool     macroRunning = false;
        uint32_t heap         = 0;
    } _lastStatus;
    bool _statusDirty = true;
    unsigned long _lastWsCleanup = 0;  // rate-limit cleanupClients to every 5s

    void _flushWsQueue();
    void _pushWsMessage(const String& msg);

    // ── Captive Portal (DNS) ──────────────────────────────────
    DNSServer _dns;
    bool      _captiveActive = false;

    // ── Route setup ───────────────────────────────────────────
    void setupStaticRoutes    ();
    void setupApiRoutes       ();
    void setupGpioRoutes      ();
    void setupGroupRoutes     ();
    void setupSchedulerRoutes ();
    void setupWifiRoutes      ();
    void setupWebSocket       ();
    void setupSdRoutes        ();
    void setupMacroRoutes     ();
    void setupModuleRoutes        ();
    void setupModuleToggleRoutes  ();

    // Batch 1
    void setupRestApiV1Routes ();
    void setupAuditRoutes     ();
    void setupDebugRoutes     ();
    void setupMicRoutes       ();
    void setupMicPinsRoute    ();

    // Batch 3
    void setupAuthRoutes         ();
    void setupCaptivePortal      ();
    void setupWatchdogRoutes     ();

    // Batch 4
    void setupLogRoutes       ();


    // WiFi Penetration Module
    void setupWpenRoutes      ();

    // AC Non-Contact Detector
    void setupAcRoutes        ();

    // SD card extended routes (features 42-45)
    void setupSdExtRoutes     ();

    // IR Jammer (Bruce port)
    void setupIrJammerRoutes  ();

    // Beacon Spam (Bruce port)
    void setupBeaconSpamRoutes();

    // LLMNR/NBNS Responder (Bruce port)
    void setupResponderRoutes ();


    // ── Button handlers ───────────────────────────────────────
    void handleGetButtons  (AsyncWebServerRequest*);
    void handleAddButton   (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleUpdateButton(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleDeleteButton(AsyncWebServerRequest*);
    void handleClearButtons(AsyncWebServerRequest*);
    void handleTransmit    (AsyncWebServerRequest*, uint8_t*, size_t);
    void handlePwmTest     (AsyncWebServerRequest*, uint8_t*, size_t);  // POST /api/v1/ir/pwm-test
    void handlePwmTestGet  (AsyncWebServerRequest*);                     // GET  /api/v1/ir/pwm-test?freq=38&duty=33
    void handlePwmInfo     (AsyncWebServerRequest*);                     // GET  /api/v1/ir/pwm-info
    void handleExport      (AsyncWebServerRequest*);
    void handleImport      (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Backup & Restore ──────────────────────────────────────
    void handleBackupCreate  (AsyncWebServerRequest*);
    void handleBackupDownload(AsyncWebServerRequest*);
    void handleBackupStatus  (AsyncWebServerRequest*);
    void handleRestore       (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleGetConfig     (AsyncWebServerRequest*);
    void handleSetConfig     (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleRestart       (AsyncWebServerRequest*);
    void handleGetStatus     (AsyncWebServerRequest*);
    void handleGetAutoSave   (AsyncWebServerRequest*);
    void handleSetAutoSave   (AsyncWebServerRequest*);

    // ── Groups ────────────────────────────────────────────────
    void handleGetGroups   (AsyncWebServerRequest*);
    void handleAddGroup    (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleUpdateGroup (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleDeleteGroup (AsyncWebServerRequest*);
    void handleReorderGroup(AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Scheduler ─────────────────────────────────────────────
    void handleGetSchedules  (AsyncWebServerRequest*);
    void handleAddSchedule   (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleUpdateSchedule(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleDeleteSchedule(AsyncWebServerRequest*);
    void handleToggleSchedule(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleGetNtpStatus  (AsyncWebServerRequest*);
    void handleSetTimezone   (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── WiFi scan ─────────────────────────────────────────────
    void handleStartScan  (AsyncWebServerRequest*);
    void handleScanResults(AsyncWebServerRequest*);

    // ── GPIO ──────────────────────────────────────────────────
    void handleGetGpioPins(AsyncWebServerRequest*);
    void handleSetGpioPins(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleGetPinList (AsyncWebServerRequest*);

    // ── Macro (LittleFS) ──────────────────────────────────────
    void handleMacroList  (AsyncWebServerRequest*);
    void handleMacroRead  (AsyncWebServerRequest*);
    void handleMacroSave  (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleMacroDelete(AsyncWebServerRequest*);
    void handleMacroRun   (AsyncWebServerRequest*);
    void handleMacroAbort (AsyncWebServerRequest*);
    void handleMacroStatus(AsyncWebServerRequest*);

    // ── SD Card ───────────────────────────────────────────────
    void handleSdStatus      (AsyncWebServerRequest*);
    void handleSdList        (AsyncWebServerRequest*);
    void handleSdDelete      (AsyncWebServerRequest*);
    void handleSdRename      (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdMkdir       (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdLog         (AsyncWebServerRequest*);
    void handleSdBackup      (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdRestore     (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdMacroRun    (AsyncWebServerRequest*);
    void handleSdMacroList   (AsyncWebServerRequest*);
    void handleSdMacroStatus (AsyncWebServerRequest*);
    void handleSdIRLibExport (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdIRLibImport (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdIRLibMerge  (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdIRLibDelete (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdIRLibRename (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdIRLibSaveBtn(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdIRLibList   (AsyncWebServerRequest*);
    void handleSdDeviceList  (AsyncWebServerRequest*);
    void handleSdDeviceRead  (AsyncWebServerRequest*);
    void handleSdDeviceSave  (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdDeviceDelete(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdDeviceImport(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdBackupList  (AsyncWebServerRequest*);
    void handleSdUpload      (AsyncWebServerRequest*, const String&,
                              size_t, uint8_t*, size_t, bool);
    void handleSdDownload    (AsyncWebServerRequest*);
    void handleSdCopy           (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdMove           (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdDeleteRecursive(AsyncWebServerRequest*);
    void handleSdFileInfo       (AsyncWebServerRequest*);
    void handleSdReadText       (AsyncWebServerRequest*);
    void handleSdFormat         (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Batch 1: REST API v1 ──────────────────────────────────
    void handleV1Status        (AsyncWebServerRequest*);
    void handleV1IrTrigger     (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleV1IrList        (AsyncWebServerRequest*);
    void handleV1MacroRun      (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleV1MacroList     (AsyncWebServerRequest*);
    void handleV1ScheduleList  (AsyncWebServerRequest*);
    void handleV1RfidLog       (AsyncWebServerRequest*);
    void handleV1SystemInfo    (AsyncWebServerRequest*);
    void handleV1SystemRestart (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Batch 1: Audit ────────────────────────────────────────
    void handleAuditGet   (AsyncWebServerRequest*);
    void handleAuditClear (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleAuditExport(AsyncWebServerRequest*);

    // ── Batch 1: Debug ────────────────────────────────────────
    void handleDebugStats  (AsyncWebServerRequest*);
    void handleDebugModules(AsyncWebServerRequest*);

    // ── Batch 3: Auth ─────────────────────────────────────────
    void handleAuthLogin   (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleAuthLogout  (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleAuthPassword(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleAuthConfig  (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Batch 4: Log Rotation ────────────────────────────────
    void handleLogExportCsv(AsyncWebServerRequest*);
    void handleLogConfig   (AsyncWebServerRequest*, uint8_t*, size_t);

    static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient*,
                          AwsEventType, void*, uint8_t*, size_t);
};

extern WebUI webUI;
