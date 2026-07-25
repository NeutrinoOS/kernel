#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bearssl.h>
#include <neutrino.h>

#include "../crt/syscall.hpp"
#include "../auth/secure_random.hpp"
#include "../helpers/http.hpp"
#include "../net/dns.hpp"
#include "../net/tcpd_protocol.hpp"

namespace {

constexpr size_t kMaxUrl = 512;
constexpr size_t kIoBufferSize = 32768;
constexpr size_t kProgressBarWidth = 20;
constexpr uint32_t kConnectWaitLimit = 200000;
constexpr uint32_t kReadWaitLimit = 200000;
constexpr uint64_t kConnectTimeoutNs = 30000000000ull;
constexpr uint64_t kReadTimeoutNs = 30000000000ull;
constexpr uint32_t kMaxRedirects = 5;
constexpr uint32_t kNetDebugDeviceIndex = 0;

struct TcpConnection {
    uint32_t server_pipe = 0;
    uint32_t reply_pipe = 0;
    uint32_t endpoint = 0;
    uint32_t connection_id = 0;
    bool closed = false;
    uint8_t pending[tcpd_protocol::kMaxPayload];
    size_t pending_offset = 0;
    size_t pending_length = 0;
};

struct Buffer {
    uint8_t* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
};

struct TrustStore {
    bool attempted = false;
    bool loaded = false;
    br_x509_trust_anchor* anchors = nullptr;
    size_t anchor_count = 0;
    size_t anchor_capacity = 0;
    uint8_t* data = nullptr;
    size_t data_size = 0;
    size_t data_capacity = 0;
};

struct Stream {
    TcpConnection tcp;
    bool tls = false;
    br_ssl_client_context ssl_client;
    br_sslio_context ssl_io;
    uint8_t ssl_iobuf[BR_SSL_BUFSIZE_BIDI];
    br_x509_minimal_context x509_minimal;
};

struct ProgressState {
    bool known_total = false;
    size_t total = 0;
    uint64_t last_units = static_cast<uint64_t>(-1);
    bool net_debug = false;
    bool quiet = false;
    uint32_t net_debug_handle = kInvalidDescriptor;
};

struct DownloadOptions {
    bool have_limit = false;
    size_t limit = 0;
    bool net_debug = false;
    bool quiet = false;
    bool compact = false;
    bool discard = false;
    bool require_valid_cert = false;
};

enum class BodyResult : uint8_t {
    Complete,
    Limited,
    Error,
};

long g_console = -1;
TrustStore* g_trust_store = reinterpret_cast<TrustStore*>(1);
uint8_t g_file_read_buffer[8192];
uint8_t g_body_buffer[kIoBufferSize];

using HttpResponseMeta = userspace::http::ResponseMeta;
using Scheme = userspace::http::Scheme;
using Url = userspace::http::Url;

void print_raw(const void* data, size_t length) {
    if (g_console >= 0 && data != nullptr && length != 0) {
        descriptor_write(static_cast<uint32_t>(g_console), data, length);
    }
}

void print(const char* text) {
    if (text != nullptr) {
        print_raw(text, strlen(text));
    }
}

void print_line(const char* text) {
    print(text);
    print("\n");
}

void print_u64(uint64_t value) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) {
        char ch = digits[--count];
        print_raw(&ch, 1);
    }
}

uint64_t wall_time_ns() {
    NeutrinoWallTime now{};
    if (!neutrino_get_time(&now)) {
        return 0;
    }
    return now.unix_seconds * 1000000000ull + now.nanoseconds;
}

bool timed_out(uint64_t start_ns,
               uint64_t timeout_ns,
               uint32_t waits,
               uint32_t wait_limit) {
    if (waits >= wait_limit) {
        return true;
    }
    uint64_t now = wall_time_ns();
    if (start_ns != 0 && now != 0 && now >= start_ns) {
        return now - start_ns >= timeout_ns;
    }
    return false;
}

void print_net_debug(ProgressState& progress) {
    if (!progress.net_debug || progress.net_debug_handle == kInvalidDescriptor) {
        return;
    }
    descriptor_defs::NetDeviceDebug debug{};
    if (descriptor_get_property(
            progress.net_debug_handle,
            static_cast<uint32_t>(descriptor_defs::Property::NetDeviceDebug),
            &debug,
            sizeof(debug)) != 0) {
        return;
    }
    print(" rxq=");
    print_u64(debug.rx_queued);
    print(" drop=");
    print_u64(debug.rx_frames_dropped);
}

void close_progress_debug(ProgressState& progress) {
    if (progress.net_debug_handle != kInvalidDescriptor) {
        descriptor_close(progress.net_debug_handle);
        progress.net_debug_handle = kInvalidDescriptor;
    }
}

uint64_t progress_units(const ProgressState& progress, size_t written) {
    if (progress.known_total) {
        if (progress.total == 0) {
            return 100;
        }
        return (static_cast<uint64_t>(written) * 100u) /
               static_cast<uint64_t>(progress.total);
    }
    return static_cast<uint64_t>(written / 4096u);
}

void render_progress(ProgressState& progress, size_t written, bool force) {
    if (progress.quiet) {
        return;
    }
    uint64_t units = progress_units(progress, written);
    if (!force && units == progress.last_units) {
        return;
    }
    progress.last_units = units;

    print("\rdownload: ");
    if (progress.known_total) {
        size_t filled = progress.total == 0
                            ? kProgressBarWidth
                            : (written * kProgressBarWidth) / progress.total;
        if (filled > kProgressBarWidth) {
            filled = kProgressBarWidth;
        }
        uint64_t percent = progress.total == 0
                               ? 100
                               : (static_cast<uint64_t>(written) * 100u) /
                                     static_cast<uint64_t>(progress.total);
        if (percent > 100) {
            percent = 100;
        }
        print("[");
        for (size_t i = 0; i < kProgressBarWidth; ++i) {
            print(i < filled ? "#" : " ");
        }
        print("] ");
        print_u64(percent);
        print("% ");
        print_u64(written);
        print("/");
        print_u64(progress.total);
        print("B");
        print_net_debug(progress);
        print("        ");
        return;
    }

    print_u64(written);
    print("B");
    print_net_debug(progress);
    print("        ");
}

