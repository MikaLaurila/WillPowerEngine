#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* sockets, fcntl */
#endif
#define WPE_LOG_MODULE "net"
#include "platform/net.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET wpe__SocketHandle;
#define WPE__INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int wpe__SocketHandle;
#define WPE__INVALID_SOCKET (-1)
#endif

#include "core/alloc.h"
#include "core/log.h"

struct WPE_Socket {
    wpe__SocketHandle handle;
    bool blocking;
};

/* Winsock is reference-counted: every socket holds one reference. */
static bool wpe__net_start(void) {
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        wpe_log_error("Winsock initialization failed");
        return false;
    }
#endif
    return true;
}

static void wpe__net_stop(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}

static void wpe__close_handle(wpe__SocketHandle handle) {
#if defined(_WIN32)
    closesocket(handle);
#else
    close(handle);
#endif
}

static bool wpe__would_block(void) {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static bool wpe__set_nonblocking(wpe__SocketHandle handle) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(handle, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    return flags >= 0 && fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static struct sockaddr_in wpe__loopback(uint16_t port) {
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

/* Wraps an OS handle (consumed on failure). */
static WPE_Result wpe__wrap(wpe__SocketHandle handle, bool blocking, WPE_Socket** out) {
    WPE_Socket* s = wpe_alloc(sizeof *s);
    if (!s) {
        wpe__close_handle(handle);
        wpe__net_stop();
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    *s = (WPE_Socket){.handle = handle, .blocking = blocking};
    *out = s;
    return WPE_RESULT_OK;
}

WPE_Result wpe_net_listen(uint16_t port, WPE_Socket** out_listener) {
    if (!out_listener) return WPE_RESULT_ERROR_NULL_ARG;
    *out_listener = NULL;
    if (!wpe__net_start()) return WPE_RESULT_ERROR_IO;
    const wpe__SocketHandle handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == WPE__INVALID_SOCKET) {
        wpe__net_stop();
        wpe_log_error("cannot create a socket");
        return WPE_RESULT_ERROR_IO;
    }
#if !defined(_WIN32)
    /* Allow restarting right after a previous run (sockets linger in TIME_WAIT). On Windows this
     * option would let two processes share the port, so it is left off there. */
    const int reuse = 1;
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
    const struct sockaddr_in address = wpe__loopback(port);
    if (bind(handle, (const struct sockaddr*)&address, sizeof address) != 0) {
        wpe__close_handle(handle);
        wpe__net_stop();
        wpe_log_error("port %u is already in use", port);
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }
    if (listen(handle, 8) != 0 || !wpe__set_nonblocking(handle)) {
        wpe__close_handle(handle);
        wpe__net_stop();
        wpe_log_error("cannot listen on port %u", port);
        return WPE_RESULT_ERROR_IO;
    }
    return wpe__wrap(handle, false, out_listener);
}

WPE_Result wpe_net_accept(WPE_Socket* listener, WPE_Socket** out_connection) {
    if (!listener || !out_connection) return WPE_RESULT_ERROR_NULL_ARG;
    *out_connection = NULL;
    const wpe__SocketHandle handle = accept(listener->handle, NULL, NULL);
    if (handle == WPE__INVALID_SOCKET)
        return wpe__would_block() ? WPE_RESULT_ERROR_NOT_FOUND : WPE_RESULT_ERROR_IO;
    if (!wpe__set_nonblocking(handle) || !wpe__net_start()) {
        wpe__close_handle(handle);
        return WPE_RESULT_ERROR_IO;
    }
    return wpe__wrap(handle, false, out_connection);
}

WPE_Result wpe_net_connect(uint16_t port, uint32_t timeout_ms, WPE_Socket** out_connection) {
    if (!out_connection) return WPE_RESULT_ERROR_NULL_ARG;
    *out_connection = NULL;
    if (!wpe__net_start()) return WPE_RESULT_ERROR_IO;
    const wpe__SocketHandle handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == WPE__INVALID_SOCKET) {
        wpe__net_stop();
        return WPE_RESULT_ERROR_IO;
    }
    const struct sockaddr_in address = wpe__loopback(port);
    if (connect(handle, (const struct sockaddr*)&address, sizeof address) != 0) {
        wpe__close_handle(handle);
        wpe__net_stop();
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
#if defined(_WIN32)
    const DWORD timeout = timeout_ms;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);
#else
    const struct timeval timeout = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#endif
    return wpe__wrap(handle, true, out_connection);
}

WPE_Result wpe_net_send(WPE_Socket* socket_, const void* data, size_t size) {
    if (!socket_ || (!data && size > 0)) return WPE_RESULT_ERROR_NULL_ARG;
    const char* at = data;
    while (size > 0) {
        const int chunk = size > 1u << 20 ? 1 << 20 : (int)size;
#if defined(_WIN32)
        const int sent = send(socket_->handle, at, chunk, 0);
#else
        const ssize_t sent = send(socket_->handle, at, (size_t)chunk, MSG_NOSIGNAL);
#endif
        if (sent < 0) {
            if (wpe__would_block()) continue; /* Non-blocking socket with a full buffer: retry. */
            return WPE_RESULT_ERROR_IO;
        }
        at += sent;
        size -= (size_t)sent;
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_net_receive(WPE_Socket* socket_, void* buffer, size_t capacity, size_t* out_received) {
    if (!socket_ || !buffer || !out_received) return WPE_RESULT_ERROR_NULL_ARG;
    *out_received = 0;
    const int chunk = capacity > 1u << 20 ? 1 << 20 : (int)capacity;
#if defined(_WIN32)
    const int received = recv(socket_->handle, buffer, chunk, 0);
#else
    const ssize_t received = recv(socket_->handle, buffer, (size_t)chunk, 0);
#endif
    if (received == 0) return WPE_RESULT_ERROR_NOT_FOUND;
    if (received < 0) return !socket_->blocking && wpe__would_block() ? WPE_RESULT_OK : WPE_RESULT_ERROR_IO;
    *out_received = (size_t)received;
    return WPE_RESULT_OK;
}

void wpe_net_close(WPE_Socket* socket_) {
    if (!socket_) return;
    wpe__close_handle(socket_->handle);
    wpe_free(socket_);
    wpe__net_stop();
}
