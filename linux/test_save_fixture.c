/* Generate synthetic Gen 3 UI fixtures in a caller-owned temporary directory, never in a user's DATA folder. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pd_save.h"

#define CHECK(condition)                                                                                                  \
  do {                                                                                                                    \
    bool checkResult = (condition);                                                                                       \
    if (!checkResult) {                                                                                                   \
      fprintf(stderr, "Fixture check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);                         \
      exit(1);                                                                                                            \
    }                                                                                                                     \
  } while (0)

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, unsigned v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
  put16(p, v & 0xFFFF);
  put16(p + 2, v >> 16);
}
static void make_path(char *out, size_t len, const char *dir, const char *file) {
  int n = snprintf(out, len, "%s/%s", dir, file);
  CHECK(n > 0 && (size_t)n < len);
}
static void write_file(const char *path, const void *bytes, size_t len) {
  FILE *f = fopen(path, "wb");
  CHECK(f && fwrite(bytes, 1, len, f) == len && !fclose(f));
}
static int chunk_size(const struct pd_save_layout *f, int id) {
  int size = id == 0 ? f->block2Size : id <= 4 ? f->block1Size - (id - 1) * PD_SAVE_DATA
                                               : PD_SAVE_STORAGE - (id - 5) * PD_SAVE_DATA;
  if (id == 0)
    return size;
  if (size < 0)
    return 0;
  return size > PD_SAVE_DATA ? PD_SAVE_DATA : size;
}

static size_t synthetic_save(uint8_t *out, const char *code, uint32_t counter, long money, int species,
                             const char *nickname, bool rtc) {
  const struct pd_save_layout *f = pd_save_layout_for(code);
  CHECK(f && counter > 0 && species > 0 && species < PD_SAVE_SPECIES);
  size_t len = PD_SAVE_MIN_SIZE + (rtc ? 16 : 0);
  memset(out, 0xFF, len);
  for (size_t i = 28 * PD_SAVE_SECTOR; i < len; i++)
    out[i] = (uint8_t)(i * 7 + 3);
  uint8_t b2[PD_SAVE_BLOCK2_MAX] = {0}, b1[PD_SAVE_BLOCK1_MAX] = {0}, storage[PD_SAVE_STORAGE] = {0};
  uint32_t key = 0xA5A51234u;
  if (f->encryptionKeyOff >= 0)
    put32(b2 + f->encryptionKeyOff, key);
  else
    key = 0;
  put32(b1 + f->moneyOff, (uint32_t)money ^ key);
  if (f->linkRsFlag >= 0)
    b1[f->flagsOff + f->linkRsFlag / 8] |= (uint8_t)(1u << (f->linkRsFlag % 8));
  b2[f->nationalMagicOff] = (uint8_t)f->nationalMagic;
  put16(b1 + f->varsOff + (f->nationalVar - 0x4000) * 2, (unsigned)f->nationalVarValue);
  b1[f->flagsOff + f->nationalFlag / 8] |= (uint8_t)(1u << (f->nationalFlag % 8));
  b2[0x19] = 1;
  put16(b2 + 0x0A, 0x1234);
  put16(b2 + 0x0C, 0xA5A5);
  b1[f->partyCountOff] = 1;

  struct pd_box_mon mon;
  memset(&mon, 0, sizeof mon);
  put32(mon.header, 0x12345678u + (unsigned)species);
  put32(mon.header + 4, 0xA5A51234u);
  mon.header[0x12] = 2;
  mon.checksumOk = true;
  put16(mon.data, (unsigned)species);
  put32(mon.data + 4, pd_save_exp_for(species, 10));
  mon.data[9] = 70;
  put16(mon.data + 12, 98);
  uint8_t *party = b1 + f->partyOff;
  pd_box_mon_encode(&mon, party);
  party[0x54] = 10;
  put16(party + 0x56, 20);
  for (int i = 0; i < 6; i++)
    put16(party + 0x58 + i * 2, 20);
  pd_box_mon_encode(&mon, storage + 4); // after currentBox: box-to-box trades keep the party's last able Pokémon

  for (int slot = 0; slot < 2; slot++) {
    for (int id = 0; id < PD_SAVE_SLOT_SECTORS; id++) {
      int rotation = slot ? 3 : 0;
      int off = (slot * PD_SAVE_SLOT_SECTORS + (id + rotation) % PD_SAVE_SLOT_SECTORS) * PD_SAVE_SECTOR;
      memset(out + off, 0, PD_SAVE_SECTOR);
      int size = chunk_size(f, id);
      const uint8_t *chunk = id == 0 ? b2 : id <= 4 ? b1 + (id - 1) * PD_SAVE_DATA : storage + (id - 5) * PD_SAVE_DATA;
      memcpy(out + off, chunk, (size_t)size);
      uint32_t sum = 0;
      for (int i = 0; i < size / 4; i++)
        sum += get32(out + off + i * 4);
      put16(out + off + 0xFF4, (unsigned)id);
      put16(out + off + 0xFF6, (unsigned)((sum >> 16) + sum));
      put32(out + off + 0xFF8, 0x08012025u);
      put32(out + off + 0xFFC, counter - (unsigned)(1 - slot));
    }
  }
  struct pd_save save;
  CHECK(!pd_save_parse(&save, out, len, f));
  const char *mismatch = pd_save_mismatch(&save);
  if (mismatch)
    fprintf(stderr, "fixture mismatch: %s\n", mismatch);
  CHECK(!mismatch);
  CHECK(!pd_save_set_nickname(&save, PD_AT_PARTY(0), nickname));
  CHECK(!pd_save_build(&save, out));
  return len;
}

static void make_rom(const char *dir, const char *file, const char *code, bool ds) {
  uint8_t rom[1024] = {0};
  for (size_t i = 0; i < sizeof rom; i++)
    rom[i] = (uint8_t)(i * 13 + 7);
  memcpy(rom + 0xA0, "POKEMON", 7);
  memcpy(rom + (ds ? 0x0C : 0xAC), code, 4);
  char path[PATH_MAX];
  make_path(path, sizeof path, dir, file);
  write_file(path, rom, sizeof rom);
}

static void inspect(const char *path, const char *code) {
  uint8_t bytes[PD_SAVE_MAX_SIZE];
  FILE *f = fopen(path, "rb");
  CHECK(f);
  size_t len = fread(bytes, 1, sizeof bytes, f);
  CHECK(!fclose(f) && len >= PD_SAVE_MIN_SIZE);
  struct pd_save save;
  CHECK(!pd_save_parse(&save, bytes, len, pd_save_layout_for(code)));
  CHECK(!pd_save_mismatch(&save));
  struct pd_box_mon mon;
  CHECK(pd_save_record_at(&save, 0, &mon));
  char boxName[32];
  pd_save_box_name(&save, 0, boxName, sizeof boxName);
  printf("money=%ld\nbox=%d\nparty=%d\nboxName=%s\n", pd_save_money(&save), pd_box_mon_species(&mon),
         pd_save_party_count(&save), boxName);
}

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "--inspect")) {
    inspect(argv[2], argv[3]);
    return 0;
  }
  CHECK(argc == 2);
  char romDir[PATH_MAX], saveDir[PATH_MAX], stateDir[PATH_MAX];
  make_path(romDir, sizeof romDir, argv[1], "roms");
  make_path(saveDir, sizeof saveDir, argv[1], "saves");
  make_path(stateDir, sizeof stateDir, argv[1], "states");
  CHECK(!mkdir(romDir, 0755) && !mkdir(saveDir, 0755) && !mkdir(stateDir, 0755));
  make_rom(romDir, "first.gba", "BPEE", false);
  make_rom(romDir, "second.gba", "BPEE", false);
  make_rom(romDir, "unsupported.gb", "POKE", false);
  make_rom(romDir, "unsupported.nds", "ADA0", true);
  static uint8_t first[PD_SAVE_MAX_SIZE], second[PD_SAVE_MAX_SIZE];
  size_t firstLen = synthetic_save(first, "BPEE", 7, 3000, 252, "TREECKO", true);
  size_t secondLen = synthetic_save(second, "BPEE", 11, 4000, 255, "TORCHIC", false);
  char path[PATH_MAX];
  make_path(path, sizeof path, saveDir, "first.gba.sav");
  write_file(path, first, firstLen);
  make_path(path, sizeof path, saveDir, "second.gba.sav");
  write_file(path, second, secondLen);
  uint8_t unsupported[8192] = {0};
  make_path(path, sizeof path, saveDir, "unsupported.gb.sav");
  write_file(path, unsupported, sizeof unsupported);
  make_path(path, sizeof path, saveDir, "unsupported.nds.sav");
  write_file(path, unsupported, sizeof unsupported);
  make_path(path, sizeof path, stateDir, "first.gba.armgba.resume.ss");
  write_file(path, first, firstLen);
  make_path(path, sizeof path, stateDir, "first.gba.armgba.resume.sav");
  write_file(path, first, firstLen);
  printf("PASS synthetic editor/trade fixtures in %s\n", argv[1]);
  return 0;
}
