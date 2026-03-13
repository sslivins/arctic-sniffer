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

}  // namespace sniffer
