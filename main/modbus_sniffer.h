#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

namespace sniffer {

// ---------------------------------------------------------------------------
// Parsed Modbus transaction (request + response paired)
// ---------------------------------------------------------------------------

/// Maximum register count we can hold in one transaction
constexpr size_t MAX_REGS = 64;

struct Transaction {
    int64_t  timestamp_ms;              // epoch ms (NTP) or uptime ms (fallback)
    uint8_t  slave_addr;
    uint8_t  fc;                        // function code
    uint16_t reg_addr;                  // starting register address
    uint16_t reg_count;                 // number of registers
    uint16_t values[MAX_REGS];          // register values (from response)
    bool     has_response;              // response was captured
    uint8_t  error_code;               // Modbus exception code (0 = none)
};

/// Callback type for completed transactions
using TransactionCallback = std::function<void(const Transaction &txn)>;

// ---------------------------------------------------------------------------
// Reverse-engineering aids: raw register snapshot + fc=0x06 command capture
// ---------------------------------------------------------------------------

/// A captured controller command frame (fc=0x06). On the real bus the
/// controller sends dir=0xF0 (controller -> unit); the unit may echo dir=0x0F.
struct CommandRec {
    int64_t  timestamp_ms;   // when captured
    uint8_t  dir;            // 0xF0 controller->unit, 0x0F unit->controller
    uint16_t selector;       // wire field_a (command selector, e.g. 0xFFFF)
    uint16_t value;          // wire field_b (command value, e.g. 0x0001 = ON)
};

constexpr size_t COMMAND_RING_SZ = 32;

/// One register's latest raw (undecoded) byte as seen on the wire.
struct RegisterSample {
    uint16_t addr;   // Arctic register number (2000.. or 2100..)
    uint8_t  raw;    // raw byte value on the wire
};

/// Copy up to `max` most-recent captured command frames (newest last) into
/// `out`. Returns the number written.
size_t get_recent_commands(CommandRec *out, size_t max);

/// Total fc=0x06 command frames seen since init/clear.
uint32_t get_command_count();

/// Copy the latest raw snapshot of every register seen so far into `out`
/// (ascending address). Returns the number written (<= max).
size_t get_register_snapshot(RegisterSample *out, size_t max);

/// Clear the raw register snapshot and command ring (for a clean OFF/ON
/// baseline before capturing).
void clear_snapshot();

/// Initialize the UART-based Modbus sniffer.  Starts a FreeRTOS task that
/// continuously reads bytes, detects frame boundaries, parses Modbus RTU
/// frames, and pairs master requests with slave responses.
void init(TransactionCallback cb);

/// Return total frames received since init.
uint32_t get_frame_count();

/// Return total CRC errors detected since init.
uint32_t get_crc_errors();

/// Return total transactions (paired req+resp) since init.
uint32_t get_transaction_count();

/// Get current baud rate.
uint32_t get_baud_rate();

/// Change baud rate at runtime. Returns true on success.
bool set_baud_rate(uint32_t baud);

/// Reset error/frame counters.
void reset_stats();

/// Get RX signal inversion state.
bool get_rx_inverted();

/// Set RX signal inversion (for swapped A/B wires). Returns true on success.
bool set_rx_inverted(bool inverted);

/// Parity options
enum class Parity { NONE, EVEN, ODD };

/// Get current parity setting.
Parity get_parity();

/// Set parity. Returns true on success.
bool set_parity(Parity p);

/// Get current stop bits (1 or 2).
int get_stop_bits();

/// Set stop bits (1 or 2). Returns true on success.
bool set_stop_bits(int bits);

}  // namespace sniffer
