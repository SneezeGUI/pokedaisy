/* Real file and Gen 3 save round trips, all under a new temporary directory. */
#include "save_tools.h"

#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pd_save.h"

static int failNewRename;
typedef int (*rename_fn)(const char *, const char *);

int rename(const char *from, const char *to) {
  static rename_fn realRename;
  if (!realRename)
    realRename = (rename_fn)dlsym(RTLD_NEXT, "rename");
  assert(realRename);
  if (failNewRename && strstr(from, ".pokedaisy-save-tools-") && !strcmp(strrchr(from, '.'), ".new")) {
    if (!--failNewRename) {
      errno = EIO;
      return -1;
    }
  }
  return realRename(from, to);
}

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
  assert(n > 0 && (size_t)n < len);
}

static void write_file(const char *path, const void *bytes, size_t len) {
  FILE *f = fopen(path, "wb");
  assert(f && fwrite(bytes, 1, len, f) == len && !fclose(f));
}

static void assert_file(const char *path, const void *expected, size_t len) {
  FILE *f = fopen(path, "rb");
  assert(f);
  uint8_t *bytes = malloc(len ? len : 1);
  assert(bytes && fread(bytes, 1, len, f) == len && fgetc(f) == EOF && !fclose(f));
  assert(!memcmp(bytes, expected, len));
  free(bytes);
}

static void remove_tree(const char *path) {
  DIR *d = opendir(path);
  if (!d) {
    unlink(path);
    return;
  }
  for (struct dirent *e; (e = readdir(d));) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char child[PATH_MAX];
    make_path(child, sizeof child, path, e->d_name);
    struct stat st;
    assert(!lstat(child, &st));
    if (S_ISDIR(st.st_mode))
      remove_tree(child);
    else
      assert(!unlink(child));
  }
  assert(!closedir(d));
  assert(!rmdir(path));
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

static size_t synthetic_save(uint8_t *out, const char *code, uint32_t counter, long money, bool rtc) {
  const struct pd_save_layout *f = pd_save_layout_for(code);
  assert(f && counter > 0);
  size_t len = PD_SAVE_MIN_SIZE + (rtc ? 16 : 0);
  memset(out, 0xFF, len);
  for (size_t i = 28 * PD_SAVE_SECTOR; i < len; i++)
    out[i] = (uint8_t)(i * 7 + 3);
  uint8_t b2[PD_SAVE_BLOCK2_MAX] = {0}, b1[PD_SAVE_BLOCK1_MAX] = {0}, storage[PD_SAVE_STORAGE] = {0};
  uint32_t key = 0;
  if (f->encryptionKeyOff >= 0) {
    key = 0xA5A51234u;
    put32(b2 + f->encryptionKeyOff, key);
  }
  put32(b1 + f->moneyOff, (uint32_t)money ^ key);
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
  return len;
}

static void make_rom(const char *dir, const char *file, const char *code) {
  uint8_t rom[1024] = {0};
  for (size_t i = 0; i < sizeof rom; i++)
    rom[i] = (uint8_t)(i * 13 + 7);
  memcpy(rom + 0xA0, "POKEMON", 7);
  memcpy(rom + 0xAC, code, 4);
  char path[PATH_MAX];
  make_path(path, sizeof path, dir, file);
  write_file(path, rom, sizeof rom);
}

static int backup_count(const char *saveDir, const char *stem) {
  char dir[PATH_MAX], prefix[128];
  make_path(dir, sizeof dir, saveDir, "pokedaisy-backups");
  snprintf(prefix, sizeof prefix, "%s.backup-", stem);
  DIR *d = opendir(dir);
  int count = 0;
  for (struct dirent *e; d && (e = readdir(d));)
    count += !strncmp(e->d_name, prefix, strlen(prefix));
  if (d)
    closedir(d);
  return count;
}

static bool backup_has(const char *saveDir, const char *stem, const uint8_t *bytes, size_t len) {
  char dir[PATH_MAX], prefix[128];
  make_path(dir, sizeof dir, saveDir, "pokedaisy-backups");
  snprintf(prefix, sizeof prefix, "%s.backup-", stem);
  DIR *d = opendir(dir);
  assert(d);
  bool found = false;
  for (struct dirent *e; !found && (e = readdir(d));) {
    if (strncmp(e->d_name, prefix, strlen(prefix)))
      continue;
    char path[PATH_MAX];
    make_path(path, sizeof path, dir, e->d_name);
    FILE *f = fopen(path, "rb");
    assert(f);
    uint8_t *got = malloc(len);
    assert(got && fread(got, 1, len, f) == len && fgetc(f) == EOF && !fclose(f));
    found = !memcmp(got, bytes, len);
    free(got);
  }
  assert(!closedir(d));
  return found;
}

