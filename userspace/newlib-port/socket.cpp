#include "socket_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "../crt/syscall.hpp"
#include "../net/dns.hpp"
#include "../net/tcpd_protocol.hpp"

namespace {

constexpr int kSocketFdBase = 0x40000000;
constexpr size_t kSocketCount = 32;

struct SocketState {
    bool used;
    bool connected;
    bool nonblocking;
    bool shut_read;
    bool shut_write;
    uint32_t server_pipe;
    uint32_t reply_pipe;
    uint32_t endpoint;
    uint32_t connection_id;
    sockaddr_in peer;
};

SocketState g_sockets[kSocketCount]{};
volatile uint32_t g_socket_lock = 0;
volatile uint32_t g_protocol_write_lock = 0;

void lock_sockets() {
    while (__atomic_exchange_n(&g_socket_lock, 1, __ATOMIC_ACQUIRE) != 0)
        yield();
}

void unlock_sockets() {
    __atomic_store_n(&g_socket_lock, 0, __ATOMIC_RELEASE);
}

SocketState* state_for(int fd) {
    if (fd < kSocketFdBase || fd >= kSocketFdBase + (int)kSocketCount)
        return nullptr;
    SocketState* state = &g_sockets[(size_t)(fd - kSocketFdBase)];
    return state->used ? state : nullptr;
}

bool write_tcpd_message(uint32_t pipe, const tcpd_protocol::Message& message) {
    while (__atomic_exchange_n(&g_protocol_write_lock, 1, __ATOMIC_ACQUIRE) != 0)
        yield();
    bool result = tcpd_protocol::write_message(pipe, message);
    __atomic_store_n(&g_protocol_write_lock, 0, __ATOMIC_RELEASE);
    return result;
}

void close_state(SocketState& state) {
    if (state.server_pipe != 0 && state.connection_id != 0) {
        tcpd_protocol::Message request{};
        tcpd_protocol::init_message(request, tcpd_protocol::kCloseRequest);
        request.close_request.connection_id = state.connection_id;
        (void)write_tcpd_message(state.server_pipe, request);
    }
    if (state.endpoint != 0) descriptor_close(state.endpoint);
    if (state.reply_pipe != 0) descriptor_close(state.reply_pipe);
    if (state.server_pipe != 0) descriptor_close(state.server_pipe);
    state = SocketState{};
}

int parse_port(const char* text, uint16_t& port) {
    if (text == nullptr || *text == '\0') return EAI_SERVICE;
    unsigned value = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return EAI_SERVICE;
        value = value * 10u + (unsigned)(*p - '0');
        if (value > 65535u) return EAI_SERVICE;
    }
    port = (uint16_t)value;
    return 0;
}

} // namespace

extern "C" int neutrino_socket_is_fd(int fd) {
    return state_for(fd) != nullptr;
}

extern "C" int socket(int domain, int type, int protocol) {
    int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (domain != AF_INET || base_type != SOCK_STREAM ||
        (protocol != 0 && protocol != IPPROTO_TCP)) {
        errno = (domain != AF_INET) ? EAFNOSUPPORT : EPROTONOSUPPORT;
        return -1;
    }
    lock_sockets();
    for (size_t i = 0; i < kSocketCount; ++i) {
        if (!g_sockets[i].used) {
            g_sockets[i] = SocketState{};
            g_sockets[i].used = true;
            g_sockets[i].nonblocking = (type & SOCK_NONBLOCK) != 0;
            unlock_sockets();
            return kSocketFdBase + (int)i;
        }
    }
    unlock_sockets();
    errno = EMFILE;
    return -1;
}

