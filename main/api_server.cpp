#include "api_server.h"
#include "recorder.h"
#include "macon_registers.h"
#include "modbus_sniffer.h"
#include "wifi_manager.h"
#include "ota_manager.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <mutex>
#include <algorithm>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "api";

// Embedded gzip'd dashboard — generated at build time
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

namespace api {

// ---------------------------------------------------------------------------
// Transaction log ring buffer (last N transactions for GET /api/log)
// ---------------------------------------------------------------------------

constexpr size_t LOG_CAPACITY = 100;

static std::mutex s_log_mutex;
static std::vector<sniffer::Transaction> s_log;
static size_t s_log_head = 0;

static void log_add(const sniffer::Transaction &txn)
{
    std::lock_guard<std::mutex> lock(s_log_mutex);
    if (s_log.size() < LOG_CAPACITY) {
        s_log.push_back(txn);
    } else {
        s_log[s_log_head] = txn;
    }
    s_log_head = (s_log_head + 1) % LOG_CAPACITY;
}

// ---------------------------------------------------------------------------
// WebSocket client tracking
// ---------------------------------------------------------------------------

static httpd_handle_t s_server = nullptr;
static std::mutex     s_ws_mutex;
static std::vector<int> s_ws_fds;

static void ws_add_client(int fd)
{
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    s_ws_fds.push_back(fd);
    ESP_LOGI(TAG, "WS client connected (fd=%d, total=%u)", fd, (unsigned)s_ws_fds.size());
}

static void ws_remove_client(int fd)
{
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    s_ws_fds.erase(std::remove(s_ws_fds.begin(), s_ws_fds.end(), fd), s_ws_fds.end());
    ESP_LOGI(TAG, "WS client disconnected (fd=%d, total=%u)", fd, (unsigned)s_ws_fds.size());
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

/// Format a transaction as JSON into buf.  Returns length written.
static int txn_to_json(const sniffer::Transaction &txn, char *buf, size_t buf_len)
{
    const char *fc_name = arctic::function_code_name(txn.fc);

    // Format epoch ms manually — %lld is unreliable on Xtensa toolchain
    char ts_buf[24];
    {
        int64_t v = txn.timestamp_ms;
        bool neg = v < 0;
        if (neg) v = -v;
        char *p = ts_buf + sizeof(ts_buf) - 1;
        *p = '\0';
        do { *--p = '0' + (v % 10); v /= 10; } while (v);
        if (neg) *--p = '-';
        memmove(ts_buf, p, ts_buf + sizeof(ts_buf) - p);
    }

    int off = snprintf(buf, buf_len,
        "{\"ts\":%s,\"slave\":%u,\"fc\":%u,\"fc_name\":\"%s\","
        "\"addr\":%u,\"count\":%u,\"error\":%u,\"regs\":[",
        ts_buf, txn.slave_addr, txn.fc, fc_name,
        txn.reg_addr, txn.reg_count, txn.error_code);

    for (uint16_t i = 0; i < txn.reg_count && off < (int)buf_len - 128; ++i) {
        uint16_t addr = txn.reg_addr + i;
        const arctic::RegisterInfo *ri = arctic::register_lookup(addr);
        char val_buf[64];
        arctic::register_format_value(addr, txn.values[i], val_buf, sizeof(val_buf));

        if (i > 0) buf[off++] = ',';
        off += snprintf(buf + off, buf_len - off,
            "{\"addr\":%u,\"raw\":%u,\"name\":\"%s\",\"value\":\"%s\"}",
            addr, txn.values[i],
            ri ? ri->name : "Unknown",
            val_buf);
    }

    off += snprintf(buf + off, buf_len - off, "]}");
    return off;
}

// ---------------------------------------------------------------------------
// CORS helper
// ---------------------------------------------------------------------------

static esp_err_t set_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET / — serve gzip'd dashboard
// ---------------------------------------------------------------------------

static esp_err_t handle_root(httpd_req_t *req)
{
    size_t gz_len = index_html_gz_end - index_html_gz_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)index_html_gz_start, gz_len);
}

// ---------------------------------------------------------------------------
// GET /api/status
// ---------------------------------------------------------------------------

