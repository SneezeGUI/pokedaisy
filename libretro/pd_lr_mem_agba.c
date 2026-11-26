/* The companion's memory reader over armGBA: the same contract as pd_lr_mem.c's, read straight from the machine's
 * arrays so nothing a game could notice happens (a bus read costs cycles and moves the prefetcher). EWRAM, IWRAM and the
 * cartridge are what the companion reads; anything else says no. GPL-3.0, see ../NOTICE. */
#include "core.h"
#include "gba.h"
#include "cpu/jit.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool pd_lr_read_memory(void *ctx, uint32_t a, void *out, size_t n) {
  (void)ctx;
  if (!gba || !n)
    return false;
  const uint8_t *base;
  size_t size, off;
  switch (a >> 24) {
  case 0x02:
    base = gba->ewram;
    size = SIZE_EWRAM;
    off = (a & 0x00ffffffu) % SIZE_EWRAM;
    break;
  case 0x03:
    base = gba->iwram;
    size = SIZE_IWRAM;
    off = (a & 0x00ffffffu) % SIZE_IWRAM;
    break;
  case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
    base = gba->rom;
    size = gba->romSize;
    off = a & 0x01ffffffu;
    break;
  default:
    return false;
  }
  if (!base || off > size || n > size - off)
    return false;
  memcpy(out, base + off, n);
  return true;
}

bool pd_lr_write_memory(void *ctx, uint32_t a, const void *data, size_t n) {
  (void)ctx;
  if (!gba || (a >> 24 != 0x02 && a >> 24 != 0x03))
    return false;
  const uint8_t *bytes = data;
  for (size_t i = 0; i < n; i++)
    memoryStore8(gba, a + (uint32_t)i, bytes[i], NULL);
  uint8_t check[16];
  for (size_t i = 0; i < n; i += sizeof check) {
    size_t part = n - i < sizeof check ? n - i : sizeof check;
    if (!pd_lr_read_memory(ctx, a + (uint32_t)i, check, part) || memcmp(check, bytes + i, part))
      return false;
  }
  return true;
}

bool pd_lr_write_rom(void *ctx, uint32_t a, const void *data, size_t n) {
  (void)ctx;
  if (!gba || a < 0x08000000u)
    return false;
  size_t off = a & 0x01ffffffu;
  if (!gba->rom || off > gba->romSize || n > gba->romSize - off)
    return false;
  memcpy(gba->rom + off, data, n);
  jitStateLoaded(gba, false);
  return true;
}
