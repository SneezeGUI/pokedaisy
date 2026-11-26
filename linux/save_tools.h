/* Filesystem adapter for the portable Gen 3 save editor and trade tools. */
#ifndef PD_LINUX_SAVE_TOOLS_H
#define PD_LINUX_SAVE_TOOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pd_library.h"

#define PD_LINUX_SAVE_TOOLS_ROM_MAX (64u * 1024u * 1024u)

struct pd_linux_save_tools;

typedef bool (*pd_linux_save_text)(void *ctx, const char *hint, const char *initial, int max, char *out, size_t outLen);

/* romDir is used for relative ROM keys; NULL uses each key as its path. saveDir uses the same save naming and legacy
 * fallback as the emulator. backupDir NULL puts pokedaisy-backups beside each save. The returned host owns its ctx and
 * read_rom, read_save, write_saves, load_rom and edit_text callbacks; the caller may fill its other fields. */
struct pd_linux_save_tools *pd_linux_save_tools_create(const char *romDir, const char *saveDir, const char *backupDir);
void pd_linux_save_tools_close(struct pd_linux_save_tools *tools);
struct pd_library_host *pd_linux_save_tools_host(struct pd_linux_save_tools *tools);

/* The UI's modal text prompt. Without one, edit_text safely cancels. */
void pd_linux_save_tools_set_text(struct pd_linux_save_tools *tools, pd_linux_save_text edit, void *ctx);

/* Direct forms of the host callbacks. A save must be read through this adapter before it can be written. Each write
 * checks that the file still holds those read bytes, stages and verifies every output, and keeps a verified backup.
 * Two-save writes roll back earlier replacements if a later one fails. Call only while no emulator core has either save
 * open. These calls block and, like pd_library, are not thread-safe. */
long pd_linux_save_tools_read_save(struct pd_linux_save_tools *tools, const char *file, uint8_t *buf, long max);
bool pd_linux_save_tools_write_saves(struct pd_linux_save_tools *tools, int n, const char *const *files,
                                     const uint8_t *const *bytes, const long *lens, char *why, size_t whyLen);

#endif
