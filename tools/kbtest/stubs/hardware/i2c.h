#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct i2c_inst i2c_inst_t;
#define i2c0 ((i2c_inst_t*)0)
#define i2c1 ((i2c_inst_t*)1)
#define GPIO_FUNC_I2C 3
uint32_t i2c_init(i2c_inst_t*, uint32_t);
int i2c_write_blocking(i2c_inst_t*, uint8_t, const uint8_t*, size_t, bool);
