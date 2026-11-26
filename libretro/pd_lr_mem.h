#ifndef PD_LR_MEM_H
#define PD_LR_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool pd_lr_read_memory(void* ctx, uint32_t addr, void* out, size_t len);
bool pd_lr_write_memory(void* ctx, uint32_t addr, const void* data, size_t len);
bool pd_lr_write_rom(void* ctx, uint32_t addr, const void* data, size_t len);

#endif