static esp_err_t handle_status(httpd_req_t *req)
{
    set_cors(req);

    unsigned ws_count;
    {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        ws_count = (unsigned)s_ws_fds.size();
    }

    const esp_app_desc_t *app = esp_app_get_description();

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"ip\":\"%s\",\"baud\":%lu,\"frames\":%lu,"
        "\"parse_errors\":%lu,\"resync_bytes\":%lu,\"skipped_total\":%lu,"
        "\"crc_errors\":%lu,"  // deprecated alias for parse_errors (downstream compat)
        "\"transactions\":%lu,\"recording\":%s,"
        "\"rec_entries\":%lu,"
        "\"rec_available\":%s,"
        "\"ws_clients\":%u}",
        app->version,
        wifi::get_ip(),
        (unsigned long)sniffer::get_baud_rate(),
        (unsigned long)sniffer::get_frame_count(),
        (unsigned long)sniffer::get_parse_errors(),
        (unsigned long)sniffer::get_resync_bytes(),
        (unsigned long)sniffer::get_skipped_total(),
        (unsigned long)sniffer::get_parse_errors(),
        (unsigned long)sniffer::get_transaction_count(),
        recorder::is_recording() ? "true" : "false",
        (unsigned long)recorder::get_entry_count(),
        recorder::has_memory_recording() ? "true" : "false",
        ws_count);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

// ---------------------------------------------------------------------------
// GET /api/log — returns last N transactions as JSON array
// ---------------------------------------------------------------------------

