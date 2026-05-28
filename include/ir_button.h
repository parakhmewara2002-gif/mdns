#pragma once
// ============================================================
//  ir_button.h  -  Data model for a saved IR button
//
//  v2.0.0: groupId, repeatCount, repeatDelay
//  v2.2.0: icon, color fields + ALL protocols from
//           IRremoteESP8266 v2.8.6 decode_type_t enum
//
//  PROTOCOL STRATEGY
//  ─────────────────
//  • Simple (<=64-bit) protocols:  decoded to code + bits,
//    retransmitted with the matching IRsend::sendXxx() call.
//  • Complex AC (state[]) protocols: captured as RAW timings,
//    stored in rawData[], retransmitted via sendRaw().
//    This is safe and always works - the library's own decoder
//    fills decode_results.state[] but we just re-send the
//    timing array which is 100% faithful to the original signal.
// ============================================================
#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include "config.h"

// ── Supported protocols ──────────────────────────────────────
// Grouped: Simple (send via code), Complex AC (send via RAW)
enum class IRProtocol : uint8_t {
    UNKNOWN          = 0,

    // ── Simple / TV protocols ────────────────────────────────
    NEC              = 1,
    SONY             = 2,
    SAMSUNG          = 3,
    LG               = 4,
    PANASONIC        = 5,
    RAW              = 6,   // explicit RAW capture
    RC5              = 7,
    RC6              = 8,
    JVC              = 9,
    DISH             = 10,
    SHARP            = 11,
    DENON            = 12,
    MITSUBISHI       = 13,  // Mitsubishi TV (simple)
    MITSUBISHI2      = 14,
    SANYO            = 15,
    AIWA_RC_T501     = 16,
    NIKAI            = 17,
    MAGIQUEST        = 18,
    LASERTAG         = 19,
    RCMM             = 20,
    LEGOPF           = 21,
    PIONEER          = 22,
    EPSON            = 23,
    SYMPHONY         = 24,
    BOSE             = 25,
    METZ             = 26,
    DOSHISHA         = 27,
    GORENJE          = 28,
    INAX             = 29,   // Inax toilet (simple)
    SAMSUNG36        = 30,
    LUTRON           = 31,
    MWM              = 32,
    MULTIBRACKETS    = 33,
    ELITESCREENS     = 34,
    MILESTAG2        = 35,
    XMP              = 36,
    ARRIS            = 37,
    AIWA_RC_T501_2   = 38,  // alias kept for compat
    TRUMA            = 39,
    WOWWEE           = 40,
    ZEPEAL           = 41,
    TECO             = 42,  // simple Teco
    GOODWEATHER      = 43,
    MIDEA            = 44,
    MIDEA24          = 45,
    GICABLE          = 46,
    NEC_LIKE         = 47,

    // ── Complex AC protocols (captured as RAW) ───────────────
    // All of these use state[] arrays in the library.
    // We store them as RAW timings and replay via sendRaw().
    DAIKIN           = 50,
    DAIKIN2          = 51,
    DAIKIN64         = 52,
    DAIKIN128        = 53,
    DAIKIN152        = 54,
    DAIKIN160        = 55,
    DAIKIN176        = 56,
    DAIKIN200        = 57,
    DAIKIN216        = 58,
    DAIKIN312        = 59,
    MITSUBISHI_AC    = 60,
    MITSUBISHI136    = 61,
    MITSUBISHI112    = 62,
    MITSUBISHI_HEAVY_88  = 63,
    MITSUBISHI_HEAVY_152 = 64,
    SAMSUNG_AC       = 65,
    FUJITSU_AC       = 66,
    TOSHIBA_AC       = 67,
    SHARP_AC         = 68,
    PANASONIC_AC     = 69,
    PANASONIC_AC32   = 70,
    LG_AC            = 71,  // LG A/C (state-based)
    KELVINATOR       = 72,
    GREE             = 73,
    ARGO             = 74,
    TROTEC           = 75,
    TROTEC_3550      = 76,
    HAIER_AC         = 77,
    HAIER_AC_YRW02   = 78,
    HAIER_AC176      = 79,
    HAIER_AC160      = 80,
    HITACHI_AC       = 81,
    HITACHI_AC1      = 82,
    HITACHI_AC2      = 83,
    HITACHI_AC3      = 84,
    HITACHI_AC264    = 85,
    HITACHI_AC296    = 86,
    HITACHI_AC344    = 87,
    HITACHI_AC424    = 88,
    WHYNTER          = 89,
    WHIRLPOOL_AC     = 90,
    COOLIX           = 91,
    COOLIX48         = 92,
    ELECTRA_AC       = 93,
    CARRIER_AC       = 94,
    CARRIER_AC40     = 95,
    CARRIER_AC64     = 96,
    CARRIER_AC84     = 97,
    CARRIER_AC128    = 98,
    VESTEL_AC        = 99,
    TCL112AC         = 100,
    TCL96AC          = 101,
    NEOCLIMA         = 102,
    AMCOR            = 103,
    MIDEA_AC         = 104, // Midea A/C (complex)
    SANYO_AC         = 105,
    SANYO_AC88       = 106,
    SANYO_AC152      = 107,
    VOLTAS           = 108,
    KELON            = 109,
    KELON168         = 110,
    CORONA_AC        = 111,
    DELONGHI_AC      = 112,
    ELECTRA_AC2      = 113,
    MIRAGE           = 114,
    RHOSS            = 115,
    AIRTON           = 116,
    AIRWELL          = 117,
    TECHNIBEL_AC     = 118,
    TEKNOPOINT       = 119,
    TRANSCOLD        = 120,
    ECOCLIM          = 121,
    BOSCH144         = 122,
    YORK             = 123,
    BLUESTARHEAVY    = 124,
    EUROM            = 125,
    CLIMABUTLER      = 126,
    TOTO             = 127,   // Toto toilet
};

