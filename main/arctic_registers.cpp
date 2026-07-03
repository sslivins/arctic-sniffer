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
// MACON layout — see arctic_registers.h header comment. Every wire register is
// one byte. Temperatures are whole-°C signed bytes (scale 1.0, NOT the legacy
// ÷2). Electrical scales validated against synchronized on-unit A-code reads:
//   AC voltage  raw 23  → 230 V   (x10)
//   DC bus      raw 36  → 360 V   (x10)
//   AC current  raw 12  → 12 A    (x1)
//   power (A9)  raw 28  → 2800 W  (x100)  [power ≈ V_raw*I_raw/10 confirmed x1]
// Only confirmed Macon registers are named; unlisted addresses decode as raw
// "Unknown" bytes rather than guessing at legacy ECO-600 meanings.
static const RegEntry s_registers[] = {
    // "Holding" window (base 2000) — telemetry on the Macon unit
    { 2000, { "AC Current",             "A",     1.0f,   false } },  // A4
    { 2001, { "DC Bus Voltage",         "V",     10.0f,  false } },  // A7
    { 2003, { "DC Motor Speed",         nullptr, 1.0f,   false } },  // A10
    { 2008, { "Water Tank Temp",        "°C",    1.0f,   true  } },  // o1
    { 2012, { "Hot Water Setpoint",     "°C",    1.0f,   false } },

    // "Telemetry" window (base 2100)
    { 2101, { "AC Voltage",             "V",     10.0f,  false } },  // A13
    { 2104, { "Main EEV",               "steps", 1.0f,   false } },  // A5
    { 2113, { "IPM Temp",               "°C",    1.0f,   true  } },  // A8
    { 2114, { "Real-time Power",        "W",     100.0f, false } },  // A9
    { 2128, { "Fault",                  nullptr, 1.0f,   false } },  // bitfield
    { 2129, { "Running Flag",           nullptr, 1.0f,   false } },
    { 2130, { "Status",                 nullptr, 1.0f,   false } },  // bit2=comp,bit3=pump
    { 2132, { "Outlet Water Temp",      "°C",    1.0f,   true  } },  // o3
    { 2133, { "Inlet Water Temp",       "°C",    1.0f,   true  } },  // o2
    { 2134, { "Outdoor Ambient Temp",   "°C",    1.0f,   true  } },  // o4
    { 2135, { "Cool Coil Temp",         "°C",    1.0f,   true  } },  // A6
    { 2136, { "Suction Temp",           "°C",    1.0f,   true  } },  // A3
    { 2137, { "Coil Temp",              "°C",    1.0f,   true  } },  // A2
    { 2138, { "Discharge Temp",         "°C",    1.0f,   true  } },  // A1
    { 2141, { "Compressor Freq",        "Hz",    1.0f,   false } },  // A14
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
// Value formatter
// ---------------------------------------------------------------------------

char *register_format_value(uint16_t address, uint16_t raw, char *buf, size_t buf_len)
{
    // Bitfield registers decode to a list of active flags.
    if (address == REG_FAULT || address == REG_STATUS_BYTE) {
        register_format_bitmap(address, raw, buf, buf_len);
        return buf;
    }

    const RegisterInfo *ri = register_lookup(address);
    if (!ri) {
        snprintf(buf, buf_len, "%u", raw);
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
            snprintf(buf, buf_len, "%.0f%s%s", fv, ri->unit ? " " : "", ri->unit ? ri->unit : "");
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

// Status byte reg2130 (Macon) — active outputs. Confirmed live:
//   bit2 (0x04) = compressor running, bit3 (0x08) = water pump running.
// Other bits observed but not yet decoded.
static const BitDesc s_status_bits[] = {
    { 2, "Compressor" },
    { 3, "WaterPump" },
};

// Fault register reg2128 (Macon) — protection/fault bitfield. Only bit7 has
// been confirmed live (P01 water-flow, captured during a real flow fault).
// The Macon bit ordering does NOT match the legacy Arctic error tables, so
// unconfirmed bits are intentionally left unlabeled (they show as raw hex).
static const BitDesc s_fault_bits[] = {
    { 7, "P01:WaterFlow" },
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
        case REG_STATUS_BYTE:
            return format_bits(s_status_bits, sizeof(s_status_bits)/sizeof(s_status_bits[0]),
                               raw, buf, buf_len);
        case REG_FAULT:
            // Show decoded flags plus the raw byte so undecoded fault bits
            // remain visible for the ongoing fault-bit reverse-engineering.
            if (raw == 0) {
                return snprintf(buf, buf_len, "(none)");
            } else {
                int off = format_bits(s_fault_bits,
                                      sizeof(s_fault_bits)/sizeof(s_fault_bits[0]),
                                      raw, buf, buf_len);
                if ((size_t)off < buf_len - 1) {
                    off += snprintf(buf + off, buf_len - off, " (0x%02X)", raw & 0xFF);
                }
                return off;
            }
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
