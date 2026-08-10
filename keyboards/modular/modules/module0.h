/* Module 0 — the primary. 3x6 alphas plus a 3-key thumb cluster, 4 rows x 6. */
#pragma once
#define MATRIX_ROW_PINS { 4, 5, 6, 7 }
#define MATRIX_COL_PINS { 8, 9, 10, 11, 12, 13 }
#define SPLIT_MODULE_ROWS 4

/* A volume knob on the primary. A, B, push. */
#define NUM_LOCAL_ENCODERS 1
#define ENCODERS \
    ENCODER(26, 27, 28)

/*
 * Status display: a 128x64 SSD1306 on i2c1.
 *
 * GP2/GP3 is a valid i2c1 pair and is clear of everything else this module
 * claims — the matrix (GP4-15), the split bus (GP20/21) and the encoder
 * (GP26-28). Wire SDA to GP2, SCL to GP3, and the panel's VCC to 3V3.
 *
 * Set OLED_SH1106 if the picture comes up shifted two pixels with a wrapped
 * stripe down one edge; those panels are sold as SSD1306 constantly.
 */
#define OLED_ENABLE   1
#define OLED_I2C_INST i2c1
#define OLED_SDA_PIN  2
#define OLED_SCL_PIN  3
#define OLED_WIDTH    128
#define OLED_HEIGHT   64
#define OLED_TITLE    "MODULAR"
