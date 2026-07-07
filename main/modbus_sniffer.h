#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

#include "macon_state.h"

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

/// An "unknown" register: an address seen on the wire that the decoder has no
/// metadata for (arctic::register_lookup() returns nullptr). Tracked always-on
/// (independent of recording / snapshot clears) so unexpected registers that
/// appear while the device is left unattended are captured for later review.
struct UnknownReg {
    uint16_t addr;       // Arctic register number
    uint8_t  last_raw;   // most recent raw byte seen
    uint32_t seen;       // total times observed
    uint32_t changes;    // times the value changed
    int64_t  first_ms;   // when first observed
    int64_t  last_ms;    // when last observed
};

/// Copy up to `max` most-recent captured command frames (newest last) into
/// `out`. Returns the number written.
size_t get_recent_commands(CommandRec *out, size_t max);

/// Total fc=0x06 command frames seen since init/clear.
uint32_t get_command_count();

/// Copy the latest raw snapshot of every register seen so far into `out`
/// (ascending address). Returns the number written (<= max).
size_t get_register_snapshot(RegisterSample *out, size_t max);

/// Decode the latest full register image into an arctic::MaconState.
///
/// This is the "bulk-image → library" pattern: the sniffer maintains the raw
/// value of every register seen on the wire (both the telemetry and holding
/// windows) and hands the WHOLE image to the arctic-macon library, which owns
/// the register→field mapping. Callers never cherry-pick individual registers
/// to parse; they read decoded fields off MaconState.
///
/// `out` must be non-null. Returns true if at least one register has been seen
/// since boot/clear (i.e. the decoded state reflects live bus data); false if
/// the snapshot is still empty (all fields left at their zero/Unknown default).
bool get_macon_state(arctic::MaconState *out);

/// Copy the table of unknown registers seen so far into `out` (ascending
/// address). Returns the number written (<= max). This table is always-on and
/// is NOT affected by clear_snapshot().
size_t get_unknown_registers(UnknownReg *out, size_t max);

/// Total number of distinct unknown registers seen since boot/clear.
uint32_t get_unknown_count();

/// Clear the unknown-register table.
void clear_unknown_registers();

/// Clear the raw register snapshot and command ring (for a clean OFF/ON
/// baseline before capturing).
void clear_snapshot();

/// Initialize the UART-based Modbus sniffer.  Starts a FreeRTOS task that
/// continuously reads bytes, detects frame boundaries, parses Modbus RTU
/// frames, and pairs master requests with slave responses.
void init(TransactionCallback cb);

/// Return total frames received since init.
uint32_t get_frame_count();

/// Return genuinely malformed / unpaired frames (parse/checksum failures,
/// insane lengths, orphan responses) since init. This is what used to be
/// mislabeled "crc_errors"; in practice it stays near zero.
uint32_t get_parse_errors();

/// Return the count of benign inter-frame bytes discarded during resync
/// (e.g. half-duplex line-turnaround bytes) since init. NOT errors.
uint32_t get_resync_bytes();

/// Total bytes ever captured into the skipped-byte ring since init/clear.
uint32_t get_skipped_total();

/// Copy up to `max` most-recent skipped (resync-discarded) raw bytes into
/// `vals` and their capture times into `ms_out`, plus the length / direction /
/// function-code of the valid frame consumed immediately before each byte into
/// `prevlen_out` / `prevdir_out` / `prevfc_out`, plus the inter-byte gap (us)
/// to the preceding frame's last byte (`gap_before_out`) and to the next
/// frame's first byte (`gap_after_out`); a gap of -1 means unknown. Any output
/// pointer may be null. Bytes are written oldest-first. Returns the number
/// written.
size_t get_skipped_bytes(uint8_t *vals, int64_t *ms_out,
                         uint16_t *prevlen_out, uint8_t *prevdir_out,
                         uint8_t *prevfc_out,
                         int32_t *gap_before_out, int32_t *gap_after_out,
                         size_t max);

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
