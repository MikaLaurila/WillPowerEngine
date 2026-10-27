#include "wpe_test.h"

#include "platform/net.h"

/* Listens on the first free port of a small range (another program may hold one). */
static WPE_Socket* listen_somewhere(uint16_t* out_port) {
    wpe_log_set_stderr_enabled(false);
    for (uint16_t port = 47600; port < 47700; ++port) {
        WPE_Socket* listener = NULL;
        if (wpe_net_listen(port, &listener) == WPE_RESULT_OK) {
            wpe_log_set_stderr_enabled(true);
            *out_port = port;
            return listener;
        }
    }
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_MSG(false, "no free port in 47600..47699");
    return NULL;
}

/* Receives exactly `size` bytes from a non-blocking socket. */
static void receive_all(WPE_Socket* socket, char* buffer, size_t size) {
    size_t got = 0;
    for (int spins = 0; got < size && spins < 1000000; ++spins) {
        size_t n = 0;
        WPE_TEST_ASSERT_OK(wpe_net_receive(socket, buffer + got, size - got, &n));
        got += n;
    }
    WPE_TEST_ASSERT_EQ_UINT(size, got);
}

static void test_net_local_round_trip(void) {
    uint16_t port = 0;
    WPE_Socket* listener = listen_somewhere(&port);
    WPE_Socket* pending = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_net_accept(listener, &pending)); /* Nobody yet. */
    /* The port is taken now. */
    WPE_Socket* again = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS, wpe_net_listen(port, &again));
    wpe_log_set_stderr_enabled(true);

    WPE_Socket* client = NULL;
    WPE_TEST_ASSERT_OK(wpe_net_connect(port, 2000, &client));
    WPE_Socket* server = NULL;
    for (int spins = 0; !server && spins < 1000000; ++spins) {
        const WPE_Result result = wpe_net_accept(listener, &server);
        WPE_TEST_ASSERT(result == WPE_RESULT_OK || result == WPE_RESULT_ERROR_NOT_FOUND);
    }
    WPE_TEST_ASSERT(server != NULL);

    /* Nothing sent yet: the non-blocking side returns at once. */
    char buffer[64];
    size_t n = 99;
    WPE_TEST_ASSERT_OK(wpe_net_receive(server, buffer, sizeof buffer, &n));
    WPE_TEST_ASSERT_EQ_UINT(0, n);

    WPE_TEST_ASSERT_OK(wpe_net_send(client, "hello\n", 6));
    receive_all(server, buffer, 6);
    WPE_TEST_ASSERT(memcmp(buffer, "hello\n", 6) == 0);
    WPE_TEST_ASSERT_OK(wpe_net_send(server, "world", 5));
    size_t got = 0;
    while (got < 5) {
        WPE_TEST_ASSERT_OK(wpe_net_receive(client, buffer + got, sizeof buffer - got, &n)); /* Blocking. */
        got += n;
    }
    WPE_TEST_ASSERT(memcmp(buffer, "world", 5) == 0);

    /* The peer closing is reported. */
    wpe_net_close(client);
    WPE_Result result = WPE_RESULT_OK;
    for (int spins = 0; result == WPE_RESULT_OK && spins < 1000000; ++spins)
        result = wpe_net_receive(server, buffer, sizeof buffer, &n);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, result);
    wpe_net_close(server);
    wpe_net_close(listener);

    /* Nothing listens any more. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_net_connect(port, 100, &client));
    wpe_net_close(NULL);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_net_local_round_trip))
