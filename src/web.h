#pragma once

// Initialise and start the HTTP server on HTTP_PORT.
// Must be called after cyw43_arch_init() and lwIP are running,
// inside cyw43_arch_lwip_begin/end().
void web_init(void);
