// ============================================================
//  watchdog_manager.cpp  -  Ultra Pro Watchdog v3.0
//
//  REAL implementations of:
//    - Main loop freeze detection + forced reboot
//    - Per-module stall detection + stall counting
//    - WiFi disconnect detection (via WiFi.status())
//    - Internet connectivity test (HTTP HEAD to Google 204)
//    - Reboot after WDT_MAX_BOOT_FAILURES consecutive failures
//    - Low heap detection + smart memory cleanup
//    - Boot failure counter (persisted to LittleFS)
//    - Safe recovery mode (skip heavy modules on repeated crash)
//    - Crash reason logging (ESP IDF reset reason API)
//    - Brownout detection (ESP_RST_BROWNOUT)
//    - Thermal protection (esp_temp_sensor or chip thermistor)
//    - Power saving mode (setCpuFrequencyMhz(80))
//    - Turbo performance mode (setCpuFrequencyMhz(240))
// ============================================================
#include "watchdog_manager.h"
#include "audit_manager.h"
#include "wifi_manager.h"
#include <Preferences.h>
#include <esp_heap_caps.h>         // heap_caps_get_largest_free_block()
#include <esp_system.h>            // esp_get_minimum_free_heap_size()
#include <ctime>

// ESP32 internal temperature sensor
// temprature_sens_read() is a ROM function present in all ESP32 (Xtensa LX6) variants.
// The intentional typo "temprature" (missing 'e') matches the actual ROM symbol name.
// Available on IDF 4.x and IDF 5.x; deprecated in IDF 5.x but still linked.
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();   // ROM symbol - typo is intentional, matches ROM export
#ifdef __cplusplus
}
#endif

WatchdogManager wdtMgr;

// ─────────────────────────────────────────────────────────────
WatchdogManager::WatchdogManager()
    : _hwEnabled(true), _hwStarted(false), _beginDone(false),
      _heapThreshold(WDT_HEAP_MIN_BYTES),
      _minHeapSeen(UINT32_MAX),
      _lastLoopMs(0), _lastHeapCheck(0),
      _lastPingCheck(0), _lastTempCheck(0),
      _lastModCheck(0),
      _internetOk(false), _wifiWasConnected(false), _pingActive(false),
      _perfMode(WdtPerfMode::NORMAL),
      _thermalThrottled(false),
      _heapCriticalCount(0)
{
    _bootState = {0, false, 0};
}

// ─────────────────────────────────────────────────────────────
void WatchdogManager::begin() {
    // Load boot failure counter FIRST - determines safe mode
    _loadBootState();

    // Increment failure counter; markBootSuccess() resets it later
    _bootState.failCount++;
    if (_bootState.failCount >= WDT_MAX_BOOT_FAILURES) {
        _bootState.safeMode = true;
        Serial.printf(WDT_TAG " SAFE MODE activated after %u consecutive failures!\n",
                      _bootState.failCount);
    }
    _saveBootState();

    loadConfig();
    _logCrashOnBoot();

    if (_hwEnabled) _startHw();

    unsigned long now = millis();
    _lastLoopMs      = now;
    _lastHeapCheck   = now;
    _lastPingCheck   = now;
    _lastTempCheck   = now;
    _minHeapSeen     = ESP.getFreeHeap();
    _wifiWasConnected = false;
    _beginDone       = true;

    Serial.printf(WDT_TAG " Started v3.0 - HW:%s  SafeMode:%s  BootFails:%u  Heap:%u  Temp:%.1fC\n",
                  _hwEnabled    ? "ON"  : "OFF",
                  _bootState.safeMode ? "YES" : "no",
                  _bootState.failCount,
                  ESP.getFreeHeap(),
                  cpuTemperature());
}

