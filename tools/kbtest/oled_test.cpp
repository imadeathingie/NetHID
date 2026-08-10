/*
 * oled_test — framebuffer, font and dirty-page flushing.
 *
 * The property that matters most is the last one. A full 128x64 flush is ~25 ms
 * of I2C; if a redraw that changes one line pushed all eight pages, the display
 * would cost eight times what it needs to, and on a module that time sits
 * directly in front of the primary's next poll.
 */
#include <stdio.h>
#include <string.h>
#include "oled/oled.h"
#include "oled/kb_status.h"
#include "hardware/i2c.h"

extern uint32_t fake_now_ms;

/* Count what actually reaches the wire. */
static int wire_writes;
static int wire_bytes;
uint32_t i2c_init(i2c_inst_t *, uint32_t) { return 0; }
int i2c_write_blocking(i2c_inst_t *, uint8_t, const uint8_t *, size_t n, bool) {
    wire_writes++;
    wire_bytes += (int)n;
    return (int)n;
}

static int fails = 0;
static void ck(const char *n, bool ok) {
    printf("%-46s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static int page_writes(void) {
    /* Data writes are 1 + OLED_WIDTH bytes; commands are 2. */
    return wire_bytes / (1 + OLED_WIDTH);
}

static void flush_all(void) {
    for (int i = 0; i < OLED_PAGES * 2 && oled_dirty(); i++) oled_task();
}

int main(void) {
    oled_init();
    flush_all();

    /* 1. A glyph lands where it was asked to. 'A' has a set pixel at its top
     *    row, second column, per the font art. */
    oled_clear(); flush_all();
    oled_text(0, 0, "A");
    const uint8_t *fb = oled_buffer();
    ck("text writes into the framebuffer", fb[1] != 0 || fb[2] != 0);

    /* 2. Clearing really clears. */
    oled_clear();
    bool blank = true;
    for (int i = 0; i < OLED_PAGES * OLED_WIDTH; i++) if (fb[i]) blank = false;
    ck("clear blanks the framebuffer", blank);

    /* 3. THE point: changing one line flushes one page, not eight. */
    flush_all();
    wire_writes = wire_bytes = 0;
    oled_text(0, 0, "HELLO");            /* row 0 -> page 0 only */
    flush_all();
    ck("a one-line change flushes one page", page_writes() == 1);

    /* 4. A redraw producing identical pixels costs nothing at all. This is what
     *    makes rendering on every pass of the main loop affordable. */
    wire_writes = wire_bytes = 0;
    oled_text(0, 0, "HELLO");
    flush_all();
    ck("an identical redraw sends nothing", page_writes() == 0 && !oled_dirty());

    /* 5. Text spanning two pages dirties exactly two. */
    flush_all();
    wire_writes = wire_bytes = 0;
    oled_text(0, 0, "X");
    oled_text(0, 40, "Y");
    flush_all();
    ck("two separated lines flush two pages", page_writes() == 2);

    /* 6. oled_task() must never send more than one page per call, or it is not
     *    safe to sit in a loop that has other work to do. */
    oled_clear();
    wire_writes = wire_bytes = 0;
    oled_task();
    ck("one call sends at most one page", page_writes() <= 1);

    /* 7. Drawing off-panel is clipped, not wrapped onto the opposite edge. */
    oled_clear(); flush_all();
    oled_pixel(-5, -5, true);
    oled_pixel(OLED_WIDTH + 10, OLED_HEIGHT + 10, true);
    blank = true;
    for (int i = 0; i < OLED_PAGES * OLED_WIDTH; i++) if (fb[i]) blank = false;
    ck("out-of-range pixels are clipped", blank);

    /* 8. The status pack/unpack pair is the wire format between the primary and
     *    every module; a mismatch there is a module showing the wrong layer. */
    { kb_status_t a = {}, b = {};
      a.layer = 3; a.mods = 0x22; a.flags = KB_ST_WIFI | KB_ST_USB;
      a.wpm = 77; a.modules = 0x0105;
      uint8_t buf[KB_STATUS_BYTES];
      kb_status_pack(&a, buf);
      kb_status_unpack(buf, &b);
      ck("status survives a round trip through the wire format",
         a.layer == b.layer && a.mods == b.mods && a.flags == b.flags &&
         a.wpm == b.wpm && a.modules == b.modules); }

    /* 9. Rendering the status screen puts something on the panel and does not
     *    run off the end of it. */
    { kb_status_t s = {};
      s.layer = 1; s.mods = 0x02; s.flags = KB_ST_USB | KB_ST_WIFI;
      s.wpm = 42; s.modules = 0x07;
      kb_status_set(&s);
      oled_clear();
      oled_render_status();
      int lit = 0;
      for (int i = 0; i < OLED_PAGES * OLED_WIDTH; i++) if (fb[i]) lit++;
      ck("status screen renders visible content", lit > 40); }

    printf("\n%s\n", fails ? "FAILURES" : "all green");
    return fails;
}
