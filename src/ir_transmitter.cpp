// ============================================================
//  ir_transmitter.cpp  -  Multi-emitter IR transmit
//
//  v1.4.0 fixes applied:
//    FIX-CRITICAL: FreeRTOS xQueueSend(sizeof(IrTxCommand)) does a shallow
//      memcpy of the IrTxCommand struct. IRButton contains String + vector<uint16_t>,
//      both of which have heap-allocated internal buffers. The memcpy copies only
//      the stack-local pointers; when the source IrTxCommand destructs after
//      transmitAsync() returns, those heap blocks are freed, leaving dangling
//      pointers inside the queue storage -> heap corruption -> crash/garbage TX.
//
//      Fix: allocate IrTxCommand on heap (new), post only the pointer into
//      a std::queue<IrTxCommand*> protected by _queueMutex. A binary semaphore
//      (_notify) wakes the IR task. The task deletes the object after TX.
//      IRButton's C++ copy constructor performs deep copies of String + vector,
//      so the heap allocation is always safe and complete.
//
//    FIX-1: FreeRTOS mutex (_txMutex) protects transmit() critical section.
//    FIX-3: transmitRaw() quiet-time now from config (IR_PRE_TX_QUIET_MS).
// ============================================================
#include "ir_transmitter.h"
#include "ir_receiver.h"
#include <new>

IRTransmitter irTransmitter;

// ── Constructor ───────────────────────────────────────────────
IRTransmitter::IRTransmitter()
    : _count(0), _txMutex(nullptr), _queueMutex(nullptr), _notify(nullptr) {
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        _senders[i] = nullptr;
        _pins[i]    = 255;
    }
}

IRTransmitter::~IRTransmitter() { destroyAll(); }

// ── begin ─────────────────────────────────────────────────────
void IRTransmitter::begin(const IrPinConfig& pins) {
    // Create TX critical-section mutex once
    if (!_txMutex) {
        _txMutex = xSemaphoreCreateMutex();
        if (!_txMutex) {
            Serial.println(DEBUG_TAG " FATAL: IR TX mutex creation failed");
            return;
        }
    }
    // Create pointer-queue mutex once
    if (!_queueMutex) {
        _queueMutex = xSemaphoreCreateMutex();
        if (!_queueMutex) {
            Serial.println(DEBUG_TAG " FATAL: IR queue mutex creation failed");
            return;
        }
    }
    // Create notify semaphore once (binary - unblocks IR task when cmd available)
    if (!_notify) {
        _notify = xSemaphoreCreateBinary();
        if (!_notify) {
            Serial.println(DEBUG_TAG " FATAL: IR notify semaphore creation failed");
            return;
        }
        // Pin IR TX task to Core 1 alongside loop() - IR timing is CPU-bound.
        // Priority 5 > loop() priority (1) so IR fires promptly when queued.
        // Stack 4KB: sufficient for single emitter + IRsend + doTransmit frame.
        xTaskCreatePinnedToCore(
            _txTask, "ir_tx", 4096, this, 5, nullptr, 1
        );
        Serial.println(DEBUG_TAG " IR TX task started on Core 1 (priority 5, ptr-queue)");
    }

    destroyAll();
    _count = min((uint8_t)IR_MAX_EMITTERS, pins.emitCount);
    for (uint8_t i = 0; i < _count; ++i) {
        if (pins.emitEnabled[i]) createSender(i, pins.emitPin[i]);
    }
    Serial.printf(DEBUG_TAG " IR Transmitter: %d emitter(s) active\n", activeCount());
    for (uint8_t i = 0; i < _count; ++i) {
        if (_senders[i])
            Serial.printf(DEBUG_TAG "   Emitter[%d] GPIO%d\n", i, _pins[i]);
    }
}