extern "C" int connect(int fd, const sockaddr* address, socklen_t address_len) {
    SocketState* state = state_for(fd);
    if (state == nullptr) { errno = EBADF; return -1; }
    if (address == nullptr || address_len < sizeof(sockaddr_in) ||
        address->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (state->connected) { errno = EISCONN; return -1; }
    const sockaddr_in* remote = reinterpret_cast<const sockaddr_in*>(address);

    long registry_handle = shared_memory_open(tcpd_protocol::kRegistryName,
                                               sizeof(tcpd_protocol::Registry));
    if (registry_handle < 0) { errno = ENETDOWN; return -1; }
    descriptor_defs::SharedMemoryInfo registry_info{};
    if (shared_memory_get_info((uint32_t)registry_handle, &registry_info) != 0 ||
        registry_info.base == 0 || registry_info.length < sizeof(tcpd_protocol::Registry)) {
        descriptor_close((uint32_t)registry_handle);
        errno = ENETDOWN;
        return -1;
    }
    auto* registry = reinterpret_cast<tcpd_protocol::Registry*>(registry_info.base);
    while (registry->magic != tcpd_protocol::kRegistryMagic ||
           registry->version != tcpd_protocol::kRegistryVersion ||
           registry->server_pipe_id == 0)
        yield();
    uint32_t server_pipe_id = registry->server_pipe_id;
    descriptor_close((uint32_t)registry_handle);

    long reply = pipe_open_new((uint64_t)descriptor_defs::Flag::Readable |
                               (uint64_t)descriptor_defs::Flag::Async);
    long server = pipe_open_existing((uint64_t)descriptor_defs::Flag::Writable |
                                     (uint64_t)descriptor_defs::Flag::Async,
                                     server_pipe_id);
    long endpoint = net_endpoint_open_new((uint64_t)descriptor_defs::Flag::Async);
    if (reply < 0 || server < 0 || endpoint < 0) {
        if (reply >= 0) descriptor_close((uint32_t)reply);
        if (server >= 0) descriptor_close((uint32_t)server);
        if (endpoint >= 0) descriptor_close((uint32_t)endpoint);
        errno = ENETDOWN;
        return -1;
    }
    descriptor_defs::PipeInfo pipe_info{};
    descriptor_defs::NetEndpointInfo endpoint_info{};
    if (pipe_get_info((uint32_t)reply, &pipe_info) != 0 || pipe_info.id == 0 ||
        net_endpoint_get_info((uint32_t)endpoint, &endpoint_info) != 0 ||
        endpoint_info.id == 0) {
        descriptor_close((uint32_t)reply); descriptor_close((uint32_t)server);
        descriptor_close((uint32_t)endpoint); errno = EIO; return -1;
    }

    tcpd_protocol::Message request{};
    tcpd_protocol::init_message(request, tcpd_protocol::kConnectRequest);
    request.connect_request.reply_pipe_id = pipe_info.id;
    request.connect_request.remote_port = ntohs(remote->sin_port);
    request.connect_request.endpoint_id = endpoint_info.id;
    memcpy(request.connect_request.remote_ip, &remote->sin_addr.s_addr, 4);
    if (!write_tcpd_message((uint32_t)server, request)) {
        descriptor_close((uint32_t)reply); descriptor_close((uint32_t)server);
        descriptor_close((uint32_t)endpoint); errno = EIO; return -1;
    }

    tcpd_protocol::Message response{};
    for (;;) {
        if (!tcpd_protocol::read_message((uint32_t)reply, response)) {
            yield();
            continue;
        }
        if (response.type == tcpd_protocol::kConnectResponse) break;
    }
    if (response.connect_response.status != tcpd_protocol::kStatusOk) {
        descriptor_close((uint32_t)reply); descriptor_close((uint32_t)server);
        descriptor_close((uint32_t)endpoint); errno = ECONNREFUSED; return -1;
    }
    state->server_pipe = (uint32_t)server;
    state->reply_pipe = (uint32_t)reply;
    state->endpoint = (uint32_t)endpoint;
    state->connection_id = response.connect_response.connection_id;
    state->peer = *remote;
    state->connected = true;
    return 0;
}

extern "C" ssize_t send(int fd, const void* buffer, size_t length, int flags) {
    SocketState* state = state_for(fd);
    if (state == nullptr) { errno = EBADF; return -1; }
    if (!state->connected) { errno = ENOTCONN; return -1; }
    if (state->shut_write) { errno = EPIPE; return -1; }
    if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0) { errno = ENOTSUP; return -1; }
    const uint8_t* bytes = static_cast<const uint8_t*>(buffer);
    size_t sent = 0;
    while (sent < length) {
        size_t chunk = length - sent;
        if (chunk > tcpd_protocol::kMaxPayload) chunk = tcpd_protocol::kMaxPayload;
        tcpd_protocol::Message request{};
        tcpd_protocol::init_message(request, tcpd_protocol::kSendRequest);
        request.send_request.connection_id = state->connection_id;
        request.send_request.payload_length = (uint16_t)chunk;
        memcpy(request.send_request.payload, bytes + sent, chunk);
        if (!write_tcpd_message(state->server_pipe, request)) {
            if (sent != 0) return (ssize_t)sent;
            errno = EIO; return -1;
        }
        sent += chunk;
    }
    return (ssize_t)sent;
}

