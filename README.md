# IR Remote Web GUI — v11.0.0 (Stability Release)

**Target hardware:** ESP32-WROOM-32 (4 MB flash). Full feature build —
IR + classic-NFC (PN532) + RFID (MFRC522) + Sub-GHz (CC1101) +
nRF24 + Wi-Fi pen-testing + I²S/ADC microphone + SD-card storage,
all driven from a single-page web UI served straight off the chip.

After the device boots it brings up the AP `IR-Remote` /
`irremote123`; point a browser at <http://192.168.4.1> (or
<http://ir-remote.local> once mDNS is up on the STA side) and the
whole UI loads from LittleFS as a gzipped `index.html`.

---

## What's new in v11.0.0 — Stability Release

**Removed modules (freed ~110 KB heap):**
- BLE (scanner / central / peripheral / HID / proxy / bonds) — freed ~90 KB
- A2DP Bluetooth audio — freed ~5 KB
- Rules Engine — freed ~3 KB
- Walkie-Talkie (I²S voice over UDP) — freed ~8 KB
- Speaker driver — freed ~4 KB

**Boot heap before:** ~9 KB free (crash-prone)
**Boot heap after:** ~125 KB free (stable)

**Bug fixes:**
- IR TX queue starvation: inner drain loop added — all queued signals now transmit
- Audit log race condition: `filter()` now holds mutex during iteration (was crashing under concurrent writes)
- WebSocket message JSON injection: button names now serialized via ArduinoJson
- Scheduler fire JSON injection: ArduinoJson replaces raw `snprintf`
- SD SPI mutex missing in `serveIndex` and `handleSdDownload` — wrapped with `g_spi_vspi_mutex`
- WebSocket protocol: `ws://` hardcoded → dynamic (`wss://` on HTTPS)
- WS status pill: amber animated dot when disconnected, retry counter shown, green steady on connect
- SD Extra tab: `SdExtraUI` JS object was missing entirely (13 buttons threw `ReferenceError`)
- Delete endpoints: buttons/groups/schedules were using GET → corrected to POST
- Group reorder: backend now handles `{order:[...]}` array body correctly

---

## Feature matrix

| Module | Backend | UI | Notes |
|---|---|---|---|
| IR receiver / transmitter | ✅ | ✅ | 116 protocols enabled, raw capture + replay, groups, scheduler |
| AC mains-detector (non-contact) | ✅ | ✅ | Live RMS poll, threshold + hysteresis, buzzer alert, WS events |
| NFC (PN532) | ✅ | ✅ | Read / save / clone / emulate, dictionary attack, SD library |
| RFID (MFRC522) | ✅ | ✅ | Read / write / emulate, allowlist + per-card macros |
| Sub-GHz (CC1101) | ✅ | ✅ | Capture, save, replay, raw modulation, SD signals |
| nRF24 | ✅ | ✅ | Scan, sniff, replay, channel-hop |
| Wi-Fi pen-test (deauth / PMKID / handshake / rogue-AP / captive) | ✅ | ✅ | Uses WSL-bypass to inject raw 802.11 frames |
| Microphone (I²S + ADC hybrid) | ✅ | ✅ | Stream over WS as binary frames, record to SD |
| SD-card (mount, ls, upload, download, backup, IR library) | ✅ | ✅ | Live device picker, profile save/load |
| Audit log | ✅ | ✅ | RAM ring + LittleFS rotation; SD mirror toggleable |
| Watchdog | ✅ | ✅ | Crash log persisted on LittleFS |
| Auth (login / logout / password / config) | ✅ | ✅ | Bearer-token; can be disabled via `/api/v1/auth/config` |
| Scheduler | ✅ | ✅ | NTP-driven, timezone-aware |
| mDNS + NetBIOS | ✅ | n/a | `ir-remote.local` (mDNS) + `http://ir-remote` (NetBIOS/NBNS — Windows without Bonjour) |

Legend: ✅ fully wired

---

## Quick start

### 1. Flash a built release

Grab the latest artifact from the [GitHub Releases](../../releases)
page (it includes `bootloader.bin`, `partitions.bin`, `firmware.bin`,
`littlefs.bin`), then:

```bash
pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash \
  0x1000    bootloader.bin \
  0x8000    partitions.bin \
  0x10000   firmware.bin \
  0x310000  littlefs.bin
```

See [FLASHING.md](FLASHING.md) for the full procedure (Windows
syntax, address warnings for v6.x/v5.x/v4.x migrations,
factory-erase recipe).

### 2. Or build from source

```bash
# PlatformIO Core ≥ 6.1.19, Python ≥ 3.8
git clone <this repo>
cd ir_remote_mdns
pio run -e esp32dev                     # firmware.bin
pio run -e esp32dev --target buildfs    # littlefs.bin
pio run -e esp32dev --target upload     # flash firmware via USB
pio run -e esp32dev --target uploadfs   # flash web UI to LittleFS
```

### 3. Connect

| | |
|---|---|
| Default AP | `IR-Remote` / `irremote123` ⚠️ same on every unit — change on first boot |
| AP URL | <http://192.168.4.1> |
| mDNS (after STA join) | <http://ir-remote.local> |
| Web-panel auth | disabled by default. Default username `admin`. The password is derived from the chip's WiFi STA MAC and printed to the serial console at boot as `*** DEFAULT PASSWORD: IR-XXYYZZ ***`. Enable via Settings → Auth, then change the password. |

---

## Partition layout v7.0.1

