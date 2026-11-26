#include "save_tools_ui.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_state.h"
#include "library.h"
#include "pd_library_internal.h"
#include "pd_settings.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct pd_linux_save_tools_ui {
  struct pd_library library;
  struct pd_linux_save_tools *tools;
  struct pd_library_host *adapter, host;
  struct pd_linux_save_tools_ui_game *games;
  uint32_t *topPixels, *bottomPixels;
  int page, result;
  int touchX, touchY, touchW, touchH;
  char cachePath[PATH_MAX];
  void (*settingsChanged)(void *);
  void *settingsCtx;
  unsigned ticks, staleAt;
  char stalePath[PATH_MAX];
  bool staleDone, pickerReady;
};

static long read_rom(void *ctx, const char *file, long offset, void *buf, long len) {
  struct pd_linux_save_tools_ui *ui = ctx;
  return ui->adapter->read_rom(ui->adapter->ctx, file, offset, buf, len);
}
static long read_save(void *ctx, const char *file, uint8_t *buf, long max) {
  struct pd_linux_save_tools_ui *ui = ctx;
  return ui->adapter->read_save(ui->adapter->ctx, file, buf, max);
}
static const uint8_t *load_rom(void *ctx, const char *file, size_t *size) {
  struct pd_linux_save_tools_ui *ui = ctx;
  return ui->adapter->load_rom(ui->adapter->ctx, file, size);
}
static bool edit_text(void *ctx, const char *hint, const char *initial, int max, char *out, size_t outLen) {
  struct pd_linux_save_tools_ui *ui = ctx;
  return ui->adapter->edit_text(ui->adapter->ctx, hint, initial, max, out, outLen);
}

static bool read_card(void *ctx, const char *file, struct pd_game_card *out) {
  (void)ctx;
  (void)file;
  (void)out;
  return false; // the Linux emulator does not produce the 3DS session-card format
}

static void settings_changed(void *ctx) {
  struct pd_linux_save_tools_ui *ui = ctx;
  if (ui->settingsChanged)
    ui->settingsChanged(ui->settingsCtx);
}

static void cache_changed(void *ctx) {
  struct pd_linux_save_tools_ui *ui = ctx;
  size_t cap = (size_t)(ui->library.count + 1) * 450;
  char *text = malloc(cap);
  if (!text)
    return;
  size_t n = pd_library_cache_write(&ui->library, text, cap);
  pd_state_write_file(ui->cachePath, text, n, true);
  free(text);
}

static bool invalidate_resume(const char *rom, char *why, size_t whyLen, const char *dataDir) {
  const char *name = strrchr(rom, '/');
  name = name ? name + 1 : rom;
  static const char *const cores[] = {".armgba", ""};
  for (size_t i = 0; i < sizeof cores / sizeof *cores; i++) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/states/%s%s.fresh", dataDir, name, cores[i]);
    if (n < 0 || (size_t)n >= sizeof path || !pd_state_write_file(path, "fresh\n", 6, true)) {
      snprintf(why, whyLen, "Couldn't invalidate automatic resume for %s", name);
      return false;
    }
  }
  fprintf(stderr, "Save tools: automatic resume invalidated for %s\n", name);
  return true;
}

static bool write_saves(void *ctx, int n, const char *const *files, const uint8_t *const *bytes, const long *lens,
                        char *why, size_t whyLen) {
  struct pd_linux_save_tools_ui *ui = ctx;
  if (n < 1 || n > 2 || !files)
    return false;
  char dataDir[PATH_MAX];
  snprintf(dataDir, sizeof dataDir, "%s", ui->cachePath);
  char *slash = strrchr(dataDir, '/');
  if (slash)
    *slash = 0;
  /* Do this before replacing either save. A failed edit only forces a safe fresh boot; it cannot revive stale data. */
  for (int i = 0; i < n; i++)
    if (!files[i] || !invalidate_resume(files[i], why, whyLen, dataDir)) {
      fprintf(stderr, "Save tools: %s\n", why && why[0] ? why : "resume invalidation failed");
      return false;
    }
  bool ok = pd_linux_save_tools_write_saves(ui->tools, n, files, bytes, lens, why, whyLen);
  if (!ok)
    fprintf(stderr, "Save tools: %s\n", why && why[0] ? why : "write failed");
  else
    fprintf(stderr, "Save tools: wrote %d save%s\n", n, n == 1 ? "" : "s");
  return ok;
}

