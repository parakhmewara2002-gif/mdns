#pragma once
// ============================================================
//  config.h  -  Compile-time tunables for IR Remote Web GUI
//
//  v2.0.0 - Added:
//    + Button Groups / Remote Presets
//    + Scheduled IR Transmission (NTP + cron-style)
//    + IR Repeat / Timing Controls
//    + Raw IR Signal Visualization support
//    + System Status Dashboard
//    + AP+STA dual-mode Wi-Fi (reliable)
//    + Wi-Fi network scanner
// ============================================================

// ── Firmware version ─────────────────────────────────────────
// Defined via -DFIRMWARE_VERSION in platformio.ini build_flags
// (do NOT redefine here - would cause "macro redefined" warning).
// The fallback below is only used if someone builds outside PlatformIO;
// keep it in step with platformio.ini's -DFIRMWARE_VERSION.
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "10.0.0"
#endif

// ── Wi-Fi Access Point defaults ─────────────────────────────
#define DEFAULT_AP_SSID       "IR-Remote"
#define DEFAULT_AP_PASS       "irremote123"   // min 8 chars, or "" for open
#define DEFAULT_AP_CHANNEL    1
#define DEFAULT_AP_HIDDEN     false
#define DEFAULT_AP_MAX_CONN   4
#define AP_IP                 "192.168.4.1"

// ── mDNS ─────────────────────────────────────────────────────
// Device reachable at http://<MDNS_HOSTNAME>.local when STA is connected.
// Must be lowercase letters, digits, hyphens only (no spaces, no dots).
#define MDNS_HOSTNAME         "ir-remote"

// ── Wi-Fi Station mode (optional) ───────────────────────────
#define DEFAULT_STA_SSID      ""
#define DEFAULT_STA_PASS      ""
#define STA_CONNECT_TIMEOUT   20000   // ms - increased for reliability
#define STA_RECONNECT_INTERVAL 30000  // ms - retry if disconnected
#define WIFI_SCAN_MAX_RESULTS 20      // max SSIDs returned

// ── Hardware pins ────────────────────────────────────────────
#define IR_RECV_PIN           14
#define IR_SEND_PIN           27
#define IR_RECV_ENABLE_PULLUP true

// ── IR capture settings ──────────────────────────────────────
#define IR_RECV_BUFFER_SIZE   1024
#define IR_CAPTURE_TIMEOUT_MS 15
#define IR_MIN_UNKNOWN_SIZE   12
#define IR_DEBOUNCE_MS        250

// ── IR transmit settings ─────────────────────────────────────
#define IR_DEFAULT_FREQ_KHZ   38
#define IR_DEFAULT_DUTY_CYCLE 33
// IR_SEND_REPEATS: protocol-level repeat frames (inside one sendXXX() call).
// 2 gives reliable reception on cheap/distant receivers without over-spamming.
// Per-protocol overrides in doTransmit() take precedence (e.g. SONY needs >=3,
// DISH needs >=3). This default applies to all other protocols.
#define IR_SEND_REPEATS       2

// ── IR Repeat / Timing defaults ──────────────────────────────
// IR_DEFAULT_REPEAT_COUNT: how many times the full button command is resent.
// 1 = send once (good for AC, macros). Scheduler / user can increase per-button.
#define IR_DEFAULT_REPEAT_COUNT  1
// IR_DEFAULT_REPEAT_DELAY: gap (ms) between repeat sends.
// 110ms = standard NEC "keep-pressed" frame interval. Works for small/large devices.
#define IR_DEFAULT_REPEAT_DELAY  110  // ms - matches NEC/Samsung hold-key timing
#define IR_MAX_REPEAT_COUNT      20
#define IR_MAX_REPEAT_DELAY      2000 // ms

// ── Pre-TX quiet time ─────────────────────────────────────────
// How long to silence the IR bus before transmitting.
// 20ms works for most; increase to 30ms for stubborn devices.
#define IR_PRE_TX_QUIET_MS    20   // ms - pause receiver before TX burst
// Post-TX settle time - extra ms after signal finishes before RX resumes.
#define IR_POST_TX_SETTLE_MS  30   // ms

// ── Button Groups ────────────────────────────────────────────
#define MAX_GROUPS            16
#define GROUP_NAME_MAX        32
#define DEFAULT_GROUP_NAME    "Default"