// ── Simple (<=64-bit) protocols - send via code ───────────────
// Complex AC protocols fall through to sendRaw() automatically.
inline bool isSimpleProtocol(IRProtocol p) {
    switch (p) {
        case IRProtocol::NEC:
        case IRProtocol::NEC_LIKE:
        case IRProtocol::SONY:
        case IRProtocol::SAMSUNG:
        case IRProtocol::SAMSUNG36:
        case IRProtocol::LG:
        case IRProtocol::PANASONIC:
        case IRProtocol::RC5:
        case IRProtocol::RC6:
        case IRProtocol::JVC:
        case IRProtocol::DISH:
        case IRProtocol::SHARP:
        case IRProtocol::DENON:
        case IRProtocol::MITSUBISHI:
        case IRProtocol::MITSUBISHI2:
        case IRProtocol::SANYO:
        case IRProtocol::AIWA_RC_T501:
        case IRProtocol::AIWA_RC_T501_2:
        case IRProtocol::NIKAI:
        case IRProtocol::MAGIQUEST:
        case IRProtocol::LASERTAG:
        case IRProtocol::RCMM:
        case IRProtocol::LEGOPF:
        case IRProtocol::PIONEER:
        case IRProtocol::EPSON:
        case IRProtocol::SYMPHONY:
        case IRProtocol::BOSE:
        case IRProtocol::METZ:
        case IRProtocol::DOSHISHA:
        case IRProtocol::GORENJE:
        case IRProtocol::INAX:
        case IRProtocol::LUTRON:
        case IRProtocol::MULTIBRACKETS:
        case IRProtocol::ELITESCREENS:
        case IRProtocol::MILESTAG2:
        case IRProtocol::XMP:
        case IRProtocol::ARRIS:
        case IRProtocol::TRUMA:
        case IRProtocol::WOWWEE:
        case IRProtocol::ZEPEAL:
        case IRProtocol::TECO:
        case IRProtocol::GOODWEATHER:
        case IRProtocol::MIDEA:
        case IRProtocol::MIDEA24:
        case IRProtocol::GICABLE:
        case IRProtocol::COOLIX:
        case IRProtocol::COOLIX48:
            return true;
        // MWM uses byte-array sendMWM(), NOT a simple 64-bit code - exclude from simple
        default:
            return false;
    }
}

