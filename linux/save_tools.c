#include "save_tools.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "pd_fs.h"
#include "pd_save.h"

#ifdef _WIN32
#define dev_t _dev_t
#define ino_t _ino_t
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BACKUP_DIR "pokedaisy-backups"
#define MAX_WRITES 2

struct read_snapshot {
  char *path;
  dev_t dev;
  ino_t ino;
  uint8_t *bytes;
  size_t len;
};

struct pd_linux_save_tools {
  char *romDir, *saveDir, *backupDir;
  struct read_snapshot *reads;
  size_t readCount, readCap;
  uint8_t *rom;
  size_t romSize;
  pd_linux_save_text edit;
  void *editCtx;
  unsigned serial;
  struct pd_library_host host;
};

static unsigned next_serial(void) {
  static unsigned serial;
  return ++serial; // fresh adapters share one process-wide backup/temp namespace
}

static void errorf(char *out, size_t len, const char *fmt, ...) {
  if (!out || !len)
    return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(out, len, fmt, args);
  va_end(args);
}

static char *join_path(const char *dir, const char *file) {
  size_t d = strlen(dir), f = strlen(file);
  bool slash = d && dir[d - 1] != '/';
  char *out = malloc(d + f + (slash ? 2 : 1));
  if (out)
    snprintf(out, d + f + (slash ? 2 : 1), "%s%s%s", dir, slash ? "/" : "", file);
  return out;
}

static char *parent_dir(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash)
    return strdup(".");
  if (slash == path)
    return strdup("/");
  size_t len = (size_t)(slash - path);
  char *out = malloc(len + 1);
  if (out) {
    memcpy(out, path, len);
    out[len] = 0;
  }
  return out;
}

static char *rom_path(const struct pd_linux_save_tools *tools, const char *file) {
  if (!file || !*file)
    return NULL;
  if (file[0] == '/' || !tools->romDir)
    return strdup(file);
  return join_path(tools->romDir, file);
}

static bool save_path(const struct pd_linux_save_tools *tools, const char *file, char out[PATH_MAX]) {
  char *rom = rom_path(tools, file);
  if (!rom)
    return false;
  const char *name = strrchr(rom, '/');
  name = name ? name + 1 : rom;
  size_t needed = strlen(tools->saveDir) + strlen(name) + 6;
  if (needed > PATH_MAX) {
    free(rom);
    return false;
  }
  pd_save_path(tools->saveDir, rom, out, PATH_MAX);
  free(rom);
  return true;
}

static bool read_file(const char *path, size_t cap, uint8_t **out, size_t *len, struct stat *info) {
  *out = NULL;
  *len = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  struct stat st;
  bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && (uintmax_t)st.st_size <= cap;
  size_t size = ok ? (size_t)st.st_size : 0;
  uint8_t *bytes = ok ? malloc(size ? size : 1) : NULL;
  ok = ok && bytes != NULL;
  size_t at = 0;
  while (ok && at < size) {
    ssize_t got = pread(fd, bytes + at, size - at, (off_t)at);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0) {
      ok = false;
      break;
    }
    at += (size_t)got;
  }
  if (ok) {
    uint8_t extra;
    ssize_t got;
    do {
      got = pread(fd, &extra, 1, (off_t)size);
    } while (got < 0 && errno == EINTR);
    ok = got == 0;
  }
  if (close(fd))
    ok = false;
  if (!ok) {
    free(bytes);
    return false;
  }
  *out = bytes;
  *len = size;
  if (info)
    *info = st;
  return true;
}

static bool sync_parent(const char *path) {
#ifdef _WIN32
  (void)path;
  return true;
#else
  char *dir = parent_dir(path);
  if (!dir)
    return false;
  int fd = open(dir, O_RDONLY);
  free(dir);
  if (fd < 0)
    return false;
  bool ok = !fsync(fd);
  if (close(fd))
    ok = false;
  return ok;
#endif
}