extern "C" ssize_t recv(int fd, void* buffer, size_t length, int flags) {
    SocketState* state = state_for(fd);
    if (state == nullptr) { errno = EBADF; return -1; }
    if (!state->connected) { errno = ENOTCONN; return -1; }
    if (state->shut_read) return 0;
    if ((flags & ~(MSG_DONTWAIT)) != 0) { errno = ENOTSUP; return -1; }
    for (;;) {
        long result = descriptor_read(state->endpoint, buffer, length);
        if (result == kDescriptorWouldBlock) {
            if (state->nonblocking || (flags & MSG_DONTWAIT)) { errno = EAGAIN; return -1; }
            descriptor_defs::DescriptorWait wait{state->endpoint,
                                                  descriptor_defs::kWaitRead, 0, 0};
            (void)descriptor_wait(&wait, 1);
            continue;
        }
        if (result < 0) { errno = EIO; return -1; }
        return (ssize_t)result;
    }
}

extern "C" ssize_t neutrino_socket_read(int fd, void* buffer, size_t length) {
    return recv(fd, buffer, length, 0);
}
extern "C" ssize_t neutrino_socket_write(int fd, const void* buffer, size_t length) {
    return send(fd, buffer, length, 0);
}

extern "C" int neutrino_socket_close(int fd) {
    SocketState* state = state_for(fd);
    if (state == nullptr) { errno = EBADF; return -1; }
    lock_sockets(); close_state(*state); unlock_sockets();
    return 0;
}

extern "C" int shutdown(int fd, int how) {
    SocketState* state = state_for(fd);
    if (state == nullptr) { errno = EBADF; return -1; }
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
        errno = EINVAL; return -1;
    }
    if (how == SHUT_RD || how == SHUT_RDWR) state->shut_read = true;
    if (how == SHUT_WR || how == SHUT_RDWR) state->shut_write = true;
    return 0;
}

extern "C" int neutrino_socket_fcntl(int fd, int command, int value) {
    SocketState* state = state_for(fd);
    if (state == nullptr) { errno = EBADF; return -1; }
    if (command == F_GETFD) return 0;
    if (command == F_SETFD) return 0;
    if (command == F_GETFL) return O_RDWR | (state->nonblocking ? O_NONBLOCK : 0);
    if (command == F_SETFL) {
        if ((value & ~O_NONBLOCK) != 0) { errno = ENOTSUP; return -1; }
        state->nonblocking = (value & O_NONBLOCK) != 0;
        return 0;
    }
    errno = ENOSYS; return -1;
}

extern "C" int neutrino_socket_poll_handle(int fd, uint32_t* handle, int* connected) {
    SocketState* state = state_for(fd);
    if (state == nullptr) return 0;
    if (handle != nullptr) *handle = state->endpoint;
    if (connected != nullptr) *connected = state->connected ? 1 : 0;
    return 1;
}

