// ============================================================
//  ir_jammer.cpp  -  IR Jammer (ported from Bruce firmware)
// ============================================================
#include "ir_jammer.h"

IrJammer irJammer;

void IrJammer::begin(uint8_t txPin) {
    _pin = txPin;
    if (_ir) { delete _ir; _ir = nullptr; }
    _ir = new IRsend(txPin);
    _ir->begin();
    pinMode(txPin, OUTPUT);
    digitalWrite(txPin, LOW);
    Serial.printf("[IRJAM] Ready on GPIO%u\n", txPin);
}

void IrJammer::start(JamMode mode, uint8_t freqIdx, uint8_t density) {
    if (!_ir) return;
    _state.active    = true;
    _state.mode      = mode;
    _state.freqIdx   = freqIdx  < IR_JAM_FREQ_COUNT ? freqIdx  : 3;
    _state.density   = density  < 1                 ? 1        : (density > 20 ? 20 : density);
    _state.jamCount  = 0;
    _state.startMs   = millis();
    _state.lastJamMs = 0;
    _state.markTiming  = 12;
    _state.spaceTiming = 12;
    _state.minTiming   = 8;
    _state.maxTiming   = 70;
    _state.sweepSpeed  = 1;
    _state.sweepDir    = 1;
    randomSeed(millis());
    for (int i = 0; i < 30; i++) _state.randomPat[i] = random(10, 1000);
    _updatePatterns();
    Serial.printf("[IRJAM] Started mode=%u freq=%uHz density=%u\n",
                  mode, IR_JAM_FREQS[_state.freqIdx], density);
}

void IrJammer::stop() {
    _state.active = false;
    if (_pin) digitalWrite(_pin, LOW);
    Serial.printf("[IRJAM] Stopped jams=%lu runtime=%lus\n",
                  _state.jamCount, (millis() - _state.startMs) / 1000);
}

void IrJammer::loop() {
    if (!_state.active || !_ir) return;

    uint32_t now = millis();
    uint32_t interval = 10 / max((uint8_t)1, _state.density);
    if (now - _state.lastJamMs < interval) return;
    _state.lastJamMs = now;

    switch (_state.mode) {
        case JAM_BASIC:    _doBasic();    break;
        case JAM_ENHANCED: _doEnhanced(); break;
        case JAM_SWEEP:    _doSweep();    break;
        case JAM_RANDOM:   _doRandom();   break;
        case JAM_EMPTY:    _doEmpty();    break;
    }
    _state.jamCount++;
}

void IrJammer::_updatePatterns() {
    for (int i = 0; i < 20; i += 2) {
        _state.basicPat[i]   = _state.markTiming;
        _state.basicPat[i+1] = _state.spaceTiming;
    }
}

void IrJammer::_doBasic() {
    uint16_t freq = IR_JAM_FREQS[_state.freqIdx];
    // Cap iterations to prevent blocking loop() more than ~5ms
    uint8_t d = min(_state.density, (uint8_t)4);
    for (int i = 0; i < 50 * d; i++) {
        digitalWrite(_pin, HIGH); delayMicroseconds(_state.markTiming);
        digitalWrite(_pin, LOW);  delayMicroseconds(_state.markTiming);
    }
    _ir->sendRaw(_state.basicPat, 20, freq / 1000);
}

void IrJammer::_doEnhanced() {
    uint16_t freq = IR_JAM_FREQS[_state.freqIdx];
    uint8_t d = min(_state.density, (uint8_t)4);
    for (int i = 0; i < 25 * d; i++) {
        digitalWrite(_pin, HIGH); delayMicroseconds(_state.markTiming);
        digitalWrite(_pin, LOW);  delayMicroseconds(_state.spaceTiming);
    }
    _ir->sendRaw(_state.basicPat, 20, freq / 1000);
}

void IrJammer::_doSweep() {
    uint16_t freq = IR_JAM_FREQS[_state.freqIdx];
    _state.markTiming += _state.sweepDir * _state.sweepSpeed;
    if (_state.markTiming > _state.maxTiming || _state.markTiming < _state.minTiming) {
        _state.sweepDir *= -1;
        _state.markTiming = constrain(_state.markTiming, _state.minTiming, _state.maxTiming);
    }
    _state.spaceTiming = _state.markTiming;
    _updatePatterns();
    // Cap density to 4 — same as _doBasic()/_doEnhanced() — to keep the
    // worst-case loop() block at ~5ms. Without this cap, density=20 and
    // markTiming=70µs produces ~56ms of busy-wait per loop tick.
    uint8_t d = min(_state.density, (uint8_t)4);
    for (int i = 0; i < 20 * d; i++) {
        digitalWrite(_pin, HIGH); delayMicroseconds(_state.markTiming);
        digitalWrite(_pin, LOW);  delayMicroseconds(_state.markTiming);
    }
    _ir->sendRaw(_state.basicPat, 20, freq / 1000);
}

void IrJammer::_doRandom() {
    for (int i = 0; i < 30; i++) _state.randomPat[i] = random(5, 1000);
    for (int i = 0; i < _state.density / 2 + 1; i++) {
        if (random(10) < 3) _state.freqIdx = random(IR_JAM_FREQ_COUNT);
        _ir->sendRaw(_state.randomPat, 30, IR_JAM_FREQS[_state.freqIdx] / 1000);
    }
}

void IrJammer::_doEmpty() {
    uint16_t freq = IR_JAM_FREQS[_state.freqIdx];
    for (int i = 0; i < _state.density; i++)
        _ir->sendRaw(_state.emptyPat, 4, freq / 1000);
    if (random(5) < 2) _state.freqIdx = (_state.freqIdx + 1) % IR_JAM_FREQ_COUNT;
}

String IrJammer::statusJson() const {
    uint32_t runtime = _state.active ? (millis() - _state.startMs) / 1000 : 0;
    float jps = runtime > 0 ? (float)_state.jamCount / runtime : 0;
    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"active\":%s,\"mode\":%u,\"modeName\":\"%s\","
        "\"freqHz\":%u,\"density\":%u,"
        "\"jamCount\":%lu,\"runtimeSec\":%lu,\"jps\":%.1f}",
        _state.active ? "true" : "false",
        (uint8_t)_state.mode,
        JAM_MODE_NAMES[_state.mode],
        IR_JAM_FREQS[_state.freqIdx],
        _state.density,
        _state.jamCount,
        runtime,
        jps);
    return String(buf);
}
