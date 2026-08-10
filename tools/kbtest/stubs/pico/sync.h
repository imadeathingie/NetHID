#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct spin_lock spin_lock_t;
uint32_t spin_lock_blocking(spin_lock_t*);
void spin_unlock(spin_lock_t*, uint32_t);
spin_lock_t* spin_lock_instance(unsigned);
int spin_lock_claim_unused(bool);