static esp_err_t handle_log(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    // Build JSON array
    httpd_resp_sendstr_chunk(req, "[");
    {
        std::lock_guard<std::mutex> lock(s_log_mutex);
        bool first = true;
        size_t n = s_log.size();
        for (size_t i = 0; i < n; ++i) {
            // Read in ring order: oldest first
            size_t idx = (n < LOG_CAPACITY)
                ? i
                : (s_log_head + i) % LOG_CAPACITY;
            char buf[2048];
            txn_to_json(s_log[idx], buf, sizeof(buf));
            if (!first) httpd_resp_sendstr_chunk(req, ",");
            httpd_resp_sendstr_chunk(req, buf);
            first = false;
        }
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---------------------------------------------------------------------------
// GET /api/registers — latest RAW byte of every register seen on the wire.
// Undecoded (no scaling/sign) so OFF vs ON snapshots can be diffed exactly.
// ---------------------------------------------------------------------------

static esp_err_t handle_registers(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    static sniffer::RegisterSample samples[sniffer::MAX_REGS * 4];
    size_t n = sniffer::get_register_snapshot(samples, sizeof(samples) / sizeof(samples[0]));

    httpd_resp_sendstr_chunk(req, "{");
    char buf[48];
    for (size_t i = 0; i < n; ++i) {
        snprintf(buf, sizeof(buf), "%s\"%u\":%u",
                 (i == 0) ? "" : ",", samples[i].addr, samples[i].raw);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "}");
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---------------------------------------------------------------------------
// GET /api/state — decoded heat-pump state (arctic-macon library).
//
// Uses the bulk-image → library pattern: sniffer::get_macon_state() feeds the
// WHOLE captured register image to arctic::decode_state() and returns a decoded
// MaconState. This endpoint only serializes that struct — it does not know or
// care which wire register carries which field (the library owns that).
// ---------------------------------------------------------------------------

static esp_err_t handle_state(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    arctic::MaconState ms;
    const bool seen = sniffer::get_macon_state(&ms);

    // Emit a signed int, or JSON null when the source register was absent.
    auto iv = [](char *b, size_t n, int v, bool valid) {
        if (valid) snprintf(b, n, "%d", v);
        else       snprintf(b, n, "null");
    };

    char tank[8], outlet[8], inlet[8], amb[8], icoil[8], ipm[8];
    char disc[8], suct[8], ocoil[8], setp[8];
    char acv[8], acc[8], dcv[8], eev[8], freq[8];
    iv(tank,   sizeof(tank),   ms.water_tank_c,       ms.water_tank_valid);
    iv(outlet, sizeof(outlet), ms.outlet_c,           ms.outlet_valid);
    iv(inlet,  sizeof(inlet),  ms.inlet_c,            ms.inlet_valid);
    iv(amb,    sizeof(amb),    ms.outdoor_ambient_c,  ms.outdoor_ambient_valid);
    iv(icoil,  sizeof(icoil),  ms.indoor_coil_c,      ms.indoor_coil_valid);
    iv(ipm,    sizeof(ipm),    ms.ipm_c,              ms.ipm_valid);
    iv(disc,   sizeof(disc),   ms.discharge_c,        ms.discharge_valid);
    iv(suct,   sizeof(suct),   ms.suction_c,          ms.suction_valid);
    iv(ocoil,  sizeof(ocoil),  ms.outdoor_coil_c,     ms.outdoor_coil_valid);
    iv(setp,   sizeof(setp),   ms.hot_water_setpoint, ms.hot_water_setpoint_valid);
    iv(acv,    sizeof(acv),    ms.ac_voltage,         ms.ac_voltage_valid);
    iv(acc,    sizeof(acc),    ms.ac_current,         ms.ac_current_valid);
    iv(dcv,    sizeof(dcv),    ms.dc_voltage,         ms.dc_voltage_valid);
    iv(eev,    sizeof(eev),    ms.primary_eev,        ms.primary_eev_valid);
    iv(freq,   sizeof(freq),   ms.compressor_freq,    ms.compressor_freq_valid);

    // Human-readable active-fault list, decoded by the library from the four
    // fault bitfield registers (reg2125-2128). reg2007 bit5 is the run flag,
    // not a fault, so it is excluded here.
    char fault_text[224];
    fault_text[0] = '\0';
    if (ms.faults_valid) {
        const struct { uint16_t addr; uint8_t raw; } fr[] = {
            { arctic::REG_FAULT_SENSOR_EE,   ms.fault_ee   },
            { arctic::REG_FAULT_SENSOR_COMP, ms.fault_comp },
            { arctic::REG_FAULT_ELEC,        ms.fault_elec },
            { arctic::REG_FAULT,             ms.fault_ref  },
        };
        for (const auto &f : fr) {
            if (f.raw == 0) continue;
            char one[64];
            arctic::register_format_value(f.addr, f.raw, one, sizeof(one));
            if (fault_text[0] != '\0') {
                strncat(fault_text, " | ", sizeof(fault_text) - strlen(fault_text) - 1);
            }
            strncat(fault_text, one, sizeof(fault_text) - strlen(fault_text) - 1);
        }
    }
    if (fault_text[0] == '\0') {
        snprintf(fault_text, sizeof(fault_text), "%s", ms.faults_valid ? "OK" : "—");
    }

    // --- performance: thermal output + estimated COP -----------------------
    // Loop thermal output Q = m_dot * cp * dT, using the arctic-logger's tuned
    // constants for this install: 11 GPM design flow with 25% propylene glycol
    // (cp 3900 J/kgK, rho 1.0145 kg/L) => ~2745.8 W per °C of loop dT. dT is
    // signed (outlet - inlet): +ve heating the loop, -ve cooling it. COP is
    // |Q| / real-time electrical power (reg2114), reported only when the
    // compressor is actually drawing (>= 200 W) and there is a non-zero dT.
    static constexpr float LOOP_W_PER_K   = 2745.8f;
    static constexpr float COP_MIN_INPUT_W = 200.0f;

    char rtp[12], thermal[12], cop[12];
    const char *cop_dir = "null";

    if (ms.realtime_power_valid) snprintf(rtp, sizeof(rtp), "%lu", (unsigned long)ms.realtime_power_w);
    else                         snprintf(rtp, sizeof(rtp), "null");

    const bool have_dt = ms.outlet_valid && ms.inlet_valid;
    const int  dt = have_dt ? (ms.outlet_c - ms.inlet_c) : 0;
    long thermal_w = 0;
    if (have_dt) {
        thermal_w = lroundf(LOOP_W_PER_K * (float)dt);
        snprintf(thermal, sizeof(thermal), "%ld", thermal_w);
    } else {
        snprintf(thermal, sizeof(thermal), "null");
    }

    if (have_dt && dt != 0 && ms.realtime_power_valid &&
        (float)ms.realtime_power_w >= COP_MIN_INPUT_W) {
        float c = fabsf((float)thermal_w) / (float)ms.realtime_power_w;
        snprintf(cop, sizeof(cop), "%.2f", c);
        cop_dir = (dt > 0) ? "\"heating\"" : "\"cooling\"";
    } else {
        snprintf(cop, sizeof(cop), "null");
    }

    char buf[1440];
    snprintf(buf, sizeof(buf),
        "{\"seen\":%s,"
        "\"mode\":\"%s\",\"mode_valid\":%s,\"operation\":\"%s\",\"running\":%s,"
        "\"outputs\":{\"compressor\":%s,\"pump\":%s,\"fan\":%s,\"defrost\":%s},"
        "\"fan_level\":%u,"
        "\"setpoint_c\":%s,"
        "\"temperatures\":{\"tank\":%s,\"outlet\":%s,\"inlet\":%s,\"outdoor\":%s,"
        "\"indoor_coil\":%s,\"ipm\":%s,\"discharge\":%s,\"suction\":%s,\"outdoor_coil\":%s},"
        "\"electrical\":{\"ac_voltage\":%s,\"ac_current\":%s,\"dc_voltage\":%s,"
        "\"primary_eev\":%s,\"compressor_freq\":%s,\"realtime_power\":%s},"
        "\"performance\":{\"thermal_w\":%s,\"cop\":%s,\"cop_dir\":%s},"
        "\"faults\":{\"valid\":%s,\"run\":%u,\"ee\":%u,\"comp\":%u,\"elec\":%u,\"ref\":%u,"
        "\"text\":\"%s\"}}",
        seen ? "true" : "false",
        arctic::mode_name(ms.mode), ms.mode_valid ? "true" : "false",
        arctic::operation_name(arctic::decode_operation(ms)),
        ms.running ? "true" : "false",
        ms.compressor_on ? "true" : "false", ms.pump_on ? "true" : "false",
        ms.fan_on ? "true" : "false", ms.defrost_on ? "true" : "false",
        (unsigned)ms.fan_level,
        setp,
        tank, outlet, inlet, amb, icoil, ipm, disc, suct, ocoil,
        acv, acc, dcv, eev, freq, rtp,
        thermal, cop, cop_dir,
        ms.faults_valid ? "true" : "false",
        (unsigned)ms.fault_run, (unsigned)ms.fault_ee, (unsigned)ms.fault_comp,
        (unsigned)ms.fault_elec, (unsigned)ms.fault_ref,
        fault_text);

    return httpd_resp_send(req, buf, strlen(buf));
}

// ---------------------------------------------------------------------------
// GET /api/commands — recent fc=0x06 controller command frames (power/mode/
// setpoint). dir 0xF0 = controller->unit, 0x0F = unit->controller echo.
// ---------------------------------------------------------------------------

static esp_err_t handle_commands(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    static sniffer::CommandRec cmds[sniffer::COMMAND_RING_SZ];
    size_t n = sniffer::get_recent_commands(cmds, sniffer::COMMAND_RING_SZ);

    char head[64];
    snprintf(head, sizeof(head), "{\"total\":%lu,\"commands\":[",
             (unsigned long)sniffer::get_command_count());
    httpd_resp_sendstr_chunk(req, head);

    char buf[192];
    for (size_t i = 0; i < n; ++i) {
        snprintf(buf, sizeof(buf),
                 "%s{\"dir\":\"0x%02X\",\"selector\":\"0x%04X\",\"value\":\"0x%04X\","
                 "\"value_dec\":%u,\"frame\":\"55AA%02X06%04X%04X\"}",
                 (i == 0) ? "" : ",",
                 cmds[i].dir, cmds[i].selector, cmds[i].value, cmds[i].value,
                 cmds[i].dir, cmds[i].selector, cmds[i].value);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---------------------------------------------------------------------------
// POST /api/snapshot/clear — reset the raw register snapshot + command ring
// so the next capture starts from a clean baseline.
// ---------------------------------------------------------------------------

static esp_err_t handle_snapshot_clear(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    sniffer::clear_snapshot();
    return httpd_resp_sendstr(req, "{\"status\":\"cleared\"}");
}

// ---------------------------------------------------------------------------
// GET /api/baud — get current baud rate
// POST /api/baud — set baud rate {"baud": 9600}
// ---------------------------------------------------------------------------

static esp_err_t handle_baud(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"baud\":%lu}",
                 (unsigned long)sniffer::get_baud_rate());
        return httpd_resp_sendstr(req, buf);
    }

    // POST — parse JSON body
    char body[64] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"No body\"}");
    }

    // Simple JSON parse for {"baud": NNNN}
    uint32_t baud = 0;
    const char *p = strstr(body, "\"baud\"");
    if (p) {
        p = strchr(p, ':');
        if (p) baud = (uint32_t)atoi(p + 1);
    }

    if (baud == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"Invalid baud value\"}");
    }

    if (!sniffer::set_baud_rate(baud)) {
        httpd_resp_set_status(req, "400 Bad Request");
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"Unsupported baud rate: %lu\"}",
                 (unsigned long)baud);
        return httpd_resp_sendstr(req, buf);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"baud\":%lu,\"status\":\"ok\"}",
             (unsigned long)baud);
    return httpd_resp_sendstr(req, buf);
}

