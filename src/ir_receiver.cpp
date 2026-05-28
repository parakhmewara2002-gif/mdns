// ============================================================
//  ir_receiver.cpp  -  Dynamic-pin IR receive
//  v2.2.0: All decode_type_t values from IRremoteESP8266 v2.8.6
// ============================================================
#include "ir_receiver.h"
#include <new>

IRReceiver irReceiver;

IRReceiver::IRReceiver()
    : _irRecv(nullptr), _paused(false),
      _pin(255), _lastCodeMs(0), _lastCode(0), _lastProtocol(0)
{}  // _results removed from member - local variable in loop() avoids ISR race

IRReceiver::~IRReceiver() { destroyRecv(); }

void IRReceiver::begin(uint8_t pin) {
    createRecv(pin);
    Serial.printf(DEBUG_TAG " IR Receiver on GPIO%d  buf=%d  timeout=%dms\n",
                  _pin, IR_RECV_BUFFER_SIZE, IR_CAPTURE_TIMEOUT_MS);
}

bool IRReceiver::changePin(uint8_t newPin) {
    if (newPin == _pin) return true;
    PinStatus st = validateRxPin(newPin);
    if (st == PinStatus::ERR_FORBIDDEN || st == PinStatus::ERR_INVALID) {
        Serial.printf(DEBUG_TAG " changePin(%d) rejected: %s\n",
                      newPin, pinStatusMsg(st));
        return false;
    }
    Serial.printf(DEBUG_TAG " IR Receiver pin: GPIO%d -> GPIO%d\n", _pin, newPin);
    destroyRecv();
    createRecv(newPin);
    return true;
}

void IRReceiver::createRecv(uint8_t pin) {
    _pin    = pin;
    _paused = false;
    _irRecv = new (std::nothrow) IRrecv(pin, IR_RECV_BUFFER_SIZE,
                                        IR_CAPTURE_TIMEOUT_MS, IR_RECV_ENABLE_PULLUP);
    if (!_irRecv) {
        Serial.println(DEBUG_TAG " ERROR: IRrecv allocation failed (OOM)");
        return;
    }
    _irRecv->enableIRIn();
}

void IRReceiver::destroyRecv() {
    if (_irRecv) { _irRecv->disableIRIn(); delete _irRecv; _irRecv = nullptr; }
    _pin = 255; _paused = false;
}

void IRReceiver::loop() {
    if (_paused || !_irRecv) return;
    decode_results results;   // local - avoids race with ISR writing to member
    if (!_irRecv->decode(&results)) return;
    _irRecv->resume();
    if (shouldFilter(results)) return;
    _lastCodeMs   = millis();
    _lastCode     = results.value;
    _lastProtocol = static_cast<uint16_t>(results.decode_type);
    IRButton btn = decodeToButton(results);
    Serial.printf(DEBUG_TAG " RX %-18s 0x%llX  %db\n",
                  protocolName(btn.protocol),
                  (unsigned long long)btn.code, btn.bits);
    if (_callback) _callback(btn);
}

void IRReceiver::pause()  { if (!_paused && _irRecv) { _paused=true;  _irRecv->disableIRIn(); } }
void IRReceiver::resume() { if ( _paused && _irRecv) { _paused=false; _irRecv->enableIRIn();  } }

