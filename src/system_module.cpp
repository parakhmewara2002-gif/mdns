// ============================================================
//  system_module.cpp  -  LED Real Implementation
// ============================================================
#include "system_module.h"
#include "sd_manager.h"  // Feature 39-41: SD integration
#include <WiFi.h>
#include <ctime>

SystemModule sysModule;

// ─────────────────────────────────────────────────────────────
void SystemModule::begin() {
    _loadConfig();
    _loadScheduleTasks();
    _ledCfg  = loadLedConfig();
    _initFastLED();
    Serial.println("[SYS] System module initialized");
}

void SystemModule::loop() {
    // LED tick every 20ms
    if (millis() - _ledTimer > 20) {
        _ledTimer = millis();
        ledTick();
    }
}

// ─────────────────────────────────────────────────────────────
void SystemModule::_initFastLED() {
    if (_ledCfg.numLeds == 0) return;
    uint8_t n = min((uint8_t)MAX_LEDS, _ledCfg.numLeds);

    // FASTLED FIX: skip if already initialised. FastLED's controller
    // list is append-only — clearData() zeros pixel buffers but NOT
    // the controller chain, so every addLeds() call added a duplicate
    // WS2812 driver. After a few preset loads .show() would write the
    // same DMA buffer through N stacked controllers, dragging frame
    // time and leaking driver state. Now: only the first call wires
    // up the controller; subsequent loads just update brightness +
    // refresh.
    if (_fastledInited) {
        FastLED.setBrightness(_ledCfg.brightness);
        FastLED.clear(true);
        return;
    }

    // FastLED requires compile-time DATA_PIN template parameter.
    // GPIO2 is forbidden (boot strapping pin) - use GPIO13 instead.
    // GPIO13 is safe: not forbidden, not input-only, no other module uses it permanently.
    FastLED.clearData();
    FastLED.addLeds<WS2812B, 13, GRB>(_leds, n);
    FastLED.setBrightness(_ledCfg.brightness);
    FastLED.clear(true);
    _fastledInited = true;
    Serial.printf("[SYS] FastLED: %u LEDs GPIO13 brightness=%u\n",
                  n, _ledCfg.brightness);
}

void SystemModule::ledTick() {
    if (!_fastledInited) return;
    uint8_t n = min((uint8_t)MAX_LEDS, _ledCfg.numLeds);

    switch (_ledCfg.mode) {
        case LedMode::SOLID:
            for (int i = 0; i < n; i++)
                _leds[i] = CRGB(_ledCfg.r, _ledCfg.g, _ledCfg.b);
            FastLED.show();
            break;

        case LedMode::RAINBOW:
            for (int i = 0; i < n; i++)
                _leds[i] = CHSV(_ledHue + (i * 255 / n), 255, 200);
            _ledHue++;
            FastLED.show();
            break;

        case LedMode::RAVE:
            for (int i = 0; i < n; i++)
                _leds[i] = CHSV(random8(), 255, 200);
            FastLED.show();
            break;

        case LedMode::BLINK:
            _ledBlink = (_ledBlink + 1) & 0x1F;
            for (int i = 0; i < n; i++)
                _leds[i] = (_ledBlink < 16)
                    ? CRGB(_ledCfg.r, _ledCfg.g, _ledCfg.b)
                    : CRGB::Black;
            FastLED.show();
            break;

        case LedMode::PULSE: {
            uint8_t bright = (uint8_t)(128 + 127 * sin(_ledHue * 0.05f));
            for (int i = 0; i < n; i++)
                _leds[i] = CRGB(_ledCfg.r, _ledCfg.g, _ledCfg.b).nscale8(bright);
            _ledHue++;
            FastLED.show();
            break;
        }

        case LedMode::OFF:
        default:
            FastLED.clear(); FastLED.show();
            break;
    }
}

// ─────────────────────────────────────────────────────────────
void SystemModule::setLedMode(const LedConfig& cfg) {
    _ledCfg = cfg;
    saveLedConfig(cfg);
    if (!_fastledInited) _initFastLED();
    FastLED.setBrightness(cfg.brightness);
    _applyLed();
}

void SystemModule::_applyLed() {
    if (_ledCfg.mode == LedMode::OFF) {
        FastLED.clear(); FastLED.show();
    }
}

