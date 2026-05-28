#pragma once
// ============================================================
//  gpio_config.h  -  ESP32-WROOM-32 GPIO safety validation
//
//  Provides:
//    • Compile-time forbidden/input-only pin lists
//    • Runtime pin validation with human-readable reason
//    • The GpioPinConfig struct for persistent storage
//
//  Maximum supported configuration:
//    Emitters (TX): 4 independent IRsend instances, any safe GPIO
//    Receivers (RX): 1 active IRrecv instance, any safe GPIO
//      (IRremoteESP8266 hw_timer ISR is global - only one RX at a time)
// ============================================================
#include <Arduino.h>

// ── Hardware limits ──────────────────────────────────────────
#define IR_MAX_EMITTERS   8    // max simultaneous IRsend instances
                               // ESP32 has 8 RMT TX channels - we use all 8.
                               // Each emitter gets its own RMT channel, GPIO,
                               // and IRsend instance.  All fire in parallel
                               // inside transmit() / transmitRaw().
#define IR_MAX_RECEIVERS  1    // IRremoteESP8266 timer-based: 1 active

// ── Default pin assignments ──────────────────────────────────
#define IR_DEFAULT_RECV_PIN   14   // TSOP/VS1838B data pin
#define IR_DEFAULT_EMIT_PIN_0 27   // Primary emitter (NPN driver)
// Additional emitter defaults (only used if enabled by user):
#define IR_DEFAULT_EMIT_PIN_1 26
#define IR_DEFAULT_EMIT_PIN_2 25
#define IR_DEFAULT_EMIT_PIN_3 33
#define IR_DEFAULT_EMIT_PIN_4 32
#define IR_DEFAULT_EMIT_PIN_5 17
#define IR_DEFAULT_EMIT_PIN_6 16
#define IR_DEFAULT_EMIT_PIN_7 21

// ── Pin classification ───────────────────────────────────────

// Forbidden: internal flash SPI, UART0, and boot-critical strapping pins.
// These MUST never be used for IR - touching them causes crashes or
// prevents the chip from booting.
//
// NOTE: GPIO12 is also forbidden (flash voltage select - must be LOW at boot).
//   NRF24 and SubGHz MISO moved from GPIO12 -> GPIO26 (safe, mutually exclusive with IR TX-1).
//   LED DATA moved from GPIO2 -> GPIO13 (safe, not boot-critical).
//   NFC IRQ/RST/SPI-SS moved from GPIO4/33/5 -> GPIO13 (safe, never active simultaneously).
//
// constexpr + inline: single definition shared across all TUs (C++17).
// Avoids the flash/RAM waste of per-TU copies that `static const` would create.
inline constexpr uint8_t FORBIDDEN_PINS[] = {
    1,   // UART0 TX (Serial monitor)
    3,   // UART0 RX (Serial monitor)
    6,   // SPI Flash CLK
    7,   // SPI Flash D0
    8,   // SPI Flash D1
    9,   // SPI Flash D2
    10,  // SPI Flash D3
    11,  // SPI Flash CMD
    // Boot-strapping pins - changing them corrupts boot mode or
    // flash voltage selection.  GPIO0 can be used in theory but
    // the BOOT button makes it unreliable.
    0,   // BOOT button / download mode
    2,   // Boot mode (must be LOW at boot)
    5,   // SPI CS0 (must be HIGH at boot)
    12,  // Flash voltage (must be LOW at boot - 3.3V select)
    15,  // JTAG / debug output (HIGH at boot)
};
inline constexpr uint8_t FORBIDDEN_COUNT = sizeof(FORBIDDEN_PINS) / sizeof(FORBIDDEN_PINS[0]);

// Input-only: these GPIOs have no output driver.
// They work fine for IR RX (receiver) but cannot drive IR LED (TX).
inline constexpr uint8_t INPUT_ONLY_PINS[] = { 34, 35, 36, 39 };
inline constexpr uint8_t INPUT_ONLY_COUNT  = sizeof(INPUT_ONLY_PINS) / sizeof(INPUT_ONLY_PINS[0]);

// All GPIOs exposed on the 38-pin ESP32-WROOM-32 module
// (excludes 20, 24, 28-31 which are not exposed)
inline constexpr uint8_t VALID_GPIO_NUMS[] = {
    0, 1, 2, 3, 4, 5,
    // 6-11 forbidden (flash)
    12, 13, 14, 15,
    16, 17, 18, 19,
    // 20 not exposed
    21, 22, 23,
    // 24 not exposed
    25, 26, 27,
    // 28-31 not exposed
    32, 33, 34, 35, 36,
    // 37-38 not exposed / internal
    39
};
inline constexpr uint8_t VALID_GPIO_COUNT = sizeof(VALID_GPIO_NUMS) / sizeof(VALID_GPIO_NUMS[0]);