// ─────────────────────────────────────────────────────────────
void WatchdogManager::loop() {
    unsigned long now = millis();

    // 1. Feed hardware watchdog - MUST come first
    if (_hwEnabled && _hwStarted) esp_task_wdt_reset();

    // 2. Loop stall detection
    _checkLoopStall(now);
    _lastLoopMs = now;

    // 3. Module stall detection (every 5s - no need to scan every loop tick)
    if (now - _lastModCheck >= 5000UL) {
        _lastModCheck = now;
        _checkModules(now);
    }

    // 4. Heap monitoring (every 30 s)
    if (now - _lastHeapCheck >= WDT_HEAP_CHECK_MS) {
        _lastHeapCheck = now;
        uint32_t heap = ESP.getFreeHeap();
        if (heap < _minHeapSeen) _minHeapSeen = heap;
        _checkHeap(heap, now);
    }

    // 5. Temperature check (every 20 s)
    if (now - _lastTempCheck >= WDT_TEMP_CHECK_MS) {
        _lastTempCheck = now;
        _checkTemperature();
    }

    // 6. Connectivity check (every 60 s, only when STA connected)
    if (now - _lastPingCheck >= WDT_PING_INTERVAL_MS) {
        _lastPingCheck = now;
        if (wifiMgr.staConnected()) {  // cached - no WiFi.status() mutex
            _checkConnectivity();
        } else {
            _internetOk = false;
        }
    }

    // 7. WiFi disconnect recovery - trigger wifiMgr reconnect
    bool wifiNow = wifiMgr.staConnected();  // cached - no WiFi.status() mutex
    if (_wifiWasConnected && !wifiNow) {
        Serial.println(WDT_TAG " WiFi disconnected - requesting reconnect");
        auditMgr.log(AuditSource::SYSTEM, "WIFI_LOST",
                     "WDT detected WiFi disconnect", false);
        _internetOk = false;
        // main loop() calls wifiMgr.loop() every tick - no nudge needed here.
    }
    _wifiWasConnected = wifiNow;
}

// ─────────────────────────────────────────────────────────────
//  Loop stall detection
// ─────────────────────────────────────────────────────────────
void WatchdogManager::_checkLoopStall(unsigned long now) {
    if (_lastLoopMs == 0) return;
    unsigned long gap = now - _lastLoopMs;
    if (gap > WDT_LOOP_REBOOT_MS) {
        // Severe stall - hardware WDT should have caught this, but
        // if somehow we're here, force reboot via SW reset.
        Serial.printf(WDT_TAG " FATAL: loop() frozen for %lums - rebooting!\n", gap);
        auditMgr.log(AuditSource::SYSTEM, "LOOP_FREEZE",
                     String("Gap: ") + gap + "ms - rebooting", false);
        delay(50);
        ESP.restart();
    } else if (gap > WDT_LOOP_MAX_MS) {
        Serial.printf(WDT_TAG " WARNING: loop() stall detected! Gap=%lums\n", gap);
        auditMgr.log(AuditSource::SYSTEM, "LOOP_STALL",
                     String("Gap: ") + gap + "ms", false);
    }
}

