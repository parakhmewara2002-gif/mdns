# IR Remote Web GUI v10.0.0 — Flashing Instructions

> Partition layout **v7.0.1** (no OTA, factory-only). LittleFS lives at **`0x310000`** — see warning below if you've flashed older builds.

## Quick Start

Flash **4 files** in the correct order using `esptool.py`.

### Windows (one command)

```bat
esptool.py --chip esp32 --port COM3 --baud 921600 ^
  write_flash ^
  0x1000    bootloader.bin ^
  0x8000    partitions.bin ^
  0x10000   firmware.bin ^
  0x310000  littlefs.bin
```

### macOS / Linux

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash \
  0x1000    bootloader.bin \
  0x8000    partitions.bin \
  0x10000   firmware.bin \
  0x310000  littlefs.bin
```

> Replace `COM3` / `/dev/ttyUSB0` with your actual serial port.

> ⚠️ **Important:** `littlefs.bin` must be written to **`0x310000`** for partition layout v7.0.1.
> Earlier releases used different addresses — flashing to any of these on a v7.0.1 build will corrupt the filesystem or write outside the spiffs partition:
> - `0x390000` (v4.x)
> - `0x2D0000` (v5.x)
> - `0x350000` (v6.x / early v7.0)

---

## Flash Map (4 MB ESP32 — Partition Layout v7.0.1)

| File             | Address          | Description                              |
|------------------|------------------|------------------------------------------|
| `bootloader.bin` | `0x001000`       | ESP32 first-stage bootloader             |
| `partitions.bin` | `0x008000`       | Custom partition table                   |
| `firmware.bin`   | `0x010000`       | Application firmware (IR Remote v10.0.0) |
| `littlefs.bin`   | **`0x310000`**   | Web GUI files + LittleFS storage         |

---

## Partition Layout v7.0.1

OTA was removed in v7.0.1 — the freed 8 KB and the second app slot were folded into one big factory partition + a larger LittleFS.

```
Name       Type   SubType   Offset     Size       Notes
nvs        data   nvs       0x009000    20 KB     WiFi credentials, settings
app0       app    factory   0x010000  3072 KB     Firmware (factory slot, no OTA)
spiffs     data   spiffs    0x310000   960 KB     LittleFS (Web GUI + data)
```

Total used: 4 052 KB out of the 4 096 KB flash. The 8 KB gap between the end of `nvs` (`0x00E000`) and the start of `app0` (`0x010000`) is reserved by the ESP32 SDK for the partition table and bootloader header; combined with the 36 KB bootloader region at 0x000000 this accounts for the remaining 44 KB.

---

## Building from Source (PlatformIO)

### Prerequisites

- [PlatformIO Core](https://platformio.org/install/cli) ≥ 6.1.19, or PlatformIO IDE (VSCode extension)
- Python 3.8+
- USB driver for CP2102 / CH340 chip on your ESP32 board

### Build & Flash Steps

```bash
# Clone or extract the project
cd ir_remote_mdns

# Build firmware only
pio run -e esp32dev

# Build filesystem (LittleFS) image only
pio run -e esp32dev -t buildfs

# Upload firmware via USB
pio run -e esp32dev -t upload

# Upload filesystem via USB (required on first flash)
pio run -e esp32dev -t uploadfs

# Monitor serial output
pio device monitor --baud 115200
```

> **Important:** The filesystem (`uploadfs`) must be flashed at least once.
> The firmware will start, but the Web GUI will be missing until `littlefs.bin` is written to `0x310000`.

---

## Migrating from an older build

If your device currently runs v6.x or earlier and you flash v10.0.0 firmware **without** also re-flashing LittleFS, the device will boot but the web UI will be either blank or partly stale (the FS image at the old address now overlaps the new firmware partition).

Always flash all four binaries together when changing major version. To wipe everything first:

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 erase_flash
# then run the full write_flash command above
```

---

