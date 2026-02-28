#include "arctic_registers.h"
#include <cstdio>
#include <cstring>

namespace arctic {

// ---------------------------------------------------------------------------
// Static register table
// ---------------------------------------------------------------------------

struct RegEntry {
    uint16_t     addr;
    RegisterInfo info;
};

// clang-format off
static const RegEntry s_registers[] = {
    // Holding registers (R/W) — 2000–2057
    { 2000, { "Unit ON/OFF",            nullptr, 1.0f, false } },
    { 2001, { "Working Mode",           nullptr, 1.0f, false } },
    { 2002, { "Cooling Setpoint",       "°C",    1.0f, false } },
    { 2003, { "Heating Setpoint",       "°C",    1.0f, false } },
    { 2004, { "Hot Water Setpoint",     "°C",    1.0f, false } },
    { 2005, { "Cooling ΔT",            "°C",    1.0f, false } },
    { 2006, { "Heating ΔT",            "°C",    1.0f, false } },
    { 2007, { "Hot Water ΔT",          "°C",    1.0f, false } },
    { 2008, { "Fan Coil Heating ΔT",   "°C",    1.0f, false } },
    { 2009, { "P1: EEV Opening",        "steps", 1.0f, false } },
    { 2013, { "P5: Sterilize Time",     "min",   1.0f, false } },
    { 2021, { "P13: Max Temp Setting",  "°C",    1.0f, false } },
    { 2031, { "P23: Cooling Auto Temp", "°C",    1.0f, false } },
    { 2032, { "P24: Heating Auto Temp", "°C",    1.0f, false } },
    { 2036, { "P28: Mode Switch Delay", "min",   1.0f, false } },
    { 2037, { "P29: Defrost Cycle",     "min",   1.0f, false } },
    { 2038, { "P30: Defrost Enter",     "°C",    1.0f, true  } },
    { 2039, { "P31: Defrost Extend",    "°C",    1.0f, true  } },
    { 2040, { "P32: Defrost Temp Diff", "°C",    1.0f, false } },
    { 2041, { "P33: Defrost Ext Time",  "min",   1.0f, false } },
    { 2042, { "P34: Max Defrost Time",  "min",   1.0f, false } },
    { 2043, { "P35: Defrost Exit Temp", "°C",    1.0f, false } },
    { 2044, { "P36: Water Return Temp", "°C",    1.0f, false } },
    { 2045, { "P37: Water Return Time", "min",   1.0f, false } },
    { 2046, { "P38: Low Ambient Prot",  "°C",    1.0f, true  } },
    { 2047, { "P39: Freq Reduction",    "°C",    1.0f, false } },
    { 2048, { "P40: Cool Low Ambient",  "°C",    1.0f, true  } },
    { 2049, { "P41: EEV Superheat Mode",nullptr, 1.0f, false } },
    { 2050, { "P42: Target Superheat",  "°C",    1.0f, false } },
    { 2051, { "P43: 3-Way Valve Time",  "sec",   1.0f, false } },
    { 2052, { "P44: Pump Target Mode",  nullptr, 1.0f, false } },
    { 2053, { "P45: Pump Interval",     "min",   1.0f, false } },
    { 2054, { "P46: Pump Low Ambient",  "°C",    1.0f, true  } },
    { 2055, { "P47: Waterway Cleaning", nullptr, 1.0f, false } },
    { 2056, { "Freq Control Enable",    nullptr, 1.0f, false } },
    { 2057, { "Freq Control Setting",   "Hz",    1.0f, false } },

    // Input registers (read-only) — 2100–2138
    { 2100, { "Water Tank Temp",        "°C",    1.0f, true  } },
    { 2102, { "Outlet Water Temp",      "°C",    1.0f, true  } },
    { 2103, { "Inlet Water Temp",       "°C",    1.0f, true  } },
    { 2104, { "Discharge Temp",         "°C",    1.0f, true  } },
    { 2105, { "Suction Temp",           "°C",    1.0f, true  } },
    { 2106, { "EVI Suction Temp",       "°C",    1.0f, true  } },
    { 2107, { "Outdoor Coil Temp",      "°C",    1.0f, true  } },
    { 2108, { "Indoor Coil Temp",       "°C",    1.0f, true  } },
    { 2109, { "Indoor Ambient Temp",    "°C",    1.0f, true  } },
    { 2110, { "Outdoor Ambient Temp",   "°C",    1.0f, true  } },
    { 2111, { "HP Saturation Temp",     "°C",    1.0f, true  } },
    { 2112, { "LP Saturation Temp",     "°C",    1.0f, true  } },
    { 2113, { "EVI LP Sat Temp",        "°C",    1.0f, true  } },
    { 2114, { "IPM Temp",               "°C",    1.0f, true  } },
    { 2115, { "Brine Inlet Temp",       "°C",    1.0f, true  } },
    { 2116, { "Brine Outlet Temp",      "°C",    1.0f, true  } },
    { 2118, { "Compressor Freq",        "Hz",    1.0f, false } },
    { 2119, { "Fan Speed",              "RPM",   1.0f, false } },
    { 2120, { "AC Voltage",             "V",     1.0f, false } },
    { 2121, { "AC Current",             "A",     0.1f, false } },
    { 2122, { "DC Voltage",             "V",     0.1f, false } },
    { 2123, { "Comp Phase Current",     "A",     0.1f, false } },
    { 2124, { "Primary EEV",           "steps", 1.0f, false } },
    { 2125, { "Secondary EEV",         "steps", 1.0f, false } },
    { 2126, { "High Pressure",          "MPa",   0.01f,false } },
    { 2127, { "Low Pressure",           "MPa",   0.01f,false } },
    { 2128, { "EE Coding",              nullptr, 1.0f, false } },
    { 2133, { "Status 1",               nullptr, 1.0f, false } },
    { 2134, { "Error Code 1",           nullptr, 1.0f, false } },
    { 2135, { "Status 2",               nullptr, 1.0f, false } },
    { 2136, { "Status 3",               nullptr, 1.0f, false } },
    { 2137, { "Error Code 2",           nullptr, 1.0f, false } },
    { 2138, { "Error Code 3",           nullptr, 1.0f, false } },
};
// clang-format on

static constexpr size_t NUM_REGISTERS = sizeof(s_registers) / sizeof(s_registers[0]);

const RegisterInfo *register_lookup(uint16_t address)
{
    for (size_t i = 0; i < NUM_REGISTERS; ++i) {
        if (s_registers[i].addr == address)
            return &s_registers[i].info;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Working mode enum
// ---------------------------------------------------------------------------

static const char *working_mode_str(uint16_t v)
{
    switch (v) {
        case 0: return "Cooling";
        case 1: return "Floor Heating";
        case 2: return "Fan Coil Heating";
        case 5: return "Hot Water";
        case 6: return "Auto";
        default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Enum helpers for specific registers
// ---------------------------------------------------------------------------

static const char *on_off_str(uint16_t v) { return v ? "ON" : "OFF"; }

static const char *eev_mode_str(uint16_t v)
{
    switch (v) {
        case 0: return "Superheat Adj";
        case 1: return "Fixed-Point Adj";
        default: return nullptr;
    }
}

static const char *pump_mode_str(uint16_t v)
{
    switch (v) {
        case 0: return "Per P45 Interval";
        case 1: return "OFF";
        case 2: return "Always ON";
        default: return nullptr;
    }
}

static const char *waterway_clean_str(uint16_t v)
{
    switch (v) {
        case 0: return "OFF";
        case 1: return "Pump";
        case 2: return "Pump+3WV1";
        case 3: return "Pump+3WV1+3WV2";
        default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Value formatter
// ---------------------------------------------------------------------------

char *register_format_value(uint16_t address, uint16_t raw, char *buf, size_t buf_len)
{
    // Special-case enum registers first
    const char *str = nullptr;
    switch (address) {
        case REG_UNIT_ON_OFF:
        case REG_FREQ_CTRL_ENABLE:
            str = on_off_str(raw);
            break;
        case REG_WORKING_MODE:
            str = working_mode_str(raw);
            break;
        case REG_P41_EEV_SH_MODE:
            str = eev_mode_str(raw);
            break;
        case REG_P44_PUMP_TARGET_MODE:
            str = pump_mode_str(raw);
            break;
        case REG_P47_WATERWAY_CLEAN:
            str = waterway_clean_str(raw);
            break;
        default:
            break;
    }
    if (str) {
        snprintf(buf, buf_len, "%s", str);
        return buf;
    }

    const RegisterInfo *ri = register_lookup(address);
    if (!ri) {
        snprintf(buf, buf_len, "%u", raw);
        return buf;
    }

    // Bitmap registers — just show hex
    if (address >= REG_STATUS_1 && address <= REG_ERROR_CODE_3) {
        snprintf(buf, buf_len, "0x%04X", raw);
        return buf;
    }

    // Scaled / signed values
    if (ri->is_signed) {
        int16_t sv = to_signed(raw);
        if (ri->scale != 1.0f) {
            float fv = sv * ri->scale;
            snprintf(buf, buf_len, "%.1f%s%s", fv, ri->unit ? " " : "", ri->unit ? ri->unit : "");
        } else {
            snprintf(buf, buf_len, "%d%s%s", (int)sv, ri->unit ? " " : "", ri->unit ? ri->unit : "");
        }
    } else {
        if (ri->scale != 1.0f) {
            float fv = raw * ri->scale;
            snprintf(buf, buf_len, "%.2f%s%s", fv, ri->unit ? " " : "", ri->unit ? ri->unit : "");
        } else {
            snprintf(buf, buf_len, "%u%s%s", raw, ri->unit ? " " : "", ri->unit ? ri->unit : "");
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Bitmap decoders
// ---------------------------------------------------------------------------

struct BitDesc {
    uint8_t     bit;
    const char *label;
};

// Status Register 2133 — Frequency Limit
static const BitDesc s_status1_bits[] = {
    { 0, "FreqUpperLimit" },
    { 1, "FreqLowerLimit" },
};

// Error Register 2134 — Error Code 1
static const BitDesc s_error1_bits[] = {
    { 0, "BrineInletSensErr" },
    { 1, "BrineOutletSensErr" },
    { 2, "BrineFlowProtect" },
    { 3, "E20:TankTempSensErr" },
};

// Status Register 2135 — System Status 2
static const BitDesc s_status2_bits[] = {
    { 0,  "UnitON" },
    { 1,  "Compressor" },
    { 2,  "FanHigh" },
    { 3,  "FanMed" },
    { 4,  "FanLow" },
    { 5,  "WaterPump" },
    { 6,  "4WayValve" },
    { 7,  "BackupHeater" },
    { 8,  "WaterFlowSW" },
    { 9,  "HighPressSW" },
    { 10, "LowPressSW" },
    { 11, "EmergencySW" },
    { 12, "ModeSwitch" },
    { 13, "3WayV1" },
    { 14, "3WayV2" },
    { 15, "BrineFlow" },
};

// Status Register 2136 — System Status 3
static const BitDesc s_status3_bits[] = {
    { 0,  "SolenoidValve" },
    { 1,  "UnloadingValve" },
    { 2,  "OilReturnValve" },
    { 3,  "BrineWaterPump" },
    { 4,  "BrineAntifreeze" },
    { 5,  "Defrosting" },
    { 6,  "RefrigRecovery" },
    { 7,  "OilReturn" },
    { 8,  "WiredCtrlConn" },
    { 9,  "EnergySaving" },
    { 10, "Antifreeze1" },
    { 11, "Antifreeze2" },
    { 12, "Sterilization" },
    { 13, "SecondaryPump" },
    { 14, "RemoteOnOff" },
};

// Error Register 2137 — Error Code 2
static const BitDesc s_error2_bits[] = {
    { 0,  "E27:IndoorEE" },
    { 1,  "E28:OutdoorEE" },
    { 2,  "E19:InletTempSens" },
    { 3,  "E18:OutletTempSens" },
    { 4,  "E13:IndoorCoilSens" },
    { 5,  "E05:OutdoorCoilSens" },
    { 6,  "E01:DischargeSens" },
    { 7,  "E09:SuctionSens" },
    { 8,  "E22:OutdoorTempSens" },
    { 9,  "E10:DriveCommErr" },
    { 10, "E21:WiredCtrlComm" },
    { 11, "r02:CompStartFault" },
    { 12, "E12:CompDrive" },
    { 13, "r01:IPMFault" },
    { 14, "PA:TankTempProtect" },
    { 15, "r10:ACVoltageProt" },
};

// Error Register 2138 — Error Code 3
static const BitDesc s_error3_bits[] = {
    { 0,  "P19:ACCurrentProt" },
    { 1,  "r06:CompCurrentProt" },
    { 2,  "FA:FanMotor" },
    { 3,  "r11:BusVoltageProt" },
    { 4,  "r05:IPMHighTemp" },
    { 5,  "P11:HighDischarge" },
    { 6,  "P02:HighPressure" },
    { 7,  "P06:LowPressure" },
    { 8,  "P01:WaterFlow" },
    { 9,  "P27:CoolHighCoil" },
    { 10, "E26:LowAmbientTemp" },
    { 11, "EC:EEVLowPress" },
    { 12, "ED:EVILowPress" },
    { 13, "P15:WaterTempDiff" },
    { 14, "P16:LowOutletTemp" },
    { 15, "r20:CompPressDiff" },
};

static int format_bits(const BitDesc *descs, size_t n, uint16_t raw, char *buf, size_t buf_len)
{
    int off = 0;
    bool first = true;
    for (size_t i = 0; i < n && (size_t)off < buf_len - 1; ++i) {
        if (raw & (1u << descs[i].bit)) {
            int w = snprintf(buf + off, buf_len - off, "%s%s", first ? "" : " | ", descs[i].label);
            if (w > 0) off += w;
            first = false;
        }
    }
    if (first) {
        off = snprintf(buf, buf_len, "(none)");
    }
    return off;
}

int register_format_bitmap(uint16_t address, uint16_t raw, char *buf, size_t buf_len)
{
    switch (address) {
        case REG_STATUS_1:
            return format_bits(s_status1_bits, sizeof(s_status1_bits)/sizeof(s_status1_bits[0]),
                               raw, buf, buf_len);
        case REG_ERROR_CODE_1:
            return format_bits(s_error1_bits, sizeof(s_error1_bits)/sizeof(s_error1_bits[0]),
                               raw, buf, buf_len);
        case REG_STATUS_2:
            return format_bits(s_status2_bits, sizeof(s_status2_bits)/sizeof(s_status2_bits[0]),
                               raw, buf, buf_len);
        case REG_STATUS_3:
            return format_bits(s_status3_bits, sizeof(s_status3_bits)/sizeof(s_status3_bits[0]),
                               raw, buf, buf_len);
        case REG_ERROR_CODE_2:
            return format_bits(s_error2_bits, sizeof(s_error2_bits)/sizeof(s_error2_bits[0]),
                               raw, buf, buf_len);
        case REG_ERROR_CODE_3:
            return format_bits(s_error3_bits, sizeof(s_error3_bits)/sizeof(s_error3_bits[0]),
                               raw, buf, buf_len);
        default:
            return snprintf(buf, buf_len, "0x%04X", raw);
    }
}

// ---------------------------------------------------------------------------
// Function code names
// ---------------------------------------------------------------------------

const char *function_code_name(uint8_t fc)
{
    switch (fc) {
        case 0x03: return "Read Holding";
        case 0x06: return "Write Single";
        case 0x10: return "Write Multiple";
        case 0x83: return "Read Holding (err)";
        case 0x86: return "Write Single (err)";
        case 0x90: return "Write Multiple (err)";
        default:   return "Unknown FC";
    }
}

}  // namespace arctic