struct pd_linux_save_tools_ui *pd_linux_save_tools_ui_create(const char *dataDir, const char *saveDir,
                                                             struct pd_settings *settings,
                                                             const struct pd_linux_save_tools_ui_game *games, int count,
                                                             int page, const char *version, pd_linux_save_text edit,
                                                             void *editCtx, void (*settingsChanged)(void *),
                                                             void *settingsCtx) {
  if (!dataDir || !saveDir || !settings || count < 0 || count > PD_LIBRARY_MAX || (count && !games) ||
      (page != PD_PAGE_EDITOR && page != PD_PAGE_TRADE))
    return NULL;
  struct pd_linux_save_tools_ui *ui = calloc(1, sizeof *ui);
  if (!ui)
    return NULL;
  ui->page = page;
  ui->games = malloc((size_t)(count ? count : 1) * sizeof *ui->games);
  ui->topPixels = malloc(400 * 240 * sizeof *ui->topPixels);
  ui->bottomPixels = malloc(320 * 240 * sizeof *ui->bottomPixels);
  ui->tools = pd_linux_save_tools_create(NULL, saveDir, NULL);
  if (!ui->games || !ui->topPixels || !ui->bottomPixels || !ui->tools ||
      (games && count && memmove(ui->games, games, (size_t)count * sizeof *games) != ui->games)) {
    pd_linux_save_tools_ui_destroy(ui);
    return NULL;
  }
  ui->settingsChanged = settingsChanged;
  ui->settingsCtx = settingsCtx;
  snprintf(ui->cachePath, sizeof ui->cachePath, "%s/save-tools-cache.txt", dataDir);
  ui->adapter = pd_linux_save_tools_host(ui->tools);
  ui->host = *ui->adapter;
  ui->host.ctx = ui;
  ui->host.read_rom = read_rom;
  ui->host.read_save = read_save;
  ui->host.load_rom = load_rom;
  ui->host.edit_text = edit_text;
  ui->host.write_saves = write_saves;
  ui->host.read_card = read_card;
  ui->host.settings_changed = settings_changed;
  ui->host.cache_changed = cache_changed;
  ui->host.version = version;
  ui->host.devLine = NULL;
  pd_linux_save_tools_set_text(ui->tools, edit, editCtx);
  pd_library_init(&ui->library, settings, &ui->host);
  ui->library.count = count;
  for (int i = 0; i < count; i++) {
    struct pd_library_game *game = &ui->library.games[i];
    game->file = ui->games[i].file;
    game->size = ui->games[i].size;
    game->console = PD_CONSOLE_GBA;
    snprintf(game->title, sizeof game->title, "%s", ui->games[i].title ? ui->games[i].title : ui->games[i].file);
  }
  pd_library_refresh(&ui->library);
  size_t cacheSize = 0;
  unsigned char *cache = pd_state_read_file(ui->cachePath, &cacheSize, 1u << 20);
  if (cache) {
    char *text = malloc(cacheSize + 1);
    if (text) {
      memcpy(text, cache, cacheSize);
      text[cacheSize] = 0;
      pd_library_cache_read(&ui->library, text);
      free(text);
    }
    free(cache);
    /* A same-sized replacement is not the same ROM. Revalidate both header layout and content identity. */
    for (int i = 0; i < ui->library.count; i++) {
      ui->library.games[i].code[0] = 0;
      ui->library.games[i].sha1[0] = 0;
      ui->library.games[i].named = false;
    }
    pd_library_refresh(&ui->library);
  }
  ui->library.page = page;
  lib_saves_open(&ui->library, page);
  const char *stale = getenv("POCKET_SAVE_TOUCH");
  if (stale && sscanf(stale, "%u:%4095[^\n]", &ui->staleAt, ui->stalePath) != 2)
    ui->staleAt = 0, ui->stalePath[0] = 0;
  fprintf(stderr, "Save tools: %s opened\n", page == PD_PAGE_EDITOR ? "editor" : "trade");
  return ui;
}

void pd_linux_save_tools_ui_destroy(struct pd_linux_save_tools_ui *ui) {
  if (!ui)
    return;
  pd_linux_save_tools_close(ui->tools);
  free(ui->games);
  free(ui->topPixels);
  free(ui->bottomPixels);
  free(ui);
}

static void complete_pick(struct pd_linux_save_tools_ui *ui) {
  if (ui->library.picked >= 0) {
    ui->library.picked = -1;
    ui->result = lib_saves_unsaved(&ui->library) ? 3 : 1;
  }
}

bool pd_linux_save_tools_ui_confirm_close(struct pd_linux_save_tools_ui *ui) {
  if (!ui || !lib_saves_unsaved(&ui->library))
    return false;
  ui->library.quit = false;
  ui->library.page = ui->page;
  lib_saves_ask_quit(&ui->library);
  fprintf(stderr, "Save tools: close confirmation requested\n");
  return true;
}