OTA was removed in v7.0.1; the second app slot + the `otadata` partition
were folded into a single 3 MB factory firmware slot plus a 960 KB
LittleFS.

```
Name      Type   SubType   Offset      Size       Notes
nvs       data   nvs       0x009000     20 KB     WiFi creds + settings
app0      app    factory   0x010000   3072 KB     Firmware
spiffs    data   spiffs    0x310000    960 KB     LittleFS (web UI + data)
```

Total used 4 052 KB out of 4 096 KB — 44 KB is reserved (36 KB
bootloader region + partition table + 8 KB SDK gap between `nvs` and
`app0`).

---

## API overview

All endpoints return JSON unless they're file downloads or
`text/event-stream`-style responses. POST handlers expect
`Content-Type: application/json` and (when auth is on) an
`Authorization: Bearer <token>` header.

Live status is pushed over WebSocket at `ws://<host>/ws`. Events:
`ir_received`, `status`, `message`, `scheduled_tx`, `ac_detected`,
`ac_lost`, `rfid`, `nfc`, `connected`, `pong`.

Key REST roots:

- `/api/buttons`, `/api/groups`, `/api/transmit`, `/api/macro*` — IR
  database CRUD + transmit + macros
- `/api/schedules`, `/api/ntp/*` — scheduler + NTP
- `/api/ac/*` — AC detector
- `/api/nfc/*`, `/api/rfid/*` — tag I/O
- `/api/subghz/*`, `/api/nrf24/*` — sub-GHz + nRF24
- `/api/wpen/*` — Wi-Fi pen-test (deauth, handshake capture, PMKID, etc.)
- `/api/mic/*` — microphone control + recordings
- `/api/sd/*` — SD card file ops
- `/api/system/*`, `/api/modules/*` — module enable/disable, system info
- `/api/v1/*` — public/v1 API for external scripts (auth, audit,
  logs, watchdog, system info)

See `src/web_server*.cpp` for the canonical list.

---

## Project layout

```
ir_remote_mdns/
├── data/
│   └── index.html.gz          # Web UI (gzipped, served from LittleFS)
├── include/                   # All module headers
├── src/
│   ├── main.cpp               # Boot + module orchestration
│   ├── web_server*.cpp        # HTTP routes (split across files)
│   ├── ac_detector.cpp        # Non-contact AC mains detector
│   ├── ir_database.cpp        # Saved IR buttons + groups
│   ├── ir_receiver.cpp        # NEC/Sony/Samsung/raw RX
│   ├── ir_transmitter.cpp     # IR TX
│   ├── nfc_module.cpp         # PN532 driver
│   ├── rfid_module.cpp        # MFRC522 driver
│   ├── subghz_module.cpp      # CC1101 driver
│   ├── nrf24_module.cpp       # nRF24L01+ driver
│   ├── wifi_pen_module.cpp    # 802.11 attack engine + WSL bypass
│   ├── mic_module.cpp         # I²S + ADC audio
│   ├── sd_manager.cpp         # SD card ops
│   ├── scheduler.cpp          # Cron + NTP
│   ├── auth_manager.cpp       # Bearer-token auth
│   ├── audit_manager.cpp      # Audit log (RAM + LittleFS)
│   └── watchdog_manager.cpp   # Task watchdog + crash log
├── platformio.ini             # Build config (espressif32 @ 6.6.0)
├── partitions_custom.csv      # 3-partition layout v7.0.1
└── .github/workflows/         # CI builds + GitHub releases
```

---

## Build / CI

GitHub Actions runs on every push to `main`/`master` and every PR:

1. PlatformIO 6.1.19 + Python 3.12 + cached toolchain
2. Validate `partitions_custom.csv` (no overlaps, no overflow)
3. `pio run` → `firmware.bin`
4. `pio run --target buildfs` → `littlefs.bin`
5. Verify all four binaries exist, report sizes vs partition caps
6. On push to a release branch: tag + upload all four binaries as a
   GitHub Release with flash-command snippets

Local builds need:

- PlatformIO Core ≥ 6.1.19
- Python 3.8+
- A CP2102 or CH340 USB-UART (or use a programmer)

---

## Hardware

See `gpio_config.h` for pin assignments. The defaults are non-conflicting
for a single ESP32-WROOM-32 with:

- IR LED + receiver
- PN532 (NFC) on I²C
- MFRC522 (RFID) on SPI
- CC1101 (Sub-GHz) on SPI
- nRF24L01+ on SPI
- SD card on SPI (VSPI bus, CS=GPIO4)
- Hybrid mic (I²S or ADC, both supported)
- WS2812 status LED on GPIO13

Every module can be enabled / disabled at runtime from
Settings → Modules, freeing its GPIOs for other uses.

---

## Versioning

| Layer | Where |
|---|---|
| Product version | `README.md` (this file) — currently `v11.0.0` |
| Build constant | `platformio.ini` → `-DFIRMWARE_VERSION` (kept in step) |
| Fallback | `include/config.h` `#ifndef FIRMWARE_VERSION` block |
| CI release tag | `.github/workflows/build.yml` `env.FIRMWARE_VERSION` |

When you bump the product version, also bump the build constant in
`platformio.ini`, the fallback in `config.h`, and the workflow env var
— or use a single source of truth via `git describe` later.

See [CHANGELOG.md](CHANGELOG.md) for per-version notes.

---

## License

See `LICENSE`. The codebase pulls in third-party libraries (FastLED,
IRremoteESP8266, ESPAsyncWebServer, ArduinoJson, RF24, Adafruit
PN532, Adafruit BusIO) under their respective licenses.
