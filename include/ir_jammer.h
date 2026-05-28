#pragma once
// ============================================================
//  ir_jammer.h  -  IR Jammer (ported from Bruce firmware)
//  WebUI controlled — no screen needed
// ============================================================
#include <Arduino.h>
#include <IRsend.h>

enum JamMode : uint8_t { JAM_BASIC=0, JAM_ENHANCED, JAM_SWEEP, JAM_RANDOM, JAM_EMPTY };
#define JAM_MODE_COUNT 5

static const char* const JAM_MODE_NAMES[] = {"BASIC","ENHANCED","SWEEP","RANDOM","EMPTY"};
static const uint16_t    IR_JAM_FREQS[]   = {30000,33000,36000,38000,40000,42000,56000};
#define IR_JAM_FREQ_COUNT 7

struct IrJamState {
    bool     active        = false;
    JamMode  mode          = JAM_BASIC;
    uint8_t  freqIdx       = 3;    // default 38kHz
    uint8_t  density       = 5;
    uint8_t  markTiming    = 12;
    uint8_t  spaceTiming   = 12;
    uint8_t  minTiming     = 8;
    uint8_t  maxTiming     = 70;
    uint8_t  sweepSpeed    = 1;
    int8_t   sweepDir      = 1;
    uint32_t jamCount      = 0;
    uint32_t startMs       = 0;
    uint32_t lastJamMs     = 0;
    uint16_t basicPat[20]  = {};
    uint16_t randomPat[30] = {};
    uint16_t emptyPat[4]   = {9000,4500,560,560};
};

class IrJammer {
public:
    void   begin(uint8_t txPin);
    void   loop();

    void   start(JamMode mode = JAM_BASIC, uint8_t freqIdx = 3, uint8_t density = 5);
    void   stop();

    bool   isActive()   const { return _state.active; }
    JamMode mode()      const { return _state.mode; }
    String statusJson() const;

private:
    IrJamState _state;
    IRsend*    _ir    = nullptr;
    uint8_t    _pin   = 0;

    void _updatePatterns();
    void _doBasic();
    void _doEnhanced();
    void _doSweep();
    void _doRandom();
    void _doEmpty();
};

extern IrJammer irJammer;
