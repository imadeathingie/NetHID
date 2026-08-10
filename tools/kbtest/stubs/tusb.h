#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct __attribute__((packed)) { uint8_t modifier, reserved, keycode[6]; } hid_keyboard_report_t;
typedef struct __attribute__((packed)) { uint8_t buttons; int8_t x,y,wheel,pan; } hid_mouse_report_t;
typedef enum { HID_REPORT_TYPE_INVALID } hid_report_type_t;
bool tud_hid_n_report(uint8_t,uint8_t,void const*,uint16_t);
bool tud_mounted(void); bool tud_hid_n_ready(uint8_t); void tud_task(void); bool tud_remote_wakeup(void);
