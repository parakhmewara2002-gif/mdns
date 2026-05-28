# IR Remote Web GUI — Changelog

## v7.0.1 — OTA Removal + Partition Optimization

- **Complete OTA removal**: All OTA update code removed from firmware and web UI.
  Web-browser firmware flashing, SD-card OTA, version-check, and related routes
  (`/api/ota`, `/api/sd/ota`, `/api/v1/ota/version`) are gone.
- **Partition table fixed**: `otadata` partition removed (freed 8 KB → added to LittleFS).
  LittleFS now 960 KB (was 956 KB). Single `factory` app slot = 3072 KB.
- **`AuditSource::OTA` removed** from audit log enum.
- **`esp_ota_mark_app_valid_cancel_rollback()`** call removed from WiFi manager.
- No functional change to IR, RFID, NFC, SubGHz, NRF24, BT, Mic, SD, or Web UI features.
## v7.1.0 — SD Card IR Library & Device Profile Management

### IR Library (SD Card)
- **Export** current IR database to `/sd/ir_library/<name>.json`
- **Import** (replace) IR database from SD library file
- **Merge** SD library into existing DB — existing buttons preserved, only new ones added
- **Delete** a library file from SD
- **Rename** a library file on SD
- **Save single button** to a named SD library (upsert by button name)

### Device Profiles (SD Card)
- **Save** current IR buttons as a named device profile to `/sd/devices/<name>.json`
- **Delete** a device profile from SD
- **Import buttons** from a device profile into irDB (merge — keeps existing)

### IR Database Core
- Added `IRDatabase::mergeJson()` — merges buttons by name, re-assigns IDs, no duplicates

### API Endpoints Added
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/sd/irlibrary` | List all library files |
| POST | `/api/sd/irlibrary/export` | Export DB → SD |
| POST | `/api/sd/irlibrary/import` | Import SD → DB (replace) |
| POST | `/api/sd/irlibrary/merge` | Merge SD → DB (keep existing) |
| POST | `/api/sd/irlibrary/delete` | Delete library file |
| POST | `/api/sd/irlibrary/rename` | Rename library file |
| POST | `/api/sd/irlibrary/savebtn` | Save single button to library |
| GET | `/api/sd/devices` | List device profiles |
| GET | `/api/sd/device?name=X` | Read device profile |
| POST | `/api/sd/device/save` | Create/overwrite device profile |
| POST | `/api/sd/device/delete` | Delete device profile |
| POST | `/api/sd/device/import` | Import profile buttons → DB |
## v2.0.1 — Audit & Bug-Fix Release

> Production-ready patch release. Full audit of v2.0.0 firmware.
> All 12 identified bugs fixed. No new features added.

### 🔴 Critical Fixes

- Fix: Target string validated first; `LittleFS.end()` only called after confirmation
  it's a filesystem update.

**`push_back()` inside spinlock** (`ir_database.cpp`)
- Bug: `_buttons.push_back()` was called inside `portENTER_CRITICAL`. On ESP32,
  `std::vector` and `String` internally call `malloc()`. Running heap allocation
  with interrupts disabled can deadlock FreeRTOS's heap allocator → WDT reset.
- Fix: `push_back()` moved fully outside the spinlock. ID assignment (cheap int op)
  still protected by the spinlock.

### 🟠 High-Severity Fixes

- Bug: `handleGetStatus()` unconditionally called `LittleFS.totalBytes()` /
  calling these functions returns garbage or causes an exception fault.

**WebSocket queue unbounded** (`web_server.cpp`, `web_server.h`, `config.h`)
- Bug: `std::queue<String> _wsQueue` had no depth cap. A misbehaving or
  high-frequency client could push the queue deep enough to exhaust heap → crash.
- Fix: Added `_pushWsMessage()` helper with a ring-buffer cap of `WS_QUEUE_MAX`
  (64 entries). Oldest entry dropped when full. All broadcast paths use the helper.

**IR receiver stuck paused** (`ir_transmitter.cpp`)
- Bug: `doTransmit()` returned `false` early (e.g. empty RAW data) without the
  caller ever calling `irReceiver.resume()`. After such a transmit attempt the
  IR receiver ISR would remain permanently disabled until next reboot.
- Fix: `irReceiver.resume()` now always called in both `transmit()` and
  `transmitOn()` regardless of `doTransmit()` result.

### 🟡 Medium-Severity Fixes

**NTP started before WiFi STA connected** (`scheduler.cpp`)
- Bug: `Scheduler::begin()` called `configTime()` / `_startNtp()` immediately
  on boot, even in AP-only mode. SNTP retry budget wasted; spurious DNS lookups.
- Fix: NTP deferred until `WiFi.status() == WL_CONNECTED`. `scheduler.loop()`
  auto-starts NTP the moment STA first connects. Timezone pre-configured via
  `configTime()` so SNTP daemon picks it up immediately when WiFi is ready.

**Orphaned buttons on group delete** (`web_server.cpp`)
- Bug: Deleting a group left all its buttons with a `groupId` pointing to a
  non-existent group. These buttons became invisible in the Remote tab and
  the Groups filter — effectively lost until re-assigned manually.
- Fix: `handleDeleteGroup()` now iterates the database, finds all buttons with
  the deleted `groupId`, and reassigns them to `groupId=0` (ungrouped) before
  returning the response.

**Wi-Fi scan result format mismatch** (`wifi_manager.cpp`, `index.html`)
- Bug: `scanResultsJson()` returned `{"scanning":true}` (object) while scanning,
  but `[]` (array) when complete. JS code expected a plain array in all cases,
  causing a TypeError crash in the scan poll loop.
- Fix: API now always returns `{"scanning": bool, "networks": [...]}`. JS
  `_pollScan()` updated to check `r.scanning` and iterate `r.networks`.

**Reconnect back-off never reset** (`wifi_manager.cpp`)
- Bug: `_reconnectInterval` was only ever increased (exponential back-off up to
  5 min). After a successful connection it was reset to `STA_RECONNECT_INTERVAL`
  (30 s) instead of back to the initial 5 s. First disconnect after recovery
  would wait 30 s before the first retry.
- Fix: On `WL_CONNECTED`, interval resets to `5000` ms (initial fast value).

### 🟢 Low-Severity Fixes

**`handleSetConfig` incorrect status note** (`web_server.cpp`)
- Bug: Response note said "Restart to apply AP changes" even when only the STA
  password was changed (which applies immediately without restart).
- Fix: Note now correctly distinguishes STA changes (applied live) from AP
  changes (require restart). `staPass` change also triggers `applyStaConfig()`.

**`FIRMWARE_VERSION` macro redefinition warning** (`config.h`)
- Bug: `config.h` hard-defined `FIRMWARE_VERSION "2.0.0"` while `platformio.ini`
  also passes `-DFIRMWARE_VERSION=\"2.0.0\"` via build flags. Caused a compiler
  warning: `macro 'FIRMWARE_VERSION' redefined`.