extern "C" int getsockopt(int fd, int level, int option, void* value, socklen_t* len) {
    SocketState* state = state_for(fd);
    if (state == nullptr || value == nullptr || len == nullptr || *len < sizeof(int)) {
        errno = EBADF; return -1;
    }
    int result = 0;
    if (level == SOL_SOCKET && option == SO_ERROR) result = 0;
    else if (level == SOL_SOCKET && option == SO_TYPE) result = SOCK_STREAM;
    else if (level == SOL_SOCKET && (option == SO_RCVBUF || option == SO_SNDBUF)) result = 65536;
    else { errno = ENOPROTOOPT; return -1; }
    memcpy(value, &result, sizeof(result)); *len = sizeof(result); return 0;
}

extern "C" int setsockopt(int fd, int level, int option, const void*, socklen_t) {
    if (state_for(fd) == nullptr) { errno = EBADF; return -1; }
    if ((level == SOL_SOCKET && (option == SO_RCVBUF || option == SO_SNDBUF ||
         option == SO_KEEPALIVE || option == SO_REUSEADDR || option == SO_NOSIGPIPE)) ||
        (level == IPPROTO_TCP && (option == TCP_NODELAY || option == TCP_MAXSEG)))
        return 0;
    errno = ENOPROTOOPT; return -1;
}

extern "C" int bind(int, const sockaddr*, socklen_t) { errno = EOPNOTSUPP; return -1; }
extern "C" int listen(int, int) { errno = EOPNOTSUPP; return -1; }
extern "C" int accept(int, sockaddr*, socklen_t*) { errno = EOPNOTSUPP; return -1; }

extern "C" int getpeername(int fd, sockaddr* address, socklen_t* length) {
    SocketState* state = state_for(fd);
    if (state == nullptr || !state->connected || address == nullptr || length == nullptr) {
        errno = ENOTCONN; return -1;
    }
    if (*length < sizeof(sockaddr_in)) { errno = EINVAL; return -1; }
    memcpy(address, &state->peer, sizeof(state->peer)); *length = sizeof(state->peer); return 0;
}

extern "C" int getsockname(int fd, sockaddr* address, socklen_t* length) {
    if (state_for(fd) == nullptr || address == nullptr || length == nullptr ||
        *length < sizeof(sockaddr_in)) { errno = EINVAL; return -1; }
    sockaddr_in local{}; local.sin_family = AF_INET;
    memcpy(address, &local, sizeof(local)); *length = sizeof(local); return 0;
}

