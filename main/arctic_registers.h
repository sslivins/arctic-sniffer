#pragma once

#include <cstdint>
#include <cstddef>

namespace arctic {

// ---------------------------------------------------------------------------
// Register address constants  (protocol native numbering)
// ---------------------------------------------------------------------------

// Holding registers (R/W) — controller → heat pump
constexpr uint16_t REG_UNIT_ON_OFF          = 2000;
constexpr uint16_t REG_WORKING_MODE         = 2001;
constexpr uint16_t REG_COOLING_SETPOINT     = 2002;
constexpr uint16_t REG_HEATING_SETPOINT     = 2003;
constexpr uint16_t REG_HOT_WATER_SETPOINT   = 2004;
constexpr uint16_t REG_COOLING_DELTA_T      = 2005;
constexpr uint16_t REG_HEATING_DELTA_T      = 2006;
constexpr uint16_t REG_HOT_WATER_DELTA_T    = 2007;
constexpr uint16_t REG_FAN_COIL_HEATING_DT  = 2008;
constexpr uint16_t REG_P1_EEV_OPENING       = 2009;
constexpr uint16_t REG_P5_STERILIZE_TIME    = 2013;
constexpr uint16_t REG_P13_MAX_TEMP         = 2021;
constexpr uint16_t REG_P23_COOL_AUTO_TEMP   = 2031;
constexpr uint16_t REG_P24_HEAT_AUTO_TEMP   = 2032;
constexpr uint16_t REG_P28_MODE_SW_DELAY    = 2036;
constexpr uint16_t REG_P29_DEFROST_CYCLE    = 2037;
constexpr uint16_t REG_P30_DEFROST_ENTER    = 2038;
constexpr uint16_t REG_P31_DEFROST_EXTEND   = 2039;
constexpr uint16_t REG_P32_DEFROST_DIFF     = 2040;
constexpr uint16_t REG_P33_DEFROST_EXT_TIME = 2041;
constexpr uint16_t REG_P34_MAX_DEFROST_TIME = 2042;
constexpr uint16_t REG_P35_DEFROST_EXIT     = 2043;
constexpr uint16_t REG_P36_WATER_RETURN_T   = 2044;
constexpr uint16_t REG_P37_WATER_RETURN_TM  = 2045;
constexpr uint16_t REG_P38_LOW_AMB_PROTECT  = 2046;
constexpr uint16_t REG_P39_FREQ_REDUCTION   = 2047;
constexpr uint16_t REG_P40_COOL_LOW_AMB     = 2048;
constexpr uint16_t REG_P41_EEV_SH_MODE      = 2049;
constexpr uint16_t REG_P42_TARGET_SUPERHEAT = 2050;
constexpr uint16_t REG_P43_3WAY_VALVE_TIME  = 2051;
constexpr uint16_t REG_P44_PUMP_TARGET_MODE = 2052;
constexpr uint16_t REG_P45_PUMP_INTERVAL    = 2053;
constexpr uint16_t REG_P46_PUMP_LOW_AMB     = 2054;
constexpr uint16_t REG_P47_WATERWAY_CLEAN   = 2055;
constexpr uint16_t REG_FREQ_CTRL_ENABLE     = 2056;
constexpr uint16_t REG_FREQ_CTRL_SETTING    = 2057;

constexpr uint16_t HOLDING_START = 2000;
constexpr uint16_t HOLDING_COUNT = 58;   // 2000–2057

// Input registers (read-only) — heat pump → controller
constexpr uint16_t REG_WATER_TANK_TEMP      = 2100;
constexpr uint16_t REG_OUTLET_WATER_TEMP    = 2102;
constexpr uint16_t REG_INLET_WATER_TEMP     = 2103;
constexpr uint16_t REG_DISCHARGE_TEMP       = 2104;
constexpr uint16_t REG_SUCTION_TEMP         = 2105;
constexpr uint16_t REG_EVI_SUCTION_TEMP     = 2106;
constexpr uint16_t REG_OUTDOOR_COIL_TEMP    = 2107;
constexpr uint16_t REG_INDOOR_COIL_TEMP     = 2108;
constexpr uint16_t REG_INDOOR_AMBIENT_TEMP  = 2109;
constexpr uint16_t REG_OUTDOOR_AMBIENT_TEMP = 2110;
constexpr uint16_t REG_HP_SAT_TEMP          = 2111;
constexpr uint16_t REG_LP_SAT_TEMP          = 2112;
constexpr uint16_t REG_EVI_LP_SAT_TEMP      = 2113;
constexpr uint16_t REG_IPM_TEMP             = 2114;
constexpr uint16_t REG_BRINE_INLET_TEMP     = 2115;
constexpr uint16_t REG_BRINE_OUTLET_TEMP    = 2116;

constexpr uint16_t REG_COMPRESSOR_FREQ      = 2118;
constexpr uint16_t REG_FAN_SPEED            = 2119;
constexpr uint16_t REG_AC_VOLTAGE           = 2120;
constexpr uint16_t REG_AC_CURRENT           = 2121;
constexpr uint16_t REG_DC_VOLTAGE           = 2122;
constexpr uint16_t REG_COMP_PHASE_CURRENT   = 2123;
constexpr uint16_t REG_PRIMARY_EEV          = 2124;
constexpr uint16_t REG_SECONDARY_EEV        = 2125;
constexpr uint16_t REG_HIGH_PRESSURE        = 2126;
constexpr uint16_t REG_LOW_PRESSURE         = 2127;
constexpr uint16_t REG_EE_CODING            = 2128;

constexpr uint16_t REG_STATUS_1             = 2133;
constexpr uint16_t REG_ERROR_CODE_1         = 2134;
constexpr uint16_t REG_STATUS_2             = 2135;
constexpr uint16_t REG_STATUS_3             = 2136;
constexpr uint16_t REG_ERROR_CODE_2         = 2137;
constexpr uint16_t REG_ERROR_CODE_3         = 2138;

constexpr uint16_t INPUT_START = 2100;
constexpr uint16_t INPUT_COUNT = 39;     // 2100–2138

// ---------------------------------------------------------------------------
// Register metadata
// ---------------------------------------------------------------------------

struct RegisterInfo {
    const char *name;        // Short name (e.g. "Outlet Water Temp")
    const char *unit;        // Unit string ("°C", "Hz", …) or nullptr
    float       scale;       // Multiply raw by this for display (1.0 = none)
    bool        is_signed;   // Interpret 16-bit raw as signed (two's complement)
};

/// Look up register metadata.  Returns nullptr for unknown addresses.
const RegisterInfo *register_lookup(uint16_t address);

/// Decode raw uint16 to a human-readable string (e.g. "25 °C", "ON", "Cooling").
/// buf must be at least 64 bytes.  Returns buf.
char *register_format_value(uint16_t address, uint16_t raw, char *buf, size_t buf_len);

/// Decode a status/error bitmap register into a readable description.
/// buf must be at least 256 bytes.  Returns number of chars written.
int register_format_bitmap(uint16_t address, uint16_t raw, char *buf, size_t buf_len);

/// Return a short function-code name ("Read Holding", "Write Single", etc.).
const char *function_code_name(uint8_t fc);

/// Format a signed temperature value (two's complement uint16).
inline int16_t to_signed(uint16_t raw) { return static_cast<int16_t>(raw); }

}  // namespace arctic
