#pragma once
// ============================================================
//  task_manager.h  -  Centralized FreeRTOS Task Architecture
//
//  Core 1 (Realtime/Loop):
//    Arduino loop() (priority 1)
//    ir_tx     (already created by IRTransmitter, priority 5)
//    hw_poll   (NFC + RFID + SubGHz + NRF24, priority 2)
// ============================================================
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ── Task stack sizes ──────────────────────────────────────────
// hw_poll stack is chosen at task-creation time from detected NFC presence
// (see TaskManager::begin). NFC MIFARE block reads + the dictionary attack have
// deep Adafruit_PN532/I2C call stacks and overflow anything below ~6144 (this
// was the root cause of FIX S-01). 3072 is only safe when the PN532 is ABSENT,
// because nfcModule.loop() early-returns in that case. Never hardcode 3072
// unconditionally: a connected PN532 will silently smash the stack into heap.
#define TASK_HW_POLL_STACK_NFC   6144    // PN532 connected: MIFARE reads need this
#define TASK_HW_POLL_STACK_BASE  3072    // PN532 absent: hw_poll returns immediately

// ── Task priorities ───────────────────────────────────────────
#define TASK_HW_POLL_PRIO    2

class TaskManager {
public:
    // Call once from setup() after all modules are initialized
    static void begin();

private:
    static void _hwPollTask(void* param);
};

extern TaskManager taskMgr;