// ---------------------------------------------------------------------------
// GET /api/config — get all UART config
// POST /api/config — set config {"invert_rx": true, "parity": "even"}
// ---------------------------------------------------------------------------

static esp_err_t handle_config(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        const char *parity_str;
        switch (sniffer::get_parity()) {
            case sniffer::Parity::EVEN: parity_str = "even"; break;
            case sniffer::Parity::ODD:  parity_str = "odd";  break;
            default:                    parity_str = "none"; break;
        }
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"baud\":%lu,\"invert_rx\":%s,\"parity\":\"%s\",\"stop_bits\":%d}",
                 (unsigned long)sniffer::get_baud_rate(),
                 sniffer::get_rx_inverted() ? "true" : "false",
                 parity_str,
                 sniffer::get_stop_bits());
        return httpd_resp_sendstr(req, buf);
    }

    // POST — parse JSON body
    char body[128] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"No body\"}");
    }

    // Parse invert_rx
    const char *inv = strstr(body, "\"invert_rx\"");
    if (inv) {
        bool invert = (strstr(inv, "true") != nullptr);
        sniffer::set_rx_inverted(invert);
    }

    // Parse parity
    const char *par = strstr(body, "\"parity\"");
    if (par) {
        if (strstr(par, "\"even\"")) {
            sniffer::set_parity(sniffer::Parity::EVEN);
        } else if (strstr(par, "\"odd\"")) {
            sniffer::set_parity(sniffer::Parity::ODD);
        } else if (strstr(par, "\"none\"")) {
            sniffer::set_parity(sniffer::Parity::NONE);
        }
    }

    // Parse baud
    const char *bp = strstr(body, "\"baud\"");
    if (bp) {
        bp = strchr(bp, ':');
        if (bp) {
            uint32_t baud = (uint32_t)atoi(bp + 1);
            if (baud > 0) sniffer::set_baud_rate(baud);
        }
    }

    // Parse stop_bits
    const char *sb = strstr(body, "\"stop_bits\"");
    if (sb) {
        sb = strchr(sb, ':');
        if (sb) {
            int bits = atoi(sb + 1);
            if (bits == 1 || bits == 2) sniffer::set_stop_bits(bits);
        }
    }

    // Return current config
    const char *parity_str;
    switch (sniffer::get_parity()) {
        case sniffer::Parity::EVEN: parity_str = "even"; break;
        case sniffer::Parity::ODD:  parity_str = "odd";  break;
        default:                    parity_str = "none"; break;
    }
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"baud\":%lu,\"invert_rx\":%s,\"parity\":\"%s\",\"stop_bits\":%d,\"status\":\"ok\"}",
             (unsigned long)sniffer::get_baud_rate(),
             sniffer::get_rx_inverted() ? "true" : "false",
             parity_str,
             sniffer::get_stop_bits());
    return httpd_resp_sendstr(req, buf);
}