- Fix: Wrapped in `#ifndef FIRMWARE_VERSION … #endif`. Build-flag value wins;
  `config.h` value is a safe fallback for IDE tooling.

---

## v2.0.0 — Feature Release

Initial full-feature build. See README.md for complete feature list.

- Button Groups / Remote Presets (tabs per group on Remote screen)
- Per-button repeat count + inter-repeat delay with protocol defaults
- Cron-style IR scheduler with NTP time sync
- Wi-Fi AP+STA dual-mode — AP never drops while STA connects
- Async Wi-Fi network scanner with RSSI bars
- Raw IR waveform SVG visualizer with zoom + hover tooltip
- System Status Dashboard (heap, flash, RSSI, NTP time, uptime)
- Live WebSocket status push every 5 s

---

## v1.2.0

- Multi-emitter support (up to 4 independent IR LEDs via RMT)
- Dynamic GPIO reassignment from Web GUI — no reflash required
- GPIO safety validation (forbidden/input-only pins blocked)

## v1.1.0

- WebSocket live IR event broadcast

## v1.0.0

- Initial release: IR capture, save, transmit, virtual remote pad

## v3.0.0 — Tasks 7–11

### New Modules
- **NFC (PN532)**: Read tags (UID/type/ATQA/SAK/blocks), clone, emulate, save, MIFARE dictionary attack, Flipper Zero .nfc import/export, tag parser, Chameleon device support. GPIO config: I2C/SPI/UART interface selector with all pins.
- **RFID 125kHz**: Read (EM4100/HID auto-detect), write UID, emulate, save cards. GPIO config: DATA/CLK/MOD/PWR pins.
- **Sub-1GHz (CC1101)**: Live signal capture (315/433/868/915MHz + custom), replay, save signals, rename/delete. GPIO config: GDO0/GDO2/CS/SCK/MOSI/MISO + VSPI/HSPI selector.
- **NRF24L01 2.4GHz**: RC car D-pad + speed control, 125-channel animated scanner, packet sniffer, replay attack, device presets (RC/Drone/MouseJacker/KeyJacker). GPIO config: CE/CSN/SCK/MOSI/MISO/IRQ + SPI bus selector.
- **System Module**: Auto-refresh status (firmware/uptime/heap/chip/MAC/CPU), GPS info (lat/lon/speed/sats/alt/fix), Global GPIO overview panel (40-pin), RGB LED control (WS2812B/APA102/Single, Off/Solid/Rainbow/Rave), GPS GPIO config, timezone setter, GhostLink toggle, schedule tasks (add/enable/disable/delete), factory reset + reboot with confirm dialogs.

### New API Endpoints (40+)
- `/api/nfc/*` — tags, read, save, delete, emulate, clone, dict, gpio, export, import, parse
- `/api/rfid/*` — cards, read, save, delete, write, emulate, gpio
- `/api/subghz/*` — signals, capture/start/stop/data, save, replay, delete, rename, gpio
- `/api/nrf24/*` — gpio, rc, scan/start/stop/data, sniff/start/stop/data, replay
- `/api/system/*` — status, led, timezone, schedule CRUD, reboot, reset
- `/api/gps/*` — info, gpio
- `/api/ghostlink` — toggle

### New Files
- `include/nfc_module.h` + `src/nfc_module.cpp`
- `include/rfid_module.h` + `src/rfid_module.cpp`
- `include/subghz_module.h` + `src/subghz_module.cpp`
- `include/nrf24_module.h` + `src/nrf24_module.cpp`
- `include/system_module.h` + `src/system_module.cpp`
- `src/web_server_modules.cpp`
