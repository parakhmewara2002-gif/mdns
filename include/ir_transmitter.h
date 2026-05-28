#pragma once
// ============================================================
//  ir_transmitter.h  -  Multi-emitter IR transmit wrapper
//
//  v1.4.0 fixes:
//    - FIX-CRITICAL: FreeRTOS xQueueSend does shallow memcpy of sizeof(IrTxCommand).
//      IRButton contains String + std::vector<uint16_t> with heap-allocated internals.
//      Memcpy copies only the internal pointers; when the source IRButton destructs
//      (end of transmitAsync() stack frame) those heap blocks are freed, leaving
//      dangling pointers in the queue item -> heap corruption + crash on dequeue.
//      Fix: replace FreeRTOS queue with a pointer-based std::queue<IrTxCommand*>
//      protected by a binary semaphore (notify) + mutex (queue access).
//      Heap-allocated IrTxCommand objects live until the IR task deletes them.
// ============================================================
#include <Arduino.h>
#include <vector>
#include <queue>
#include <IRsend.h>
#include "ir_button.h"
#include "config.h"
#include "gpio_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ── IR TX command (heap-allocated; owned by pointer queue) ───
// Heap-allocated via new/delete so FreeRTOS never memcpy's C++ objects.
struct IrTxCommand {
    IRButton btn;      // deep-copyable via C++ copy constructor (safe)
    bool     rawMode;  // true -> use rawData directly
};

// IR TX queue capacity (max pending async commands)
#define IR_TX_PTR_QUEUE_MAX  8

class IRTransmitter {
public:
    IRTransmitter();
    ~IRTransmitter();

    // Call once from setup() - also starts the IR TX task.
    void begin(const IrPinConfig& pins);

    // Reconfigure emitters at runtime (no reboot needed).
    void reconfigure(const IrPinConfig& pins);

    // Blocking transmit - waits for mutex, does TX, returns result.
    // Safe to call from any task. Blocks caller for TX duration.
    bool transmit(const IRButton& btn);

    // Non-blocking async transmit - heap-allocates IrTxCommand and
    // posts pointer to the ptr queue. Returns false if queue full or OOM.
    bool transmitAsync(const IRButton& btn);

    // Transmit on a specific emitter index only (0-based). Blocking.
    bool transmitOn(uint8_t emitterIdx, const IRButton& btn);

    // Raw transmit on all enabled emitters. Blocking.
    bool transmitRaw(const uint16_t* data, size_t len,
                     uint16_t freqKHz = IR_DEFAULT_FREQ_KHZ);

    // Number of currently active emitters.
    uint8_t activeCount() const;

    // Active GPIO pin numbers for all enabled slots.
    std::vector<uint8_t> activePins() const;

    // Active GPIO for emitter at index i (255 = disabled/invalid).
    uint8_t emitterPin(uint8_t idx) const;

private:
    IRsend*           _senders[IR_MAX_EMITTERS];
    uint8_t           _pins   [IR_MAX_EMITTERS];
    uint8_t           _count;
    SemaphoreHandle_t _txMutex;    // protects transmit() critical section
    SemaphoreHandle_t _queueMutex; // protects _ptrQueue access
    SemaphoreHandle_t _notify;     // binary semaphore: signals IR task
    std::queue<IrTxCommand*> _ptrQueue; // pointer queue - no memcpy of C++ objects

    void destroyAll();
    void createSender(uint8_t idx, uint8_t pin);
    bool doTransmit(IRsend* s, const IRButton& btn);

    // IR TX task body - static so it can be passed to xTaskCreate
    static void _txTask(void* param);
};

extern IRTransmitter irTransmitter;

