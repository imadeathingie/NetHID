#pragma once

#include <stdint.h>

// Initialise and start the HTTP server on HTTP_PORT.
// Must be called after cyw43_arch_init() and lwIP are running,
// inside cyw43_arch_lwip_begin/end().
void web_init(void);

// Listener on `port` with TLS forced off, whatever the build says. Used by AP
// setup mode; see the note in src/ap_mode.cpp for why it cannot be HTTPS.
void web_init_plain(uint16_t port);
