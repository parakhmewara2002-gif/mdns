// ============================================================
//  task_manager.cpp  -  FreeRTOS Task Architecture v1.1
//
//  hw_poll (Core 1, priority 2):
//    Polls hardware modules on SPI/I2C bus:
//      - NFC (I2C)
//      - RFID (SPI)
//      - SubGHz (SPI)
//      - NRF24 (SPI)
//    Removes 4 SPI polling calls from loop().
//    Runs at 20ms tick - matches hardware polling needs.
// ============================================================
#include "task_manager.h"
#include "nfc_module.h"
#include "rfid_module.h"
#include "subghz_module.h"
#include "nrf24_module.h"

TaskManager taskMgr;

// ── begin ─────────────────────────────────────────────────────
void TaskManager::begin() {
    // Stack sizing: a connected PN532 makes nfcModule.loop() run deep MIFARE/I2C
    // call chains that overflow a 3072-byte stack (silent heap corruption — this
    // was FIX S-01). Size from detected hardware so absent-NFC builds keep the
    // RAM saving while connected-NFC builds get the headroom they require.
    uint32_t stack = nfcModule.isConnected() ? TASK_HW_POLL_STACK_NFC
                                             : TASK_HW_POLL_STACK_BASE;
    xTaskCreatePinnedToCore(
        _hwPollTask, "hw_poll",
        stack, nullptr,
        TASK_HW_POLL_PRIO, nullptr,
        1   // Core 1 - SPI bus access alongside loop()
    );

    Serial.printf("[TASK] hw_poll task -> Core 1, stack=%u (NFC %s)\n",
                  stack, nfcModule.isConnected() ? "present" : "absent");
}

// ── _hwPollTask ───────────────────────────────────────────────
// Runs on Core 1, priority 2 (below ir_tx=5, above loop=1).
// Polls hardware modules at 20ms intervals.
void TaskManager::_hwPollTask(void* param) {
    TickType_t lastWake = xTaskGetTickCount();
    uint32_t   lastHwmLog = 0;
    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
        nfcModule.loop();
        rfidModule.loop();
        subGhzModule.loop();
        nrf24Module.loop();

        // Stack high-water guard: warn if this task ever drops below ~512 bytes
        // of free stack. Cheap insurance against silently re-introducing the
        // FIX S-01 stack-overflow regression (logged at most once / 10s).
        uint32_t now = (uint32_t)(lastWake * portTICK_PERIOD_MS);
        if (now - lastHwmLog >= 10000UL) {
            lastHwmLog = now;
            UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
            if (freeWords < 128)  // 128 words = 512 bytes
                Serial.printf("[TASK] WARNING hw_poll stack low: %u words free\n",
                              (unsigned)freeWords);
        }
    }
}