// ── Validation result ────────────────────────────────────────
enum class PinStatus : uint8_t {
    OK           = 0,   // Pin is safe for both TX and RX
    OK_RX_ONLY   = 1,   // Pin safe for RX only (input-only GPIO)
    ERR_FORBIDDEN= 2,   // Pin is forbidden (flash/boot-critical)
    ERR_INVALID  = 3,   // Pin number not on this module
    ERR_CONFLICT = 4,   // Pin already used by another IR instance
    ERR_INPUT_ONLY = 5, // Pin is input-only - cannot be used for TX
};

// ── Validate a pin for TX use ────────────────────────────────
inline PinStatus validateTxPin(uint8_t pin,
                                const uint8_t* usedPins = nullptr,
                                uint8_t usedCount = 0)
{
    // Check exists on module
    bool found = false;
    for (uint8_t i = 0; i < VALID_GPIO_COUNT; ++i)
        if (VALID_GPIO_NUMS[i] == pin) { found = true; break; }
    if (!found) return PinStatus::ERR_INVALID;

    // Check forbidden
    for (uint8_t i = 0; i < FORBIDDEN_COUNT; ++i)
        if (FORBIDDEN_PINS[i] == pin) return PinStatus::ERR_FORBIDDEN;

    // Check input-only - these cannot drive TX (no output driver)
    for (uint8_t i = 0; i < INPUT_ONLY_COUNT; ++i)
        if (INPUT_ONLY_PINS[i] == pin) return PinStatus::ERR_INPUT_ONLY;

    // Check conflict with other instances
    if (usedPins) {
        for (uint8_t i = 0; i < usedCount; ++i)
            if (usedPins[i] == pin) return PinStatus::ERR_CONFLICT;
    }

    return PinStatus::OK;
}

// ── Validate a pin for RX use ────────────────────────────────
inline PinStatus validateRxPin(uint8_t pin,
                                const uint8_t* usedPins = nullptr,
                                uint8_t usedCount = 0)
{
    // Check exists on module
    bool found = false;
    for (uint8_t i = 0; i < VALID_GPIO_COUNT; ++i)
        if (VALID_GPIO_NUMS[i] == pin) { found = true; break; }
    if (!found) return PinStatus::ERR_INVALID;

    // Check forbidden (boot/flash critical)
    for (uint8_t i = 0; i < FORBIDDEN_COUNT; ++i)
        if (FORBIDDEN_PINS[i] == pin) return PinStatus::ERR_FORBIDDEN;

    // Input-only is fine for RX
    for (uint8_t i = 0; i < INPUT_ONLY_COUNT; ++i)
        if (INPUT_ONLY_PINS[i] == pin) return PinStatus::OK_RX_ONLY;

    // Check conflict
    if (usedPins) {
        for (uint8_t i = 0; i < usedCount; ++i)
            if (usedPins[i] == pin) return PinStatus::ERR_CONFLICT;
    }

    return PinStatus::OK;
}

// ── Human-readable status ────────────────────────────────────
inline const char* pinStatusMsg(PinStatus s) {
    switch (s) {
        case PinStatus::OK:            return "OK";
        case PinStatus::OK_RX_ONLY:    return "OK (input-only, RX use only)";
        case PinStatus::ERR_FORBIDDEN: return "Forbidden (boot/flash critical pin)";
        case PinStatus::ERR_INPUT_ONLY:return "Input-only GPIO - cannot be used for TX";
        case PinStatus::ERR_INVALID:   return "Invalid (not on ESP32-WROOM-32)";
        case PinStatus::ERR_CONFLICT:  return "Conflict (already used by another IR instance)";
        default:                       return "Unknown";
    }
}

// ── IR pin configuration (persisted in config.json) ──────────
struct IrPinConfig {
    // Receiver
    uint8_t  recvPin;           // Active receiver GPIO

    // Emitters (up to IR_MAX_EMITTERS)
    uint8_t  emitPin[IR_MAX_EMITTERS];
    bool     emitEnabled[IR_MAX_EMITTERS];
    uint8_t  emitCount;         // Number of configured emitters (1-8)

    IrPinConfig() {
        recvPin        = IR_DEFAULT_RECV_PIN;
        emitPin[0]     = IR_DEFAULT_EMIT_PIN_0;
        emitPin[1]     = IR_DEFAULT_EMIT_PIN_1;
        emitPin[2]     = IR_DEFAULT_EMIT_PIN_2;
        emitPin[3]     = IR_DEFAULT_EMIT_PIN_3;
        emitPin[4]     = IR_DEFAULT_EMIT_PIN_4;
        emitPin[5]     = IR_DEFAULT_EMIT_PIN_5;
        emitPin[6]     = IR_DEFAULT_EMIT_PIN_6;
        emitPin[7]     = IR_DEFAULT_EMIT_PIN_7;
        emitEnabled[0] = true;
        emitEnabled[1] = false;
        emitEnabled[2] = false;
        emitEnabled[3] = false;
        emitEnabled[4] = false;
        emitEnabled[5] = false;
        emitEnabled[6] = false;
        emitEnabled[7] = false;
        emitCount      = 1;
    }

    // Count how many emitters are enabled
    uint8_t activeEmitterCount() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < emitCount && i < IR_MAX_EMITTERS; ++i)
            if (emitEnabled[i]) ++n;
        return n;
    }
};