// ── decodeToButton ───────────────────────────────────────────
// Maps every decode_type_t to our IRProtocol.
// Complex AC protocols (state[]) are stored as RAW timings.
IRButton IRReceiver::decodeToButton(const decode_results& r) const {
    IRButton btn;
    btn.bits    = r.bits;
    btn.freqKHz = IR_DEFAULT_FREQ_KHZ;  // will be overridden below per-protocol
    btn.repeats = IR_SEND_REPEATS;      // will be overridden below per-protocol
    btn.code    = r.value;

    // ── Simple protocols (<=64-bit, decoded to code) ──────────
    switch (r.decode_type) {
        case decode_type_t::NEC:
        case decode_type_t::NEC_LIKE:     btn.protocol = IRProtocol::NEC_LIKE;
            if (r.decode_type == decode_type_t::NEC) btn.protocol = IRProtocol::NEC;
            break;
        case decode_type_t::SONY:         btn.protocol = IRProtocol::SONY;         break;
        case decode_type_t::SAMSUNG:      btn.protocol = IRProtocol::SAMSUNG;      break;
        case decode_type_t::SAMSUNG36:    btn.protocol = IRProtocol::SAMSUNG36;    break;
        case decode_type_t::LG:           btn.protocol = IRProtocol::LG;           break;
        case decode_type_t::LG2:          btn.protocol = IRProtocol::LG;           break;
        case decode_type_t::PANASONIC:    btn.protocol = IRProtocol::PANASONIC;    break;
        case decode_type_t::RC5:          btn.protocol = IRProtocol::RC5;          break;
        case decode_type_t::RC6:          btn.protocol = IRProtocol::RC6;          break;
        case decode_type_t::JVC:          btn.protocol = IRProtocol::JVC;          break;
        case decode_type_t::DISH:         btn.protocol = IRProtocol::DISH;         break;
        case decode_type_t::SHARP:        btn.protocol = IRProtocol::SHARP;        break;
        case decode_type_t::DENON:        btn.protocol = IRProtocol::DENON;        break;
        case decode_type_t::MITSUBISHI:   btn.protocol = IRProtocol::MITSUBISHI;   break;
        case decode_type_t::MITSUBISHI2:  btn.protocol = IRProtocol::MITSUBISHI2;  break;
        case decode_type_t::SANYO:        btn.protocol = IRProtocol::SANYO;        break;
        case decode_type_t::AIWA_RC_T501: btn.protocol = IRProtocol::AIWA_RC_T501; break;
        case decode_type_t::NIKAI:        btn.protocol = IRProtocol::NIKAI;        break;
        case decode_type_t::MAGIQUEST:    btn.protocol = IRProtocol::MAGIQUEST;    break;
        case decode_type_t::LASERTAG:     btn.protocol = IRProtocol::LASERTAG;     break;
        case decode_type_t::RCMM:         btn.protocol = IRProtocol::RCMM;         break;
        case decode_type_t::LEGOPF:       btn.protocol = IRProtocol::LEGOPF;       break;
        case decode_type_t::PIONEER:      btn.protocol = IRProtocol::PIONEER;      break;
        case decode_type_t::EPSON:        btn.protocol = IRProtocol::EPSON;        break;
        case decode_type_t::SYMPHONY:     btn.protocol = IRProtocol::SYMPHONY;     break;
        case decode_type_t::BOSE:         btn.protocol = IRProtocol::BOSE;         break;
        case decode_type_t::METZ:         btn.protocol = IRProtocol::METZ;         break;
        case decode_type_t::DOSHISHA:     btn.protocol = IRProtocol::DOSHISHA;     break;
        case decode_type_t::GORENJE:      btn.protocol = IRProtocol::GORENJE;      break;
        case decode_type_t::INAX:         btn.protocol = IRProtocol::INAX;         break;
        case decode_type_t::LUTRON:       btn.protocol = IRProtocol::LUTRON;       break;
        case decode_type_t::MWM:          btn.protocol = IRProtocol::MWM;          break;
        case decode_type_t::MULTIBRACKETS:btn.protocol = IRProtocol::MULTIBRACKETS;break;
        case decode_type_t::ELITESCREENS: btn.protocol = IRProtocol::ELITESCREENS; break;
        case decode_type_t::MILESTAG2:    btn.protocol = IRProtocol::MILESTAG2;    break;
        case decode_type_t::XMP:          btn.protocol = IRProtocol::XMP;          break;
        case decode_type_t::ARRIS:        btn.protocol = IRProtocol::ARRIS;        break;
        case decode_type_t::TRUMA:        btn.protocol = IRProtocol::TRUMA;        break;
        case decode_type_t::WOWWEE:       btn.protocol = IRProtocol::WOWWEE;       break;
        case decode_type_t::ZEPEAL:       btn.protocol = IRProtocol::ZEPEAL;       break;
        case decode_type_t::TECO:         btn.protocol = IRProtocol::TECO;         break;
        case decode_type_t::GOODWEATHER:  btn.protocol = IRProtocol::GOODWEATHER;  break;
        case decode_type_t::MIDEA:        btn.protocol = IRProtocol::MIDEA;        break;
        case decode_type_t::MIDEA24:      btn.protocol = IRProtocol::MIDEA24;      break;
        case decode_type_t::GICABLE:      btn.protocol = IRProtocol::GICABLE;      break;
        case decode_type_t::COOLIX:       btn.protocol = IRProtocol::COOLIX;       break;
        case decode_type_t::COOLIX48:     btn.protocol = IRProtocol::COOLIX48;     break;
        case decode_type_t::SAMSUNG_AC:   btn.protocol = IRProtocol::SAMSUNG_AC;   goto capture_raw;
        case decode_type_t::PANASONIC_AC: btn.protocol = IRProtocol::PANASONIC_AC; goto capture_raw;
        case decode_type_t::PANASONIC_AC32:btn.protocol= IRProtocol::PANASONIC_AC32;goto capture_raw;
        case decode_type_t::DAIKIN:       btn.protocol = IRProtocol::DAIKIN;       goto capture_raw;
        case decode_type_t::DAIKIN2:      btn.protocol = IRProtocol::DAIKIN2;      goto capture_raw;
        case decode_type_t::DAIKIN64:     btn.protocol = IRProtocol::DAIKIN64;     goto capture_raw;
        case decode_type_t::DAIKIN128:    btn.protocol = IRProtocol::DAIKIN128;    goto capture_raw;
        case decode_type_t::DAIKIN152:    btn.protocol = IRProtocol::DAIKIN152;    goto capture_raw;
        case decode_type_t::DAIKIN160:    btn.protocol = IRProtocol::DAIKIN160;    goto capture_raw;
        case decode_type_t::DAIKIN176:    btn.protocol = IRProtocol::DAIKIN176;    goto capture_raw;
        case decode_type_t::DAIKIN200:    btn.protocol = IRProtocol::DAIKIN200;    goto capture_raw;
        case decode_type_t::DAIKIN216:    btn.protocol = IRProtocol::DAIKIN216;    goto capture_raw;
        case decode_type_t::DAIKIN312:    btn.protocol = IRProtocol::DAIKIN312;    goto capture_raw;
        case decode_type_t::MITSUBISHI_AC:btn.protocol = IRProtocol::MITSUBISHI_AC;goto capture_raw;
        case decode_type_t::MITSUBISHI136:btn.protocol = IRProtocol::MITSUBISHI136;goto capture_raw;
        case decode_type_t::MITSUBISHI112:btn.protocol = IRProtocol::MITSUBISHI112;goto capture_raw;
        case decode_type_t::MITSUBISHI_HEAVY_88: btn.protocol=IRProtocol::MITSUBISHI_HEAVY_88; goto capture_raw;
        case decode_type_t::MITSUBISHI_HEAVY_152:btn.protocol=IRProtocol::MITSUBISHI_HEAVY_152;goto capture_raw;
        case decode_type_t::FUJITSU_AC:   btn.protocol = IRProtocol::FUJITSU_AC;   goto capture_raw;
        case decode_type_t::TOSHIBA_AC:   btn.protocol = IRProtocol::TOSHIBA_AC;   goto capture_raw;
        case decode_type_t::SHARP_AC:     btn.protocol = IRProtocol::SHARP_AC;     goto capture_raw;
        case decode_type_t::KELVINATOR:   btn.protocol = IRProtocol::KELVINATOR;   goto capture_raw;
        case decode_type_t::GREE:         btn.protocol = IRProtocol::GREE;         goto capture_raw;
        case decode_type_t::ARGO:         btn.protocol = IRProtocol::ARGO;         goto capture_raw;
        case decode_type_t::TROTEC:       btn.protocol = IRProtocol::TROTEC;       goto capture_raw;
        case decode_type_t::TROTEC_3550:  btn.protocol = IRProtocol::TROTEC_3550;  goto capture_raw;
        case decode_type_t::HAIER_AC:     btn.protocol = IRProtocol::HAIER_AC;     goto capture_raw;
        case decode_type_t::HAIER_AC_YRW02:btn.protocol=IRProtocol::HAIER_AC_YRW02;goto capture_raw;
        case decode_type_t::HAIER_AC176:  btn.protocol = IRProtocol::HAIER_AC176;  goto capture_raw;
        case decode_type_t::HAIER_AC160:  btn.protocol = IRProtocol::HAIER_AC160;  goto capture_raw;
        case decode_type_t::HITACHI_AC:   btn.protocol = IRProtocol::HITACHI_AC;   goto capture_raw;
        case decode_type_t::HITACHI_AC1:  btn.protocol = IRProtocol::HITACHI_AC1;  goto capture_raw;
        case decode_type_t::HITACHI_AC2:  btn.protocol = IRProtocol::HITACHI_AC2;  goto capture_raw;
        case decode_type_t::HITACHI_AC3:  btn.protocol = IRProtocol::HITACHI_AC3;  goto capture_raw;
        case decode_type_t::HITACHI_AC264:btn.protocol = IRProtocol::HITACHI_AC264;goto capture_raw;
        case decode_type_t::HITACHI_AC296:btn.protocol = IRProtocol::HITACHI_AC296;goto capture_raw;
        case decode_type_t::HITACHI_AC344:btn.protocol = IRProtocol::HITACHI_AC344;goto capture_raw;
        case decode_type_t::HITACHI_AC424:btn.protocol = IRProtocol::HITACHI_AC424;goto capture_raw;
        case decode_type_t::WHYNTER:      btn.protocol = IRProtocol::WHYNTER;      goto capture_raw;
        case decode_type_t::WHIRLPOOL_AC: btn.protocol = IRProtocol::WHIRLPOOL_AC; goto capture_raw;
        case decode_type_t::ELECTRA_AC:   btn.protocol = IRProtocol::ELECTRA_AC;   goto capture_raw;
        case decode_type_t::CARRIER_AC:   btn.protocol = IRProtocol::CARRIER_AC;   goto capture_raw;
        case decode_type_t::CARRIER_AC40: btn.protocol = IRProtocol::CARRIER_AC40; goto capture_raw;
        case decode_type_t::CARRIER_AC64: btn.protocol = IRProtocol::CARRIER_AC64; goto capture_raw;
        case decode_type_t::CARRIER_AC84: btn.protocol = IRProtocol::CARRIER_AC84; goto capture_raw;
        case decode_type_t::CARRIER_AC128:btn.protocol = IRProtocol::CARRIER_AC128;goto capture_raw;
        case decode_type_t::VESTEL_AC:    btn.protocol = IRProtocol::VESTEL_AC;    goto capture_raw;
        case decode_type_t::TCL112AC:     btn.protocol = IRProtocol::TCL112AC;     goto capture_raw;
        case decode_type_t::TCL96AC:      btn.protocol = IRProtocol::TCL96AC;      goto capture_raw;
        case decode_type_t::NEOCLIMA:     btn.protocol = IRProtocol::NEOCLIMA;     goto capture_raw;
        case decode_type_t::AMCOR:        btn.protocol = IRProtocol::AMCOR;        goto capture_raw;
        case decode_type_t::SANYO_AC:     btn.protocol = IRProtocol::SANYO_AC;     goto capture_raw;
        case decode_type_t::SANYO_AC88:   btn.protocol = IRProtocol::SANYO_AC88;   goto capture_raw;
        case decode_type_t::SANYO_AC152:  btn.protocol = IRProtocol::SANYO_AC152;  goto capture_raw;
        case decode_type_t::VOLTAS:       btn.protocol = IRProtocol::VOLTAS;       goto capture_raw;
        case decode_type_t::KELON:        btn.protocol = IRProtocol::KELON;        goto capture_raw;
        case decode_type_t::KELON168:     btn.protocol = IRProtocol::KELON168;     goto capture_raw;
        case decode_type_t::CORONA_AC:    btn.protocol = IRProtocol::CORONA_AC;    goto capture_raw;
        case decode_type_t::DELONGHI_AC:  btn.protocol = IRProtocol::DELONGHI_AC;  goto capture_raw;
        case decode_type_t::MIRAGE:       btn.protocol = IRProtocol::MIRAGE;       goto capture_raw;
        case decode_type_t::RHOSS:        btn.protocol = IRProtocol::RHOSS;        goto capture_raw;
        case decode_type_t::AIRTON:       btn.protocol = IRProtocol::AIRTON;       goto capture_raw;
        case decode_type_t::AIRWELL:      btn.protocol = IRProtocol::AIRWELL;      goto capture_raw;
        case decode_type_t::TECHNIBEL_AC: btn.protocol = IRProtocol::TECHNIBEL_AC; goto capture_raw;
        case decode_type_t::TEKNOPOINT:   btn.protocol = IRProtocol::TEKNOPOINT;   goto capture_raw;
        case decode_type_t::TRANSCOLD:    btn.protocol = IRProtocol::TRANSCOLD;    goto capture_raw;
        case decode_type_t::ECOCLIM:      btn.protocol = IRProtocol::ECOCLIM;      goto capture_raw;
        case decode_type_t::BOSCH144:     btn.protocol = IRProtocol::BOSCH144;     goto capture_raw;
        case decode_type_t::YORK:         btn.protocol = IRProtocol::YORK;         goto capture_raw;
        case decode_type_t::CLIMABUTLER:  btn.protocol = IRProtocol::CLIMABUTLER;  goto capture_raw;
        case decode_type_t::TOTO:         btn.protocol = IRProtocol::TOTO;         goto capture_raw;
        // NOTE: LG_AC, MIDEA_AC, ELECTRA_AC2, BLUESTARHEAVY, EUROM are defined in
        // our IRProtocol enum but do NOT exist in IRremoteESP8266 v2.8.6 decode_type_t.
        // They will be decoded as UNKNOWN and fall to the default RAW handler below.
        default:
            // Truly unknown - fall through to generic RAW
            btn.protocol = IRProtocol::RAW;
            btn.code = 0; btn.bits = 0;
            goto capture_raw;
    }

    {   // Build name for simple protocols and apply correct TX defaults
        // Apply per-protocol repeat count + delay so the button is ready to
        // transmit perfectly right after capture - no manual tuning needed.
        RepeatPreset rp = defaultRepeatForProtocol(btn.protocol);
        btn.repeatCount = (rp.count  > 0) ? rp.count  : 1;
        btn.repeatDelay = (rp.delayMs > 0) ? rp.delayMs : IR_DEFAULT_REPEAT_DELAY;
        // SONY uses 40kHz carrier; RC5/RC6 use 36kHz; everything else is 38kHz.
        switch (btn.protocol) {
            case IRProtocol::SONY:                       btn.freqKHz = 40; break;
            case IRProtocol::RC5:  case IRProtocol::RC6: btn.freqKHz = 36; break;
            default:                                     btn.freqKHz = 38; break;
        }
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "%s_%llX",
                 protocolName(btn.protocol), (unsigned long long)btn.code);
        btn.name = String(tmp);
        return btn;
    }