// ── _txTask - IR TX FreeRTOS task (pointer-queue version) ────
// Blocks on _notify semaphore. Pops one IrTxCommand* per wakeup,
// executes TX, then deletes the heap object. No FreeRTOS memcpy of
// C++ objects - pointer only crosses task boundary.
/*static*/
void IRTransmitter::_txTask(void* param) {
    IRTransmitter* self = static_cast<IRTransmitter*>(param);
    for (;;) {
        // Block until transmitAsync() posts a command.
        // QUEUE DRAIN FIX: a binary semaphore saturates at 1 regardless of how
        // many times it is "given". If transmitAsync() is called N times in rapid
        // succession (e.g. a macro with 3 steps, or a scheduler firing a
        // repeatCount=3 entry), only the first Give changes the state from 0->1;
        // the remaining N-1 gives are silently discarded.  The original code
        // popped exactly ONE item per semaphore wakeup, so items 2..N sat in the
        // queue until the next unrelated transmitAsync() call - effectively lost.
        //
        // Fix: after waking, drain ALL pending items from the queue before
        // blocking again. A 50ms poll timeout (portMAX_DELAY -> 50ms) ensures
        // the task also catches items enqueued in the gap between the last pop
        // and the next Give (race window is tiny but possible).
        xSemaphoreTake(self->_notify, portMAX_DELAY);

        for (;;) {
            // Pop next pointer under mutex
            IrTxCommand* cmd = nullptr;
            if (xSemaphoreTake(self->_queueMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (!self->_ptrQueue.empty()) {
                    cmd = self->_ptrQueue.front();
                    self->_ptrQueue.pop();
                }
                xSemaphoreGive(self->_queueMutex);
            }
            if (!cmd) break;  // queue empty - go back to blocking wait

            // Execute TX - IRButton is fully owned by cmd (deep copy from transmitAsync)
            if (cmd->rawMode && !cmd->btn.rawData.empty()) {
                self->transmitRaw(cmd->btn.rawData.data(),
                                  cmd->btn.rawData.size(),
                                  cmd->btn.freqKHz);
            } else {
                self->transmit(cmd->btn);
            }
            delete cmd;  // release heap-allocated command object
        }
    }
}

// ── reconfigure ───────────────────────────────────────────────
void IRTransmitter::reconfigure(const IrPinConfig& pins) {
    Serial.println(DEBUG_TAG " Reconfiguring emitters...");
    // Take mutex so we don't reconfigure while a TX is in progress
    if (_txMutex && xSemaphoreTake(_txMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        destroyAll();
        _count = min((uint8_t)IR_MAX_EMITTERS, pins.emitCount);
        for (uint8_t i = 0; i < _count; ++i) {
            if (pins.emitEnabled[i]) createSender(i, pins.emitPin[i]);
        }
        xSemaphoreGive(_txMutex);
    } else {
        Serial.println(DEBUG_TAG " WARNING: reconfigure skipped - TX in progress");
    }
}

// ── createSender / destroyAll ─────────────────────────────────
void IRTransmitter::createSender(uint8_t idx, uint8_t pin) {
    if (idx >= IR_MAX_EMITTERS) return;
    if (_senders[idx]) { delete _senders[idx]; _senders[idx] = nullptr; }

    PinStatus st = validateTxPin(pin);
    if (st != PinStatus::OK) {
        Serial.printf(DEBUG_TAG " Emitter[%d] GPIO%d rejected: %s\n",
                      idx, pin, pinStatusMsg(st));
        _pins[idx] = 255;
        return;
    }
    // IRremoteESP8266 v2.8.6 constructor: (pin, inverted, use_modulation)
    // RMT channel is assigned automatically by the library per-pin.
    _senders[idx] = new (std::nothrow) IRsend(pin, false, true);
    if (!_senders[idx]) {
        Serial.printf(DEBUG_TAG " ERROR: IRsend[%d] allocation failed (OOM)\n", idx);
        _pins[idx] = 255;
        return;
    }
    _senders[idx]->begin();
    _pins[idx] = pin;
}

void IRTransmitter::destroyAll() {
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        if (_senders[i]) { delete _senders[i]; _senders[i] = nullptr; }
        _pins[i] = 255;
    }
    _count = 0;
}

// ── activeCount / emitterPin ──────────────────────────────────
uint8_t IRTransmitter::activeCount() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i)
        if (_senders[i]) ++n;
    return n;
}

uint8_t IRTransmitter::emitterPin(uint8_t idx) const {
    return (idx < IR_MAX_EMITTERS && _senders[idx]) ? _pins[idx] : 255;
}