// ── Scheduler ────────────────────────────────────────────────
#define MAX_SCHEDULES         32
#define SCHEDULER_CHECK_INTERVAL_MS 10000  // check every 10 seconds
#define NTP_SERVER1           "pool.ntp.org"
#define NTP_SERVER2           "time.nist.gov"
#define NTP_TIMEZONE_OFFSET   19800  // IST = UTC+5:30 (India Standard Time)
#define NTP_DST_OFFSET        0      // DST offset in seconds (user configurable)
#define NTP_SYNC_INTERVAL_MS  3600000UL  // re-sync every hour

// ── Web server ───────────────────────────────────────────────
#define HTTP_PORT             80
#define WS_PATH               "/ws"
#define WS_MAX_CLIENTS        2      // was 4 — saves ~10KB WS client buffers
#define WS_QUEUE_MAX          16     // was 64 — saves ~3KB queue RAM
#define HTTP_MAX_BODY         16384  // was 65536 — saves ~48KB; IR DB restore uses chunked upload anyway

// ── Filesystem ───────────────────────────────────────────────
#define DB_FILE               "/ir_database.json"
#define DB_BACKUP_FILE        "/ir_database_backup.json"  // Backup & Restore
#define DB_TMP_FILE           "/ir_db.tmp"                // atomic write temp
#define CFG_FILE              "/config.json"
#define GROUPS_FILE           "/groups.json"
#define SCHEDULES_FILE        "/schedules.json"
#define MAX_BUTTONS           32      // was 64 — reserve(32) saves ~2KB vs reserve(64)
#define MAX_RAW_EDGES         512

// ── Backup & Restore limits ───────────────────────────────────
// Maximum JSON body accepted for a restore upload (64 KB).
// Larger files are rejected with HTTP 413 before parsing.
#define DB_RESTORE_MAX_BYTES  65536UL

// ── IR Storage improvements (Fix 1-4) ───────────────────────
//
// Fix 1: Auto-save received IR signals
//   When enabled, every decoded IR signal is checked against
//   the DB and saved automatically if it is a new code.
#define IR_AUTO_SAVE_DEFAULT  false        // off by default; user enables via API
#define IR_AUTO_SAVE_FILE     "/ir_autosave.json"
//
// Fix 2: Lazy save - batch flash writes to reduce wear
//   Instead of writing to flash on every add/update/delete,
//   a dirty flag is set and the DB is flushed after this many
//   milliseconds of inactivity.
#define DB_LAZY_SAVE_MS       5000UL       // flush dirty DB 5 s after last change
//
// Fix 3: RAW button memory guard
//   Each RAW button holds up to MAX_RAW_EDGES uint16_t values
//   (512 x 2 = 1 KB).  Allowing all 128 buttons to be RAW
//   would consume ~136 KB heap - unsafe on ESP32.
#define MAX_RAW_BUTTONS       16           // max simultaneous RAW buttons in DB
//
// Fix 4: Streaming JSON save (see ir_database.cpp save())
//   save() serialises one button at a time directly to the
//   LittleFS file instead of building a full in-RAM JsonDocument.
//   Peak extra RAM during save ~1 x IRButton (~1 KB)
//   instead of N x IRButton (up to 136 KB for 128 RAW buttons).

// ── Serial debug ─────────────────────────────────────────────
#define SERIAL_BAUD           115200
#define DEBUG_TAG             "[IR]"

// ── SD Card (optional) ───────────────────────────────────────
// VSPI bus - wiring: MOSI=23, MISO=19, SCK=18, CS=4
// All SD features are automatically disabled if card is absent.
// Wrapped in #ifndef so -D flags in platformio.ini take precedence
// without triggering "macro redefined" warnings.
#ifndef SD_CS_PIN
  #define SD_CS_PIN          4    // Chip-select; change if wired differently
#endif
#ifndef SD_MOSI_PIN
  #define SD_MOSI_PIN        23
#endif
#ifndef SD_MISO_PIN
  #define SD_MISO_PIN        19
#endif
#ifndef SD_SCK_PIN
  #define SD_SCK_PIN         18
#endif
#ifndef SD_SPI_FREQ
  #define SD_SPI_FREQ        4000000UL   // 4 MHz initial; can increase to 20 MHz
#endif

// ── Internal Macros (LittleFS - no SD required) ── v2.2.0 ────
// Up to MACRO_MAX_INTERNAL macros stored directly in LittleFS.
// Each macro is a JSON file at /macros/<name>.json
// Same format as SD macros so they are interchangeable.
#define MACRO_DIR              "/macros"
#define MACRO_MAX_INTERNAL     16       // max macros in LittleFS
#define MACRO_NAME_MAX         32       // max filename chars (without .json)
#define MACRO_MAX_STEPS        32       // max steps per LittleFS macro
#define MACRO_STEP_MAX_DELAY   10000    // ms max delay between steps