static bool write_temp(const char *target, const char *kind, unsigned serial, int index, const uint8_t *bytes, size_t len,
                       mode_t mode, char **result) {
  *result = NULL;
  char *dir = parent_dir(target);
  if (!dir)
    return false;
  int fd = -1;
  char *path = NULL;
  for (unsigned attempt = 0; attempt < 100 && fd < 0; attempt++) {
    char name[128];
    snprintf(name, sizeof name, ".pokedaisy-save-tools-%ld-%u-%d-%u.%s", (long)getpid(), serial, index, attempt, kind);
    free(path);
    path = join_path(dir, name);
    if (path)
      fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 && errno != EEXIST) {
      free(path);
      path = NULL;
      break;
    }
  }
  free(dir);
  if (fd < 0) {
    free(path);
    return false;
  }
  size_t at = 0;
  bool ok = true;
  while (at < len) {
    ssize_t put = write(fd, bytes + at, len - at);
    if (put < 0 && errno == EINTR)
      continue;
    if (put <= 0) {
      ok = false;
      break;
    }
    at += (size_t)put;
  }
  if (fchmod(fd, mode & 0777) || fsync(fd))
    ok = false;
  if (close(fd))
    ok = false;
  uint8_t *check = NULL;
  size_t checkLen = 0;
  if (ok)
    ok = read_file(path, len, &check, &checkLen, NULL) && checkLen == len && !memcmp(check, bytes, len);
  free(check);
  if (!ok) {
    unlink(path);
    free(path);
    return false;
  }
  *result = path;
  return true;
}

static bool make_backup(const struct pd_linux_save_tools *tools, const char *save, const uint8_t *bytes, size_t len,
                        unsigned serial, int index) {
  char *dir;
  if (tools->backupDir) {
    dir = strdup(tools->backupDir);
  } else {
    char *parent = parent_dir(save);
    dir = parent ? join_path(parent, BACKUP_DIR) : NULL;
    free(parent);
  }
  if (!dir)
    return false;
  if (mkdir(dir, 0755) && errno != EEXIST) {
    free(dir);
    return false;
  }
  struct stat st;
  if (stat(dir, &st) || !S_ISDIR(st.st_mode)) {
    free(dir);
    return false;
  }
  const char *name = strrchr(save, '/');
  name = name ? name + 1 : save;
  const char *dot = strrchr(name, '.');
  size_t stemLen = dot && dot != name ? (size_t)(dot - name) : strlen(name);
  const char *ext = dot && dot != name ? dot + 1 : "sav";
  time_t now = time(NULL);
  struct tm tm;
  if (!localtime_r(&now, &tm)) {
    free(dir);
    return false;
  }
  char stamp[32];
  if (!strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm)) {
    free(dir);
    return false;
  }
  char file[NAME_MAX + 1], *path = NULL;
  for (unsigned attempt = 0; attempt < 100 && !path; attempt++) {
    int n = attempt ? snprintf(file, sizeof file, "%.*s.backup-%s-%ld-%u-%d-%u.%s", (int)stemLen, name, stamp,
                              (long)getpid(), serial, index, attempt, ext)
                    : snprintf(file, sizeof file, "%.*s.backup-%s-%ld-%u-%d.%s", (int)stemLen, name, stamp,
                              (long)getpid(), serial, index, ext);
    if (n < 0 || (size_t)n >= sizeof file) {
      free(dir);
      return false;
    }
    char *candidate = join_path(dir, file);
    if (!candidate) {
      free(dir);
      return false;
    }
    if (lstat(candidate, &st))
      path = candidate;
    else
      free(candidate);
  }
  free(dir);
  if (!path)
    return false;
  char *tmp = NULL;
  bool ok = write_temp(path, "backup", serial, index, bytes, len, 0600, &tmp);
  bool moved = false;
  if (ok) {
    moved = !pd_rename(tmp, path);
    ok = moved && sync_parent(path);
  }
  if (!ok && moved)
    unlink(path);
  if (tmp)
    unlink(tmp);
  free(tmp);
  free(path);
  return ok;
}

static struct read_snapshot *find_read(struct pd_linux_save_tools *tools, const char *path, const struct stat *st) {
  struct read_snapshot *sameInode = NULL;
  for (size_t i = 0; i < tools->readCount; i++) {
    struct read_snapshot *r = &tools->reads[i];
    if (!strcmp(r->path, path))
      return r;
    if (r->dev == st->st_dev && r->ino == st->st_ino)
      sameInode = r;
  }
  return sameInode;
}