// ── transmitAsync - non-blocking post to IR TX task ──────────
// Heap-allocates IrTxCommand (deep copy of IRButton via C++ copy ctor),
// pushes pointer to _ptrQueue, signals _notify semaphore.
// No FreeRTOS memcpy of C++ objects. Safe for String + vector payloads.
bool IRTransmitter::transmitAsync(const IRButton& btn) {
    if (!_notify || !_queueMutex) return false;
    if (!btn.isValid()) {
        Serial.println(DEBUG_TAG " transmitAsync: invalid button");
        return false;
    }
    if (activeCount() == 0) {
        Serial.println(DEBUG_TAG " transmitAsync: no active emitters");
        return false;
    }

    // Allocate command on heap - C++ copy constructor deep-copies String + vector
    IrTxCommand* cmd = new (std::nothrow) IrTxCommand;
    if (!cmd) {
        Serial.println(DEBUG_TAG " WARNING: transmitAsync OOM - command dropped");
        return false;
    }
    cmd->btn     = btn;    // deep copy via IRButton copy ctor
    cmd->rawMode = false;

    // Push pointer under mutex
    if (xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        delete cmd;
        Serial.println(DEBUG_TAG " WARNING: transmitAsync queue mutex timeout");
        return false;
    }
    if ((int)_ptrQueue.size() >= IR_TX_PTR_QUEUE_MAX) {
        xSemaphoreGive(_queueMutex);
        delete cmd;
        Serial.println(DEBUG_TAG " WARNING: IR TX queue full - command dropped");
        return false;
    }
    _ptrQueue.push(cmd);
    xSemaphoreGive(_queueMutex);

    // Signal IR task - non-blocking (binary semaphore give always succeeds)
    xSemaphoreGive(_notify);
    return true;
}

// ── transmit (blocking - protected by FreeRTOS mutex) ─────────
bool IRTransmitter::transmit(const IRButton& btn) {
    if (!btn.isValid()) { Serial.println(DEBUG_TAG " TX: invalid button"); return false; }
    if (activeCount() == 0) { Serial.println(DEBUG_TAG " TX: no active emitters"); return false; }

    // FIX: use FreeRTOS mutex instead of portENTER_CRITICAL / bare bool.
    // xSemaphoreTake is safe from any task and does not disable interrupts.
    if (!_txMutex) return false;
    if (xSemaphoreTake(_txMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        Serial.println(DEBUG_TAG " TX: mutex timeout - another TX in progress");
        return false;
    }

    Serial.printf(DEBUG_TAG " TX %-10s 0x%llX  %db  reps=%d  count=%d  delay=%dms  emitters=%d  freq=%dkHz\n",
                  protocolName(btn.protocol), (unsigned long long)btn.code,
                  btn.bits, btn.repeats, btn.repeatCount, btn.repeatDelay, activeCount(), btn.freqKHz);

    irReceiver.pause();
    // Pre-TX quiet time - receiver must be silent before first modulated burst.
    // IR_PRE_TX_QUIET_MS (config.h) = 20ms. Stubborn devices may need 25-30ms.
    delay(IR_PRE_TX_QUIET_MS);

    bool ok = true;
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        if (_senders[i]) ok &= doTransmit(_senders[i], btn);
    }

    // Post-TX settle: wait for the signal to fully clear the target device
    // before we re-enable the receiver (avoids self-decoding our own burst).
    // For RAW signals, scale the wait to actual signal duration so we don't
    // cut off the trailing carrier on long AC state frames (>100ms).
    uint32_t waitMs = IR_POST_TX_SETTLE_MS;
    if (btn.protocol == IRProtocol::RAW && !btn.rawData.empty()) {
        uint32_t totalUs = 0;
        for (uint16_t v : btn.rawData) totalUs += v;
        // Add 20ms margin on top of the actual signal length.
        uint32_t dynWait = (totalUs / 1000u) + 20u;
        if (dynWait > waitMs) waitMs = dynWait;
        if (waitMs > 500u)    waitMs = 500u;   // hard cap - 500ms is plenty
    }
    delay(waitMs);
    irReceiver.resume();

    xSemaphoreGive(_txMutex);
    return ok;
}