inline const char* protocolName(IRProtocol p) {
    switch (p) {
        case IRProtocol::NEC:              return "NEC";
        case IRProtocol::NEC_LIKE:         return "NEC_LIKE";
        case IRProtocol::SONY:             return "SONY";
        case IRProtocol::SAMSUNG:          return "SAMSUNG";
        case IRProtocol::SAMSUNG36:        return "SAMSUNG36";
        case IRProtocol::SAMSUNG_AC:       return "SAMSUNG_AC";
        case IRProtocol::LG:               return "LG";
        case IRProtocol::LG_AC:            return "LG_AC";
        case IRProtocol::PANASONIC:        return "PANASONIC";
        case IRProtocol::PANASONIC_AC:     return "PANASONIC_AC";
        case IRProtocol::PANASONIC_AC32:   return "PANASONIC_AC32";
        case IRProtocol::RAW:              return "RAW";
        case IRProtocol::RC5:              return "RC5";
        case IRProtocol::RC6:              return "RC6";
        case IRProtocol::JVC:              return "JVC";
        case IRProtocol::DISH:             return "DISH";
        case IRProtocol::SHARP:            return "SHARP";
        case IRProtocol::SHARP_AC:         return "SHARP_AC";
        case IRProtocol::DENON:            return "DENON";
        case IRProtocol::MITSUBISHI:       return "MITSUBISHI";
        case IRProtocol::MITSUBISHI2:      return "MITSUBISHI2";
        case IRProtocol::MITSUBISHI_AC:    return "MITSUBISHI_AC";
        case IRProtocol::MITSUBISHI136:    return "MITSUBISHI136";
        case IRProtocol::MITSUBISHI112:    return "MITSUBISHI112";
        case IRProtocol::MITSUBISHI_HEAVY_88:  return "MITSUBISHI_HEAVY_88";
        case IRProtocol::MITSUBISHI_HEAVY_152: return "MITSUBISHI_HEAVY_152";
        case IRProtocol::SANYO:            return "SANYO";
        case IRProtocol::SANYO_AC:         return "SANYO_AC";
        case IRProtocol::SANYO_AC88:       return "SANYO_AC88";
        case IRProtocol::SANYO_AC152:      return "SANYO_AC152";
        case IRProtocol::AIWA_RC_T501:     return "AIWA_RC_T501";
        case IRProtocol::AIWA_RC_T501_2:   return "AIWA_RC_T501_2";
        case IRProtocol::NIKAI:            return "NIKAI";
        case IRProtocol::MAGIQUEST:        return "MAGIQUEST";
        case IRProtocol::LASERTAG:         return "LASERTAG";
        case IRProtocol::RCMM:             return "RCMM";
        case IRProtocol::LEGOPF:           return "LEGOPF";
        case IRProtocol::PIONEER:          return "PIONEER";
        case IRProtocol::EPSON:            return "EPSON";
        case IRProtocol::SYMPHONY:         return "SYMPHONY";
        case IRProtocol::BOSE:             return "BOSE";
        case IRProtocol::METZ:             return "METZ";
        case IRProtocol::DOSHISHA:         return "DOSHISHA";
        case IRProtocol::GORENJE:          return "GORENJE";
        case IRProtocol::INAX:             return "INAX";
        case IRProtocol::LUTRON:           return "LUTRON";
        case IRProtocol::MWM:              return "MWM";
        case IRProtocol::MULTIBRACKETS:    return "MULTIBRACKETS";
        case IRProtocol::ELITESCREENS:     return "ELITESCREENS";
        case IRProtocol::MILESTAG2:        return "MILESTAG2";
        case IRProtocol::XMP:              return "XMP";
        case IRProtocol::ARRIS:            return "ARRIS";
        case IRProtocol::TRUMA:            return "TRUMA";
        case IRProtocol::WOWWEE:           return "WOWWEE";
        case IRProtocol::ZEPEAL:           return "ZEPEAL";
        case IRProtocol::TECO:             return "TECO";
        case IRProtocol::GOODWEATHER:      return "GOODWEATHER";
        case IRProtocol::MIDEA:            return "MIDEA";
        case IRProtocol::MIDEA24:          return "MIDEA24";
        case IRProtocol::MIDEA_AC:         return "MIDEA_AC";
        case IRProtocol::GICABLE:          return "GICABLE";
        case IRProtocol::DAIKIN:           return "DAIKIN";
        case IRProtocol::DAIKIN2:          return "DAIKIN2";
        case IRProtocol::DAIKIN64:         return "DAIKIN64";
        case IRProtocol::DAIKIN128:        return "DAIKIN128";
        case IRProtocol::DAIKIN152:        return "DAIKIN152";
        case IRProtocol::DAIKIN160:        return "DAIKIN160";
        case IRProtocol::DAIKIN176:        return "DAIKIN176";
        case IRProtocol::DAIKIN200:        return "DAIKIN200";
        case IRProtocol::DAIKIN216:        return "DAIKIN216";
        case IRProtocol::DAIKIN312:        return "DAIKIN312";
        case IRProtocol::FUJITSU_AC:       return "FUJITSU_AC";
        case IRProtocol::TOSHIBA_AC:       return "TOSHIBA_AC";
        case IRProtocol::KELVINATOR:       return "KELVINATOR";
        case IRProtocol::GREE:             return "GREE";
        case IRProtocol::ARGO:             return "ARGO";
        case IRProtocol::TROTEC:           return "TROTEC";
        case IRProtocol::TROTEC_3550:      return "TROTEC_3550";
        case IRProtocol::HAIER_AC:         return "HAIER_AC";
        case IRProtocol::HAIER_AC_YRW02:   return "HAIER_AC_YRW02";
        case IRProtocol::HAIER_AC176:      return "HAIER_AC176";
        case IRProtocol::HAIER_AC160:      return "HAIER_AC160";
        case IRProtocol::HITACHI_AC:       return "HITACHI_AC";
        case IRProtocol::HITACHI_AC1:      return "HITACHI_AC1";
        case IRProtocol::HITACHI_AC2:      return "HITACHI_AC2";
        case IRProtocol::HITACHI_AC3:      return "HITACHI_AC3";
        case IRProtocol::HITACHI_AC264:    return "HITACHI_AC264";
        case IRProtocol::HITACHI_AC296:    return "HITACHI_AC296";
        case IRProtocol::HITACHI_AC344:    return "HITACHI_AC344";
        case IRProtocol::HITACHI_AC424:    return "HITACHI_AC424";
        case IRProtocol::WHYNTER:          return "WHYNTER";
        case IRProtocol::WHIRLPOOL_AC:     return "WHIRLPOOL_AC";
        case IRProtocol::COOLIX:           return "COOLIX";
        case IRProtocol::COOLIX48:         return "COOLIX48";
        case IRProtocol::ELECTRA_AC:       return "ELECTRA_AC";
        case IRProtocol::ELECTRA_AC2:      return "ELECTRA_AC2";
        case IRProtocol::CARRIER_AC:       return "CARRIER_AC";
        case IRProtocol::CARRIER_AC40:     return "CARRIER_AC40";
        case IRProtocol::CARRIER_AC64:     return "CARRIER_AC64";
        case IRProtocol::CARRIER_AC84:     return "CARRIER_AC84";
        case IRProtocol::CARRIER_AC128:    return "CARRIER_AC128";
        case IRProtocol::VESTEL_AC:        return "VESTEL_AC";
        case IRProtocol::TCL112AC:         return "TCL112AC";
        case IRProtocol::TCL96AC:          return "TCL96AC";
        case IRProtocol::NEOCLIMA:         return "NEOCLIMA";
        case IRProtocol::AMCOR:            return "AMCOR";
        case IRProtocol::VOLTAS:           return "VOLTAS";
        case IRProtocol::KELON:            return "KELON";
        case IRProtocol::KELON168:         return "KELON168";
        case IRProtocol::CORONA_AC:        return "CORONA_AC";
        case IRProtocol::DELONGHI_AC:      return "DELONGHI_AC";
        case IRProtocol::MIRAGE:           return "MIRAGE";
        case IRProtocol::RHOSS:            return "RHOSS";
        case IRProtocol::AIRTON:           return "AIRTON";
        case IRProtocol::AIRWELL:          return "AIRWELL";
        case IRProtocol::TECHNIBEL_AC:     return "TECHNIBEL_AC";
        case IRProtocol::TEKNOPOINT:       return "TEKNOPOINT";
        case IRProtocol::TRANSCOLD:        return "TRANSCOLD";
        case IRProtocol::ECOCLIM:          return "ECOCLIM";
        case IRProtocol::BOSCH144:         return "BOSCH144";
        case IRProtocol::YORK:             return "YORK";
        case IRProtocol::BLUESTARHEAVY:    return "BLUESTARHEAVY";
        case IRProtocol::EUROM:            return "EUROM";
        case IRProtocol::CLIMABUTLER:      return "CLIMABUTLER";
        case IRProtocol::TOTO:             return "TOTO";
        default:                           return "UNKNOWN";
    }
}

