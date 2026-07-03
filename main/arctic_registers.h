#pragma once

#include <cstdint>
#include <cstddef>

namespace arctic {

// ---------------------------------------------------------------------------
// Register address constants  (protocol native numbering)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// MACON layout (reverse-engineered from live captures on the real heat pump;
// source of truth: arctic-controller applyMaconMapping()). This is NOT the
// legacy Arctic/ECO-600 numbering — the Macon device reuses the same wire
// register numbers for entirely different fields. Only registers confirmed
// against synchronized on-unit menu (o/A code) reads are named; everything
// else decodes as a raw byte ("Unknown").
//
// Each wire register is ONE signed/unsigned byte (see decode_byte()).
// ---------------------------------------------------------------------------

// "Holding" window (wire addr=50, base 2000). Despite the legacy "holding"
// name, on the Macon unit this block carries telemetry, not control regs.
constexpr uint16_t REG_AC_CURRENT           = 2000;  // A4  AC input current
constexpr uint16_t REG_DC_BUS_VOLTAGE       = 2001;  // A7  DC bus voltage (x10 = V)
constexpr uint16_t REG_DC_MOTOR_SPEED       = 2003;  // A10 DC (fan) motor speed
constexpr uint16_t REG_WATER_TANK_TEMP      = 2008;  // o1  water tank temp
constexpr uint16_t REG_HOT_WATER_SETPOINT   = 2012;  // hot water setpoint

constexpr uint16_t HOLDING_START = 2000;
constexpr uint16_t HOLDING_COUNT = 58;   // 2000–2057

// "Telemetry" window (wire addr=0, base 2100 after a 7-byte prefix).
constexpr uint16_t REG_AC_VOLTAGE           = 2101;  // A13 AC input voltage (x10 = V)
constexpr uint16_t REG_MAIN_EEV             = 2104;  // A5  main elec. expansion valve
constexpr uint16_t REG_IPM_TEMP             = 2113;  // A8  IPM module temp
constexpr uint16_t REG_REALTIME_POWER       = 2114;  // A9  real-time power (x100 = W)
constexpr uint16_t REG_FAULT                = 2128;  // fault/protection bitfield
constexpr uint16_t REG_RUNNING_FLAG         = 2129;  // running flag (tentative)
constexpr uint16_t REG_STATUS_BYTE          = 2130;  // status: bit2=comp, bit3=pump
constexpr uint16_t REG_OUTLET_WATER_TEMP    = 2132;  // o3  outlet (supply) water temp
constexpr uint16_t REG_INLET_WATER_TEMP     = 2133;  // o2  inlet (return) water temp
constexpr uint16_t REG_OUTDOOR_AMBIENT_TEMP = 2134;  // o4  ambient temp
constexpr uint16_t REG_COOL_COIL_TEMP       = 2135;  // A6  cool coil temp
constexpr uint16_t REG_SUCTION_TEMP         = 2136;  // A3  suction temp
constexpr uint16_t REG_COIL_TEMP            = 2137;  // A2  coil temp
constexpr uint16_t REG_DISCHARGE_TEMP       = 2138;  // A1  discharge temp
constexpr uint16_t REG_COMPRESSOR_FREQ      = 2141;  // A14 compressor frequency

constexpr uint16_t INPUT_START = 2100;
constexpr uint16_t INPUT_COUNT = 43;     // 2100–2142 (telemetry window reaches 2142)

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