void SystemModule::saveLedConfig(const LedConfig& cfg) {
    File f = LittleFS.open(LED_CFG_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["type"]       = (int)cfg.type;
    doc["mode"]       = (int)cfg.mode;
    doc["dataPin"]    = cfg.dataPin;
    doc["numLeds"]    = cfg.numLeds;
    doc["r"]          = cfg.r;
    doc["g"]          = cfg.g;
    doc["b"]          = cfg.b;
    doc["brightness"] = cfg.brightness;
    serializeJson(doc, f);
    f.close();
}

LedConfig SystemModule::loadLedConfig() const {
    LedConfig cfg;
    if (!LittleFS.exists(LED_CFG_FILE)) return cfg;
    File f = LittleFS.open(LED_CFG_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.type       = (LedType)(doc["type"]       | 0);
        cfg.mode       = (LedMode)(doc["mode"]       | 0);
        cfg.dataPin    = doc["dataPin"]    | (uint8_t)13;  // GPIO2 forbidden - default to GPIO13
        cfg.numLeds    = doc["numLeds"]    | (uint8_t)8;
        cfg.r          = doc["r"]          | (uint8_t)255;
        cfg.g          = doc["g"]          | (uint8_t)0;
        cfg.b          = doc["b"]          | (uint8_t)128;
        cfg.brightness = doc["brightness"] | (uint8_t)128;
    }
    f.close();
    return cfg;
}

// ─────────────────────────────────────────────────────────────
String SystemModule::getStatusJson() const {
    JsonDocument doc;
    doc["ok"]         = true;
    doc["heap"]       = ESP.getFreeHeap();
    doc["uptime"]     = millis() / 1000;
    doc["cpuMhz"]     = getCpuFrequencyMhz();
    doc["cpuFreq"]    = getCpuFrequencyMhz();
    doc["chip"]       = ESP.getChipModel();
    doc["chipModel"]  = ESP.getChipModel();
    doc["firmware"]   = FIRMWARE_VERSION;
    doc["flashSize"]  = (uint32_t)(ESP.getFlashChipSize() / 1024);
    // MAC address
    uint8_t macBytes[6]; WiFi.macAddress(macBytes);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             macBytes[0], macBytes[1], macBytes[2],
             macBytes[3], macBytes[4], macBytes[5]);
    doc["mac"] = String(macStr);
    // LED
    JsonObject led = doc["led"].to<JsonObject>();
    led["mode"]   = (int)_ledCfg.mode;
    led["active"] = isLedActive();
    led["numLeds"]= _ledCfg.numLeds;
    String out; serializeJson(doc, out);
    return out;
}

String SystemModule::hardwareStatusJson() const {
    char buf[48];
    snprintf(buf, sizeof(buf),
             "{\"led\":%s}",
             _fastledInited?"true":"false");
    return String(buf);
}

// ─────────────────────────────────────────────────────────────
void SystemModule::setGhostLink(bool en) {
    _ghostLink = en; _saveConfig();
}
void SystemModule::setTimezone(const String& tz) {
    _timezone = tz; _saveConfig();
}

void SystemModule::_loadConfig() {
    if (!LittleFS.exists(SYS_CFG_FILE)) return;
    File f = LittleFS.open(SYS_CFG_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        _ghostLink = doc["ghostLink"] | false;
        _timezone  = doc["timezone"]  | (const char*)"IST";
    }
    f.close();
}