void finish_progress(ProgressState& progress, size_t written) {
    render_progress(progress, written, true);
    if (!progress.quiet) {
        print("\n");
    }
    close_progress_debug(progress);
}

BodyResult fail_progress(ProgressState& progress) {
    if (!progress.quiet && progress.last_units != static_cast<uint64_t>(-1)) {
        print("\n");
    }
    close_progress_debug(progress);
    return BodyResult::Error;
}

bool buffer_reserve(Buffer& buffer, size_t required) {
    if (required <= buffer.capacity) {
        return true;
    }
    size_t new_capacity = buffer.capacity == 0 ? 1024 : buffer.capacity;
    while (new_capacity < required) {
        if (new_capacity > static_cast<size_t>(-1) / 2) {
            return false;
        }
        new_capacity *= 2;
    }
    auto* new_data = static_cast<uint8_t*>(map_anonymous(new_capacity, MAP_WRITE));
    if (new_data == nullptr) {
        return false;
    }
    if (buffer.data != nullptr && buffer.size != 0) {
        memcpy(new_data, buffer.data, buffer.size);
    }
    if (buffer.data != nullptr) {
        unmap(buffer.data, buffer.capacity);
    }
    buffer.data = new_data;
    buffer.capacity = new_capacity;
    return true;
}

bool buffer_append(Buffer& buffer, const void* data, size_t length) {
    if (length == 0) {
        return true;
    }
    if (!buffer_reserve(buffer, buffer.size + length)) {
        return false;
    }
    memcpy(buffer.data + buffer.size, data, length);
    buffer.size += length;
    return true;
}

void buffer_clear(Buffer& buffer) {
    buffer.size = 0;
}

void buffer_release(Buffer& buffer) {
    if (buffer.data != nullptr) {
        unmap(buffer.data, buffer.capacity);
    }
    buffer = {};
}

const char* skip_spaces(const char* text) {
    while (text != nullptr && (*text == ' ' || *text == '\t')) {
        ++text;
    }
    return text;
}

bool copy_arg(const char*& cursor, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    cursor = skip_spaces(cursor);
    if (cursor == nullptr || *cursor == '\0') {
        return false;
    }

    size_t length = 0;
    while (cursor[length] != '\0' &&
           cursor[length] != ' ' &&
           cursor[length] != '\t') {
        if (length + 1 >= out_size) {
            return false;
        }
        out[length] = cursor[length];
        ++length;
    }
    out[length] = '\0';
    cursor += length;
    return true;
}

bool equals(const char* a, const char* b) {
    return a != nullptr && b != nullptr && strcmp(a, b) == 0;
}

bool has_prefix(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

bool parse_size_arg(const char* text, size_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    size_t value = 0;
    const char* cursor = text;
    if (!userspace::http::is_digit(*cursor)) {
        return false;
    }
    while (userspace::http::is_digit(*cursor)) {
        value = value * 10u + static_cast<size_t>(*cursor - '0');
        ++cursor;
    }

    size_t multiplier = 1;
    if (*cursor != '\0') {
        char suffix = userspace::http::to_lower(*cursor++);
        if (suffix == 'k') {
            multiplier = 1024u;
        } else if (suffix == 'm') {
            multiplier = 1024u * 1024u;
        } else if (suffix == 'g') {
            multiplier = 1024u * 1024u * 1024u;
        } else {
            return false;
        }
        if (userspace::http::to_lower(*cursor) == 'b') {
            ++cursor;
        }
        if (*cursor != '\0') {
            return false;
        }
    }
    if (multiplier != 0 &&
        value > (static_cast<size_t>(-1) / multiplier)) {
        return false;
    }
    out = value * multiplier;
    return true;
}

bool parse_args(const char* raw,
                DownloadOptions& options,
                char* url,
                size_t url_size,
                char* output,
                size_t output_size) {
    if (url == nullptr || url_size == 0 ||
        output == nullptr || output_size == 0) {
        return false;
    }
    url[0] = '\0';
    output[0] = '\0';

    const char* cursor = raw;
    char token[kMaxUrl];
    while (copy_arg(cursor, token, sizeof(token))) {
        if (equals(token, "--quiet") || equals(token, "--no-progress")) {
            options.quiet = true;
            continue;
        }
        if (equals(token, "--compact")) {
            options.compact = true;
            continue;
        }
        if (equals(token, "--discard")) {
            options.discard = true;
            continue;
        }
        if (equals(token, "--require-valid-cert")) {
            options.require_valid_cert = true;
            continue;
        }
        if (equals(token, "--net-debug") || equals(token, "--debug-net")) {
            options.net_debug = true;
            continue;
        }
        if (equals(token, "--limit") || equals(token, "--max-bytes")) {
            char value[32];
            if (!copy_arg(cursor, value, sizeof(value)) ||
                !parse_size_arg(value, options.limit)) {
                return false;
            }
            options.have_limit = true;
            continue;
        }
        if (has_prefix(token, "--limit=")) {
            if (!parse_size_arg(token + 8, options.limit)) {
                return false;
            }
            options.have_limit = true;
            continue;
        }
        if (has_prefix(token, "--max-bytes=")) {
            if (!parse_size_arg(token + 12, options.limit)) {
                return false;
            }
            options.have_limit = true;
            continue;
        }
        if (token[0] == '-' && token[1] == '-') {
            return false;
        }
        if (url[0] == '\0') {
            if (strlen(token) >= url_size) {
                return false;
            }
            strlcpy(url, token, url_size);
            continue;
        }
        if (output[0] == '\0') {
            if (strlen(token) >= output_size) {
                return false;
            }
            strlcpy(output, token, output_size);
            continue;
        }
        return false;
    }

    cursor = skip_spaces(cursor);
    return (cursor == nullptr || *cursor == '\0') &&
           url[0] != '\0' &&
           (output[0] != '\0' || options.discard);
}

void append_port(char* out, size_t out_size, uint16_t port) {
    size_t length = strlen(out);
    if (length + 1 >= out_size) {
        return;
    }
    out[length++] = ':';
    char digits[8];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (port % 10u));
        port = static_cast<uint16_t>(port / 10u);
    } while (port != 0 && count < sizeof(digits));
    while (count != 0 && length + 1 < out_size) {
        out[length++] = digits[--count];
    }
    out[length] = '\0';
}

