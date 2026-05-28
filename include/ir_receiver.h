#pragma once
// ============================================================
//  ir_receiver.h  -  Dynamic-pin IR receive wrapper
//
//  v1.2.0: IRrecv is now heap-allocated so the active GPIO
//  can be changed at runtime without rebooting.
//
//  Usage:
//    irReceiver.begin(pin)       - start on given pin
//    irReceiver.changePin(pin)   - switch to different pin
//    irReceiver.loop()           - call every loop()
//    irReceiver.pause/resume()   - suspend for TX window
// ============================================================
#include <Arduino.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <functional>
#include "ir_button.h"
#include "config.h"
#include "gpio_config.h"

using IRReceiveCallback = std::function<void(const IRButton&)>;

class IRReceiver {
public:
    IRReceiver();
    ~IRReceiver();

    // Initialise on the given pin (allocates IRrecv on heap).
    void begin(uint8_t pin = IR_DEFAULT_RECV_PIN);

    // Call every loop().
    void loop();

    // Change the active receive pin at runtime (no reboot needed).
    // Returns false if the pin is invalid/forbidden.
    bool changePin(uint8_t newPin);

    // Current active receive pin (255 = not initialised).
    uint8_t activePin() const { return _pin; }

    void onReceive(IRReceiveCallback cb) { _callback = cb; }

    void pause();
    void resume();
    bool isPaused() const { return _paused; }

private:
    IRrecv*           _irRecv;     // heap-allocated, nullptr when uninitialised
    IRReceiveCallback _callback;
    bool              _paused;
    uint8_t           _pin;        // current active GPIO
    unsigned long     _lastCodeMs;
    uint64_t          _lastCode;
    uint16_t          _lastProtocol;  // uint16_t: decode_type_t values can exceed 255

    void destroyRecv();
    void createRecv(uint8_t pin);
    IRButton decodeToButton(const decode_results& r) const;
    bool     shouldFilter (const decode_results& r) const;
};

extern IRReceiver irReceiver;