static bool remember_read(struct pd_linux_save_tools *tools, const char *path, const struct stat *st, uint8_t *bytes,
                          size_t len) {
  struct read_snapshot *r = find_read(tools, path, st);
  if (!r && tools->readCount == PD_LIBRARY_MAX)
    return false;
  char *copy = strdup(path);
  if (!copy)
    return false;
  if (!r) {
    if (tools->readCount == tools->readCap) {
      size_t cap = tools->readCap ? tools->readCap * 2 : 8;
      struct read_snapshot *grown = realloc(tools->reads, cap * sizeof *grown);
      if (!grown) {
        free(copy);
        return false;
      }
      tools->reads = grown;
      tools->readCap = cap;
    }
    r = &tools->reads[tools->readCount++];
    memset(r, 0, sizeof *r);
  }
  free(r->path);
  free(r->bytes);
  r->path = copy;
  r->dev = st->st_dev;
  r->ino = st->st_ino;
  r->bytes = bytes;
  r->len = len;
  return true;
}

static long read_rom(void *ctx, const char *file, long offset, void *buf, long len) {
  struct pd_linux_save_tools *tools = ctx;
  if (!tools || offset < 0 || len < 0 || (len && !buf))
    return -1;
  char *path = rom_path(tools, file);
  if (!path)
    return -1;
  int fd = open(path, O_RDONLY);
  free(path);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    close(fd);
    return -1;
  }
  if (offset >= st.st_size || !len) {
    close(fd);
    return 0;
  }
  size_t want = (uintmax_t)len > (uintmax_t)(st.st_size - offset) ? (size_t)(st.st_size - offset) : (size_t)len;
  size_t at = 0;
  while (at < want) {
    ssize_t got = pread(fd, (uint8_t *)buf + at, want - at, (off_t)offset + (off_t)at);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0) {
      close(fd);
      return -1;
    }
    at += (size_t)got;
  }
  if (close(fd))
    return -1;
  return (long)at;
}

long pd_linux_save_tools_read_save(struct pd_linux_save_tools *tools, const char *file, uint8_t *buf, long max) {
  if (!tools || max < 0 || (max && !buf))
    return -1;
  char path[PATH_MAX];
  if (!save_path(tools, file, path))
    return -1;
  size_t cap = (uintmax_t)max < PD_SAVE_MAX_SIZE ? (size_t)max : PD_SAVE_MAX_SIZE;
  uint8_t *bytes = NULL;
  size_t len = 0;
  struct stat st;
  if (!read_file(path, cap, &bytes, &len, &st))
    return -1;
  if (!remember_read(tools, path, &st, bytes, len)) {
    free(bytes);
    return -1;
  }
  if (len)
    memcpy(buf, bytes, len);
  return (long)len;
}

static long read_save_cb(void *ctx, const char *file, uint8_t *buf, long max) {
  return pd_linux_save_tools_read_save(ctx, file, buf, max);
}

static const uint8_t *load_rom(void *ctx, const char *file, size_t *size) {
  struct pd_linux_save_tools *tools = ctx;
  if (size)
    *size = 0;
  if (!tools || !size)
    return NULL;
  char *path = rom_path(tools, file);
  if (!path)
    return NULL;
  uint8_t *bytes = NULL;
  size_t len = 0;
  bool ok = read_file(path, PD_LINUX_SAVE_TOOLS_ROM_MAX, &bytes, &len, NULL) && len > 0;
  free(path);
  if (!ok) {
    free(bytes);
    return NULL;
  }
  free(tools->rom);
  tools->rom = bytes;
  tools->romSize = len;
  *size = len;
  return tools->rom;
}

static bool edit_text(void *ctx, const char *hint, const char *initial, int max, char *out, size_t outLen) {
  struct pd_linux_save_tools *tools = ctx;
  if (!tools || !initial || max < 1 || !out || outLen < 2)
    return false;
  out[0] = 0;
  if (!tools->edit)
    return false;
  bool ok = tools->edit(tools->editCtx, hint, initial, max, out, outLen);
  out[outLen - 1] = 0;
  return ok && out[0] != 0;
}

static bool write_saves_cb(void *ctx, int n, const char *const *files, const uint8_t *const *bytes, const long *lens,
                           char *why, size_t whyLen) {
  return pd_linux_save_tools_write_saves(ctx, n, files, bytes, lens, why, whyLen);
}