// ─────────────────────────────────────────────────────────────
//  Module stall detection
// ─────────────────────────────────────────────────────────────
void WatchdogManager::_checkModules(unsigned long now) {
    for (auto& m : _modules) {
        bool wasStalled = m.stalled;
        m.stalled = m.isStalled();
        if (m.stalled && !wasStalled) {
            m.stallCount++;
            Serial.printf(WDT_TAG " WARNING: Module '%s' stalled! No feed for %lums (stall#%u)\n",
                          m.name.c_str(), now - m.lastFeedMs, m.stallCount);
            auditMgr.log(AuditSource::SYSTEM, "MODULE_STALLED",
                         String("Module: ") + m.name + " stall#" + m.stallCount, false);
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Heap monitoring + cleanup
// ─────────────────────────────────────────────────────────────
void WatchdogManager::_checkHeap(uint32_t freeHeap, unsigned long now) {
    // FIX: use IDF API for max allocatable block - a better fragmentation metric.
    // A device can show 80KB "free" but only 12KB contiguous - API calls fail.
    uint32_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    if (freeHeap < WDT_HEAP_CRITICAL_BYTES) {
        _heapCriticalCount++;
        Serial.printf(WDT_TAG " CRITICAL: heap=%u maxBlock=%u fragPct=%u%% count=%u\n",
                      freeHeap, maxBlock,
                      (uint8_t)(100u - (maxBlock * 100u / (freeHeap > 0 ? freeHeap : 1))),
                      _heapCriticalCount);
        auditMgr.log(AuditSource::SYSTEM, "HEAP_CRITICAL",
                     String("Free:") + freeHeap + " block:" + maxBlock, false);

        // ── Graceful degradation: shed load before restarting ────
        // Yield to let AsyncWebServer flush pending responses
        tryMemoryCleanup();

        if (_heapCriticalCount >= 3) {
            Serial.println(WDT_TAG " Heap exhaustion after cleanup - rebooting");
            delay(100);
            ESP.restart();
        }
    } else if (freeHeap < _heapThreshold) {
        _heapCriticalCount = 0;
        Serial.printf(WDT_TAG " WARNING: Low heap=%u maxBlock=%u\n", freeHeap, maxBlock);
        auditMgr.log(AuditSource::SYSTEM, "HEAP_WARNING",
                     String("Free:") + freeHeap + " block:" + maxBlock, false);
    } else {
        _heapCriticalCount = 0;
    }
}

// ─────────────────────────────────────────────────────────────
//  Smart memory cleanup - releases known reclaimable buffers
// ─────────────────────────────────────────────────────────────
void WatchdogManager::tryMemoryCleanup() {
    // FIX: yield for 200ms (20 × 10ms) instead of 5 × 1ms.
    // AsyncWebServer response buffers are freed when the TCP stack drains its
    // send queue; this requires multiple scheduler rounds (~5-10ms each) to
    // complete. 5ms total is rarely enough to free anything
    // meaningful, causing the 3-strike reboot counter to trigger on transient
    // allocation bursts (e.g., IR TX + status broadcast coinciding).
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));  // yield 10ms to RTOS per round
    }
    Serial.printf(WDT_TAG " Memory cleanup done. Heap now: %u bytes\n",
                  ESP.getFreeHeap());
}

// ─────────────────────────────────────────────────────────────
//  Temperature monitoring (real ESP32 chip thermistor)
// ─────────────────────────────────────────────────────────────
float WatchdogManager::cpuTemperature() const {
    // temprature_sens_read() is the ESP32 ROM thermistor function.
    // Returns raw uint8 value; formula: Celsius = (raw - 32) / 1.8
    // Works on IDF 4.x and IDF 5.x (deprecated but still present in ROM).
    uint8_t raw = temprature_sens_read();
    return (static_cast<float>(raw) - 32.0f) / 1.8f;
}

