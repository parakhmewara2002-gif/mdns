#pragma once
// ============================================================
//  macro_manager.h  -  Internal LittleFS macro subsystem
//
//  v2.2.0 (Upgrade 4):
//    Stores multi-step IR macros directly in LittleFS under
//    /macros/<name>.json - NO SD card required.
//
//  FORMAT  /macros/tv_on.json
//  {
//    "name": "TV On + HDMI1",
//    "steps": [
//      {"buttonId": 12, "delayAfterMs": 500},
//      {"buttonId": 15, "delayAfterMs": 200},
//      {"buttonId": 15, "delayAfterMs": 0}
//    ]
//  }
//
//  API ENDPOINTS
//  ─────────────
//  GET  /api/macros          - list all macro names + step counts
//  GET  /api/macro?name=x    - read macro JSON
//  POST /api/macro           - create/replace macro (body = JSON)
//  GET  /api/macro/delete?name=x  - delete macro
//  GET  /api/macro/run?name=x     - run macro (non-blocking via loop())
//  GET  /api/macro/status    - running state + progress
// ============================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>
#include "config.h"

struct MacroInternalStep {
    uint32_t buttonId;
    uint32_t delayAfterMs;
};

struct MacroInternalMeta {
    String name;       // filename without .json
    String label;      // "name" field inside JSON
    uint8_t stepCount;
};

class MacroManager {
public:
    MacroManager();

    // ── Lifecycle ─────────────────────────────────────────────
    void begin();               // create /macros dir if missing
    void loop();                // tick running macro steps

    // ── CRUD ──────────────────────────────────────────────────
    // List all macros (reads directory + peeks each file)
    std::vector<MacroInternalMeta> list() const;

    // Read a macro's full step list. Returns false if not found.
    bool load(const String& name,
              String& outLabel,
              std::vector<MacroInternalStep>& outSteps) const;

    // Save/replace a macro from raw JSON body. Returns error string or "".
    String save(const String& name, const uint8_t* data, size_t len);

    // Delete a macro file. Returns false if not found.
    bool remove(const String& name);

    // ── Run ───────────────────────────────────────────────────
    // Start running a named macro. Returns false if not found or busy.
    bool run(const String& name);

    // True while a macro is executing
    bool isRunning()        const { return _running; }
    String runningName()    const { return _runName; }
    uint8_t  runStep()      const { return _stepIdx; }
    uint8_t  runTotal()     const { return (uint8_t)_steps.size(); }

    // Abort a running macro
    void abort();

    // TX callback (set before begin) - called for each step's buttonId
    using TxCallback = std::function<void(uint32_t buttonId)>;
    void onTransmit(TxCallback cb) { _txCb = cb; }

private:
    bool          _running;
    String        _runName;
    uint8_t       _stepIdx;
    unsigned long _stepAt;      // millis() when next step should fire
    std::vector<MacroInternalStep> _steps;
    TxCallback    _txCb;

    // Build full path: /macros/<name>.json
    String _path(const String& name) const;
    // Sanitise name: only [A-Za-z0-9_-], max MACRO_NAME_MAX chars
    bool   _validName(const String& name) const;
};

extern MacroManager macroMgr;
