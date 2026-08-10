#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct uart_inst uart_inst_t;
#define uart0 ((uart_inst_t*)0)
#define uart1 ((uart_inst_t*)1)
#define UART_PARITY_NONE 0
#define GPIO_FUNC_UART 2
uint32_t uart_init(uart_inst_t*, uint32_t);
void uart_set_hw_flow(uart_inst_t*, bool, bool);
void uart_set_format(uart_inst_t*, uint32_t, uint32_t, uint32_t);
void uart_set_fifo_enabled(uart_inst_t*, bool);
bool uart_is_writable(uart_inst_t*);
bool uart_is_readable(uart_inst_t*);
void uart_write_blocking(uart_inst_t*, const uint8_t*, size_t);
char uart_getc(uart_inst_t*);
void gpio_set_function(uint32_t, uint32_t);
void uart_tx_wait_blocking(uart_inst_t*);
void gpio_set_dir(uint32_t,int);
#define GPIO_FUNC_SIO 5