// ---------------------------------------------------------------------------
// POST /api/record/start
// ---------------------------------------------------------------------------

static esp_err_t handle_record_start(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    if (!recorder::has_memory_recording()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
            "{\"error\":\"No PSRAM — in-memory recording unavailable\"}");
    }
    recorder::start();
    return httpd_resp_sendstr(req, "{\"status\":\"recording\"}");
}

// ---------------------------------------------------------------------------
// POST /api/record/stop
// ---------------------------------------------------------------------------

static esp_err_t handle_record_stop(httpd_req_t *req)
{
    set_cors(req);
    recorder::stop();
    httpd_resp_set_type(req, "application/json");
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"stopped\",\"entries\":%lu}",
             (unsigned long)recorder::get_entry_count());
    return httpd_resp_sendstr(req, buf);
}

// ---------------------------------------------------------------------------
// GET /api/record/download
// ---------------------------------------------------------------------------

static esp_err_t handle_record_download(httpd_req_t *req)
{
    set_cors(req);
    size_t len;
    const char *data = recorder::get_data(len);
    httpd_resp_set_type(req, "application/x-ndjson");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"capture.jsonl\"");
    return httpd_resp_send(req, data, len);
}

// ---------------------------------------------------------------------------
// DELETE /api/record
// ---------------------------------------------------------------------------