inline IRProtocol protocolFromString(const char* s) {
    if (!s) return IRProtocol::UNKNOWN;
    // Simple lookup table - covers all 127 protocols
    struct { const char* name; IRProtocol proto; } tbl[] = {
        {"NEC",             IRProtocol::NEC},
        {"NEC_LIKE",        IRProtocol::NEC_LIKE},
        {"SONY",            IRProtocol::SONY},
        {"SAMSUNG",         IRProtocol::SAMSUNG},
        {"SAMSUNG36",       IRProtocol::SAMSUNG36},
        {"SAMSUNG_AC",      IRProtocol::SAMSUNG_AC},
        {"LG",              IRProtocol::LG},
        {"LG_AC",           IRProtocol::LG_AC},
        {"PANASONIC",       IRProtocol::PANASONIC},
        {"PANASONIC_AC",    IRProtocol::PANASONIC_AC},
        {"PANASONIC_AC32",  IRProtocol::PANASONIC_AC32},
        {"RAW",             IRProtocol::RAW},
        {"RC5",             IRProtocol::RC5},
        {"RC6",             IRProtocol::RC6},
        {"JVC",             IRProtocol::JVC},
        {"DISH",            IRProtocol::DISH},
        {"SHARP",           IRProtocol::SHARP},
        {"SHARP_AC",        IRProtocol::SHARP_AC},
        {"DENON",           IRProtocol::DENON},
        {"MITSUBISHI",      IRProtocol::MITSUBISHI},
        {"MITSUBISHI2",     IRProtocol::MITSUBISHI2},
        {"MITSUBISHI_AC",   IRProtocol::MITSUBISHI_AC},
        {"MITSUBISHI136",   IRProtocol::MITSUBISHI136},
        {"MITSUBISHI112",   IRProtocol::MITSUBISHI112},
        {"MITSUBISHI_HEAVY_88",  IRProtocol::MITSUBISHI_HEAVY_88},
        {"MITSUBISHI_HEAVY_152", IRProtocol::MITSUBISHI_HEAVY_152},
        {"SANYO",           IRProtocol::SANYO},
        {"SANYO_AC",        IRProtocol::SANYO_AC},
        {"SANYO_AC88",      IRProtocol::SANYO_AC88},
        {"SANYO_AC152",     IRProtocol::SANYO_AC152},
        {"AIWA_RC_T501",    IRProtocol::AIWA_RC_T501},
        {"AIWA_RC_T501_2",  IRProtocol::AIWA_RC_T501_2},
        {"NIKAI",           IRProtocol::NIKAI},
        {"MAGIQUEST",       IRProtocol::MAGIQUEST},
        {"LASERTAG",        IRProtocol::LASERTAG},
        {"RCMM",            IRProtocol::RCMM},
        {"LEGOPF",          IRProtocol::LEGOPF},
        {"PIONEER",         IRProtocol::PIONEER},
        {"EPSON",           IRProtocol::EPSON},
        {"SYMPHONY",        IRProtocol::SYMPHONY},
        {"BOSE",            IRProtocol::BOSE},
        {"METZ",            IRProtocol::METZ},
        {"DOSHISHA",        IRProtocol::DOSHISHA},
        {"GORENJE",         IRProtocol::GORENJE},
        {"INAX",            IRProtocol::INAX},
        {"LUTRON",          IRProtocol::LUTRON},
        {"MWM",             IRProtocol::MWM},
        {"MULTIBRACKETS",   IRProtocol::MULTIBRACKETS},
        {"ELITESCREENS",    IRProtocol::ELITESCREENS},
        {"MILESTAG2",       IRProtocol::MILESTAG2},
        {"XMP",             IRProtocol::XMP},
        {"ARRIS",           IRProtocol::ARRIS},
        {"TRUMA",           IRProtocol::TRUMA},
        {"WOWWEE",          IRProtocol::WOWWEE},
        {"ZEPEAL",          IRProtocol::ZEPEAL},
        {"TECO",            IRProtocol::TECO},
        {"GOODWEATHER",     IRProtocol::GOODWEATHER},
        {"MIDEA",           IRProtocol::MIDEA},
        {"MIDEA24",         IRProtocol::MIDEA24},
        {"MIDEA_AC",        IRProtocol::MIDEA_AC},
        {"GICABLE",         IRProtocol::GICABLE},
        {"DAIKIN",          IRProtocol::DAIKIN},
        {"DAIKIN2",         IRProtocol::DAIKIN2},
        {"DAIKIN64",        IRProtocol::DAIKIN64},
        {"DAIKIN128",       IRProtocol::DAIKIN128},
        {"DAIKIN152",       IRProtocol::DAIKIN152},
        {"DAIKIN160",       IRProtocol::DAIKIN160},
        {"DAIKIN176",       IRProtocol::DAIKIN176},
        {"DAIKIN200",       IRProtocol::DAIKIN200},
        {"DAIKIN216",       IRProtocol::DAIKIN216},
        {"DAIKIN312",       IRProtocol::DAIKIN312},
        {"FUJITSU_AC",      IRProtocol::FUJITSU_AC},
        {"TOSHIBA_AC",      IRProtocol::TOSHIBA_AC},
        {"KELVINATOR",      IRProtocol::KELVINATOR},
        {"GREE",            IRProtocol::GREE},
        {"ARGO",            IRProtocol::ARGO},
        {"TROTEC",          IRProtocol::TROTEC},
        {"TROTEC_3550",     IRProtocol::TROTEC_3550},
        {"HAIER_AC",        IRProtocol::HAIER_AC},
        {"HAIER_AC_YRW02",  IRProtocol::HAIER_AC_YRW02},
        {"HAIER_AC176",     IRProtocol::HAIER_AC176},
        {"HAIER_AC160",     IRProtocol::HAIER_AC160},
        {"HITACHI_AC",      IRProtocol::HITACHI_AC},
        {"HITACHI_AC1",     IRProtocol::HITACHI_AC1},
        {"HITACHI_AC2",     IRProtocol::HITACHI_AC2},
        {"HITACHI_AC3",     IRProtocol::HITACHI_AC3},
        {"HITACHI_AC264",   IRProtocol::HITACHI_AC264},
        {"HITACHI_AC296",   IRProtocol::HITACHI_AC296},
        {"HITACHI_AC344",   IRProtocol::HITACHI_AC344},
        {"HITACHI_AC424",   IRProtocol::HITACHI_AC424},
        {"WHYNTER",         IRProtocol::WHYNTER},
        {"WHIRLPOOL_AC",    IRProtocol::WHIRLPOOL_AC},
        {"COOLIX",          IRProtocol::COOLIX},
        {"COOLIX48",        IRProtocol::COOLIX48},
        {"ELECTRA_AC",      IRProtocol::ELECTRA_AC},
        {"ELECTRA_AC2",     IRProtocol::ELECTRA_AC2},
        {"CARRIER_AC",      IRProtocol::CARRIER_AC},
        {"CARRIER_AC40",    IRProtocol::CARRIER_AC40},
        {"CARRIER_AC64",    IRProtocol::CARRIER_AC64},
        {"CARRIER_AC84",    IRProtocol::CARRIER_AC84},
        {"CARRIER_AC128",   IRProtocol::CARRIER_AC128},
        {"VESTEL_AC",       IRProtocol::VESTEL_AC},
        {"TCL112AC",        IRProtocol::TCL112AC},
        {"TCL96AC",         IRProtocol::TCL96AC},
        {"NEOCLIMA",        IRProtocol::NEOCLIMA},
        {"AMCOR",           IRProtocol::AMCOR},
        {"VOLTAS",          IRProtocol::VOLTAS},
        {"KELON",           IRProtocol::KELON},
        {"KELON168",        IRProtocol::KELON168},
        {"CORONA_AC",       IRProtocol::CORONA_AC},
        {"DELONGHI_AC",     IRProtocol::DELONGHI_AC},
        {"MIRAGE",          IRProtocol::MIRAGE},
        {"RHOSS",           IRProtocol::RHOSS},
        {"AIRTON",          IRProtocol::AIRTON},
        {"AIRWELL",         IRProtocol::AIRWELL},
        {"TECHNIBEL_AC",    IRProtocol::TECHNIBEL_AC},
        {"TEKNOPOINT",      IRProtocol::TEKNOPOINT},
        {"TRANSCOLD",       IRProtocol::TRANSCOLD},
        {"ECOCLIM",         IRProtocol::ECOCLIM},
        {"BOSCH144",        IRProtocol::BOSCH144},
        {"YORK",            IRProtocol::YORK},
        {"BLUESTARHEAVY",   IRProtocol::BLUESTARHEAVY},
        {"EUROM",           IRProtocol::EUROM},
        {"CLIMABUTLER",     IRProtocol::CLIMABUTLER},
        {"TOTO",            IRProtocol::TOTO},
        {nullptr,           IRProtocol::UNKNOWN}
    };
    for (int i = 0; tbl[i].name; ++i)
        if (strcmp(s, tbl[i].name) == 0) return tbl[i].proto;
    return IRProtocol::UNKNOWN;
}