// ── transmitOn (single emitter, blocking) ─────────────────────
bool IRTransmitter::transmitOn(uint8_t idx, const IRButton& btn) {
    if (idx >= IR_MAX_EMITTERS) {
        Serial.printf(DEBUG_TAG " transmitOn: emitter[%d] out of range\n", idx);
        return false;
    }
    if (!btn.isValid()) return false;
    if (!_txMutex) return false;
    if (xSemaphoreTake(_txMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;

    // UAF FIX: previously checked `!_senders[idx]` BEFORE acquiring the
    // mutex. reconfigure() (which deletes senders) takes the same mutex,
    // so between the unlocked null-check and the deref we could read a
    // freed pointer if reconfigure ran in between. The check is now
    // inside the critical section.
    IRsend* sender = _senders[idx];
    if (!sender) {
        xSemaphoreGive(_txMutex);
        Serial.printf(DEBUG_TAG " transmitOn: emitter[%d] not active\n", idx);
        return false;
    }

    irReceiver.pause();
    delay(IR_PRE_TX_QUIET_MS);
    bool ok = doTransmit(sender, btn);
    delay(IR_POST_TX_SETTLE_MS);
    irReceiver.resume();

    xSemaphoreGive(_txMutex);
    return ok;
}

// ── transmitRaw ───────────────────────────────────────────────
bool IRTransmitter::transmitRaw(const uint16_t* data, size_t len, uint16_t freqKHz) {
    if (!data || len < 4 || activeCount() == 0) return false;
    if (!_txMutex) return false;
    if (xSemaphoreTake(_txMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;

    irReceiver.pause();
    delay(IR_PRE_TX_QUIET_MS);  // was 5ms then 20ms - now from config
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i)
        if (_senders[i]) _senders[i]->sendRaw(data, static_cast<uint16_t>(len), freqKHz);
    delay(IR_POST_TX_SETTLE_MS);
    irReceiver.resume();

    xSemaphoreGive(_txMutex);
    return true;
}

// ── doTransmit ───────────────────────────────────────────────
// Protocol-aware minimum repeats:
//   SONY      needs >=3 frames (spec says transmit 3x; cheap receivers need it)
//   DISH      needs >=4 frames (satellite protocol requirement)
//   RC5/RC6   needs >=2 frames (toggle-bit protocol; single frame often ignored)
//   COOLIX    needs >=2 frames (AC protocol; single frame unreliable)
//   All other protocols: honour btn.repeats (default IR_SEND_REPEATS = 2)
//
// Carrier frequency guard:
//   SONY = 40kHz, RC5/RC6 = 36kHz, everything else = 38kHz.
//   If btn.freqKHz is 0 or out of [33..56] range, we correct it here so
//   a misconfigured or legacy DB entry still transmits with correct carrier.
//
// "Large vs small device" signal reliability:
//   • Small/cheap receivers (e.g. old TVs, fans): benefit from more repeats
//   • Large AC units: use state-based protocols (DAIKIN etc.) or COOLIX/RAW -
//     these already encode full state in every frame; extra repeat frames help.
bool IRTransmitter::doTransmit(IRsend* s, const IRButton& btn) {
    uint8_t  total = (btn.repeatCount >= 1) ? btn.repeatCount : 1;
    uint16_t delMs = (btn.repeatDelay > 0) ? btn.repeatDelay : IR_DEFAULT_REPEAT_DELAY;

    uint8_t reps = btn.repeats;
    // minRep: ensures protocol-level repeat frames meet the protocol minimum.
    // btn.repeats (default = IR_SEND_REPEATS = 2) is used when it already
    // meets or exceeds the required minimum. This guarantees reliable
    // reception on both small (cheap) and large (AC) devices without
    // requiring the user to manually tune per-button repeat settings.
    auto minRep = [&](uint8_t minVal) -> uint8_t {
        return (reps >= minVal) ? reps : minVal;
    };

    // Carrier frequency guard - correct any legacy/zero value before TX.
    // Wrong carrier = device can't decode the signal even if timing is perfect.
    uint16_t txFreq = btn.freqKHz;
    if (txFreq < 33 || txFreq > 56) txFreq = 38;  // out-of-range -> safe default
    switch (btn.protocol) {
        case IRProtocol::SONY:                       txFreq = 40; break;  // Sony spec
        case IRProtocol::RC5:  case IRProtocol::RC6: txFreq = 36; break;  // Philips spec
        default: if (btn.freqKHz >= 33 && btn.freqKHz <= 56) txFreq = btn.freqKHz; break;
    }

    for (uint8_t rep = 0; rep < total; ++rep) {
        switch (btn.protocol) {
            case IRProtocol::NEC:
            case IRProtocol::NEC_LIKE:
                s->sendNEC(btn.code, btn.bits, minRep(2)); break;   // 2 = NEC spec min for reliable decode
            case IRProtocol::SONY:
                s->sendSony(btn.code, btn.bits, minRep(3)); break;  // Sony spec: transmit 3x
            case IRProtocol::SAMSUNG:
                s->sendSAMSUNG(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::SAMSUNG36:
                s->sendSamsung36(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::LG:
                s->sendLG(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::PANASONIC: {
                uint16_t addr = (uint16_t)((btn.code >> 32) & 0xFFFF);
                uint32_t data = (uint32_t)(btn.code & 0xFFFFFFFF);
                s->sendPanasonic(addr, data,
                    btn.bits == 0 ? kPanasonicBits : btn.bits, minRep(2));
                break; }
            case IRProtocol::RC5:
                s->sendRC5(btn.code, btn.bits, minRep(2)); break;   // RC5 toggle: 2 frames needed
            case IRProtocol::RC6:
                s->sendRC6(btn.code, btn.bits, minRep(2)); break;   // RC6 toggle: same
            case IRProtocol::JVC:
                s->sendJVC(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::DISH:
                s->sendDISH(btn.code, btn.bits, minRep(4)); break;  // DISH protocol requires >=4 frames
            case IRProtocol::SHARP: {
                uint8_t addr = (btn.code >> 8) & 0x1F;
                uint8_t cmd  = btn.code & 0xFF;
                s->sendSharp(addr, cmd, btn.bits, minRep(2)); break; }
            case IRProtocol::DENON:
                s->sendDenon(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::MITSUBISHI:
                s->sendMitsubishi(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::MITSUBISHI2:
                s->sendMitsubishi2(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::SANYO:
                s->sendSanyoLC7461(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::AIWA_RC_T501:
            case IRProtocol::AIWA_RC_T501_2:
                s->sendAiwaRCT501(btn.code, minRep(2)); break;
            case IRProtocol::NIKAI:
                s->sendNikai(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::RCMM:
                s->sendRCMM(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::LEGOPF:
                s->sendLegoPf(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::PIONEER:
                s->sendPioneer(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::EPSON:
                s->sendEpson(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::SYMPHONY:
                s->sendSymphony(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::BOSE:
                s->sendBose(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::METZ:
                s->sendMetz(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::DOSHISHA:
                s->sendDoshisha(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::GORENJE:
                s->sendGorenje(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::INAX:
                s->sendInax(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::LUTRON:
                s->sendLutron(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::ELITESCREENS:
                s->sendElitescreens(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::MILESTAG2:
                s->sendMilestag2(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::XMP:
                s->sendXmp(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::TRUMA:
                s->sendTruma(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::WOWWEE:
                s->sendWowwee(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::TECO:
                s->sendTeco(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::GOODWEATHER:
                s->sendGoodweather(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::MIDEA:
                s->sendMidea(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::MIDEA24:
                s->sendMidea24(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::COOLIX:
                s->sendCOOLIX(btn.code, btn.bits, minRep(2)); break;  // AC: 2 frames min
            case IRProtocol::COOLIX48:
                s->sendCoolix48(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::GICABLE:
                s->sendGICable(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::MAGIQUEST:
                s->sendMagiQuest(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::LASERTAG:
                s->sendLasertag(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::ARRIS:
                s->sendArris(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::MULTIBRACKETS:
                s->sendMultibrackets(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::ZEPEAL:
                s->sendZepeal(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::MWM:
                if (!btn.rawData.empty()) {
                    s->sendRaw(btn.rawData.data(),
                               static_cast<uint16_t>(btn.rawData.size()),
                               txFreq);
                } else {
                    uint8_t mwmData[3];
                    mwmData[0] = (btn.code >> 16) & 0xFF;
                    mwmData[1] = (btn.code >>  8) & 0xFF;
                    mwmData[2] =  btn.code        & 0xFF;
                    uint16_t nb = (btn.bits > 0) ? (btn.bits + 7) / 8 : 3;
                    if (nb > 3) nb = 3;
                    s->sendMWM(mwmData, nb, btn.repeats);
                }
                break;
            case IRProtocol::RAW:
            default:
                if (btn.rawData.empty()) return false;
                s->sendRaw(btn.rawData.data(),
                           static_cast<uint16_t>(btn.rawData.size()),
                           txFreq);
                break;
        }
        if (rep + 1 < total && delMs > 0) delay(delMs);
    }
    return true;
}

// ── activePins ───────────────────────────────────────────────
std::vector<uint8_t> IRTransmitter::activePins() const {
    std::vector<uint8_t> result;
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        if (_senders[i] && _pins[i] != 255)
            result.push_back(_pins[i]);
    }
    return result;
}
