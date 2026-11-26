/* The portable Gen 3 save editor/trade session embedded in the Linux library. */
#ifndef PD_LINUX_SAVE_TOOLS_UI_H
#define PD_LINUX_SAVE_TOOLS_UI_H

#include <stdbool.h>

#include "pd_canvas.h"
#include "pd_library.h"
#include "save_tools.h"

struct pd_linux_save_tools_ui_game {
  const char *file;
  const char *title;
  long size;
};

struct pd_linux_save_tools_ui;

/* Only supported GBA games belong in this list; the normal library keeps its GB/DS entries for browsing. */
struct pd_linux_save_tools_ui *pd_linux_save_tools_ui_create(const char *dataDir, const char *saveDir,
                                                             struct pd_settings *settings,
                                                             const struct pd_linux_save_tools_ui_game *games, int count,
                                                             int page, const char *version, pd_linux_save_text edit,
                                                             void *editCtx, void (*settingsChanged)(void *),
                                                             void *settingsCtx);
void pd_linux_save_tools_ui_destroy(struct pd_linux_save_tools_ui *ui);
void pd_linux_save_tools_ui_keys(struct pd_linux_save_tools_ui *ui, unsigned down);
void pd_linux_save_tools_ui_pointer(struct pd_linux_save_tools_ui *ui, int x, int y, bool click, int wheel);
void pd_linux_save_tools_ui_tick(struct pd_linux_save_tools_ui *ui);
void pd_linux_save_tools_ui_draw(struct pd_linux_save_tools_ui *ui, struct pd_canvas *canvas);
bool pd_linux_save_tools_ui_confirm_close(struct pd_linux_save_tools_ui *ui);
bool pd_linux_save_tools_ui_quit(const struct pd_linux_save_tools_ui *ui);
/* 0 = still open, 1 = return to the normal game list, 2 = return to TOOLS, 3 = unsaved changes block leaving. */
int pd_linux_save_tools_ui_picked(struct pd_linux_save_tools_ui *ui);

#endif
