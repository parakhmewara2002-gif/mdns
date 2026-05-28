#pragma once
// ============================================================
//  system_module.h  -  System Module
//  LED (FastLED) + GhostLink + Schedule Tasks
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <FastLED.h>
#include <vector>
#include <string>

#define SYS_CFG_FILE    "/sys_config.json"
#define SYS_SCHED_FILE  "/sys_schedules.json"
#define LED_CFG_FILE    "/led_config.json"
#define MAX_LEDS        64

enum class LedMode { OFF, SOLID, RAINBOW, RAVE, BLINK, PULSE };
enum class LedType { WS2812B, WS2811, SK6812 };

struct LedConfig {
    LedType type     = LedType::WS2812B;
    LedMode mode     = LedMode::OFF;
    uint8_t dataPin  = 13;  // GPIO2 forbidden (boot strapping) - moved to GPIO13
    uint8_t numLeds  = 8;
    uint8_t r        = 255;
    uint8_t g        = 0;
    uint8_t b        = 128;
    uint8_t brightness = 128;
};

struct SysScheduleTask {
    uint32_t id      = 0;
    String   name;
    String   time;
    String   action;
    bool     enabled = true;
};

class SystemModule {
public:
    void  begin();
    void  loop();

    // Status JSON
    String getStatusJson() const;

    // LED
    void      setLedMode(const LedConfig& cfg);
    void      saveLedConfig(const LedConfig& cfg);
    LedConfig loadLedConfig() const;
    bool      isLedActive() const { return _ledCfg.mode != LedMode::OFF; }
    void      ledTick();

    // GhostLink
    void    setGhostLink(bool en);
    bool    isGhostLinkEnabled() const { return _ghostLink; }

    // Timezone
    void    setTimezone(const String& tz);
    String  getTimezone() const { return _timezone; }

    // Schedule tasks
    uint32_t addScheduleTask(SysScheduleTask& task);
    bool     deleteScheduleTask(uint32_t id);
    bool     toggleScheduleTask(uint32_t id, bool en);
    String   scheduleTasksToJson() const;

    // GPIO overview
    String  gpioOverviewJson() const;

    // Hardware status
    String hardwareStatusJson() const;

    // ── SD Integration (features 39-41) ──────────────────────
    // Feature 39: LED config backup/restore to/from SD
    bool backupLedConfigToSD(const String& tag);
    bool restoreLedConfigFromSD(const String& tag);

    // Feature 40: LED scene presets stored on SD
    bool saveLedPreset(const String& name);
    bool loadLedPreset(const String& name);
    std::vector<String> listLedPresets() const;

    // Feature 41: Dump diagnostics snapshot to SD
    bool dumpDiagnosticsToSD();

private:
    bool    _ghostLink = false;
    String  _timezone  = "IST";
    LedConfig _ledCfg;

    std::vector<SysScheduleTask> _schedTasks;
    unsigned long _ledTimer  = 0;
    uint8_t       _ledHue    = 0;
    uint8_t       _ledBlink  = 0;

    // FastLED
    CRGB _leds[MAX_LEDS];
    bool _fastledInited = false;
    void _initFastLED();

    void _loadConfig();
    void _saveConfig() const;
    void _loadScheduleTasks();
    void _saveScheduleTasks() const;
    void _applyLed();
};

extern SystemModule sysModule;
