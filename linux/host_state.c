#include "host_state.h"

#include "pd_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool path_result(int n, size_t size) { return n >= 0 && (size_t)n < size; }

bool pd_state_slot_path(const char *root, const char *romName, const char *coreSuffix, int slot, char *out, size_t size) {
  if (!root || !romName || !coreSuffix || slot < 1 || slot > PD_STATE_SLOTS)
    return false;
  int n = slot == 1 ? snprintf(out, size, "%s/states/%s%s.ss", root, romName, coreSuffix)
                    : snprintf(out, size, "%s/states/%s%s.slot%d.ss", root, romName, coreSuffix, slot);
  return path_result(n, size);
}

bool pd_state_sidecar(const char *path, const char *suffix, char *out, size_t size) {
  return path && suffix && path_result(snprintf(out, size, "%s%s", path, suffix), size);
}

bool pd_state_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

unsigned char *pd_state_read_file(const char *path, size_t *size, size_t max) {
  *size = 0;
  FILE *f = path ? fopen(path, "rb") : NULL;
  if (!f)
    return NULL;
  long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
  unsigned char *data = NULL;
  if (n >= 0 && (size_t)n <= max && !fseek(f, 0, SEEK_SET))
    data = malloc(n ? (size_t)n : 1);
  if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
    free(data);
    data = NULL;
  }
  if (fclose(f) != 0) {
    free(data);
    data = NULL;
  }
  if (data)
    *size = (size_t)n;
  return data;
}

static bool write_all(int fd, const unsigned char *data, size_t size) {
  while (size) {
    ssize_t n = write(fd, data, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    data += n;
    size -= (size_t)n;
  }
  return true;
}

static bool sync_parent(const char *path) {
#ifdef _WIN32
  (void)path;
  return true;
#else
  if (getenv("POCKET_STATE_FSYNC_FAIL")) {
    errno = EIO;
    return false;
  }
  char dir[PATH_MAX];
  const char *slash = strrchr(path, '/');
  int n = slash ? snprintf(dir, sizeof dir, "%.*s", slash == path ? 1 : (int)(slash - path), path)
                : snprintf(dir, sizeof dir, ".");
  if (!path_result(n, sizeof dir))
    return false;
  int fd = open(dir, O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return false;
  bool ok = fsync(fd) == 0;
  if (close(fd) != 0)
    ok = false;
  return ok;
#endif
}

bool pd_state_write_file(const char *path, const void *data, size_t size, bool sync) {
  static unsigned serial;
  if (!path || (size && !data) || size > PD_STATE_FILE_MAX)
    return false;
  char tmp[PATH_MAX + 64];
  int fd = -1;
  for (unsigned attempt = 0; attempt < 100 && fd < 0; attempt++) {
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%u", path, (long)getpid(), ++serial);
    if (!path_result(n, sizeof tmp))
      return false;
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 && errno != EEXIST)
      return false;
  }
  if (fd < 0)
    return false;
  bool ok = write_all(fd, data, size);
  if (ok && sync && fsync(fd) != 0)
    ok = false;
  if (close(fd) != 0)
    ok = false;
  if (ok)
    ok = pd_rename(tmp, path) == 0;
  if (!ok) {
    unlink(tmp);
    return false;
  }
  if (sync && !sync_parent(path))
    fprintf(stderr, "State published, but its parent directory could not be synced: %s\n", path);
  return true; // publication succeeded; a directory-sync failure must not be reported as "not written"
}

bool pd_state_copy_file(const char *from, const char *to, bool sync) {
  size_t size;
  unsigned char *data = pd_state_read_file(from, &size, PD_STATE_FILE_MAX);
  if (!data)
    return false;
  bool ok = pd_state_write_file(to, data, size, sync);
  free(data);
  return ok;
}

bool pd_state_remove_file(const char *path) { return path && (unlink(path) == 0 || errno == ENOENT); }

static bool backup_file(const char *from, const char *to) {
  return pd_state_exists(from) ? pd_state_copy_file(from, to, true) : pd_state_write_file(to, "", 0, true);
}

bool pd_state_slot_save(const char *slotPath, const void *state, size_t stateSize, const void *thumb, size_t thumbSize) {
  char backup[PATH_MAX + 32], thumbPath[PATH_MAX + 32], backupThumb[PATH_MAX + 40];
  if (!state || !stateSize || !pd_state_sidecar(slotPath, ".bak", backup, sizeof backup) ||
      !pd_state_sidecar(slotPath, ".thumb", thumbPath, sizeof thumbPath) ||
      !pd_state_sidecar(slotPath, ".bak.thumb", backupThumb, sizeof backupThumb))
    return false;
  /* Refuse the overwrite unless its one-level undo bundle was committed first. */
  if (!backup_file(slotPath, backup) || !backup_file(thumbPath, backupThumb))
    return false;
  if (!pd_state_write_file(slotPath, state, stateSize, true))
    return false;
  /* A picture is optional, but a failed replacement must not leave the old state's picture behind. */
  if ((!thumb || !thumbSize || !pd_state_write_file(thumbPath, thumb, thumbSize, true)) &&
      !pd_state_remove_file(thumbPath))
    return false;
  return true;
}

bool pd_state_undo_save(const char *slotPath) {
  char backup[PATH_MAX + 32], thumbPath[PATH_MAX + 32], backupThumb[PATH_MAX + 40];
  if (!pd_state_sidecar(slotPath, ".bak", backup, sizeof backup) ||
      !pd_state_sidecar(slotPath, ".thumb", thumbPath, sizeof thumbPath) ||
      !pd_state_sidecar(slotPath, ".bak.thumb", backupThumb, sizeof backupThumb) || !pd_state_exists(backup))
    return false;
  size_t size;
  unsigned char *state = pd_state_read_file(backup, &size, PD_STATE_FILE_MAX);
  if (!state)
    return false;
  bool ok = size ? pd_state_write_file(slotPath, state, size, true) : pd_state_remove_file(slotPath);
  free(state);
  if (!ok)
    return false;
  if (pd_state_exists(backupThumb)) {
    size_t thumbSize;
    unsigned char *thumb = pd_state_read_file(backupThumb, &thumbSize, PD_STATE_FILE_MAX);
    if (!thumb)
      return false;
    ok = thumbSize ? pd_state_write_file(thumbPath, thumb, thumbSize, true) : pd_state_remove_file(thumbPath);
    free(thumb);
    if (!ok)
      return false;
  } else if (!pd_state_remove_file(thumbPath))
    return false;
  pd_state_remove_file(backup);
  pd_state_remove_file(backupThumb);
  return true;
}