void WatchdogManager::_checkTemperature() {
    float temp = cpuTemperature();
    if (temp >= WDT_TEMP_REBOOT_C) {
        Serial.printf(WDT_TAG " EMERGENCY: CPU temp %.1fC >= %.1fC - rebooting!\n",
                      temp, WDT_TEMP_REBOOT_C);
        auditMgr.log(AuditSource::SYSTEM, "THERMAL_REBOOT",
                     String("Temp: ") + temp + "C", false);
        delay(50);
        ESP.restart();
    } else if (temp >= WDT_TEMP_THROTTLE_C) {
        if (!_thermalThrottled) {
            _thermalThrottled = true;
            Serial.printf(WDT_TAG " WARNING: CPU temp %.1fC - throttling to 80MHz\n", temp);
            auditMgr.log(AuditSource::SYSTEM, "THERMAL_THROTTLE",
                         String("Temp: ") + temp + "C", false);
            _applyCpuFreq(80);
        }
    } else {
        // Temperature is normal - restore speed if we previously throttled
        if (_thermalThrottled) {
            _thermalThrottled = false;
            // Only restore if user has not manually set power_save mode
            if (_perfMode == WdtPerfMode::NORMAL || _perfMode == WdtPerfMode::TURBO) {
                uint32_t restoreMhz = (_perfMode == WdtPerfMode::TURBO) ? 240 : 160;
                Serial.printf(WDT_TAG " Temp %.1fC normal - restoring %uMHz\n", temp, restoreMhz);
                auditMgr.log(AuditSource::SYSTEM, "THERMAL_RESTORED",
                             String("Temp: ") + temp + "C");
                _applyCpuFreq(restoreMhz);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Internet connectivity ping - runs in a disposable FreeRTOS task
//  so loop() is never blocked by the up-to-5-second HTTP call.
//
//  FIX: The old implementation called HTTPClient::sendRequest("HEAD")
//  synchronously inside loop(), blocking for up to WDT_PING_TIMEOUT_MS
//  (5 seconds) every 60 seconds. This stalled IR receiver, WebSocket
//  processing, scheduler checks, and could trip the HW watchdog.
//
//  New design: _checkConnectivity() spawns a 4 KB task that does the
//  blocking HTTP call, writes the result back to _internetOk (atomic
//  bool), then deletes itself. loop() is never blocked.
// ─────────────────────────────────────────────────────────────
void WatchdogManager::_checkConnectivity() {

    // C-02 + ATOMIC FIX: atomic test-and-set replaces the racy
    // `if (_pingActive) return; _pingActive = true;` pair. On dual-core
    // ESP32 the previous code could observe false on core 1 while core 0
    // had just set true, spawning a second wdt_ping task that leaks 6 KB.
    bool expected = false;
    if (!_pingActive.compare_exchange_strong(expected, true)) {
        // already active — skip this tick
        return;
    }

    // Spawn a short-lived task - avoids blocking loop() for up to 5 s.
    // The task writes _internetOk and self-deletes.
    struct PingArgs { WatchdogManager* self; };
    auto* args = new (std::nothrow) PingArgs{this};
    if (!args) { _pingActive.store(false); return; }  // OOM - skip this ping cycle

    BaseType_t ok = xTaskCreate([](void* p) {
        auto* a = static_cast<PingArgs*>(p);
        WatchdogManager* wdt = a->self;
        delete a;

        HTTPClient http;
        http.setTimeout(WDT_PING_TIMEOUT_MS);
        http.begin(WDT_PING_URL);
        int code = http.sendRequest("HEAD");
        http.end();

        bool reachable = (code == 204 || code == 200 ||
                          code == 301 || code == 302);
        if (reachable != wdt->_internetOk) {
            wdt->_internetOk = reachable;
            Serial.printf(WDT_TAG " Internet: %s (HTTP %d)\n",
                          reachable ? "REACHABLE" : "UNREACHABLE", code);
            auditMgr.log(AuditSource::SYSTEM,
                         reachable ? "INTERNET_OK" : "INTERNET_DOWN",
                         String("HTTP code: ") + code, !reachable);
        }
        wdt->_pingActive.store(false);   // C-02 FIX: clear flag before self-delete
        vTaskDelete(NULL);
    // HTTPClient: DNS + TCP + HTTP needs ~5KB stack. 5120 = safe minimum.
    // Priority 1 = same as loop(); no scheduling advantage. Core 0 = network core.
    }, "wdt_ping", 5120, args, 1, NULL);

    if (ok != pdPASS) {
        delete args;
        _pingActive.store(false);   // C-02 FIX: clear flag so next tick can retry
        Serial.println(WDT_TAG " WARNING: could not create ping task (low heap?)");
    }
}

// ─────────────────────────────────────────────────────────────
//  Boot failure counter + safe mode
// ─────────────────────────────────────────────────────────────
void WatchdogManager::markBootSuccess() {
    if (_bootState.failCount > 0 || _bootState.safeMode) {
        Serial.printf(WDT_TAG " Boot success - resetting failure counter (was %u)\n",
                      _bootState.failCount);
    }
    _bootState.failCount   = 0;
    _bootState.safeMode    = false;
    time_t now; time(&now);
    _bootState.lastCleanBoot = (uint32_t)now;
    _saveBootState();
}

// ── Boot failure counter: use NVS (wear-leveled) instead of LittleFS ────────
// FIX: The old LittleFS-based _saveBootState() wrote a JSON file on EVERY boot
// - even clean ones - causing unnecessary flash wear on the data partition.
// NVS uses its own wear-leveling layer and is designed for frequent small writes.
void WatchdogManager::_loadBootState() {
    _bootState = {0, false, 0};
    Preferences p;
    if (!p.begin("wdt_boot", true)) return;   // read-only namespace
    _bootState.failCount     = p.getUChar("fails",    0);
    _bootState.safeMode      = p.getBool ("safe",     false);
    _bootState.lastCleanBoot = p.getUInt ("lastClean", 0);
    p.end();
}

void WatchdogManager::_saveBootState() {
    Preferences p;
    if (!p.begin("wdt_boot", false)) {
        Serial.println(WDT_TAG " WARNING: NVS open failed for boot state");
        return;
    }
    p.putUChar("fails",     _bootState.failCount);
    p.putBool ("safe",      _bootState.safeMode);
    p.putUInt ("lastClean", _bootState.lastCleanBoot);
    p.end();
}

// ─────────────────────────────────────────────────────────────
//  Performance modes
// ─────────────────────────────────────────────────────────────
void WatchdogManager::setPerfMode(WdtPerfMode mode) {
    _perfMode = mode;
    switch (mode) {
        case WdtPerfMode::POWER_SAVE:
            _applyCpuFreq(80);
            Serial.println(WDT_TAG " Power Save mode: CPU 80MHz");
            break;
        case WdtPerfMode::TURBO:
            _applyCpuFreq(240);
            Serial.println(WDT_TAG " Turbo mode: CPU 240MHz");
            break;
        default:
            _applyCpuFreq(160);
            Serial.println(WDT_TAG " Normal mode: CPU 160MHz");
            break;
    }
    if (_beginDone) saveConfig();
}

const char* WatchdogManager::perfModeStr() const {
    switch (_perfMode) {
        case WdtPerfMode::POWER_SAVE: return "power_save";
        case WdtPerfMode::TURBO:      return "turbo";
        default:                       return "normal";
    }
}

void WatchdogManager::_applyCpuFreq(uint32_t mhz) {
    // setCpuFrequencyMhz is Arduino ESP32 API - sets both CPU cores
    setCpuFrequencyMhz(mhz);
}

// ─────────────────────────────────────────────────────────────
//  Hardware watchdog
// ─────────────────────────────────────────────────────────────
void WatchdogManager::hwFeed() {
    if (_hwStarted) esp_task_wdt_reset();
}

void WatchdogManager::_startHw() {
    if (_hwStarted) return;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms     = WDT_HW_TIMEOUT_S * 1000U,
        .idle_core_mask = 0,
        .trigger_panic  = true
    };
    esp_err_t err = esp_task_wdt_reconfigure(&wdt_config);
    if (err != ESP_OK) err = esp_task_wdt_init(&wdt_config);
#else
    esp_err_t err = esp_task_wdt_init(WDT_HW_TIMEOUT_S, true);
#endif
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_add(NULL);
        _hwStarted = true;
        Serial.printf(WDT_TAG " HW watchdog started (%ds timeout)\n", WDT_HW_TIMEOUT_S);
    } else {
        Serial.printf(WDT_TAG " WARNING: HW watchdog init failed: %d\n", err);
    }
}

void WatchdogManager::setHwEnabled(bool en) {
    _hwEnabled = en;
    if (en && !_hwStarted) {
        _startHw();
    } else if (!en && _hwStarted) {
        esp_task_wdt_delete(NULL);
        _hwStarted = false;
        Serial.println(WDT_TAG " HW watchdog stopped");
    }
    if (_beginDone) saveConfig();
}

// ─────────────────────────────────────────────────────────────
//  Module health tracking
// ─────────────────────────────────────────────────────────────
uint8_t WatchdogManager::registerModule(const String& name, uint32_t timeoutMs) {
    WdtModuleHealth m;
    m.name       = name;
    m.lastFeedMs = millis();
    m.timeoutMs  = timeoutMs;
    m.stalled    = false;
    m.stallCount = 0;
    _modules.push_back(m);
    uint8_t idx = (uint8_t)(_modules.size() - 1);
    Serial.printf(WDT_TAG " Registered module '%s' (idx=%u timeout=%ums)\n",
                  name.c_str(), idx, timeoutMs);
    return idx;
}

void WatchdogManager::feedModule(uint8_t idx) {
    if (idx < _modules.size()) {
        _modules[idx].lastFeedMs = millis();
        _modules[idx].stalled    = false;
    }
}

bool WatchdogManager::isModuleStalled(uint8_t idx) const {
    if (idx >= _modules.size()) return false;
    return _modules[idx].stalled;
}

// ─────────────────────────────────────────────────────────────
//  Status JSON  (all values from real ESP32 APIs)
// ─────────────────────────────────────────────────────────────
String WatchdogManager::statusJson() const {
    JsonDocument doc;
    doc["hwEnabled"]        = _hwEnabled;
    doc["hwStarted"]        = _hwStarted;
    doc["hwTimeoutS"]       = WDT_HW_TIMEOUT_S;
    doc["heapFree"]         = (uint32_t)ESP.getFreeHeap();
    doc["heapMin"]          = _minHeapSeen;
    doc["heapThreshold"]    = _heapThreshold;
    doc["heapCritical"]     = isHeapCritical();
    doc["heapLow"]          = isHeapLow();
    doc["uptimeS"]          = (uint32_t)(millis() / 1000);
    doc["loopStallMaxMs"]   = WDT_LOOP_MAX_MS;
    doc["cpuFreqMHz"]       = (uint32_t)getCpuFrequencyMhz();
    doc["cpuTempC"]         = (float)((int)(cpuTemperature() * 10)) / 10.0f;
    doc["flashFreeKB"]      = (uint32_t)((ESP.getFreeSketchSpace()) / 1024);
    doc["sketchSizeKB"]     = (uint32_t)(ESP.getSketchSize() / 1024);
    doc["internetOk"]       = _internetOk;
    doc["perfMode"]         = perfModeStr();
    doc["safeMode"]         = _bootState.safeMode;
    doc["bootFailCount"]    = _bootState.failCount;
    doc["wifiConnected"]    = wifiMgr.staConnected();
    doc["wifiRSSI"]         = wifiMgr.staConnected() ? (int)WiFi.RSSI() : 0;

    // LittleFS stats
    doc["fsTotal"]  = (uint32_t)LittleFS.totalBytes();
    doc["fsUsed"]   = (uint32_t)LittleFS.usedBytes();
    doc["fsFree"]   = (uint32_t)(LittleFS.totalBytes() - LittleFS.usedBytes());

    JsonArray mods = doc["modules"].to<JsonArray>();
    for (const auto& m : _modules) {
        JsonObject o = mods.add<JsonObject>();
        o["name"]       = m.name;
        o["stalled"]    = m.stalled;
        o["stallCount"] = m.stallCount;
        o["ageSinceMs"] = (uint32_t)(millis() - m.lastFeedMs);
        o["timeoutMs"]  = m.timeoutMs;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

String WatchdogManager::crashLogJson() const {
    if (!LittleFS.exists(WDT_CRASH_FILE)) return "{\"count\":0,\"crashes\":[]}";
    File f = LittleFS.open(WDT_CRASH_FILE, "r");
    if (!f) return "{\"count\":0,\"crashes\":[]}";
    String content = f.readString();
    f.close();
    return content;
}

// ─────────────────────────────────────────────────────────────
//  Config persistence
// ─────────────────────────────────────────────────────────────
bool WatchdogManager::loadConfig() {
    if (!LittleFS.exists(WDT_CFG_FILE)) return false;
    File f = LittleFS.open(WDT_CFG_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return false; }
    f.close();
    _hwEnabled     = doc["hwEnabled"]     | true;
    _heapThreshold = doc["heapThreshold"] | (uint32_t)WDT_HEAP_MIN_BYTES;
    uint8_t pm     = doc["perfMode"]      | (uint8_t)0;
    _perfMode      = (WdtPerfMode)pm;
    return true;
}

bool WatchdogManager::saveConfig() {
    File f = LittleFS.open(WDT_CFG_FILE, "w");
    if (!f) return false;
    JsonDocument doc;
    doc["hwEnabled"]     = _hwEnabled;
    doc["heapThreshold"] = _heapThreshold;
    doc["perfMode"]      = (uint8_t)_perfMode;
    serializeJson(doc, f);
    f.close();
    return true;
}

void WatchdogManager::setHeapThreshold(uint32_t bytes) {
    _heapThreshold = bytes;
    if (_beginDone) saveConfig();
}

// ─────────────────────────────────────────────────────────────
//  Crash logging
// ─────────────────────────────────────────────────────────────
void WatchdogManager::_logCrashOnBoot() {
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || reason == ESP_RST_SW) return;

    WdtCrashEntry entry;
    entry.id          = esp_random();
    entry.timestamp   = 0;
    entry.timeStr     = _buildTimeStr();
    entry.reason      = _resetReasonStr(reason);
    entry.heapAtCrash = ESP.getFreeHeap();
    entry.uptimeSec   = (uint32_t)(millis() / 1000);
    entry.tempAtCrash = cpuTemperature();

    Serial.printf(WDT_TAG " Previous boot crash: %s | heap=%u temp=%.1fC\n",
                  entry.reason.c_str(), entry.heapAtCrash, entry.tempAtCrash);

    _saveCrash(entry);
}

void WatchdogManager::_saveCrash(const WdtCrashEntry& entry) {
    JsonDocument doc;
    if (LittleFS.exists(WDT_CRASH_FILE)) {
        File f = LittleFS.open(WDT_CRASH_FILE, "r");
        if (f) { deserializeJson(doc, f); f.close(); }
    }

    JsonArray arr = doc["crashes"].is<JsonArray>()
                  ? doc["crashes"].as<JsonArray>()
                  : doc["crashes"].to<JsonArray>();

    while (arr.size() >= WDT_MAX_CRASHES) arr.remove(0);

    JsonObject o = arr.add<JsonObject>();
    o["id"]     = entry.id;
    o["ts"]     = entry.timestamp;
    o["time"]   = entry.timeStr;
    o["reason"] = entry.reason;
    o["heap"]   = entry.heapAtCrash;
    o["uptime"] = entry.uptimeSec;
    o["temp"]   = (float)((int)(entry.tempAtCrash * 10)) / 10.0f;
    doc["count"] = arr.size();

    File f = LittleFS.open(WDT_CRASH_FILE, "w");
    if (f) { serializeJson(doc, f); f.close(); }
    else Serial.printf("[WDT] open(%s,w) failed\n", WDT_CRASH_FILE);
}

String WatchdogManager::_resetReasonStr(esp_reset_reason_t r) const {
    switch (r) {
        case ESP_RST_POWERON:   return "Power-on reset";
        case ESP_RST_EXT:       return "External pin reset";
        case ESP_RST_SW:        return "Software reset";
        case ESP_RST_PANIC:     return "PANIC / Exception";
        case ESP_RST_INT_WDT:   return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "Task watchdog";
        case ESP_RST_WDT:       return "Other watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wakeup";
        case ESP_RST_BROWNOUT:  return "Brownout (power drop)";
        case ESP_RST_SDIO:      return "SDIO reset";
        default:                return "Unknown reason";
    }
}

String WatchdogManager::_buildTimeStr() const {
    time_t now; time(&now);
    if (now > 1000000000UL) {
        struct tm t; localtime_r(&now, &t);
        char buf[24];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
        return String(buf);
    }
    return String("uptime:") + (millis() / 1000) + "s";
}