bool open_tcpd_registry(uint32_t& handle, tcpd_protocol::Registry*& registry) {
    long shm = shared_memory_open(tcpd_protocol::kRegistryName,
                                  sizeof(tcpd_protocol::Registry));
    if (shm < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(shm), &info) != 0 ||
        info.base == 0 ||
        info.length < sizeof(tcpd_protocol::Registry)) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<tcpd_protocol::Registry*>(info.base);
    return true;
}

bool send_connect_request(uint32_t server_handle,
                          uint32_t reply_pipe_id,
                          uint32_t endpoint_id,
                          const uint8_t ip[4],
                          uint16_t port) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kConnectRequest);
    message.connect_request.reply_pipe_id = reply_pipe_id;
    message.connect_request.remote_port = port;
    message.connect_request.endpoint_id = endpoint_id;
    for (size_t i = 0; i < 4; ++i) {
        message.connect_request.remote_ip[i] = ip[i];
    }
    return tcpd_protocol::write_message(server_handle, message);
}

bool send_close_request(uint32_t server_handle, uint32_t connection_id) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kCloseRequest);
    message.close_request.connection_id = connection_id;
    return tcpd_protocol::write_message(server_handle, message);
}

bool send_payload(uint32_t server_handle,
                  uint32_t connection_id,
                  const uint8_t* payload,
                  size_t payload_length) {
    if (payload_length > tcpd_protocol::kMaxPayload) {
        return false;
    }
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kSendRequest);
    message.send_request.connection_id = connection_id;
    message.send_request.payload_length = static_cast<uint16_t>(payload_length);
    for (size_t i = 0; i < payload_length; ++i) {
        message.send_request.payload[i] = payload[i];
    }
    return tcpd_protocol::write_message(server_handle, message);
}

bool tcp_connect(TcpConnection& conn, const uint8_t ip[4], uint16_t port) {
    uint32_t registry_handle = 0;
    tcpd_protocol::Registry* registry = nullptr;
    if (!open_tcpd_registry(registry_handle, registry)) {
        print_line("download: failed to open tcpd registry");
        return false;
    }
    uint64_t registry_start = wall_time_ns();
    uint32_t registry_waits = 0;
    while (registry->magic != tcpd_protocol::kRegistryMagic ||
           registry->version != tcpd_protocol::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        if (timed_out(registry_start,
                      kConnectTimeoutNs,
                      registry_waits++,
                      kConnectWaitLimit)) {
            descriptor_close(registry_handle);
            print_line("download: tcpd registry timeout");
            return false;
        }
        yield();
    }
    uint32_t server_pipe_id = registry->server_pipe_id;
    descriptor_close(registry_handle);

    uint64_t reply_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                           static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply_pipe = pipe_open_new(reply_flags);
    if (reply_pipe < 0) {
        print_line("download: failed to create reply pipe");
        return false;
    }
    descriptor_defs::PipeInfo reply_info{};
    if (pipe_get_info(static_cast<uint32_t>(reply_pipe), &reply_info) != 0 ||
        reply_info.id == 0) {
        descriptor_close(static_cast<uint32_t>(reply_pipe));
        return false;
    }

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_existing(server_flags, server_pipe_id);
    if (server_pipe < 0) {
        descriptor_close(static_cast<uint32_t>(reply_pipe));
        print_line("download: failed to open tcpd pipe");
        return false;
    }

    long endpoint =
        net_endpoint_open_new(static_cast<uint64_t>(descriptor_defs::Flag::Async));
    if (endpoint < 0) {
        descriptor_close(static_cast<uint32_t>(reply_pipe));
        descriptor_close(static_cast<uint32_t>(server_pipe));
        print_line("download: failed to create endpoint");
        return false;
    }
    descriptor_defs::NetEndpointInfo endpoint_info{};
    if (net_endpoint_get_info(static_cast<uint32_t>(endpoint), &endpoint_info) != 0 ||
        endpoint_info.id == 0) {
        descriptor_close(static_cast<uint32_t>(endpoint));
        descriptor_close(static_cast<uint32_t>(reply_pipe));
        descriptor_close(static_cast<uint32_t>(server_pipe));
        print_line("download: failed to query endpoint");
        return false;
    }

    if (!send_connect_request(static_cast<uint32_t>(server_pipe),
                              reply_info.id,
                              endpoint_info.id,
                              ip,
                              port)) {
        descriptor_close(static_cast<uint32_t>(endpoint));
        descriptor_close(static_cast<uint32_t>(reply_pipe));
        descriptor_close(static_cast<uint32_t>(server_pipe));
        print_line("download: failed to send connect request");
        return false;
    }

    uint32_t connect_waits = 0;
    uint64_t connect_start = wall_time_ns();
    for (;;) {
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(static_cast<uint32_t>(reply_pipe), message)) {
            if (timed_out(connect_start,
                          kConnectTimeoutNs,
                          connect_waits++,
                          kConnectWaitLimit)) {
                descriptor_close(static_cast<uint32_t>(endpoint));
                descriptor_close(static_cast<uint32_t>(reply_pipe));
                descriptor_close(static_cast<uint32_t>(server_pipe));
                print_line("download: tcp connect timeout");
                return false;
            }
            yield();
            continue;
        }
        if (message.type != tcpd_protocol::kConnectResponse) {
            continue;
        }
        if (message.connect_response.status != tcpd_protocol::kStatusOk) {
            descriptor_close(static_cast<uint32_t>(endpoint));
            descriptor_close(static_cast<uint32_t>(reply_pipe));
            descriptor_close(static_cast<uint32_t>(server_pipe));
            print_line("download: tcp connect failed");
            return false;
        }
        conn.server_pipe = static_cast<uint32_t>(server_pipe);
        conn.reply_pipe = static_cast<uint32_t>(reply_pipe);
        conn.endpoint = static_cast<uint32_t>(endpoint);
        conn.connection_id = message.connect_response.connection_id;
        conn.closed = false;
        conn.pending_offset = 0;
        conn.pending_length = 0;
        return true;
    }
}