struct pd_linux_save_tools *pd_linux_save_tools_create(const char *romDir, const char *saveDir, const char *backupDir) {
  if (!saveDir || !*saveDir)
    return NULL;
  struct pd_linux_save_tools *tools = calloc(1, sizeof *tools);
  if (!tools)
    return NULL;
  tools->saveDir = strdup(saveDir);
  if (romDir && *romDir)
    tools->romDir = strdup(romDir);
  if (backupDir && *backupDir)
    tools->backupDir = strdup(backupDir);
  if (!tools->saveDir || (romDir && *romDir && !tools->romDir) || (backupDir && *backupDir && !tools->backupDir)) {
    pd_linux_save_tools_close(tools);
    return NULL;
  }
  tools->host = (struct pd_library_host){.ctx = tools,
                                         .read_rom = read_rom,
                                         .read_save = read_save_cb,
                                         .write_saves = write_saves_cb,
                                         .load_rom = load_rom,
                                         .edit_text = edit_text};
  return tools;
}

void pd_linux_save_tools_close(struct pd_linux_save_tools *tools) {
  if (!tools)
    return;
  for (size_t i = 0; i < tools->readCount; i++) {
    free(tools->reads[i].path);
    free(tools->reads[i].bytes);
  }
  free(tools->reads);
  free(tools->rom);
  free(tools->romDir);
  free(tools->saveDir);
  free(tools->backupDir);
  free(tools);
}

struct pd_library_host *pd_linux_save_tools_host(struct pd_linux_save_tools *tools) {
  return tools ? &tools->host : NULL;
}

void pd_linux_save_tools_set_text(struct pd_linux_save_tools *tools, pd_linux_save_text edit, void *ctx) {
  if (!tools)
    return;
  tools->edit = edit;
  tools->editCtx = ctx;
}

struct pending_save {
  char path[PATH_MAX];
  struct stat originalInfo, writtenInfo;
  struct read_snapshot *snapshot;
  uint8_t *original, *updated;
  size_t len;
  char *oldTemp, *newTemp;
  bool committed;
};

static bool current_original(struct pending_save *p, const uint8_t *edited, size_t len, bool afterWrite) {
  uint8_t *current = NULL;
  size_t currentLen = 0;
  struct stat st, lst;
  bool ok = read_file(p->path, PD_SAVE_MAX_SIZE, &current, &currentLen, &st) && !lstat(p->path, &lst) &&
            S_ISREG(lst.st_mode) && lst.st_dev == st.st_dev && lst.st_ino == st.st_ino && currentLen == len &&
            !memcmp(current, afterWrite ? edited : p->original, len);
  if (ok && !afterWrite)
    ok = st.st_dev == p->originalInfo.st_dev && st.st_ino == p->originalInfo.st_ino;
  free(current);
  if (ok && afterWrite)
    p->writtenInfo = st;
  return ok;
}