// ── Protocol-based repeat presets ────────────────────────────
struct RepeatPreset { uint8_t count; uint16_t delayMs; };
inline RepeatPreset defaultRepeatForProtocol(IRProtocol p) {
    switch (p) {
        case IRProtocol::NEC:
        case IRProtocol::NEC_LIKE:    return {1,   0};  // built-in repeat frame
        case IRProtocol::SONY:        return {3,  25};  // SONY needs 3x
        case IRProtocol::SAMSUNG:
        case IRProtocol::SAMSUNG36:
        case IRProtocol::SAMSUNG_AC:  return {1,  40};
        case IRProtocol::LG:
        case IRProtocol::LG_AC:       return {1,  40};
        case IRProtocol::PANASONIC:
        case IRProtocol::PANASONIC_AC:
        case IRProtocol::PANASONIC_AC32: return {1, 50};
        case IRProtocol::DISH:        return {4,  25};  // needs 4x
        case IRProtocol::DENON:       return {2,  65};
        case IRProtocol::COOLIX:
        case IRProtocol::COOLIX48:    return {3,  46};  // needs 3x
        case IRProtocol::MITSUBISHI_AC:
        case IRProtocol::MITSUBISHI136:
        case IRProtocol::MITSUBISHI112:
        case IRProtocol::MITSUBISHI_HEAVY_88:
        case IRProtocol::MITSUBISHI_HEAVY_152: return {1, 40};
        case IRProtocol::DAIKIN:
        case IRProtocol::DAIKIN2:
        case IRProtocol::DAIKIN64:
        case IRProtocol::DAIKIN128:
        case IRProtocol::DAIKIN152:
        case IRProtocol::DAIKIN160:
        case IRProtocol::DAIKIN176:
        case IRProtocol::DAIKIN200:
        case IRProtocol::DAIKIN216:
        case IRProtocol::DAIKIN312:   return {1,  35};
        case IRProtocol::FUJITSU_AC:  return {1,  50};
        case IRProtocol::TOSHIBA_AC:  return {1,  65};
        case IRProtocol::HAIER_AC:
        case IRProtocol::HAIER_AC_YRW02:
        case IRProtocol::HAIER_AC176:
        case IRProtocol::HAIER_AC160: return {1,  40};
        case IRProtocol::HITACHI_AC:
        case IRProtocol::HITACHI_AC1:
        case IRProtocol::HITACHI_AC2:
        case IRProtocol::HITACHI_AC3:
        case IRProtocol::HITACHI_AC264:
        case IRProtocol::HITACHI_AC296:
        case IRProtocol::HITACHI_AC344:
        case IRProtocol::HITACHI_AC424: return {1, 50};
        default:                       return {1, 50};
    }
}