capture_raw:
    {   // Capture timing array (works for both AC state[] and true unknowns)
        btn.code = 0; btn.bits = 0;
        btn.rawData.clear();
        for (uint16_t i = 1; i < r.rawlen && btn.rawData.size() < MAX_RAW_EDGES; ++i) {
            uint32_t us = static_cast<uint32_t>(r.rawbuf[i]) * RAWTICK;
            btn.rawData.push_back(us > 65535u ? 65535u : static_cast<uint16_t>(us));
        }
        // Apply correct repeat settings for AC protocols (most need count=1, delay varies).
        RepeatPreset rp = defaultRepeatForProtocol(btn.protocol);
        btn.repeatCount = (rp.count  > 0) ? rp.count  : 1;
        btn.repeatDelay = (rp.delayMs > 0) ? rp.delayMs : IR_DEFAULT_REPEAT_DELAY;
        // Estimate carrier from average mark (burst) duration.
        // Each carrier cycle = 1000/freqKHz µs.  Most protocols send ~21 cycles
        // per 38kHz mark (~555µs), so freqKHz ≈ 21000 / avg_mark_us.
        // RC5/RC6 use 36kHz (known from protocol); for everything else we estimate
        // from burst timings and clamp to the 33–57kHz range of common IR carriers.
        {
            uint8_t estFreq = 38; // default fallback
            switch (btn.protocol) {
                case IRProtocol::RC5: case IRProtocol::RC6: estFreq = 36; break;
                default: {
                    // Marks are odd-indexed entries (index 1,3,5,…) in rawData.
                    // Collect marks that look like carrier bursts (10–1000µs).
                    uint32_t sumUs = 0; uint16_t cnt = 0;
                    for (size_t ri = 1; ri < btn.rawData.size(); ri += 2) {
                        uint16_t us = btn.rawData[ri];
                        if (us >= 10 && us <= 1000) { sumUs += us; cnt++; }
                    }
                    if (cnt >= 4) {
                        uint32_t avgUs = sumUs / cnt;
                        // ~21 carrier cycles per burst at 38kHz (industry standard)
                        uint32_t est = 21000u / avgUs;
                        // Clamp to 33–57kHz (covers 33/36/38/40/56kHz families)
                        if      (est < 33) est = 33;
                        else if (est > 57) est = 57;
                        estFreq = (uint8_t)est;
                    }
                    break;
                }
            }
            btn.freqKHz = estFreq;
        }
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "%s_%lu",
                 protocolName(btn.protocol), (unsigned long)millis());
        btn.name = String(tmp);
        return btn;
    }
}

bool IRReceiver::shouldFilter(const decode_results& r) const {
    if (r.repeat) return true;
    if (r.decode_type == decode_type_t::UNKNOWN &&
        r.rawlen < static_cast<uint16_t>(IR_MIN_UNKNOWN_SIZE)) return true;
    unsigned long elapsed = millis() - _lastCodeMs;
    if (elapsed < static_cast<unsigned long>(IR_DEBOUNCE_MS)) {
        if (r.value != 0 && r.value == _lastCode &&
            static_cast<uint16_t>(r.decode_type) == _lastProtocol) return true;
    }
    return false;
}
