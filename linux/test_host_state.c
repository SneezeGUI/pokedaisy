#include "host_state.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_text(const char *path, size_t *size) {
  return (char *)pd_state_read_file(path, size, PD_STATE_FILE_MAX);
}

static void expect_file(const char *path, const char *text) {
  size_t size;
  char *data = read_text(path, &size);
  assert(data && size == strlen(text) && !memcmp(data, text, size));
  free(data);
}

int main(void) {
  char dir[] = "/tmp/pd-linux-state-XXXXXX";
  assert(mkdtemp(dir));
  char states[PATH_MAX];
  snprintf(states, sizeof states, "%s/states", dir);
  assert(!mkdir(states, 0700));

  char one[PATH_MAX], two[PATH_MAX], ten[PATH_MAX], arm[PATH_MAX];
  assert(pd_state_slot_path(dir, "game.gb", "", 1, one, sizeof one));
  assert(pd_state_slot_path(dir, "game.gb", "", 2, two, sizeof two));
  assert(pd_state_slot_path(dir, "game.gb", "", 10, ten, sizeof ten));
  assert(pd_state_slot_path(dir, "game.gba", ".armgba", 1, arm, sizeof arm));
  assert(strstr(one, "/states/game.gb.ss"));
  assert(strstr(two, "/states/game.gb.slot2.ss"));
  assert(strstr(ten, "/states/game.gb.slot10.ss"));
  assert(strstr(arm, "/states/game.gba.armgba.ss"));
  assert(!pd_state_slot_path(dir, "game.gb", "", 0, one, sizeof one));
  assert(!pd_state_slot_path(dir, "game.gb", "", 11, one, sizeof one));
  assert(pd_state_slot_path(dir, "game.gb", "", 1, one, sizeof one));

  assert(pd_state_slot_save(one, "first", 5, "picture-a", 9));
  expect_file(one, "first");
  char thumb[PATH_MAX + 16], backup[PATH_MAX + 16], backupThumb[PATH_MAX + 24];
  assert(pd_state_sidecar(one, ".thumb", thumb, sizeof thumb));
  assert(pd_state_sidecar(one, ".bak", backup, sizeof backup));
  assert(pd_state_sidecar(one, ".bak.thumb", backupThumb, sizeof backupThumb));
  expect_file(thumb, "picture-a");

  assert(pd_state_slot_save(one, "second", 6, "picture-b", 9));
  expect_file(one, "second");
  expect_file(backup, "first");
  expect_file(backupThumb, "picture-a");
  assert(pd_state_undo_save(one));
  expect_file(one, "first");
  expect_file(thumb, "picture-a");
  assert(!pd_state_exists(backup) && !pd_state_exists(backupThumb));
  assert(!pd_state_undo_save(one));

  assert(pd_state_slot_save(two, "new", 3, NULL, 0));
  assert(pd_state_exists(two));
  assert(pd_state_undo_save(two));
  assert(!pd_state_exists(two) && !pd_state_exists(backup));

  /* A failed undo-backup cannot overwrite the current state. */
  assert(pd_state_slot_save(one, "safe", 4, NULL, 0));
  assert(!unlink(backup));
  assert(!mkdir(backup, 0700));
  assert(!pd_state_slot_save(one, "must-not-land", 13, NULL, 0));
  expect_file(one, "safe");
  assert(!rmdir(backup));

  assert(!pd_state_write_file("/definitely/not/a/pokedaisy/folder/state", "x", 1, true));
  assert(!pd_state_write_file(one, NULL, 1, true));
  setenv("POCKET_STATE_FSYNC_FAIL", "1", 1);
  assert(pd_state_write_file(one, "committed", 9, true)); // rename happened before the injected parent-fsync failure
  unsetenv("POCKET_STATE_FSYNC_FAIL");
  expect_file(one, "committed");
  assert(pd_state_write_file(one, "", 0, true));
  size_t emptySize = 99;
  unsigned char *empty = pd_state_read_file(one, &emptySize, PD_STATE_FILE_MAX);
  assert(empty && emptySize == 0);
  free(empty);

  unlink(ten);
  unlink(thumb);
  unlink(one);
  unlink(two);
  rmdir(states);
  rmdir(dir);
  puts("PASS legacy slot paths, atomic I/O, thumbnails, save undo and failed-backup preservation");
}