static esp_err_t handle_record_clear(httpd_req_t *req)
{
    set_cors(req);
    recorder::clear();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"cleared\"}");
}

// ---------------------------------------------------------------------------
// OPTIONS handler (CORS preflight)
// ---------------------------------------------------------------------------

static esp_err_t handle_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

// ---------------------------------------------------------------------------
// GET /api/unknown — registers the decoder doesn't recognize (always-on capture)
// DELETE /api/unknown — clear the unknown-register table
// ---------------------------------------------------------------------------

static esp_err_t handle_unknown(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_DELETE) {
        sniffer::clear_unknown_registers();
        return httpd_resp_sendstr(req, "{\"status\":\"cleared\"}");
    }

    static sniffer::UnknownReg regs[sniffer::MAX_REGS > 200 ? sniffer::MAX_REGS : 200];
    size_t n = sniffer::get_unknown_registers(regs, sizeof(regs) / sizeof(regs[0]));

    char head[64];
    snprintf(head, sizeof(head), "{\"count\":%lu,\"registers\":[",
             (unsigned long)n);
    httpd_resp_sendstr_chunk(req, head);
    for (size_t i = 0; i < n; ++i) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "%s{\"addr\":%u,\"last_value\":%u,\"seen\":%lu,\"changes\":%lu,"
                 "\"first_ms\":%lld,\"last_ms\":%lld}",
                 (i == 0) ? "" : ",",
                 regs[i].addr, regs[i].last_raw,
                 (unsigned long)regs[i].seen, (unsigned long)regs[i].changes,
                 (long long)regs[i].first_ms, (long long)regs[i].last_ms);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /api/skipped — raw bytes discarded during resync (diagnose the stray
// inter-frame bytes). DELETE resets the capture via reset_stats().
// ---------------------------------------------------------------------------

static esp_err_t handle_skipped(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_DELETE) {
        sniffer::reset_stats();
        return httpd_resp_sendstr(req, "{\"status\":\"cleared\"}");
    }

    static uint8_t  vals[256];
    static int64_t  ms[256];
    static uint16_t prevlen[256];
    static uint8_t  prevdir[256];
    static uint8_t  prevfc[256];
    static int32_t  gap_before[256];
    static int32_t  gap_after[256];
    size_t n = sniffer::get_skipped_bytes(vals, ms, prevlen, prevdir, prevfc,
                                          gap_before, gap_after,
                                          sizeof(vals) / sizeof(vals[0]));

    char head[96];
    snprintf(head, sizeof(head), "{\"total\":%lu,\"count\":%lu,\"bytes\":[",
             (unsigned long)sniffer::get_skipped_total(), (unsigned long)n);
    httpd_resp_sendstr_chunk(req, head);
    for (size_t i = 0; i < n; ++i) {
        char buf[224];
        snprintf(buf, sizeof(buf),
                 "%s{\"hex\":\"0x%02X\",\"dec\":%u,\"ms\":%lld,"
                 "\"prev_len\":%u,\"prev_dir\":\"0x%02X\",\"prev_fc\":\"0x%02X\","
                 "\"gap_before_us\":%ld,\"gap_after_us\":%ld}",
                 (i == 0) ? "" : ",", vals[i], vals[i], (long long)ms[i],
                 (unsigned)prevlen[i], prevdir[i], prevfc[i],
                 (long)gap_before[i], (long)gap_after[i]);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

static const char *ota_state_str(ota_state_t s)
{
    switch (s) {
        case OTA_STATE_IDLE:            return "idle";
        case OTA_STATE_CHECKING:        return "checking";
        case OTA_STATE_UPLOADING:       return "uploading";
        case OTA_STATE_DOWNLOADING:     return "downloading";
        case OTA_STATE_VERIFYING:       return "verifying";
        case OTA_STATE_READY_TO_REBOOT: return "ready_to_reboot";
        case OTA_STATE_FAILED:          return "failed";
        default:                        return "unknown";
    }
}

// GET /api/ota/status
static esp_err_t handle_ota_status(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    ota_status_t st = ota_mgr_get_status();
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"progress\":%d,\"bytes_downloaded\":%u,"
        "\"total_bytes\":%u,\"current_version\":\"%s\",\"new_version\":\"%s\","
        "\"error\":\"%s\"}",
        ota_state_str(st.state), st.progress_percent,
        (unsigned)st.bytes_downloaded, (unsigned)st.total_bytes,
        st.current_version, st.new_version, st.error_msg);
    return httpd_resp_send(req, buf, len);
}

// GET /api/ota/releases — check GitHub for the latest release
static esp_err_t handle_ota_releases(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    ota_release_info_t info;
    if (!ota_mgr_check_github_releases(&info)) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, "{\"error\":\"Failed to check GitHub for updates\"}");
    }

    ota_status_t st = ota_mgr_get_status();
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"update_available\":%s,\"current_version\":\"%s\",\"latest_version\":\"%s\","
        "\"published_at\":\"%s\",\"download_ready\":%s}",
        info.update_available ? "true" : "false",
        st.current_version, info.latest_version, info.published_at,
        (info.download_url[0] != '\0') ? "true" : "false");
    return httpd_resp_send(req, buf, len);
}