bool pd_linux_save_tools_write_saves(struct pd_linux_save_tools *tools, int n, const char *const *files,
                                     const uint8_t *const *bytes, const long *lens, char *why, size_t whyLen) {
  if (why && whyLen)
    why[0] = 0;
  char error[192] = "";
  struct pending_save saves[MAX_WRITES];
  memset(saves, 0, sizeof saves);
  bool success = false, rollbackOk = true;
  if (!tools || n < 1 || n > MAX_WRITES || !files || !bytes || !lens) {
    errorf(error, sizeof error, "Save tools need one or two saves");
    goto out;
  }
  unsigned serial = next_serial();
  for (int i = 0; i < n; i++) {
    struct pending_save *p = &saves[i];
    if (!files[i] || !*files[i] || !bytes[i] || lens[i] <= 0 || lens[i] > PD_SAVE_MAX_SIZE ||
        !save_path(tools, files[i], p->path)) {
      errorf(error, sizeof error, "Invalid save data for %s", files[i] ? files[i] : "(null)");
      goto out;
    }
    struct stat lst;
    if (!read_file(p->path, PD_SAVE_MAX_SIZE, &p->original, &p->len, &p->originalInfo) || !p->len ||
        lstat(p->path, &lst) || !S_ISREG(lst.st_mode) || lst.st_dev != p->originalInfo.st_dev ||
        lst.st_ino != p->originalInfo.st_ino) {
      errorf(error, sizeof error, "Couldn't read the save for %s", files[i]);
      goto out;
    }
    for (int k = 0; k < i; k++)
      if (saves[k].originalInfo.st_dev == p->originalInfo.st_dev && saves[k].originalInfo.st_ino == p->originalInfo.st_ino) {
        errorf(error, sizeof error, "Both games use the same save file");
        goto out;
      }
    p->snapshot = find_read(tools, p->path, &p->originalInfo);
    if (!p->snapshot) {
      errorf(error, sizeof error, "Open the save for %s before writing it", files[i]);
      goto out;
    }
    if (p->snapshot->len != p->len || memcmp(p->snapshot->bytes, p->original, p->len)) {
      errorf(error, sizeof error, "%s changed since it was opened", files[i]);
      goto out;
    }
    if ((size_t)lens[i] != p->len) {
      errorf(error, sizeof error, "The edited save for %s has a different size", files[i]);
      goto out;
    }
    p->updated = malloc(p->len);
    if (!p->updated) {
      errorf(error, sizeof error, "Not enough memory to write %s", files[i]);
      goto out;
    }
    memcpy(p->updated, bytes[i], p->len);
  }
  for (int i = 0; i < n; i++) {
    struct pending_save *p = &saves[i];
    if (!make_backup(tools, p->path, p->original, p->len, serial, i)) {
      errorf(error, sizeof error, "Couldn't back up %s; nothing was changed", files[i]);
      goto out;
    }
    if (!write_temp(p->path, "old", serial, i, p->original, p->len, p->originalInfo.st_mode, &p->oldTemp) ||
        !write_temp(p->path, "new", serial, i, p->updated, p->len, p->originalInfo.st_mode, &p->newTemp) ||
        !sync_parent(p->path)) {
      errorf(error, sizeof error, "Couldn't stage %s; nothing was changed", files[i]);
      goto out;
    }
  }
  for (int i = 0; i < n; i++) {
    struct pending_save *p = &saves[i];
    if (!current_original(p, NULL, p->len, false)) {
      errorf(error, sizeof error, "%s changed while it was being written", files[i]);
      goto out;
    }
    if (pd_rename(p->newTemp, p->path)) {
      errorf(error, sizeof error, "Couldn't put %s in place", files[i]);
      goto out;
    }
    p->committed = true;
    if (!sync_parent(p->path) || !current_original(p, p->updated, p->len, true)) {
      errorf(error, sizeof error, "%s didn't read back as written", files[i]);
      goto out;
    }
  }
  for (int i = 0; i < n; i++) {
    struct pending_save *p = &saves[i];
    free(p->snapshot->bytes);
    p->snapshot->bytes = p->updated;
    p->snapshot->len = p->len;
    p->snapshot->dev = p->writtenInfo.st_dev;
    p->snapshot->ino = p->writtenInfo.st_ino;
    p->updated = NULL;
    unlink(p->oldTemp);
    p->committed = false;
  }
  success = true;
  goto out;

out:
  if (!success) {
    for (int i = n > 0 && n <= MAX_WRITES ? n - 1 : -1; i >= 0; i--) {
      struct pending_save *p = &saves[i];
      if (!p->committed)
        continue;
      if (!pd_rename(p->oldTemp, p->path)) {
        p->committed = false;
        sync_parent(p->path);
        if (!current_original(p, p->original, p->len, true))
          rollbackOk = false;
      } else {
        rollbackOk = false;
      }
    }
  }
  for (int i = 0; i < MAX_WRITES; i++) {
    struct pending_save *p = &saves[i];
    if (p->newTemp)
      unlink(p->newTemp);
    if (p->oldTemp && !p->committed)
      unlink(p->oldTemp);
    free(p->oldTemp);
    free(p->newTemp);
    free(p->original);
    free(p->updated);
  }
  if (!success && !rollbackOk && error[0]) {
    size_t len = strlen(error);
    snprintf(error + len, sizeof error - len, "; rollback failed, use the backups");
  }
  errorf(why, whyLen, "%s", error);
  return success;
}
