#pragma once

// Initialise and start the TCP socket server on TCP_PORT.
// Must be called after cyw43_arch_init() and lwIP are running,
// inside cyw43_arch_lwip_begin/end().
void server_init(void);