void pd_linux_save_tools_ui_keys(struct pd_linux_save_tools_ui *ui, unsigned down) {
  if (!ui || !down)
    return;
  if (down & LIB_MENU) {
    ui->result = lib_saves_unsaved(&ui->library) ? 3 : 2;
    return;
  }
  if ((down & LIB_START) && pd_linux_save_tools_ui_confirm_close(ui))
    return;
  pd_library_keys(&ui->library, down & (LIB_A | LIB_B | LIB_X | LIB_Y | LIB_L | LIB_R | LIB_UP | LIB_DOWN | LIB_LEFT |
                                        LIB_RIGHT | LIB_START | LIB_SELECT));
  complete_pick(ui);
}

void pd_linux_save_tools_ui_pointer(struct pd_linux_save_tools_ui *ui, int x, int y, bool click, int wheel) {
  if (!ui)
    return;
  if (click && ui->touchW > 0 && x >= ui->touchX && y >= ui->touchY && x < ui->touchX + ui->touchW &&
      y < ui->touchY + ui->touchH) {
    int px = (x - ui->touchX) * 320 / ui->touchW, py = (y - ui->touchY) * 240 / ui->touchH;
    pd_library_touch(&ui->library, px, py, true);
    pd_library_touch(&ui->library, px, py, false);
    complete_pick(ui);
  }
  for (int i = 0; wheel && i < (wheel < 0 ? -wheel : wheel) && i < 20; i++)
    pd_linux_save_tools_ui_keys(ui, wheel > 0 ? LIB_UP : LIB_DOWN);
}

static void inject_stale(struct pd_linux_save_tools_ui *ui) {
  if (ui->staleDone || !ui->stalePath[0] || ui->ticks++ < ui->staleAt)
    return;
  ui->staleDone = true;
  FILE *f = fopen(ui->stalePath, "r+b");
  if (f) {
    int byte = fgetc(f);
    if (byte != EOF) {
      rewind(f);
      fputc(byte ^ 0x5A, f);
      fflush(f);
    }
    fclose(f);
    fprintf(stderr, "Save tools test: stale save injected\n");
  }
}

void pd_linux_save_tools_ui_tick(struct pd_linux_save_tools_ui *ui) {
  if (!ui)
    return;
  inject_stale(ui);
  pd_library_tick(&ui->library);
  if (!ui->pickerReady) {
    bool ready = true;
    for (int i = 0; i < ui->library.count; i++)
      ready &= ui->library.games[i].code[0] != 0;
    if (ready) {
      lib_saves_open(&ui->library, ui->page);
      ui->pickerReady = true;
      for (int i = 0; i < ui->library.count; i++)
        fprintf(stderr, "Save tools: %s header %s\n", ui->library.games[i].file, ui->library.games[i].code);
    }
  }
}

static void copy_scaled(struct pd_canvas *canvas, const uint32_t *pixels, int sw, int sh, int dx, int dy, int dw, int dh) {
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      int sx = x * sw / dw, sy = y * sh / dh;
      if (dx + x >= 0 && dy + y >= 0 && dx + x < canvas->w && dy + y < canvas->h)
        canvas->px[(dy + y) * canvas->w + dx + x] = 0xFF000000u | pixels[sy * sw + sx];
    }
}

void pd_linux_save_tools_ui_draw(struct pd_linux_save_tools_ui *ui, struct pd_canvas *canvas) {
  if (!ui)
    return;
  struct pd_canvas top, bottom;
  pd_canvas_init(&top, 400, 240, ui->topPixels);
  pd_canvas_init(&bottom, 320, 240, ui->bottomPixels);
  pd_library_draw(&ui->library, &bottom);
  pd_library_draw_top(&ui->library, &top);
  pd_fill(canvas, 0, 0, canvas->w, canvas->h, 0x181818);
  int scale = canvas->w / 400 < canvas->h / 480 ? canvas->w / 400 : canvas->h / 480;
  if (scale < 1)
    scale = 1;
  int dw = 400 * scale, dh = 480 * scale;
  if (dw > canvas->w || dh > canvas->h) {
    dw = canvas->w;
    dh = canvas->h;
  }
  int x = (canvas->w - dw) / 2, y = (canvas->h - dh) / 2;
  int topH = dh / 2;
  copy_scaled(canvas, ui->topPixels, 400, 240, x, y, dw, topH);
  ui->touchX = x;
  ui->touchY = y + topH;
  ui->touchW = dw;
  ui->touchH = dh - topH;
  copy_scaled(canvas, ui->bottomPixels, 320, 240, ui->touchX, ui->touchY, ui->touchW, ui->touchH);
}

bool pd_linux_save_tools_ui_quit(const struct pd_linux_save_tools_ui *ui) { return ui && ui->library.quit; }

int pd_linux_save_tools_ui_picked(struct pd_linux_save_tools_ui *ui) {
  if (!ui)
    return 0;
  int result = ui->result;
  ui->result = 0;
  return result;
}
