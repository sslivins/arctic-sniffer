#include "api_server.h"
#include "recorder.h"
#include "arctic_registers.h"
#include "wifi_manager.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <algorithm>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"

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
        "{\"version\":\"%s\",\"ip\":\"%s\",\"frames\":%lu,\"crc_errors\":%lu,"
        "\"transactions\":%lu,\"recording\":%s,"
        "\"rec_entries\":%lu,"
        "\"ws_clients\":%u}",
        app->version,
        wifi::get_ip(),
        (unsigned long)sniffer::get_frame_count(),
        (unsigned long)sniffer::get_crc_errors(),
        (unsigned long)sniffer::get_transaction_count(),
        recorder::is_recording() ? "true" : "false",
        (unsigned long)recorder::get_entry_count(),
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
// POST /api/record/start
// ---------------------------------------------------------------------------

static esp_err_t handle_record_start(httpd_req_t *req)
{
    set_cors(req);
    recorder::start();
    httpd_resp_set_type(req, "application/json");
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
    cfg.max_uri_handlers = 16;
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
        { "/api/record/start",   HTTP_POST,   handle_record_start,    nullptr },
        { "/api/record/stop",    HTTP_POST,   handle_record_stop,     nullptr },
        { "/api/record/download",HTTP_GET,    handle_record_download, nullptr },
        { "/api/record",         HTTP_DELETE, handle_record_clear,    nullptr },
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
