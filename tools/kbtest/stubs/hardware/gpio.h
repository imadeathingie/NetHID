#pragma once
#include <stdint.h>
#include <stdbool.h>
enum gpio_dir { GPIO_IN=0, GPIO_OUT=1 };
void gpio_init(uint32_t); void gpio_set_dir(uint32_t,int); void gpio_put(uint32_t,bool);
void gpio_pull_up(uint32_t); void gpio_disable_pulls(uint32_t); uint32_t gpio_get_all(void);
bool gpio_get(uint32_t);
void gpio_set_function(uint32_t, uint32_t);