static int temp_count(const char *dir) {
  DIR *d = opendir(dir);
  assert(d);
  int count = 0;
  for (struct dirent *e; (e = readdir(d));)
    count += !strncmp(e->d_name, ".pokedaisy-save-tools-", 22);
  assert(!closedir(d));
  return count;
}

static void read_parsed(struct pd_library_host *host, const char *file, const char *code, struct pd_save *save,
                        long money) {
  static uint8_t bytes[PD_SAVE_MAX_SIZE];
  long len = host->read_save(host->ctx, file, bytes, sizeof bytes);
  assert(len >= PD_SAVE_MIN_SIZE);
  const char *why = pd_save_parse(save, bytes, (size_t)len, pd_save_layout_for(code));
  assert(!why);
  assert(!pd_save_mismatch(save));
  assert(pd_save_money(save) == money);
}

static void build_money(struct pd_save *save, long money, uint8_t *out) {
  assert(pd_save_set_money(save, money));
  const char *why = pd_save_build(save, out);
  assert(!why);
}

static bool text_answer(void *ctx, const char *hint, const char *initial, int max, char *out, size_t outLen) {
  int *calls = ctx;
  (*calls)++;
  assert(!strcmp(hint, "Nickname") && !strcmp(initial, "PIKA") && max == 10);
  if (*calls == 2)
    return false;
  snprintf(out, outLen, "DAISY");
  return true;
}