extern "C" int inet_pton(int family, const char* text, void* output) {
    if (family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (text == nullptr || output == nullptr) return 0;
    uint8_t bytes[4]{}; unsigned part = 0; int index = 0; bool have_digit = false;
    for (const char* p = text;; ++p) {
        if (*p >= '0' && *p <= '9') {
            have_digit = true; part = part * 10u + (unsigned)(*p - '0');
            if (part > 255u) return 0;
        } else if (*p == '.' || *p == '\0') {
            if (!have_digit || index >= 4) return 0;
            bytes[index++] = (uint8_t)part; part = 0; have_digit = false;
            if (*p == '\0') break;
        } else return 0;
    }
    if (index != 4) return 0;
    memcpy(output, bytes, 4); return 1;
}

extern "C" int inet_aton(const char* text, in_addr* address) {
    return address != nullptr && inet_pton(AF_INET, text, &address->s_addr) == 1;
}
extern "C" in_addr_t inet_addr(const char* text) {
    in_addr result{}; return inet_aton(text, &result) ? result.s_addr : INADDR_NONE;
}

extern "C" const char* inet_ntop(int family, const void* address, char* output, socklen_t size) {
    if (family != AF_INET) { errno = EAFNOSUPPORT; return nullptr; }
    if (address == nullptr || output == nullptr || size < 16) { errno = ENOSPC; return nullptr; }
    const uint8_t* bytes = static_cast<const uint8_t*>(address);
    char* p = output;
    for (int i = 0; i < 4; ++i) {
        unsigned value = bytes[i];
        if (value >= 100) *p++ = (char)('0' + value / 100);
        if (value >= 10) *p++ = (char)('0' + (value / 10) % 10);
        *p++ = (char)('0' + value % 10);
        if (i != 3) *p++ = '.';
    }
    *p = '\0'; return output;
}

extern "C" int getaddrinfo(const char* node, const char* service,
                            const addrinfo* hints, addrinfo** output) {
    if (output == nullptr) return EAI_FAIL;
    *output = nullptr;
    if (hints != nullptr && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;
    if (hints != nullptr && hints->ai_socktype != 0 && hints->ai_socktype != SOCK_STREAM)
        return EAI_SOCKTYPE;
    uint16_t port = 0; int port_error = parse_port(service, port);
    if (port_error != 0) return port_error;
    uint8_t ip[4]{};
    if (node == nullptr) {
        if (hints != nullptr && (hints->ai_flags & AI_PASSIVE)) memset(ip, 0, 4);
        else { ip[0] = 127; ip[3] = 1; }
    } else if (inet_pton(AF_INET, node, ip) != 1) {
        if (hints != nullptr && (hints->ai_flags & AI_NUMERICHOST)) return EAI_NONAME;
        if (!usernet::dns::resolve_a(node, ip)) return EAI_AGAIN;
    }
    auto* allocation = static_cast<unsigned char*>(calloc(1, sizeof(addrinfo) + sizeof(sockaddr_in)));
    if (allocation == nullptr) return EAI_MEMORY;
    auto* info = reinterpret_cast<addrinfo*>(allocation);
    auto* address = reinterpret_cast<sockaddr_in*>(allocation + sizeof(addrinfo));
    address->sin_family = AF_INET; address->sin_port = htons(port);
    memcpy(&address->sin_addr.s_addr, ip, 4);
    info->ai_family = AF_INET; info->ai_socktype = SOCK_STREAM;
    info->ai_protocol = IPPROTO_TCP; info->ai_addrlen = sizeof(*address);
    info->ai_addr = reinterpret_cast<sockaddr*>(address);
    *output = info; return 0;
}

extern "C" void freeaddrinfo(addrinfo* info) { free(info); }
extern "C" const char* gai_strerror(int error) {
    switch (error) {
        case 0: return "success"; case EAI_NONAME: return "name not found";
        case EAI_AGAIN: return "temporary DNS failure"; case EAI_FAMILY: return "unsupported address family";
        case EAI_SOCKTYPE: return "unsupported socket type"; case EAI_SERVICE: return "unsupported service";
        case EAI_MEMORY: return "out of memory"; default: return "address resolution failed";
    }
}

extern "C" int getnameinfo(const sockaddr* generic, socklen_t address_len,
                            char* host, socklen_t host_len,
                            char* service, socklen_t service_len, int flags) {
    if (generic == nullptr || address_len < sizeof(sockaddr_in) ||
        generic->sa_family != AF_INET ||
        (flags & ~(NI_NUMERICHOST | NI_NUMERICSERV)) != 0)
        return EAI_FAMILY;
    const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(generic);
    if (host != nullptr && inet_ntop(AF_INET, &address->sin_addr.s_addr,
                                     host, host_len) == nullptr)
        return EAI_FAIL;
    if (service != nullptr) {
        unsigned port = ntohs(address->sin_port);
        char reverse[6]; size_t count = 0;
        do { reverse[count++] = (char)('0' + port % 10); port /= 10; } while (port);
        if (service_len <= count) return EAI_FAIL;
        size_t i; for (i = 0; i < count; ++i) service[i] = reverse[count - i - 1];
        service[count] = '\0';
    }
    return 0;
}