// POST /api/ota/github — download+flash the latest GitHub release (check first)
static esp_err_t handle_ota_github(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    const ota_release_info_t *info = ota_mgr_get_release_info();
    if (!info->update_available) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"No update available - check for updates first\"}");
    }
    if (!ota_mgr_start_github_update()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"OTA already in progress or no download URL\"}");
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"started\",\"version\":\"%s\"}",
             info->latest_version);
    return httpd_resp_sendstr(req, buf);
}

// POST /api/ota/update — download+flash from an explicit (allowed) URL
static esp_err_t handle_ota_update(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    char body[320] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"No body\"}");
    }

    // Extract "url":"..."
    char url[256] = {0};
    const char *u = strstr(body, "\"url\"");
    if (u) {
        u = strchr(u, ':');
        if (u) u = strchr(u, '"');
        if (u) {
            const char *end = strchr(u + 1, '"');
            if (end && (size_t)(end - u - 1) < sizeof(url)) {
                memcpy(url, u + 1, end - u - 1);
            }
        }
    }
    if (url[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"Missing 'url' field\"}");
    }
    if (!ota_mgr_is_url_allowed(url)) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"error\":\"URL not allowed - must be from the official GitHub repo\"}");
    }
    if (!ota_mgr_start_update(url)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"OTA update already in progress\"}");
    }
    return httpd_resp_sendstr(req, "{\"status\":\"started\"}");
}

// POST /api/ota/upload — receive a raw .bin body and flash it
static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    if (!ota_mgr_try_lock_upload()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"Another OTA update is already in progress\"}");
    }

    ESP_LOGI(TAG, "Receiving firmware upload, content length: %d", req->content_len);

    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (part == nullptr) {
        ota_mgr_unlock_upload();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"No OTA partition available\"}");
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ota_mgr_unlock_upload();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"Failed to begin OTA\"}");
    }

    char *buf = (char *)malloc(4096);
    if (buf == nullptr) {
        esp_ota_abort(ota_handle);
        ota_mgr_unlock_upload();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"Out of memory\"}");
    }

    size_t total = 0;
    int received;
    bool ok = true;
    bool header_ok = false;
    while ((received = httpd_req_recv(req, buf, 4096)) > 0) {
        if (!header_ok) {
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid firmware header: 0x%02X", (uint8_t)buf[0]);
                esp_ota_abort(ota_handle);
                free(buf);
                ota_mgr_unlock_upload();
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_sendstr(req, "{\"error\":\"Invalid firmware (not an ESP32 image)\"}");
            }
            header_ok = true;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }
        total += received;
    }
    free(buf);

    if (!ok || received < 0) {
        esp_ota_abort(ota_handle);
        ota_mgr_unlock_upload();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"Upload failed\"}");
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        ota_mgr_unlock_upload();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"Image validation failed\"}");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ota_mgr_unlock_upload();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"Failed to set boot partition\"}");
    }

    ESP_LOGI(TAG, "Firmware upload complete: %u bytes — rebooting", (unsigned)total);

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"bytes_received\":%u,\"message\":\"Rebooting...\"}",
             (unsigned)total);
    httpd_resp_sendstr(req, resp);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// POST /api/ota/reboot