// ── IRButton ─────────────────────────────────────────────────
struct IRButton {
    uint32_t  id;
    String    name;
    IRProtocol protocol;
    uint64_t  code;
    uint16_t  bits;
    uint16_t  freqKHz;
    uint8_t   repeats;
    uint32_t  groupId;
    uint8_t   repeatCount;
    uint16_t  repeatDelay;
    String    icon;       // emoji icon (<=32 bytes, supports 4-8 emoji UTF-8)
    String    color;      // hex color (<=9 chars, e.g. "#6c63ff")
    std::vector<uint16_t> rawData;

    IRButton()
        : id(0), protocol(IRProtocol::UNKNOWN),
          code(0), bits(32),
          freqKHz(IR_DEFAULT_FREQ_KHZ),
          repeats(IR_SEND_REPEATS),
          groupId(0),
          repeatCount(IR_DEFAULT_REPEAT_COUNT),
          repeatDelay(IR_DEFAULT_REPEAT_DELAY),
          icon(""), color("")
    {}

    bool isValid() const {
        if (name.isEmpty() || name.length() > 96) return false;  // 96 bytes supports emoji names
        if (protocol == IRProtocol::UNKNOWN) return false;
        // RAW and complex AC protocols require rawData
        if (protocol == IRProtocol::RAW || !isSimpleProtocol(protocol))
            return rawData.size() >= 4;
        return true;
    }