int main(void) {
  char root[] = "/tmp/pd-linux-save-tools-XXXXXX";
  assert(mkdtemp(root));
  char romDir[PATH_MAX], saveDir[PATH_MAX];
  make_path(romDir, sizeof romDir, root, "roms");
  make_path(saveDir, sizeof saveDir, root, "saves");
  assert(!mkdir(romDir, 0755) && !mkdir(saveDir, 0755));
  make_rom(romDir, "first.gba", "BPRE");
  make_rom(romDir, "second.gba", "BPEE");
  make_rom(romDir, "third.gba", "BPGE");

  static uint8_t originalA[PD_SAVE_MAX_SIZE], originalB[PD_SAVE_MAX_SIZE];
  size_t lenA = synthetic_save(originalA, "BPRE", 7, 3000, true);
  size_t lenB = synthetic_save(originalB, "BPEE", 11, 4000, false);
  char pathA[PATH_MAX], pathB[PATH_MAX];
  make_path(pathA, sizeof pathA, saveDir, "first.gba.sav");
  make_path(pathB, sizeof pathB, saveDir, "second.gba.sav");
  write_file(pathA, originalA, lenA);
  write_file(pathB, originalB, lenB);
  assert(!chmod(pathA, 0640));

  struct pd_linux_save_tools *tools = pd_linux_save_tools_create(romDir, saveDir, NULL);
  assert(tools);
  struct pd_library_host *host = pd_linux_save_tools_host(tools);
  assert(host && host->ctx == tools && host->read_rom && host->read_save && host->write_saves && host->load_rom &&
         host->edit_text);

  uint8_t header[8] = {0};
  assert(host->read_rom(host->ctx, "first.gba", 0xAC, header, 4) == 4 && !memcmp(header, "BPRE", 4));
  assert(host->read_rom(host->ctx, "first.gba", 1024, header, 4) == 0);
  assert(host->read_rom(host->ctx, "first.gba", -1, header, 4) == -1);
  assert(host->read_rom(host->ctx, "missing.gba", 0, header, 4) == -1);
  size_t romSize = 0;
  const uint8_t *rom = host->load_rom(host->ctx, "second.gba", &romSize);
  assert(rom && romSize == 1024 && !memcmp(rom + 0xAC, "BPEE", 4));
  rom = host->load_rom(host->ctx, "first.gba", &romSize);
  assert(rom && romSize == 1024 && !memcmp(rom + 0xAC, "BPRE", 4));

  uint8_t tiny[8];
  assert(host->read_save(host->ctx, "first.gba", tiny, sizeof tiny) == -1);
  assert(host->read_save(host->ctx, "missing.gba", tiny, sizeof tiny) == -1);
  char text[32] = "unchanged";
  assert(!host->edit_text(host->ctx, "Nickname", "PIKA", 10, text, sizeof text) && !text[0]);
  int textCalls = 0;
  pd_linux_save_tools_set_text(tools, text_answer, &textCalls);
  assert(host->edit_text(host->ctx, "Nickname", "PIKA", 10, text, sizeof text) && !strcmp(text, "DAISY"));
  assert(!host->edit_text(host->ctx, "Nickname", "PIKA", 10, text, sizeof text) && !text[0]);
  assert(textCalls == 2 && !host->edit_text(host->ctx, "Nickname", "PIKA", 10, text, 1));

  static struct pd_save saveA, saveB;
  static uint8_t editedA[PD_SAVE_MAX_SIZE], editedB[PD_SAVE_MAX_SIZE];
  read_parsed(host, "first.gba", "BPRE", &saveA, 3000);
  assert(pd_save_set_coins(&saveA, 42));
  build_money(&saveA, 123456, editedA);
  assert(!memcmp(editedA + PD_SAVE_MIN_SIZE, originalA + PD_SAVE_MIN_SIZE, 16));
  const char *oneFile[] = {"first.gba"};
  const uint8_t *oneBytes[] = {editedA};
  long oneLen[] = {(long)lenA};
  char why[192] = "old";
  assert(host->write_saves(host->ctx, 1, oneFile, oneBytes, oneLen, why, sizeof why) && !why[0]);
  assert_file(pathA, editedA, lenA);
  struct stat mode;
  assert(!stat(pathA, &mode) && (mode.st_mode & 0777) == 0640);
  assert(backup_count(saveDir, "first.gba") == 1 && backup_has(saveDir, "first.gba", originalA, lenA));

  static uint8_t secondEdit[PD_SAVE_MAX_SIZE];
  assert(!pd_save_parse(&saveA, editedA, lenA, pd_save_layout_for("BPRE")));
  build_money(&saveA, 234567, secondEdit);
  oneBytes[0] = secondEdit;
  assert(host->write_saves(host->ctx, 1, oneFile, oneBytes, oneLen, why, sizeof why));
  read_parsed(host, "first.gba", "BPRE", &saveA, 234567);
  assert(pd_save_coins(&saveA) == 42);
  assert(backup_count(saveDir, "first.gba") == 2 && backup_has(saveDir, "first.gba", editedA, lenA));

  read_parsed(host, "first.gba", "BPRE", &saveA, 234567);
  read_parsed(host, "second.gba", "BPEE", &saveB, 4000);
  build_money(&saveA, 1111, editedA);
  build_money(&saveB, 2222, editedB);
  const char *bothFiles[] = {"first.gba", "second.gba"};
  const uint8_t *bothBytes[] = {editedA, editedB};
  long bothLens[] = {(long)lenA, (long)lenB};
  assert(host->write_saves(host->ctx, 2, bothFiles, bothBytes, bothLens, why, sizeof why));
  read_parsed(host, "first.gba", "BPRE", &saveA, 1111);
  read_parsed(host, "second.gba", "BPEE", &saveB, 2222);
  assert(backup_count(saveDir, "second.gba") == 1 && backup_has(saveDir, "second.gba", originalB, lenB));

  static uint8_t beforeA[PD_SAVE_MAX_SIZE], beforeB[PD_SAVE_MAX_SIZE], nextA[PD_SAVE_MAX_SIZE], nextB[PD_SAVE_MAX_SIZE];
  memcpy(beforeA, editedA, lenA);
  memcpy(beforeB, editedB, lenB);
  build_money(&saveA, 5555, nextA);
  build_money(&saveB, 6666, nextB);
  originalB[28 * PD_SAVE_SECTOR + 9] ^= 0x5A;
  write_file(pathB, originalB, lenB);
  bothBytes[0] = nextA;
  bothBytes[1] = nextB;
  assert(!host->write_saves(host->ctx, 2, bothFiles, bothBytes, bothLens, why, sizeof why));
  assert(strstr(why, "changed") && backup_count(saveDir, "second.gba") == 1);
  assert_file(pathA, beforeA, lenA);
  assert_file(pathB, originalB, lenB);
  write_file(pathB, beforeB, lenB);

  read_parsed(host, "first.gba", "BPRE", &saveA, 1111);
  read_parsed(host, "second.gba", "BPEE", &saveB, 2222);
  build_money(&saveA, 3333, nextA);
  build_money(&saveB, 4444, nextB);
  failNewRename = 2;
  assert(!host->write_saves(host->ctx, 2, bothFiles, bothBytes, bothLens, why, sizeof why));
  failNewRename = 0;
  assert(strstr(why, "second.gba") && !strstr(why, "rollback failed"));
  assert_file(pathA, beforeA, lenA);
  assert_file(pathB, beforeB, lenB);
  assert(!temp_count(saveDir));
  assert(backup_count(saveDir, "first.gba") == 4 && backup_count(saveDir, "second.gba") == 2);
  assert(host->write_saves(host->ctx, 2, bothFiles, bothBytes, bothLens, why, sizeof why));
  assert_file(pathA, nextA, lenA);
  assert_file(pathB, nextB, lenB);

  static uint8_t unchanged[PD_SAVE_MAX_SIZE];
  memcpy(unchanged, nextA, lenA);
  struct pd_linux_save_tools *fresh = pd_linux_save_tools_create(romDir, saveDir, NULL);
  assert(fresh);
  struct pd_library_host *freshHost = pd_linux_save_tools_host(fresh);
  const uint8_t *freshBytes[] = {nextA};
  assert(!freshHost->write_saves(freshHost->ctx, 1, oneFile, freshBytes, oneLen, why, sizeof why));
  assert(strstr(why, "Open") && !strcmp(oneFile[0], "first.gba"));
  assert_file(pathA, unchanged, lenA);
  read_parsed(freshHost, "first.gba", "BPRE", &saveA, 3333);
  assert(freshHost->write_saves(freshHost->ctx, 1, oneFile, freshBytes, oneLen, why, sizeof why));
  assert(backup_count(saveDir, "first.gba") == 6); // a fresh adapter in the same second gets a distinct backup name
  pd_linux_save_tools_close(fresh);

  const char *duplicates[] = {"first.gba", "first.gba"};
  const uint8_t *duplicateBytes[] = {nextA, nextA};
  long duplicateLens[] = {(long)lenA, (long)lenA};
  assert(!host->write_saves(host->ctx, 2, duplicates, duplicateBytes, duplicateLens, why, sizeof why));
  assert(strstr(why, "same save") && !temp_count(saveDir));
  assert_file(pathA, unchanged, lenA);

  char blocked[PATH_MAX];
  make_path(blocked, sizeof blocked, root, "not-a-directory");
  write_file(blocked, "x", 1);
  struct pd_linux_save_tools *noBackup = pd_linux_save_tools_create(romDir, saveDir, blocked);
  assert(noBackup);
  struct pd_library_host *noBackupHost = pd_linux_save_tools_host(noBackup);
  read_parsed(noBackupHost, "first.gba", "BPRE", &saveA, 3333);
  build_money(&saveA, 7777, nextA);
  freshBytes[0] = nextA;
  assert(!noBackupHost->write_saves(noBackupHost->ctx, 1, oneFile, freshBytes, oneLen, why, sizeof why));
  assert(strstr(why, "back up") && !temp_count(saveDir));
  assert_file(pathA, unchanged, lenA);
  pd_linux_save_tools_close(noBackup);

  char legacyPath[PATH_MAX];
  make_path(legacyPath, sizeof legacyPath, saveDir, "third.sav");
  size_t legacyLen = synthetic_save(originalA, "BPGE", 3, 9000, false);
  write_file(legacyPath, originalA, legacyLen);
  read_parsed(host, "third.gba", "BPGE", &saveA, 9000);

  assert(!host->write_saves(host->ctx, 0, NULL, NULL, NULL, why, sizeof why));
  assert(!host->write_saves(host->ctx, 3, NULL, NULL, NULL, why, sizeof why));
  pd_linux_save_tools_close(tools);
  remove_tree(root);
  puts("PASS ROM/save/text callbacks, Gen 3 editor and trade round trips, backups, stale checks and rollback");
  return 0;
}