void SystemModule::_saveConfig() const {
    File f = LittleFS.open(SYS_CFG_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["ghostLink"] = _ghostLink;
    doc["timezone"]  = _timezone;
    serializeJson(doc, f);
    f.close();
}

// ─────────────────────────────────────────────────────────────
uint32_t SystemModule::addScheduleTask(SysScheduleTask& task) {
    // ID FIX: the previous `static uint32_t nextId = 1` reset to 1 on every
    // boot, so the first task added after reboot collided with whichever
    // persisted task had id=1 — delete/toggle by id would then act on the
    // wrong row. Now we scan the currently loaded tasks and pick max+1.
    uint32_t nextId = 1;
    for (const auto& t : _schedTasks) {
        if (t.id >= nextId) nextId = t.id + 1;
    }
    task.id = nextId;
    _schedTasks.push_back(task);
    _saveScheduleTasks();
    return task.id;
}

bool SystemModule::deleteScheduleTask(uint32_t id) {
    for (auto it = _schedTasks.begin(); it != _schedTasks.end(); ++it) {
        if (it->id == id) { _schedTasks.erase(it); _saveScheduleTasks(); return true; }
    }
    return false;
}

bool SystemModule::toggleScheduleTask(uint32_t id, bool en) {
    for (auto& t : _schedTasks) {
        if (t.id == id) { t.enabled = en; _saveScheduleTasks(); return true; }
    }
    return false;
}

String SystemModule::scheduleTasksToJson() const {
    String out = "{\"tasks\":[";
    for (size_t i = 0; i < _schedTasks.size(); i++) {
        if (i) out += ',';
        const auto& t = _schedTasks[i];
        out += "{\"id\":" + String(t.id)
             + ",\"name\":\"" + t.name
             + "\",\"time\":\"" + t.time
             + "\",\"action\":\"" + t.action
             + "\",\"enabled\":" + (t.enabled?"true":"false") + "}";
    }
    out += "]}";
    return out;
}

void SystemModule::_loadScheduleTasks() {
    _schedTasks.clear();
    if (!LittleFS.exists(SYS_SCHED_FILE)) return;
    File f = LittleFS.open(SYS_SCHED_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        for (JsonObject o : doc["tasks"].as<JsonArray>()) {
            SysScheduleTask t;
            t.id      = o["id"]      | (uint32_t)0;
            t.name    = o["name"]    | "";
            t.time    = o["time"]    | "";
            t.action  = o["action"]  | "";
            t.enabled = o["enabled"] | true;
            _schedTasks.push_back(t);
        }
    }
    f.close();
}

void SystemModule::_saveScheduleTasks() const {
    File f = LittleFS.open(SYS_SCHED_FILE, "w");
    if (!f) return;
    f.print("{\"tasks\":[");
    bool first = true;
    for (const auto& t : _schedTasks) {
        if (!first) f.print(',');
        first = false;
        f.printf("{\"id\":%u,\"name\":\"%s\",\"time\":\"%s\","
                 "\"action\":\"%s\",\"enabled\":%s}",
                 t.id, t.name.c_str(), t.time.c_str(),
                 t.action.c_str(), t.enabled?"true":"false");
    }
    f.print("]}");
    f.close();
}

String SystemModule::gpioOverviewJson() const {
    // Return used pins for GPIO conflict detection
    JsonDocument doc;
    JsonArray pins = doc["used"].to<JsonArray>();
    // LED pin
    if (_ledCfg.mode != LedMode::OFF) {
        JsonObject o = pins.add<JsonObject>();
        o["pin"] = _ledCfg.dataPin; o["module"] = "LED";
    }
    String out; serializeJson(doc, out);
    return out;
}

// ─────────────────────────────────────────────────────────────
//  Feature 39: LED config backup/restore via SD
// ─────────────────────────────────────────────────────────────
bool SystemModule::backupLedConfigToSD(const String& tag) {
    if (!sdMgr.isAvailable()) return false;
    if (!LittleFS.exists(LED_CFG_FILE)) return false;

    // Ensure backup dir exists
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;
    if (!SD.exists(SD_DIR_BACKUPS)) SD.mkdir(SD_DIR_BACKUPS);
    if (!SD.exists(dir))            SD.mkdir(dir);

    String dst = dir + "/led_config.json";
    // Read from LittleFS, write to SD
    File src = LittleFS.open(LED_CFG_FILE, "r");
    if (!src) return false;
    File dstF = SD.open(dst, FILE_WRITE);
    if (!dstF) { src.close(); return false; }

    uint8_t buf[128];
    while (src.available()) {
        size_t n = src.readBytes((char*)buf, sizeof(buf));
        dstF.write(buf, n);
    }
    src.close();
    dstF.close();
    Serial.printf("[SYS] LED config backed up to SD: %s\n", dst.c_str());
    return true;
}

bool SystemModule::restoreLedConfigFromSD(const String& tag) {
    if (!sdMgr.isAvailable()) return false;

    String src = String(SD_DIR_BACKUPS) + "/" + tag + "/led_config.json";
    if (!SD.exists(src)) return false;

    File srcF = SD.open(src, FILE_READ);
    if (!srcF) return false;
    File dstF = LittleFS.open(LED_CFG_FILE, "w");
    if (!dstF) { srcF.close(); return false; }

    uint8_t buf[128];
    while (srcF.available()) {
        size_t n = srcF.readBytes((char*)buf, sizeof(buf));
        dstF.write(buf, n);
    }
    srcF.close();
    dstF.close();

    // Reload and re-init
    _ledCfg = loadLedConfig();
    _initFastLED();
    Serial.printf("[SYS] LED config restored from SD tag: %s\n", tag.c_str());
    return true;
}

// ─────────────────────────────────────────────────────────────
//  Feature 40: LED scene presets on SD
// ─────────────────────────────────────────────────────────────
bool SystemModule::saveLedPreset(const String& name) {
    if (!sdMgr.isAvailable()) return false;

    sdMgr.makeDir("/led");
    String path = String("/led/") + name + ".json";

    File f = sdMgr.openForWrite(path);
    if (!f) return false;

    JsonDocument doc;
    doc["type"]       = (int)_ledCfg.type;
    doc["mode"]       = (int)_ledCfg.mode;
    doc["dataPin"]    = _ledCfg.dataPin;
    doc["numLeds"]    = _ledCfg.numLeds;
    doc["r"]          = _ledCfg.r;
    doc["g"]          = _ledCfg.g;
    doc["b"]          = _ledCfg.b;
    doc["brightness"] = _ledCfg.brightness;
    serializeJson(doc, f);
    f.close();

    Serial.printf("[SYS] LED preset '%s' saved to SD\n", name.c_str());
    return true;
}

bool SystemModule::loadLedPreset(const String& name) {
    if (!sdMgr.isAvailable()) return false;

    String path = String("/led/") + name + ".json";
    if (!sdMgr.exists(path)) return false;

    File f = sdMgr.openForRead(path);
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    _ledCfg.type       = (LedType)(doc["type"]       | 0);
    _ledCfg.mode       = (LedMode)(doc["mode"]       | 0);
    _ledCfg.dataPin    = doc["dataPin"]    | (uint8_t)13;
    _ledCfg.numLeds    = doc["numLeds"]    | (uint8_t)8;
    _ledCfg.r          = doc["r"]          | (uint8_t)255;
    _ledCfg.g          = doc["g"]          | (uint8_t)0;
    _ledCfg.b          = doc["b"]          | (uint8_t)128;
    _ledCfg.brightness = doc["brightness"] | (uint8_t)128;

    _initFastLED();
    FastLED.setBrightness(_ledCfg.brightness);
    _applyLed();

    Serial.printf("[SYS] LED preset '%s' loaded from SD\n", name.c_str());
    return true;
}

std::vector<String> SystemModule::listLedPresets() const {
    std::vector<String> result;
    if (!sdMgr.isAvailable()) return result;

    auto entries = sdMgr.listDir("/led");
    for (const auto& e : entries) {
        if (!e.isDir && e.name.endsWith(".json")) {
            result.push_back(e.name);
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────
//  Feature 41: Diagnostics snapshot to SD
// ─────────────────────────────────────────────────────────────
bool SystemModule::dumpDiagnosticsToSD() {
    if (!sdMgr.isAvailable()) return false;

    // Ensure logs dir exists
    String dir = "/logs";
    if (!SD.exists(dir)) SD.mkdir(dir);

    char pathBuf[48];
    snprintf(pathBuf, sizeof(pathBuf), "/logs/diag_%lu.json", (unsigned long)millis());
    String path = String(pathBuf);

    File f = sdMgr.openForWrite(path);
    if (!f) return false;

    time_t now;
    time(&now);

    JsonDocument doc;
    doc["heap_free"]       = ESP.getFreeHeap();
    doc["heap_min"]        = ESP.getMinFreeHeap();
    doc["uptime_ms"]       = (unsigned long)millis();
    doc["cpu_freq"]        = ESP.getCpuFreqMHz();
    doc["flash_size"]      = (unsigned long)ESP.getFlashChipSize();
    doc["littlefs_total"]  = (unsigned long)LittleFS.totalBytes();
    doc["littlefs_used"]   = (unsigned long)LittleFS.usedBytes();
    doc["timestamp"]       = (unsigned long)now;
    serializeJson(doc, f);
    f.close();

    Serial.printf("[SYS] Diagnostics snapshot written to SD: %s\n", path.c_str());
    return true;
}
