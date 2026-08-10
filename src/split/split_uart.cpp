/*
 * split_uart.cpp — UART transport for the split link.
 *
 * Multi-drop: the primary's TX goes to every module's RX, and every module's TX
 * is tied together back to the primary's RX. Three conductors regardless of how
 * many modules are on the bus.
 *
 * That shared return line is why a module must idle its transmitter in high-Z.
 * Two modules driving it at once does not read as silence, it reads as
 * corrupted bytes — phantom keypresses rather than a missing module. The pin is
 * held as a plain input between transmissions and only handed to the UART for
 * the duration of a reply.
 *
 * Fit a pull-up on the shared line (10k at the primary is plenty). Between
 * transmissions nobody drives it, and a floating UART input reads noise as
 * start bits.
 *
 * Receive is a byte-at-a-time state machine driven from the poll loop rather
 * than an ISR. The link runs at a few hundred bytes per second and the FIFO is
 * 32 bytes deep, so an interrupt buys nothing here and would need locking
 * against the matrix scan on the same core.
 */

#include "split/split.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <string.h>

#ifndef SPLIT_UART_ID
#define SPLIT_UART_ID uart1        /* uart0 is the console on GP0/GP1 */
#endif
#ifndef SPLIT_UART_TX_PIN
#define SPLIT_UART_TX_PIN 20
#endif
#ifndef SPLIT_UART_RX_PIN
#define SPLIT_UART_RX_PIN 21
#endif
/*
 * 115200 is deliberately conservative. A short TRRS run will do 1 Mbaud
 * happily, but the failure mode of pushing it is corrupted rows that read as
 * phantom keypresses, and at 6 rows a full matrix packet is ~10 bytes — even a
 * 1 kHz scan is 10 kB/s, comfortably inside 115200's 11.5 kB/s. Raise it only
 * if you have measured a need.
 */
#ifndef SPLIT_UART_BAUD
#define SPLIT_UART_BAUD 115200
#endif

uint8_t split_crc8(const uint8_t *data, uint8_t len) {
    uint8_t c = 0;
    while (len--) {
        c ^= *data++;
        for (int i = 0; i < 8; i++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

/* True on a module: release the shared line whenever we are not replying. */
static bool tx_is_shared;

static void tx_release(void) {
    if (!tx_is_shared) return;
    gpio_set_function(SPLIT_UART_TX_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(SPLIT_UART_TX_PIN, GPIO_IN);
}

static void tx_take(void) {
    if (!tx_is_shared) return;
    gpio_set_function(SPLIT_UART_TX_PIN, GPIO_FUNC_UART);
}

bool split_link_init(bool listen_only) {
    tx_is_shared = listen_only;
    uart_init(SPLIT_UART_ID, SPLIT_UART_BAUD);
    gpio_set_function(SPLIT_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(SPLIT_UART_RX_PIN, GPIO_FUNC_UART);
    tx_release();
    uart_set_hw_flow(SPLIT_UART_ID, false, false);
    uart_set_format(SPLIT_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(SPLIT_UART_ID, true);
    return true;
}

bool split_link_send(const split_packet_t *pkt) {
    if (pkt->len > SPLIT_MAX_PAYLOAD) return false;

    uint8_t frame[4 + SPLIT_MAX_PAYLOAD + 1];
    frame[0] = SPLIT_SYNC;
    frame[1] = pkt->addr;
    frame[2] = pkt->type;
    frame[3] = pkt->len;
    memcpy(frame + 4, pkt->payload, pkt->len);
    frame[4 + pkt->len] = split_crc8(frame + 1, (uint8_t)(pkt->len + 3));

    /* Drop rather than block if the FIFO is full. The next packet carries the
     * complete state anyway, so a skipped one costs one scan of latency; a
     * blocking write on the scan loop costs every key on this half. */
    size_t total = (size_t)pkt->len + 5;
    if (!uart_is_writable(SPLIT_UART_ID)) return false;

    /* Drive, send, release. The write has to complete before the line is let go
     * or the tail of the frame is cut off mid-byte — uart_write_blocking()
     * returns once the data is in the FIFO, not once it is on the wire, so the
     * drain wait below is doing real work. */
    tx_take();
    uart_write_blocking(SPLIT_UART_ID, frame, total);
    if (tx_is_shared) {
        uart_tx_wait_blocking(SPLIT_UART_ID);
        tx_release();
    }
    return true;
}

/* Receive state machine. Anything unexpected resets to hunting for sync. */
static enum { RX_SYNC, RX_ADDR, RX_TYPE, RX_LEN, RX_DATA, RX_CRC } rx_state = RX_SYNC;
static split_packet_t rx_pkt;
static uint8_t rx_got;

bool split_link_recv(split_packet_t *out) {
    while (uart_is_readable(SPLIT_UART_ID)) {
        uint8_t b = uart_getc(SPLIT_UART_ID);
        switch (rx_state) {
        case RX_SYNC:
            if (b == SPLIT_SYNC) rx_state = RX_ADDR;
            break;
        case RX_ADDR:
            rx_pkt.addr = b;
            rx_state = RX_TYPE;
            break;
        case RX_TYPE:
            rx_pkt.type = b;
            rx_state = RX_LEN;
            break;
        case RX_LEN:
            if (b > SPLIT_MAX_PAYLOAD) { rx_state = RX_SYNC; break; }
            rx_pkt.len = b;
            rx_got = 0;
            rx_state = b ? RX_DATA : RX_CRC;
            break;
        case RX_DATA:
            rx_pkt.payload[rx_got++] = b;
            if (rx_got >= rx_pkt.len) rx_state = RX_CRC;
            break;
        case RX_CRC: {
            uint8_t buf[SPLIT_MAX_PAYLOAD + 3];
            buf[0] = rx_pkt.addr;
            buf[1] = rx_pkt.type;
            buf[2] = rx_pkt.len;
            memcpy(buf + 3, rx_pkt.payload, rx_pkt.len);
            rx_state = RX_SYNC;
            if (split_crc8(buf, (uint8_t)(rx_pkt.len + 3)) == b) {
                *out = rx_pkt;
                return true;
            }
            /* Bad CRC: discard silently and resynchronise. A corrupted matrix
             * row would otherwise arrive as a handful of phantom keypresses. */
            break;
        }
        }
    }
    return false;
}
