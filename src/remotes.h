// ============================================================
//  remotes.h — IR blaster + 433 MHz OOK, transmit and capture (PIO + edge IRQ).
//
//  Transmit is PIO-based (see below). Capture ("learn") is edge-timed on a GPIO
//  IRQ and gap-framed; it needs receiver modules (a TSOP-style IR demodulator
//  and a 433 MHz OOK receiver — see config.h pins). The send APIs take RAW
//  timing arrays in MICROSECONDS, which is exactly what capture produces and
//  what public IR/RF databases publish — so captured or imported codes replay
//  with no format change.
//
//  Default GPIO pins (override in config.h):
//    IR LED  -> GPIO 16   (drive through an NPN transistor; see README wiring)
//    433 RF  -> GPIO 17   (MX-FS-03V DATA; power the module from 5V for range)
// ============================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-time init: claims two PIO state machines and configures the pins.
// Safe to call once at startup (after stdio/clocks are up). Returns true on ok.
bool remotes_init(void);

// ── IR ──────────────────────────────────────────────────────────────────────
// Send a raw IR frame. `timings_us` alternates mark, space, mark, space, ...
// (microseconds), starting with a mark. `carrier_hz` is usually 38000 (use
// 36000/40000 for some protocols). `count` is the number of entries.
// Blocks until the frame has been queued to the PIO (a few ms at most).
void ir_send_raw(const uint16_t *timings_us, int count, int carrier_hz);

// Convenience: send a 32-bit NEC frame (address+command already encoded into
// the standard NEC bit pattern by the caller is NOT required — pass the raw
// 32-bit value as transmitted, LSB first per NEC). Generates the lead-in,
// 32 bits, and stop bit at 38 kHz. Many TVs use NEC, so codes published as
// hex work without capture.
void ir_send_nec(uint32_t nec_code);

// ── 433 MHz OOK ───────────────────────────────────────────────────────────────
// Send a raw OOK burst. `timings_us` alternates mark, gap, ... (microseconds),
// starting with a mark (carrier on). The whole frame is repeated `repeat`
// times (cheap remotes typically need 4–10 repeats to be reliable).
void rf_send_raw(const uint16_t *timings_us, int count, int repeat);

// ── Receive / "learn" ───────────────────────────────────────────────────────
// Needs receiver modules (see config.h pins). Capture is edge-timed and
// gap-framed: arm a channel, point the source remote at the receiver and press
// a button, then poll for the frame. The result is a raw mark/space (IR) or
// mark/gap (RF) array in microseconds — exactly the format ir_send_raw() /
// rf_send_raw() consume, so a captured frame replays with no conversion.
typedef enum { RX_IR = 0, RX_RF = 1 } rx_kind_t;

void remotes_rx_arm(rx_kind_t kind);            // begin capturing on a channel
bool remotes_rx_armed(rx_kind_t kind);          // still waiting for a frame?
int  remotes_rx_edges(rx_kind_t kind);          // edges recorded so far (diagnostic)
int  remotes_ir_raw_sample(int ms, int *transitions, int *low_seen);  // ISR-independent pin probe
void remotes_rx_poll(void);                     // call frequently from the main loop

// If a frame is ready, copy up to `max` timings into `out`, set *count, return
// true (and clear the ready flag). Otherwise return false.
bool remotes_rx_get(rx_kind_t kind, uint16_t *out, int max, int *count);

// Best-effort NEC decode of a captured IR frame -> 32-bit code (LSB-first).
// Returns true if the frame looks like NEC. Replay should still use the raw
// timings; the code is only a friendly label.
bool remotes_decode_nec(const uint16_t *t, int n, uint32_t *code_out);

#ifdef __cplusplus
}
#endif
