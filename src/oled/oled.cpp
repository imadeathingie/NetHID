/* oled.cpp — see include/oled/oled.h */

#include "oled/oled.h"
#include "oled/oled_font.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <string.h>

#if OLED_ENABLE

#ifndef OLED_I2C_INST
#define OLED_I2C_INST i2c1
#endif
#ifndef OLED_SDA_PIN
#define OLED_SDA_PIN 2
#endif
#ifndef OLED_SCL_PIN
#define OLED_SCL_PIN 3
#endif

static uint8_t  fb[OLED_PAGES][OLED_WIDTH];
static uint8_t  dirty;                 /* bit per page */
static uint8_t  next_page;
static bool     ready;
static bool     blanked;
static uint32_t last_activity;

static void cmd(uint8_t c) {
    uint8_t buf[2] = { 0x00, c };      /* 0x00 = command stream */
    i2c_write_blocking(OLED_I2C_INST, OLED_I2C_ADDR, buf, 2, false);
}

bool oled_init(void) {
    i2c_init(OLED_I2C_INST, OLED_I2C_HZ);
    gpio_set_function(OLED_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA_PIN);
    gpio_pull_up(OLED_SCL_PIN);

    static const uint8_t init_seq[] = {
        0xAE,                    /* display off                       */
        0xD5, 0x80,              /* clock divide                      */
        0xA8, OLED_HEIGHT - 1,   /* multiplex ratio                   */
        0xD3, 0x00,              /* display offset                    */
        0x40,                    /* start line 0                      */
        0x8D, 0x14,              /* charge pump on                    */
        0x20, 0x00,              /* horizontal addressing             */
        0xA1,                    /* segment remap (flip horizontally) */
        0xC8,                    /* COM scan direction (flip vert)    */
#if OLED_HEIGHT == 32
        0xDA, 0x02,
#else
        0xDA, 0x12,              /* COM pin config                    */
#endif
        0x81, 0x8F,              /* contrast                          */
        0xD9, 0xF1,              /* precharge                         */
        0xDB, 0x40,              /* VCOM detect                       */
        0xA4,                    /* resume from RAM                   */
        0xA6,                    /* normal (not inverted)             */
        0xAF,                    /* display on                        */
    };
    for (unsigned i = 0; i < sizeof(init_seq); i++) cmd(init_seq[i]);

    memset(fb, 0, sizeof(fb));
    dirty = (uint8_t)((1u << OLED_PAGES) - 1);
    next_page = 0;
    ready = true;
    blanked = false;
    last_activity = to_ms_since_boot(get_absolute_time());
    return true;
}

void oled_activity(void) {
    last_activity = to_ms_since_boot(get_absolute_time());
    if (blanked) {
        blanked = false;
        cmd(0xAF);
        dirty = (uint8_t)((1u << OLED_PAGES) - 1);   /* redraw everything */
    }
}

void oled_task(void) {
    if (!ready) return;

#if OLED_TIMEOUT_MS
    if (!blanked &&
        (to_ms_since_boot(get_absolute_time()) - last_activity) > OLED_TIMEOUT_MS) {
        blanked = true;
        cmd(0xAE);                    /* panel off; framebuffer untouched */
        return;
    }
#endif
    if (blanked || !dirty) return;

    /* At most one page per call. Two would double the worst-case stall for no
     * benefit — the screen is not the thing anyone is waiting for. */
    for (int i = 0; i < OLED_PAGES; i++) {
        uint8_t p = (uint8_t)((next_page + i) % OLED_PAGES);
        if (!(dirty & (1u << p))) continue;

        cmd((uint8_t)(0xB0 | p));                          /* page address   */
        cmd((uint8_t)(0x00 | (OLED_COL_OFFSET & 0x0F)));   /* col low nibble */
        cmd((uint8_t)(0x10 | (OLED_COL_OFFSET >> 4)));     /* col high       */

        uint8_t buf[1 + OLED_WIDTH];
        buf[0] = 0x40;                                     /* data stream    */
        memcpy(buf + 1, fb[p], OLED_WIDTH);
        i2c_write_blocking(OLED_I2C_INST, OLED_I2C_ADDR, buf, sizeof(buf), false);

        dirty &= (uint8_t)~(1u << p);
        next_page = (uint8_t)((p + 1) % OLED_PAGES);
        return;
    }
}

bool oled_dirty(void) { return dirty != 0; }
const uint8_t *oled_buffer(void) { return &fb[0][0]; }

void oled_clear(void) {
    memset(fb, 0, sizeof(fb));
    dirty = (uint8_t)((1u << OLED_PAGES) - 1);
}

void oled_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t page = (uint8_t)(y / 8), bit = (uint8_t)(1u << (y % 8));
    uint8_t before = fb[page][x];
    if (on) fb[page][x] |= bit; else fb[page][x] &= (uint8_t)~bit;
    /* Mark dirty only on an actual change: a redraw that happens to produce the
     * same pixels costs no I2C at all, which is what makes a per-scan render
     * affordable. */
    if (fb[page][x] != before) dirty |= (uint8_t)(1u << page);
}

static void draw_char(int x, int y, char c, bool inv) {
    uint8_t idx = (uint8_t)c;
    if (idx < OLED_FONT_FIRST || idx > OLED_FONT_LAST) idx = ' ';
    const uint8_t *g = OLED_FONT[idx - OLED_FONT_FIRST];
    for (int col = 0; col < OLED_FONT_W + 1; col++) {
        uint8_t bits = (col < OLED_FONT_W) ? g[col] : 0;   /* +1 = spacing */
        for (int row = 0; row < 8; row++) {
            bool on = (bits >> row) & 1;
            oled_pixel(x + col, y + row, inv ? !on : on);
        }
    }
}

void oled_text(int x, int y, const char *s) {
    for (; *s; s++, x += OLED_FONT_W + 1) draw_char(x, y, *s, false);
}

void oled_text_inv(int x, int y, const char *s) {
    for (; *s; s++, x += OLED_FONT_W + 1) draw_char(x, y, *s, true);
}

void oled_rect(int x, int y, int w, int h, bool on) {
    for (int i = 0; i < w; i++) {
        oled_pixel(x + i, y, on);
        oled_pixel(x + i, y + h - 1, on);
    }
    for (int j = 0; j < h; j++) {
        oled_pixel(x, y + j, on);
        oled_pixel(x + w - 1, y + j, on);
    }
}

#else   /* no display on this board */

bool oled_init(void) { return false; }
void oled_task(void) {}
void oled_activity(void) {}
void oled_clear(void) {}
void oled_pixel(int, int, bool) {}
void oled_text(int, int, const char *) {}
void oled_text_inv(int, int, const char *) {}
void oled_rect(int, int, int, int, bool) {}
bool oled_dirty(void) { return false; }
const uint8_t *oled_buffer(void) { return 0; }

#endif