static esp_err_t handle_ota_reboot(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// WebSocket handler — /ws
// ---------------------------------------------------------------------------

static esp_err_t handle_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // WS handshake complete
        ws_add_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    // Receive WS frame (we don't expect any, but must read to detect close)
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ws_remove_client(httpd_req_to_sockfd(req));
        return ret;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_remove_client(httpd_req_to_sockfd(req));
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Closed socket callback — clean up WS client list
// ---------------------------------------------------------------------------

static void on_close(httpd_handle_t hd, int fd)
{
    ws_remove_client(fd);
    close(fd);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t init()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 28;
    cfg.close_fn = on_close;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register URI handlers
    const httpd_uri_t uris[] = {
        { "/",                   HTTP_GET,    handle_root,            nullptr },
        { "/api/status",         HTTP_GET,    handle_status,          nullptr },
        { "/api/log",            HTTP_GET,    handle_log,             nullptr },
        { "/api/registers",      HTTP_GET,    handle_registers,       nullptr },
        { "/api/state",          HTTP_GET,    handle_state,           nullptr },
        { "/api/commands",       HTTP_GET,    handle_commands,        nullptr },
        { "/api/snapshot/clear", HTTP_POST,   handle_snapshot_clear,  nullptr },
        { "/api/baud",           HTTP_GET,    handle_baud,            nullptr },
        { "/api/baud",           HTTP_POST,   handle_baud,            nullptr },
        { "/api/config",         HTTP_GET,    handle_config,          nullptr },
        { "/api/config",         HTTP_POST,   handle_config,          nullptr },
        { "/api/record/start",   HTTP_POST,   handle_record_start,    nullptr },
        { "/api/record/stop",    HTTP_POST,   handle_record_stop,     nullptr },
        { "/api/record/download",HTTP_GET,    handle_record_download, nullptr },
        { "/api/record",         HTTP_DELETE, handle_record_clear,    nullptr },
        { "/api/unknown",        HTTP_GET,    handle_unknown,         nullptr },
        { "/api/unknown",        HTTP_DELETE, handle_unknown,         nullptr },
        { "/api/skipped",        HTTP_GET,    handle_skipped,         nullptr },
        { "/api/skipped",        HTTP_DELETE, handle_skipped,         nullptr },
        { "/api/ota/status",     HTTP_GET,    handle_ota_status,      nullptr },
        { "/api/ota/releases",   HTTP_GET,    handle_ota_releases,    nullptr },
        { "/api/ota/github",     HTTP_POST,   handle_ota_github,      nullptr },
        { "/api/ota/update",     HTTP_POST,   handle_ota_update,      nullptr },
        { "/api/ota/upload",     HTTP_POST,   handle_ota_upload,      nullptr },
        { "/api/ota/reboot",     HTTP_POST,   handle_ota_reboot,      nullptr },
        { "/api/*",              HTTP_OPTIONS,handle_options,         nullptr },
    };

    for (const auto &u : uris) {
        httpd_register_uri_handler(s_server, &u);
    }

    // WebSocket endpoint
    httpd_uri_t ws_uri = {};
    ws_uri.uri      = "/ws";
    ws_uri.method   = HTTP_GET;
    ws_uri.handler  = handle_ws;
    ws_uri.is_websocket = true;
    httpd_register_uri_handler(s_server, &ws_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
    return ESP_OK;
}

void broadcast_transaction(const sniffer::Transaction &txn)
{
    // Add to log ring
    log_add(txn);

    // Send to WebSocket clients
    char buf[2048];
    int len = txn_to_json(txn, buf, sizeof(buf));
    if (len <= 0) return;

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)buf;
    frame.len     = len;

    std::lock_guard<std::mutex> lock(s_ws_mutex);
    for (int fd : s_ws_fds) {
        esp_err_t ret = httpd_ws_send_frame_async(s_server, fd, &frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WS send failed fd=%d: %s", fd, esp_err_to_name(ret));
        }
    }
}

}  // namespace api
