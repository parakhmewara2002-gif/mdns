#pragma once
// ============================================================
//  scoped_lock.h  -  RAII guard for FreeRTOS mutexes
//
//  Takes the mutex in the constructor, gives it in the destructor, so the
//  lock is released on every exit path (including early returns). Used to
//  guard STL containers that are touched from both the AsyncTCP task (Core 0)
//  and loop()/hw_poll (Core 1).
//
//  Built for RECURSIVE mutexes (xSemaphoreCreateRecursiveMutex), so a guarded
//  public method that calls a guarded helper (e.g. addEntry -> saveToFile)
//  re-takes the same mutex on the same task without deadlocking.
//
//  Semantics of operator bool() ("ok to proceed"):
//    - mutex handle is null (not created yet, e.g. pre-begin())  -> proceed unlocked
//    - take() succeeded                                          -> proceed locked
//    - take() timed out                                          -> do NOT proceed
// ============================================================
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct ScopedLock {
    SemaphoreHandle_t _h;
    bool              _locked;

    explicit ScopedLock(SemaphoreHandle_t m, TickType_t timeout = portMAX_DELAY)
        : _h(m), _locked(false) {
        if (_h) _locked = (xSemaphoreTakeRecursive(_h, timeout) == pdTRUE);
    }
    ~ScopedLock() { if (_h && _locked) xSemaphoreGiveRecursive(_h); }

    explicit operator bool() const { return _locked || _h == nullptr; }

    ScopedLock(const ScopedLock&)            = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
};
