/**
 * @file net.h
 * @brief Minimal local TCP sockets (127.0.0.1 only) for tool channels such as the editor's command
 *        server. Nothing here is reachable from other machines.
 *
 * Listening sockets and the connections they accept are non-blocking: wpe_net_accept() and
 * wpe_net_receive() return immediately when nothing is pending, so a frame loop can poll them.
 * Connections made with wpe_net_connect() block, with a receive timeout.
 *
 * - Dependencies: core; the OS socket API (Winsock 2 on Windows)
 * - Thread affinity: a socket belongs to one thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_PLATFORM_NET_H
#define WPE_PLATFORM_NET_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"

typedef struct WPE_Socket WPE_Socket;

/**
 * Listens on 127.0.0.1:port.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_ALREADY_EXISTS (the port is in
 *         use; logged), WPE_RESULT_ERROR_OUT_OF_MEMORY or WPE_RESULT_ERROR_IO (logged).
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_net_listen(uint16_t port, WPE_Socket** out_listener);

/**
 * Accepts a pending connection, if any (non-blocking).
 *
 * @return WPE_RESULT_OK with *out_connection set; WPE_RESULT_ERROR_NOT_FOUND when nobody is waiting;
 *         WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_OUT_OF_MEMORY; WPE_RESULT_ERROR_IO.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_net_accept(WPE_Socket* listener, WPE_Socket** out_connection);

/**
 * Connects to 127.0.0.1:port (blocking), with `timeout_ms` for every later receive.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (nothing listens;
 *         not logged), WPE_RESULT_ERROR_OUT_OF_MEMORY or WPE_RESULT_ERROR_IO.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_net_connect(uint16_t port, uint32_t timeout_ms, WPE_Socket** out_connection);

/**
 * Sends all bytes (waiting while the OS buffer is full).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_IO (connection lost).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_net_send(WPE_Socket* socket, const void* data, size_t size);

/**
 * Receives what is available, up to `capacity` bytes. On non-blocking sockets, returns OK with 0
 * bytes when nothing has arrived; on connected (blocking) sockets, waits up to the timeout.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND (the peer closed the
 *         connection); WPE_RESULT_ERROR_IO (an error, or the timeout passed).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_net_receive(WPE_Socket* socket, void* buffer, size_t capacity, size_t* out_received);

/** Closes a socket (ownership consumed); NULL is a no-op. @thread_safety Owning thread */
void wpe_net_close(WPE_Socket* socket);

#endif /* WPE_PLATFORM_NET_H */
