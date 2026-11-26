/* Core-owned state bytes with host-owned atomic files, slots and one-level save undo. */
#ifndef POCKET_HOST_STATE_H
#define POCKET_HOST_STATE_H

#include <stdbool.h>
#include <stddef.h>

#define PD_STATE_SLOTS 10
#define PD_STATE_FILE_MAX (64u << 20)

/* Slot 1 is the legacy single-state path. coreSuffix is "" or ".armgba". */
bool pd_state_slot_path(const char *root, const char *romName, const char *coreSuffix, int slot, char *out, size_t size);
bool pd_state_sidecar(const char *path, const char *suffix, char *out, size_t size);
bool pd_state_exists(const char *path);
unsigned char *pd_state_read_file(const char *path, size_t *size, size_t max);
/* True once the destination is published. A later parent-fsync failure is logged, never misreported as "not written". */
bool pd_state_write_file(const char *path, const void *data, size_t size, bool sync);
bool pd_state_copy_file(const char *from, const char *to, bool sync);
bool pd_state_remove_file(const char *path);

/* Writes a numbered slot and its optional thumbnail, first preserving the previous bundle at .bak[.thumb]. */
bool pd_state_slot_save(const char *slotPath, const void *state, size_t stateSize, const void *thumb, size_t thumbSize);
/* Restores the bundle preserved by pd_state_slot_save. An empty backup means the slot did not exist. */
bool pd_state_undo_save(const char *slotPath);

#endif