void tcp_close(TcpConnection& conn) {
    if (conn.server_pipe != 0 && conn.connection_id != 0) {
        (void)send_close_request(conn.server_pipe, conn.connection_id);
    }
    if (conn.reply_pipe != 0) {
        descriptor_close(conn.reply_pipe);
    }
    if (conn.endpoint != 0) {
        descriptor_close(conn.endpoint);
    }
    if (conn.server_pipe != 0) {
        descriptor_close(conn.server_pipe);
    }
    conn.server_pipe = 0;
    conn.reply_pipe = 0;
    conn.endpoint = 0;
    conn.connection_id = 0;
    conn.closed = true;
    conn.pending_offset = 0;
    conn.pending_length = 0;
}

bool tcp_send_all(TcpConnection& conn, const uint8_t* data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset;
        if (chunk > tcpd_protocol::kMaxPayload) {
            chunk = tcpd_protocol::kMaxPayload;
        }
        if (!send_payload(conn.server_pipe, conn.connection_id, data + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

int tcp_read_some(TcpConnection& conn, uint8_t* out, size_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    if (conn.endpoint != 0) {
        uint32_t read_waits = 0;
        uint64_t read_start = wall_time_ns();
        for (;;) {
            long result = descriptor_read(conn.endpoint, out, capacity);
            if (result == kDescriptorWouldBlock) {
                if (timed_out(read_start,
                              kReadTimeoutNs,
                              read_waits++,
                              kReadWaitLimit)) {
                    print_line("download: tcp read timeout");
                    return -1;
                }
                yield();
                continue;
            }
            if (result == 0) {
                conn.closed = true;
                return -1;
            }
            if (result < 0) {
                return -1;
            }
            return static_cast<int>(result);
        }
    }
    uint32_t read_waits = 0;
    uint64_t read_start = wall_time_ns();
    while (conn.pending_offset == conn.pending_length) {
        if (conn.closed) {
            return -1;
        }
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(conn.reply_pipe, message)) {
            if (timed_out(read_start,
                          kReadTimeoutNs,
                          read_waits++,
                          kReadWaitLimit)) {
                print_line("download: tcp read timeout");
                return -1;
            }
            yield();
            continue;
        }
        read_waits = 0;
        read_start = wall_time_ns();
        if (message.type == tcpd_protocol::kDataEvent &&
            message.data_event.connection_id == conn.connection_id) {
            conn.pending_offset = 0;
            conn.pending_length = message.data_event.payload_length;
            memcpy(conn.pending, message.data_event.payload, conn.pending_length);
            break;
        }
        if (message.type == tcpd_protocol::kClosedEvent &&
            message.closed_event.connection_id == conn.connection_id) {
            conn.closed = true;
            return -1;
        }
    }
    size_t available = conn.pending_length - conn.pending_offset;
    if (available > capacity) {
        available = capacity;
    }
    memcpy(out, conn.pending + conn.pending_offset, available);
    conn.pending_offset += available;
    return static_cast<int>(available);
}

void* trust_store_alloc(TrustStore& store, size_t size) {
    if (size == 0 || store.data == nullptr ||
        store.data_size > store.data_capacity ||
        size > store.data_capacity - store.data_size) {
        return nullptr;
    }
    void* result = store.data + store.data_size;
    store.data_size += size;
    return result;
}

bool load_file(const char* path, Buffer& out) {
    long handle = file_open(path);
    if (handle < 0) {
        return false;
    }
    buffer_clear(out);
    for (;;) {
        long read = file_read(static_cast<uint32_t>(handle),
                              g_file_read_buffer,
                              sizeof(g_file_read_buffer));
        if (read < 0) {
            file_close(static_cast<uint32_t>(handle));
            return false;
        }
        if (read == 0) {
            break;
        }
        if (!buffer_append(out, g_file_read_buffer, static_cast<size_t>(read))) {
            file_close(static_cast<uint32_t>(handle));
            return false;
        }
    }
    file_close(static_cast<uint32_t>(handle));
    return true;
}

bool trust_store_reserve(TrustStore& store, size_t required) {
    if (required <= store.anchor_capacity) {
        return true;
    }
    size_t new_capacity = store.anchor_capacity == 0 ? 8 : store.anchor_capacity;
    while (new_capacity < required) {
        if (new_capacity > static_cast<size_t>(-1) / 2) {
            return false;
        }
        new_capacity *= 2;
    }
    auto* new_anchors = static_cast<br_x509_trust_anchor*>(
        map_anonymous(new_capacity * sizeof(br_x509_trust_anchor), MAP_WRITE));
    if (new_anchors == nullptr) {
        return false;
    }
    if (store.anchors != nullptr && store.anchor_count != 0) {
        memcpy(new_anchors,
               store.anchors,
               store.anchor_count * sizeof(br_x509_trust_anchor));
    }
    if (store.anchors != nullptr) {
        unmap(store.anchors,
              store.anchor_capacity * sizeof(br_x509_trust_anchor));
    }
    store.anchors = new_anchors;
    store.anchor_capacity = new_capacity;
    return true;
}

void dn_append(void* ctx, const void* src, size_t len) {
    auto* buffer = static_cast<Buffer*>(ctx);
    (void)buffer_append(*buffer, src, len);
}

struct PemDecodeContext {
    Buffer der;
    bool collecting = false;
    bool error = false;
};

void pem_dest(void* ctx, const void* src, size_t len) {
    auto* pem = static_cast<PemDecodeContext*>(ctx);
    if (!buffer_append(pem->der, src, len)) {
        pem->error = true;
    }
}

bool append_trust_anchor_from_der(TrustStore& store, const uint8_t* der, size_t der_len) {
    if (der == nullptr || der_len == 0) {
        return false;
    }

    Buffer dn{};
    br_x509_decoder_context decoder{};
    br_x509_decoder_init(&decoder, dn_append, &dn);
    br_x509_decoder_push(&decoder, der, der_len);
    if (br_x509_decoder_last_error(&decoder) != 0) {
        buffer_release(dn);
        return false;
    }

    br_x509_pkey* key = br_x509_decoder_get_pkey(&decoder);
    if (key == nullptr || dn.size == 0) {
        buffer_release(dn);
        return false;
    }
    if (!trust_store_reserve(store, store.anchor_count + 1)) {
        buffer_release(dn);
        return false;
    }

    size_t data_start = store.data_size;
    br_x509_trust_anchor anchor{};
    anchor.flags = br_x509_decoder_isCA(&decoder) ? BR_X509_TA_CA : 0;
    auto* dn_copy = static_cast<unsigned char*>(trust_store_alloc(store, dn.size));
    if (dn_copy == nullptr) {
        buffer_release(dn);
        return false;
    }
    memcpy(dn_copy, dn.data, dn.size);
    anchor.dn.data = dn_copy;
    anchor.dn.len = dn.size;
    anchor.pkey.key_type = key->key_type;

    if (key->key_type == BR_KEYTYPE_RSA) {
        auto* n_copy = static_cast<unsigned char*>(
            trust_store_alloc(store, key->key.rsa.nlen));
        auto* e_copy = static_cast<unsigned char*>(
            trust_store_alloc(store, key->key.rsa.elen));
        if (n_copy == nullptr || e_copy == nullptr) {
            store.data_size = data_start;
            buffer_release(dn);
            return false;
        }
        memcpy(n_copy, key->key.rsa.n, key->key.rsa.nlen);
        memcpy(e_copy, key->key.rsa.e, key->key.rsa.elen);
        anchor.pkey.key.rsa.n = n_copy;
        anchor.pkey.key.rsa.nlen = key->key.rsa.nlen;
        anchor.pkey.key.rsa.e = e_copy;
        anchor.pkey.key.rsa.elen = key->key.rsa.elen;
    } else if (key->key_type == BR_KEYTYPE_EC) {
        auto* q_copy = static_cast<unsigned char*>(
            trust_store_alloc(store, key->key.ec.qlen));
        if (q_copy == nullptr) {
            store.data_size = data_start;
            buffer_release(dn);
            return false;
        }
        memcpy(q_copy, key->key.ec.q, key->key.ec.qlen);
        anchor.pkey.key.ec.curve = key->key.ec.curve;
        anchor.pkey.key.ec.q = q_copy;
        anchor.pkey.key.ec.qlen = key->key.ec.qlen;
    } else {
        store.data_size = data_start;
        buffer_release(dn);
        return false;
    }

    store.anchors[store.anchor_count++] = anchor;
    buffer_release(dn);
    return true;
}

bool load_trust_store(TrustStore& store) {
    Buffer pem_bytes{};
    if (!load_file("/config/ssl/cacert.pem", pem_bytes) &&
        !load_file(".../config/ssl/cacert.pem", pem_bytes) &&
        !load_file("config/ssl/cacert.pem", pem_bytes)) {
        return false;
    }
    store.data = static_cast<uint8_t*>(
        map_anonymous(pem_bytes.size, MAP_WRITE));
    if (store.data == nullptr) {
        buffer_release(pem_bytes);
        return false;
    }
    store.data_capacity = pem_bytes.size;

    br_pem_decoder_context pem{};
    br_pem_decoder_init(&pem);
    PemDecodeContext pem_ctx{};

    size_t offset = 0;
    while (offset < pem_bytes.size) {
        size_t consumed = br_pem_decoder_push(&pem,
                                              pem_bytes.data + offset,
                                              pem_bytes.size - offset);
        offset += consumed;

        int event = br_pem_decoder_event(&pem);
        if (event == 0) {
            continue;
        }
        if (event == BR_PEM_BEGIN_OBJ) {
            buffer_clear(pem_ctx.der);
            pem_ctx.collecting = true;
            pem_ctx.error = false;
            br_pem_decoder_setdest(&pem, pem_dest, &pem_ctx);
            continue;
        }
        if (event == BR_PEM_END_OBJ) {
            if (pem_ctx.collecting && !pem_ctx.error && pem_ctx.der.size != 0) {
                (void)append_trust_anchor_from_der(store, pem_ctx.der.data, pem_ctx.der.size);
            }
            pem_ctx.collecting = false;
            pem_ctx.error = false;
            buffer_clear(pem_ctx.der);
            br_pem_decoder_setdest(&pem, nullptr, nullptr);
            continue;
        }
        if (event == BR_PEM_ERROR) {
            pem_ctx.collecting = false;
            pem_ctx.error = true;
            buffer_clear(pem_ctx.der);
            br_pem_decoder_setdest(&pem, nullptr, nullptr);
        }
    }
    buffer_release(pem_ctx.der);
    buffer_release(pem_bytes);
    return store.anchor_count != 0;
}

bool ensure_trust_store_loaded() {
    if (g_trust_store == reinterpret_cast<TrustStore*>(1)) {
        g_trust_store = static_cast<TrustStore*>(map_anonymous(sizeof(TrustStore), MAP_WRITE));
        if (g_trust_store == nullptr) {
            return false;
        }
        for (size_t i = 0; i < sizeof(*g_trust_store); ++i) {
            reinterpret_cast<uint8_t*>(g_trust_store)[i] = 0;
        }
    }
    if (g_trust_store->attempted) {
        return g_trust_store->loaded;
    }
    g_trust_store->attempted = true;
    g_trust_store->loaded = load_trust_store(*g_trust_store);
    return g_trust_store->loaded;
}

bool init_tls_client(Stream& stream,
                     const char* host,
                     uint16_t port,
                     bool require_valid_cert) {
    (void)port;
    (void)require_valid_cert;
    NeutrinoWallTime now{};
    bool have_time = neutrino_get_time(&now);
    if (!have_time) {
        print_line("download: HTTPS requires a valid system clock");
        return false;
    }

    if (ensure_trust_store_loaded() && g_trust_store->anchor_count != 0) {
        br_ssl_client_init_full(&stream.ssl_client,
                                &stream.x509_minimal,
                                g_trust_store->anchors,
                                g_trust_store->anchor_count);
        constexpr uint32_t kUnixEpochDays = 719528u;
        uint32_t days = kUnixEpochDays +
                        static_cast<uint32_t>(now.unix_seconds / 86400ull);
        uint32_t seconds = static_cast<uint32_t>(now.unix_seconds % 86400ull);
        br_x509_minimal_set_time(&stream.x509_minimal, days, seconds);
        br_ssl_engine_set_versions(&stream.ssl_client.eng, BR_TLS12, BR_TLS12);
    } else {
        print_line("download: TLS trust store unavailable; refusing HTTPS");
        return false;
    }
    br_ssl_engine_set_buffer(&stream.ssl_client.eng,
                             stream.ssl_iobuf,
                             sizeof(stream.ssl_iobuf),
                             1);
    uint8_t entropy[32];
    if (!auth::secure_random(entropy, sizeof(entropy))) {
        print_line("download: secure random source unavailable; refusing HTTPS");
        return false;
    }
    br_ssl_engine_inject_entropy(&stream.ssl_client.eng, entropy, sizeof(entropy));
    br_ssl_client_reset(&stream.ssl_client, host, 0);
    return true;
}

int tcp_low_read(void* context, unsigned char* data, size_t len) {
    auto* tcp = static_cast<TcpConnection*>(context);
    return tcp_read_some(*tcp, data, len);
}

int tcp_low_write(void* context, const unsigned char* data, size_t len) {
    auto* tcp = static_cast<TcpConnection*>(context);
    return tcp_send_all(*tcp, data, len) ? static_cast<int>(len) : -1;
}

bool stream_connect(Stream& stream, const Url& url, bool require_valid_cert) {
    uint8_t ip[4];
    if (!userspace::http::parse_ipv4_literal(url.host, ip) &&
        !usernet::dns::resolve_a(url.host, ip)) {
        print("download: dns lookup failed: ");
        print_line(usernet::dns::status_text(usernet::dns::last_status()));
        return false;
    }
    if (!tcp_connect(stream.tcp, ip, url.port)) {
        return false;
    }
    stream.tls = url.scheme == Scheme::Https;
    if (!stream.tls) {
        return true;
    }
    if (!init_tls_client(stream, url.host, url.port, require_valid_cert)) {
        tcp_close(stream.tcp);
        return false;
    }
    br_sslio_init(&stream.ssl_io,
                  &stream.ssl_client.eng,
                  tcp_low_read,
                  &stream.tcp,
                  tcp_low_write,
                  &stream.tcp);
    return true;
}

void stream_close(Stream& stream) {
    tcp_close(stream.tcp);
}

int stream_read(Stream& stream, uint8_t* data, size_t length) {
    if (!stream.tls) {
        return tcp_read_some(stream.tcp, data, length);
    }
    return br_sslio_read(&stream.ssl_io, data, length);
}

bool stream_write_all(Stream& stream, const uint8_t* data, size_t length) {
    if (!stream.tls) {
        return tcp_send_all(stream.tcp, data, length);
    }
    int write_result = br_sslio_write_all(&stream.ssl_io, data, length);
    int flush_result =
        write_result == 0 ? br_sslio_flush(&stream.ssl_io) : write_result;
    if (write_result != 0 || flush_result != 0) {
        print("download: TLS error ");
        print_u64(static_cast<uint64_t>(
            br_ssl_engine_last_error(&stream.ssl_client.eng)));
        print("\n");
        return false;
    }
    return true;
}

bool stream_read_line(Stream& stream, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    size_t length = 0;
    while (length + 1 < out_size) {
        uint8_t ch = 0;
        int got = stream_read(stream, &ch, 1);
        if (got <= 0) {
            return false;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            out[length] = '\0';
            return true;
        }
        out[length++] = static_cast<char>(ch);
    }
    out[out_size - 1] = '\0';
    return true;
}

bool append_request(char* request, size_t capacity, size_t& length, const char* text) {
    size_t text_len = strlen(text);
    if (length + text_len > capacity) {
        return false;
    }
    memcpy(request + length, text, text_len);
    length += text_len;
    return true;
}

bool send_http_request(Stream& stream, const Url& url) {
    char request[1536];
    size_t length = 0;
    if (!append_request(request, sizeof(request), length, "GET ") ||
        !append_request(request, sizeof(request), length, url.path) ||
        !append_request(request, sizeof(request), length, " HTTP/1.1\r\nHost: ") ||
        !append_request(request, sizeof(request), length, url.host)) {
        return false;
    }
    bool need_port = (url.scheme == Scheme::Http && url.port != 80) ||
                     (url.scheme == Scheme::Https && url.port != 443);
    if (need_port) {
        char port_buf[8] = {};
        append_port(port_buf, sizeof(port_buf), url.port);
        if (!append_request(request, sizeof(request), length, port_buf)) {
            return false;
        }
    }
    if (!append_request(request,
                        sizeof(request),
                        length,
                        "\r\nUser-Agent: neutrino-download/0.1\r\n"
                        "Accept: */*\r\n"
                        "Accept-Encoding: identity\r\n"
                        "Connection: close\r\n\r\n")) {
        return false;
    }
    return stream_write_all(stream, reinterpret_cast<const uint8_t*>(request), length);
}

bool read_response_headers_adapter(void* context, char* out, size_t out_size) {
    return stream_read_line(*static_cast<Stream*>(context), out, out_size);
}

bool read_response_headers(Stream& stream, HttpResponseMeta& meta) {
    return userspace::http::read_response_headers(
        &stream,
        read_response_headers_adapter,
        meta);
}

bool write_file_all(uint32_t file, const uint8_t* data, size_t length) {
    size_t written_total = 0;
    while (written_total < length) {
        long written = file_write(file, data + written_total, length - written_total);
        if (written <= 0) {
            return false;
        }
        written_total += static_cast<size_t>(written);
    }
    return true;
}

bool ensure_dir(const char* path) {
    long dir = directory_open(path);
    if (dir >= 0) {
        directory_close(static_cast<uint32_t>(dir));
        return true;
    }
    return directory_create(path) == 0;
}

bool ensure_parent_dir(const char* path) {
    if (path == nullptr) {
        return false;
    }
    char parent[256];
    strlcpy(parent, path, sizeof(parent));
    size_t len = strlen(parent);
    while (len > 0 && parent[len - 1] != '/') {
        --len;
    }
    if (len == 0 || len == 1 || (len == 4 && strncmp(parent, ".../", 4) == 0)) {
        return true;
    }
    parent[len - 1] = '\0';

    char partial[256];
    size_t used = 0;
    size_t i = 0;
    if (strncmp(parent, ".../", 4) == 0) {
        strlcpy(partial, "...", sizeof(partial));
        used = 3;
        i = 3;
    } else if (parent[0] == '/') {
        partial[used++] = '/';
        partial[used] = '\0';
        i = 1;
    } else {
        return true;
    }

    for (; parent[i] != '\0'; ++i) {
        if (used + 1 >= sizeof(partial)) {
            return false;
        }
        partial[used++] = parent[i];
        partial[used] = '\0';
        if (parent[i] == '/') {
            if (strcmp(partial, ".../") != 0) {
                partial[used - 1] = '\0';
                if (!ensure_dir(partial)) {
                    return false;
                }
                partial[used - 1] = '/';
            }
        }
    }
    return ensure_dir(partial);
}

bool write_output_all(uint32_t file,
                      const DownloadOptions& options,
                      const uint8_t* data,
                      size_t length) {
    if (options.discard) {
        return true;
    }
    return write_file_all(file, data, length);
}

bool read_exact(Stream& stream, uint8_t* data, size_t length) {
    size_t total = 0;
    while (total < length) {
        int got = stream_read(stream, data + total, length - total);
        if (got <= 0) {
            return false;
        }
        total += static_cast<size_t>(got);
    }
    return true;
}

BodyResult write_response_body(Stream& stream,
                               const HttpResponseMeta& meta,
                               uint32_t file,
                               const DownloadOptions& options,
                               size_t& bytes_written) {
    uint8_t* buffer = g_body_buffer;
    bytes_written = 0;
    ProgressState progress{};
    progress.known_total = meta.have_content_length;
    progress.total = meta.content_length;
    progress.net_debug = options.net_debug;
    progress.quiet = options.quiet;
    if (options.have_limit &&
        (!progress.known_total || options.limit < progress.total)) {
        progress.known_total = true;
        progress.total = options.limit;
    }
    if (progress.net_debug) {
        long debug_handle = net_device_open(kNetDebugDeviceIndex);
        if (debug_handle >= 0) {
            progress.net_debug_handle = static_cast<uint32_t>(debug_handle);
        } else {
            print_line("download: warning: net debug unavailable");
        }
    }
    render_progress(progress, bytes_written, true);
    if (options.have_limit && options.limit == 0) {
        finish_progress(progress, bytes_written);
        return BodyResult::Limited;
    }
    if (meta.chunked) {
        for (;;) {
            if (options.have_limit && bytes_written >= options.limit) {
                finish_progress(progress, bytes_written);
                return BodyResult::Limited;
            }
            char line[64];
            if (!stream_read_line(stream, line, sizeof(line))) {
                return fail_progress(progress);
            }
            size_t chunk_size = 0;
            if (!userspace::http::parse_hex_size(line, chunk_size)) {
                return fail_progress(progress);
            }
            if (chunk_size == 0) {
                char trailer[256];
                while (stream_read_line(stream, trailer, sizeof(trailer))) {
                    if (trailer[0] == '\0') {
                        break;
                    }
                }
                finish_progress(progress, bytes_written);
                return BodyResult::Complete;
            }
            size_t remaining = chunk_size;
            while (remaining != 0) {
                size_t want = remaining < kIoBufferSize ? remaining : kIoBufferSize;
                if (options.have_limit) {
                    size_t limit_remaining = options.limit - bytes_written;
                    if (limit_remaining == 0) {
                        finish_progress(progress, bytes_written);
                        return BodyResult::Limited;
                    }
                    if (want > limit_remaining) {
                        want = limit_remaining;
                    }
                }
                if (!read_exact(stream, buffer, want) ||
                    !write_output_all(file, options, buffer, want)) {
                    return fail_progress(progress);
                }
                bytes_written += want;
                render_progress(progress, bytes_written, false);
                remaining -= want;
                if (options.have_limit && bytes_written >= options.limit) {
                    finish_progress(progress, bytes_written);
                    return BodyResult::Limited;
                }
            }
            uint8_t crlf[2];
            if (!read_exact(stream, crlf, sizeof(crlf))) {
                return fail_progress(progress);
            }
        }
    }

    if (meta.have_content_length) {
        size_t remaining = meta.content_length;
        bool limited = false;
        if (options.have_limit && options.limit < remaining) {
            remaining = options.limit;
            limited = true;
        }
        while (remaining != 0) {
            size_t want = remaining < kIoBufferSize ? remaining : kIoBufferSize;
            if (!read_exact(stream, buffer, want) ||
                !write_output_all(file, options, buffer, want)) {
                return fail_progress(progress);
            }
            bytes_written += want;
            render_progress(progress, bytes_written, false);
            remaining -= want;
        }
        finish_progress(progress, bytes_written);
        return limited ? BodyResult::Limited : BodyResult::Complete;
    }

    for (;;) {
        size_t want = kIoBufferSize;
        if (options.have_limit) {
            if (bytes_written >= options.limit) {
                finish_progress(progress, bytes_written);
                return BodyResult::Limited;
            }
            size_t limit_remaining = options.limit - bytes_written;
            if (want > limit_remaining) {
                want = limit_remaining;
            }
        }
        int got = stream_read(stream, buffer, want);
        if (got <= 0) {
            finish_progress(progress, bytes_written);
            return BodyResult::Complete;
        }
        if (!write_output_all(file, options, buffer, static_cast<size_t>(got))) {
            return fail_progress(progress);
        }
        bytes_written += static_cast<size_t>(got);
        render_progress(progress, bytes_written, false);
        if (options.have_limit && bytes_written >= options.limit) {
            finish_progress(progress, bytes_written);
            return BodyResult::Limited;
        }
    }
}

bool prepare_output_file(const char* path, uint32_t& out_file) {
    if (!ensure_parent_dir(path)) {
        print("download: unable to create output directory for ");
        print_line(path);
        return false;
    }
    long directory = directory_open(path);
    if (directory >= 0) {
        directory_close(static_cast<uint32_t>(directory));
        print_line("download: destination is a directory");
        return false;
    }
    long existing = file_open(path);
    if (existing >= 0) {
        file_close(static_cast<uint32_t>(existing));
        if (file_remove(path) < 0) {
            print_line("download: unable to replace output file");
            return false;
        }
    }
    long file = file_create(path);
    if (file < 0) {
        print("download: unable to create output file ");
        print_line(path);
        return false;
    }
    out_file = static_cast<uint32_t>(file);
    return true;
}

bool fetch_to_file(const char* url_text,
                   const char* output_path,
                   const DownloadOptions& options,
                   size_t& bytes_written,
                   bool& stopped_by_limit) {
    stopped_by_limit = false;
    if (url_text == nullptr || strlen(url_text) >= kMaxUrl) {
        print_line("download: url too long");
        return false;
    }
    char current_url[kMaxUrl];
    strlcpy(current_url, url_text, sizeof(current_url));

    for (uint32_t redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        Url url{};
        if (!userspace::http::parse_url(
                current_url,
                url,
                userspace::http::UrlParseMode::RequireScheme)) {
            print_line("download: invalid url");
            return false;
        }

        auto* stream = static_cast<Stream*>(map_anonymous(sizeof(Stream), MAP_WRITE));
        if (stream == nullptr) {
            print_line("download: unable to allocate stream state");
            return false;
        }
        for (size_t i = 0; i < sizeof(*stream); ++i) {
            reinterpret_cast<uint8_t*>(stream)[i] = 0;
        }

        // A TLS handshake can take long enough to look like a stalled package
        // download. Show the initial progress state before networking starts;
        // write_response_body() replaces it with byte progress after headers.
        if (!options.quiet) {
            print("download: [                    ] 0% connecting to ");
            print(url.host);
            print("...\n");
        }
        if (!stream_connect(*stream, url, options.require_valid_cert)) {
            unmap(stream, sizeof(*stream));
            return false;
        }
        if (!send_http_request(*stream, url)) {
            stream_close(*stream);
            unmap(stream, sizeof(*stream));
            print_line("download: failed to send request");
            return false;
        }

        HttpResponseMeta meta{};
        if (!read_response_headers(*stream, meta)) {
            stream_close(*stream);
            unmap(stream, sizeof(*stream));
            print_line("download: failed to read response headers");
            return false;
        }

        if (meta.status_code >= 300 && meta.status_code < 400 && meta.location[0] != '\0') {
            char next_url[kMaxUrl];
            bool ok = userspace::http::build_redirect_url(
                url,
                meta.location,
                next_url,
                sizeof(next_url));
            stream_close(*stream);
            unmap(stream, sizeof(*stream));
            if (!ok) {
                print_line("download: redirect url too long");
                return false;
            }
            strlcpy(current_url, next_url, sizeof(current_url));
            continue;
        }

        if (meta.status_code < 200 || meta.status_code >= 300) {
            stream_close(*stream);
            unmap(stream, sizeof(*stream));
            print("download: http status ");
            print_u64(static_cast<uint64_t>(meta.status_code));
            print("\n");
            return false;
        }

        uint32_t file = 0;
        if (!options.discard && !prepare_output_file(output_path, file)) {
            stream_close(*stream);
            unmap(stream, sizeof(*stream));
            return false;
        }
        BodyResult body_result =
            write_response_body(*stream, meta, file, options, bytes_written);
        if (!options.discard) {
            (void)file_sync(file);
            file_close(file);
        }
        stream_close(*stream);
        unmap(stream, sizeof(*stream));
        if (body_result == BodyResult::Error) {
            if (!options.discard) {
                file_remove(output_path);
            }
            print_line("download: failed while writing response body");
            return false;
        }
        stopped_by_limit = body_result == BodyResult::Limited;
        return true;
    }

    print_line("download: too many redirects");
    return false;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    g_console = neutrino_open_stdout();

    char url[kMaxUrl];
    char output[256];
    DownloadOptions options{};
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    if (!parse_args(args, options, url, sizeof(url), output, sizeof(output))) {
        print_line("usage: download [--limit bytes|1K|1M] [--net-debug] [--quiet|--compact] [--discard] [--require-valid-cert] <url> [output-file]");
        return 1;
    }

    size_t bytes_written = 0;
    bool stopped_by_limit = false;
    uint64_t start_ns = wall_time_ns();
    if (!fetch_to_file(url, output, options, bytes_written, stopped_by_limit)) {
        return 1;
    }
    uint64_t end_ns = wall_time_ns();
    uint64_t elapsed_ns = (end_ns >= start_ns) ? end_ns - start_ns : 0;

    if (options.quiet || options.compact) {
        if (options.compact && stopped_by_limit) {
            print_line("download: stopped at limit");
        }
        return 0;
    }
    print("download: ");
    print(options.discard ? "read " : "wrote ");
    print_u64(bytes_written);
    if (stopped_by_limit) {
        print_line(" bytes (stopped at limit)");
    } else {
        print_line(" bytes");
    }
    if (elapsed_ns != 0) {
        uint64_t elapsed_ms = elapsed_ns / 1000000ull;
        if (elapsed_ms == 0) {
            elapsed_ms = 1;
        }
        uint64_t kib_per_second =
            (static_cast<uint64_t>(bytes_written) * 1000ull) /
            (elapsed_ms * 1024ull);
        print("download: elapsed ");
        print_u64(elapsed_ms);
        print(" ms, ");
        print_u64(kib_per_second);
        print_line(" KiB/s");
    }
    return 0;
}