    void toJson(JsonObject obj) const {
        obj["id"]          = id;
        obj["name"]        = name;
        obj["protocol"]    = protocolName(protocol);
        obj["bits"]        = bits;
        obj["freqKHz"]     = freqKHz;
        obj["repeats"]     = repeats;
        obj["groupId"]     = groupId;
        obj["repeatCount"] = repeatCount;
        obj["repeatDelay"] = repeatDelay;
        obj["icon"]        = icon;
        obj["color"]       = color;
        char hexBuf[20];
        snprintf(hexBuf, sizeof(hexBuf), "0x%llX", (unsigned long long)code);
        obj["code"] = hexBuf;
        if (!rawData.empty()) {
            JsonArray arr = obj["rawData"].to<JsonArray>();
            for (uint16_t v : rawData) arr.add(v);
        }
    }

    bool fromJson(JsonObjectConst obj) {
        id = obj["id"] | (uint32_t)0;
        if (!obj["name"].is<const char*>()) return false;
        name = obj["name"].as<String>(); name.trim();
        if (!obj["protocol"].is<const char*>()) return false;
        protocol = protocolFromString(obj["protocol"].as<const char*>());
        if (protocol == IRProtocol::UNKNOWN) return false;

        bits        = obj["bits"]        | (uint16_t)32;
        freqKHz     = obj["freqKHz"]     | (uint16_t)IR_DEFAULT_FREQ_KHZ;
        repeats     = obj["repeats"]     | (uint8_t)IR_SEND_REPEATS;
        groupId     = obj["groupId"]     | (uint32_t)0;
        repeatCount = obj["repeatCount"] | (uint8_t)IR_DEFAULT_REPEAT_COUNT;
        repeatDelay = obj["repeatDelay"] | (uint16_t)IR_DEFAULT_REPEAT_DELAY;
        icon        = obj["icon"]        | (const char*)"";
        color       = obj["color"]       | (const char*)"";

        // ── Protocol-aware carrier frequency correction ───────────────
        // Auto-heals legacy DB entries with wrong/zero/generic carrier.
        //   SONY      -> 40 kHz  (spec requirement)
        //   RC5 / RC6 -> 36 kHz  (Philips spec requirement)
        //   All others -> 38 kHz (most common standard)
        switch (protocol) {
            case IRProtocol::SONY:
                freqKHz = 40; break;
            case IRProtocol::RC5: case IRProtocol::RC6:
                freqKHz = 36; break;
            default:
                if (freqKHz < 20 || freqKHz > 60) freqKHz = 38;
                break;
        }

        // ── Protocol-aware repeats correction ────────────────────────
        // Raises repeats/repeatCount/repeatDelay to protocol minimums.
        // Fixes legacy buttons saved before correct defaults were set.
        {
            RepeatPreset rp = defaultRepeatForProtocol(protocol);
            if (repeats < 1) repeats = IR_SEND_REPEATS;
            if (repeatCount < 1 || (rp.count > 1 && repeatCount < rp.count))
                repeatCount = (rp.count > 0) ? rp.count : 1;
            if (repeatDelay == 0 && rp.delayMs > 0)
                repeatDelay = rp.delayMs;
        }

        if (repeatCount < 1) repeatCount = 1;
        if (repeatCount > IR_MAX_REPEAT_COUNT) repeatCount = IR_MAX_REPEAT_COUNT;
        if (repeatDelay > IR_MAX_REPEAT_DELAY) repeatDelay = IR_MAX_REPEAT_DELAY;
        if (icon.length()  > 32) icon = "";  // 32 bytes = ~4-8 emoji
        if (color.length() > 9) color = "";

        // code may be stored as hex string ("0x1A2B") or integer
        if (obj["code"].is<const char*>()) {
            code = (uint64_t)strtoull(obj["code"].as<const char*>(), nullptr, 16);
        } else {
            code = obj["code"] | (uint64_t)0;
        }

        rawData.clear();
        if (obj["rawData"].is<JsonArrayConst>()) {
            for (JsonVariantConst v : obj["rawData"].as<JsonArrayConst>()) {
                uint16_t val = v.as<uint16_t>();
                if (val > 0) rawData.push_back(val);
                if (rawData.size() >= MAX_RAW_EDGES) break;
            }
        }
        return true;
    }
};
