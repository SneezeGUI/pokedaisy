/* PokeDaisy Linux: an SDL2 frontend for the PokeDaisy libretro cores. The companion, HUD, quick menu, FLY / PC /
 * REPEL / BIKE and EXPAND are the cores' own (libretro/, libretro/armgba/), so this host shows exactly what the
 * cores do on every other frontend. It loads the core for the ROM from beside the binary
 * (armGBA for .gba, mGBA for .gb / .gbc, DSperate for .nds), tells it the screen (the pokedaisy_panel option: the core then picks its canvas), shows its frame at a whole scale, and owns
 * what a frontend owns: input, sound, pacing, the battery save and its backups, save states, fast-forward,
 * rewind, the LCD mask and the game list. */
#include "libretro.h"
#include "../3ds/desktop/pd_png.h"
#include "display_filter.h"
#include "display_shader.h"
#include "pd_canvas.h"
#include "pd_menu.h"
#include "pd_style.h"
#include <SDL.h>
#include <dirent.h>
#ifdef _WIN32
#include "win32_dl.h"
#else
#include <dlfcn.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "pd_fs.h"
#include "pd_env.h"

#include "library.h"
#include "host_input.h"
#include "host_state.h"

/* The cores' file ending: dlopen'd .so objects on Linux, .dll on Windows. */
#ifdef _WIN32
#include "SDL_syswm.h"
#include <windows.h>
#define PD_CORE_EXT ".dll"
#define PD_HOST_NAME "PokeDaisy"
#else
#define PD_CORE_EXT ".so"
#define PD_HOST_NAME "PokeDaisy Linux"
#endif

// The window's size without POCKET_FULLSCREEN / POCKET_WINDOW: the app's handheld panel, 4:3.
#define W 512
#define H 384
#define OVERLAY_MAX_W 1024
#define OVERLAY_MAX_H 768
#define KEY_COLOR 0xFF00FFu // the overlay's see-through colour
#define OUT_RATE 48000
#define MODIFIER_TAP_FRAMES 6 // a tap of the MODIFIER alone reaches the game this long (Game Boy menus miss 1-frame presses)

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_AudioDeviceID audio;
static volatile sig_atomic_t running = 1;
static int appError;
static const char *root = ".";
/* plorpOS: --save FILE, the battery save another emulator keeps (its backups then go to DATA/backups);
 * NULL = DATA/saves/ROM.sav, or the save named after the ROM without its extension. */
static const char *saveFile;
static const char *importFile; // --import-save / --restore-save, applied after the pre-open backup
static char savesDir[PATH_MAX], exeDir[PATH_MAX];
static char message[128];
static unsigned messageUntil;

#ifdef _WIN32
/* The icon the executable carries (windows/pokedaisy.rc): the title bar, the taskbar and Alt-Tab take it here, so the
 * window does not depend on SDL picking the exe's own. */
static void set_window_icon(SDL_Window *win) {
  if (!win) return;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS || !info.info.win.window) return;
  HICON big = (HICON)LoadImageA(NULL, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                GetSystemMetrics(SM_CYICON), LR_SHARED);
  HICON small = (HICON)LoadImageA(NULL, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_SHARED);
  if (big) SendMessage(info.info.win.window, WM_SETICON, ICON_BIG, (LPARAM)big);
  if (small) SendMessage(info.info.win.window, WM_SETICON, ICON_SMALL, (LPARAM)small);
}
#endif

static void stop(int sig) {
  (void)sig;
  running = 0;
}
static void notice(const char *s) {
  snprintf(message, sizeof message, "%s", s);
  messageUntil = SDL_GetTicks() + 2500;
  fprintf(stderr, "%s\n", s);
}

/* ---- input ---- */
enum {
  A,
  B,
  SELECT,
  START,
  RIGHT,
  LEFT,
  UP,
  DOWN,
  R,
  L,
  X,
  Y,
  L2,
  R2,
  MENU,
  L3,
  SUP, // the left stick's directions apart (they're also the D-pad's)
  SDOWN,
  SLEFT,
  SRIGHT,
  R3,
  P1, // the back paddles (the Steam Deck's L4 / R4 / L5 / R5, an Elite pad's), when SDL is given them
  P2,
  P3,
  P4,
  FFKEY, // direct host actions: speed, rewind, states, undo and slot selection
  RWKEY,
  SAVEKEY,
  LOADKEY,
  UNSAVEKEY,
  UNLOADKEY,
  NEXTKEY,
  PREVKEY,
  NKEYS
};
static SDL_GameController *controller;
static bool rawTrimui;
static bool swapFace; // the host menu's A/B BUTTONS: SWAPPED (host.ini swap_ab)
/* The PANEL presets: the screen the core is told to lay its canvas out for. 0 is the window's own size. */
#define PD_PANEL_PRESETS 5
static const char *panel_preset(int panel) {
  static const char *const panels[PD_PANEL_PRESETS] = {NULL, "1280x720", "1280x800", "1920x1080", "2560x1440"};
  return panel > 0 && panel < PD_PANEL_PRESETS ? panels[panel] : NULL;
}
// A controller's face button, after the host menu's A/B BUTTONS (the keyboard's keys stay where they are).
static int face(int k) {
  if (!swapFace)
    return k;
  return k == A ? B : k == B ? A : k == X ? Y : k == Y ? X : k;
}
static uint64_t held, pressed;
static bool inputReset;
static unsigned repeatAt[NKEYS];
/* One logical action per keyboard row and controller control. The old key names and defaults remain valid; L3/R3,
 * undo and slot selection are new rows. The left stick stays a second D-pad, independent of the button map. */
#define KB_COUNT PD_ACT_REMAP_COUNT
static const int actionTargets[PD_ACT_COUNT] = {
    A, B, X, Y, L, R, L2, R2, L3, R3, START, SELECT, UP, DOWN, LEFT, RIGHT, MENU, FFKEY, RWKEY, SAVEKEY, LOADKEY,
    UNSAVEKEY, UNLOADKEY, NEXTKEY, PREVKEY, P1, P2, P3, P4};
static int kbKeys[KB_COUNT] = {
    SDLK_z, SDLK_x, SDLK_TAB, SDLK_f, SDLK_q, SDLK_e, SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_RETURN,
    SDLK_BACKSPACE, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_ESCAPE, SDLK_SPACE, SDLK_r, SDLK_F2,
    SDLK_F4, SDLK_F3, SDLK_F6, SDLK_RIGHTBRACKET, SDLK_LEFTBRACKET};
static int kbCapture = -1;
static int lastKey;
static int keyboard(SDL_Keycode k) {
  for (int i = 0; i < KB_COUNT; i++)
    if (kbKeys[i] && kbKeys[i] == (int)k)
      return actionTargets[i];
  return -1;
}
static char kbRows[KB_COUNT][56];
static const char *kbPtrs[KB_COUNT];
static struct pd_menu kbMenu;
static void kb_menu_build(void) {
  for (int i = 0; i < KB_COUNT; i++) {
    const char *name = kbKeys[i] ? SDL_GetKeyName((SDL_Keycode)kbKeys[i]) : "OFF";
    snprintf(kbRows[i], sizeof kbRows[i], "%s  %s", pd_pad_action_label(i), kbCapture == i ? "..." : name);
    kbPtrs[i] = kbRows[i];
  }
}
static void kb_menu_open(void) {
  kb_menu_build();
  pd_menu_init(&kbMenu, "KEYBOARD", kbPtrs, KB_COUNT);
  kbMenu.overlay = true;
}
/* POCKET_FACE_LABELS=1 keeps SDL's face labels; otherwise positions follow a Nintendo pad. A/B BUTTONS is still a
 * compatibility swap after this map, so existing host.ini files behave as they did. */
static bool faceLabels;
static struct pd_pad_map padMap;
static int padCapture = -1, lastPadControl = -1;
static char padRows[PD_ACT_REMAP_COUNT][64];
static const char *padPtrs[PD_ACT_REMAP_COUNT];
static struct pd_menu padMenu;
static void pad_menu_build(void) {
  for (int i = 0; i < PD_ACT_REMAP_COUNT; i++) {
    char name[40];
    pd_pad_control_label(pd_pad_control_for(&padMap, i), name, sizeof name);
    snprintf(padRows[i], sizeof padRows[i], "%s  %s", pd_pad_action_label(i), padCapture == i ? "..." : name);
    padPtrs[i] = padRows[i];
  }
}
static void pad_menu_open(void) {
  pad_menu_build();
  pd_menu_init(&padMenu, "CONTROLLER", padPtrs, PD_ACT_REMAP_COUNT);
  padMenu.overlay = true;
}
static void key(int k, bool isDown);
static void pad_control(int control, bool isDown) {
  int action = pd_pad_action(&padMap, control);
  if (action >= 0)
    key(face(actionTargets[action]), isDown);
}
static int joy(int n) {
  static const int map[] = {B, A, Y, X, L, R, SELECT, START, MENU, -1, L2, R2};
  return n >= 0 && n < 12 ? map[n] : -1;
}
/* A direction is held by the D-pad (or the keyboard, the hat) and by the left stick apart: letting go of one keeps it
 * held while the other still has it. */
static unsigned padHeld;  // directions the D-pad, keyboard or hat hold
static bool fromStick;    // key() called by stick_dir
static void input_mapping_reset(void) { held = pressed = padHeld = 0; }
static int stick_of(int dir) { return dir == UP ? SUP : dir == DOWN ? SDOWN : dir == LEFT ? SLEFT : dir == RIGHT ? SRIGHT : -1; }
static void key(int k, bool isDown) {
  if (k < 0)
    return;
  uint64_t bit = 1ull << k;
  int stick = stick_of(k);
  if (stick >= 0) {
    if (fromStick) {
      if (!isDown && (padHeld & bit))
        return; // the D-pad still holds it
    } else {
      padHeld = isDown ? padHeld | (unsigned)bit : padHeld & ~(unsigned)bit;
      if (!isDown && (held & (1ull << stick)))
        return; // the stick still holds it
    }
  }
  if (isDown) {
    if (!(held & bit)) {
      pressed |= bit;
      repeatAt[k] = SDL_GetTicks() + 400;
    }
    held |= bit;
  } else
    held &= ~bit;
  if (stick_of(k) >= 0 && getenv("POCKET_INPUT_DEBUG"))
    fprintf(stderr, "Input: padHeld=%x held=%llx\n", padHeld, (unsigned long long)held);
}
/* The left stick is the D-pad too, but only when it crosses the threshold: a stick at rest still sends small axis
 * events (the Steam Deck's, all the time), and releasing the direction on each of them let go of a held D-pad. */
static void stick_dir(int stickKey, int dir, bool on) {
  if (on == ((held & (1u << stickKey)) != 0))
    return;
  key(stickKey, on);
  fromStick = true;
  key(dir, on);
  fromStick = false;
}
/* Controllers come and go (a USB or Bluetooth pad, a handheld whose built-in one shows up late): SDL reports the
 * ones it has a mapping for as game controllers (its own database, DATA/gamecontrollerdb.txt, or
 * SDL_GAMECONTROLLERCONFIG, which PortMaster sets). The first one is used; an unmapped device is named in the log
 * so a mapping can be added. */
static void controller_added(int index) {
  if (!SDL_IsGameController(index)) {
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof guid);
    fprintf(stderr,
            "Controller without a mapping: %s (%s); add one to "
            "DATA/gamecontrollerdb.txt or SDL_GAMECONTROLLERCONFIG\n",
            SDL_JoystickNameForIndex(index) ? SDL_JoystickNameForIndex(index) : "?", guid);
    return;
  }
  if (controller)
    return;
  controller = SDL_GameControllerOpen(index);
  if (controller)
    fprintf(stderr, "Controller: %s\n", SDL_GameControllerName(controller));
}
static void controller_removed(SDL_JoystickID id) {
  if (!controller || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) != id)
    return;
  SDL_GameControllerClose(controller);
  controller = NULL;
  inputReset = true;
  held = padHeld = 0; // nothing stays pressed on a pad that's gone
  for (int i = 0; i < SDL_NumJoysticks() && !controller; i++)
    if (SDL_IsGameController(i))
      controller_added(i);
}
/* The pointer (a touch screen, or the mouse): where on the core's frame, for RETRO_DEVICE_POINTER. */
static SDL_Rect frameRect;
/* A core's native touch panel: the bottom screen at 256 x 192, scaled straight to its output rectangle. */
static struct pd_touch_panel panelMsg;
static bool panelPending, panelActive;
static SDL_Texture *panelTexture;
static int panelTexW, panelTexH;
/* A core's field HUD at its own resolution, drawn over its box at the window's pixels (a ZOOM step scales the canvas). */
static struct pd_hud hudMsg;
static bool hudPending, hudActive, hudHave; // hudHave: this frame sent one (a frame shown again keeps the last)
static SDL_Texture *hudTexture;
static int hudTexW, hudTexH;
static int pointerX, pointerY;
static bool pointerDown;
static void script_mouse(void);
static bool pointerHeld; // the left button is down (pointerDown is that, held for at least a frame)
static int pointerPulse;
static int wheelSteps;     // the wheel's notches this frame (+ up), for RETRO_DEVICE_MOUSE
static int mouseWindowX, mouseWindowY; // the pointer over the window, for the library (the cores get it normalised)
static bool libraryClick;              // a left press this frame
static int libraryWheel;               // wheel notches this frame (+ up)

static void pointer_at(int x, int y) {
  if (frameRect.w <= 0 || frameRect.h <= 0)
    return;
  int px = (int)((long)(x - frameRect.x) * 0xFFFE / frameRect.w) - 0x7FFF;
  int py = (int)((long)(y - frameRect.y) * 0xFFFE / frameRect.h) - 0x7FFF;
  pointerX = px < -0x7FFF ? -0x7FFF : px > 0x7FFF ? 0x7FFF : px;
  pointerY = py < -0x7FFF ? -0x7FFF : py > 0x7FFF ? 0x7FFF : py;
}
static bool audioLost, outputChanged;
static struct {
  bool active, accepted;
  int max;
  char hint[64], text[64];
} textPrompt;
static int textScript = -1;
static int text_characters(const char *text) {
  int count = 0;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    count += (*p & 0xC0) != 0x80;
  return count;
}
static void script_text(void) {
  if (!textPrompt.active || textScript >= 0)
    return;
  const char *text = getenv("POCKET_TEXT");
  bool cancel = getenv("POCKET_TEXT_CANCEL") != NULL;
  if (!text && !cancel)
    return;
  textScript = 0;
  if (text) {
    SDL_Event e = {0};
    e.type = SDL_TEXTINPUT;
    e.text.type = SDL_TEXTINPUT;
    snprintf(e.text.text, sizeof e.text.text, "%s", text);
    SDL_PushEvent(&e);
  }
  SDL_Event e = {0};
  e.type = SDL_KEYDOWN;
  e.key.type = SDL_KEYDOWN;
  e.key.state = SDL_PRESSED;
  e.key.keysym.sym = cancel ? SDLK_ESCAPE : SDLK_RETURN;
  SDL_PushEvent(&e);
}
static int key_named(const char *s);
/* POCKET_KEYS="TICK:KEY[:HOLD],...": presses scripted by loop tick (each events() call), held HOLD ticks (2), for
 * tests: KEY is a key name (a, b, up, menu, start, l3, r2 ...: keyNames). */
static struct {
  unsigned at, hold;
  int key;
} script[256];
static int scriptN = -1;
static unsigned tick;
static void key(int k, bool isDown);
static void script_resize(void);
static void script_keys(void) {
  if (scriptN < 0) {
    scriptN = 0;
    char buf[1024], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_KEYS") ? getenv("POCKET_KEYS") : "");
    for (char *t = strtok_r(buf, ",", &save); t && scriptN < 256; t = strtok_r(NULL, ",", &save)) {
      char name[16];
      unsigned at, hold = 2;
      if (sscanf(t, "%u:%15[^:]:%u", &at, name, &hold) >= 2 && key_named(name) >= 0)
        script[scriptN++] = (typeof(script[0])){at, hold, key_named(name)};
    }
  }
  for (int i = 0; i < scriptN; i++) {
    if (tick == script[i].at)
      key(script[i].key, true);
    if (tick == script[i].at + script[i].hold)
      key(script[i].key, false);
  }
  tick++;
}
/* POCKET_RAWKEY="TICK:KEYCODE[:HOLD]", a real SDL key event at that tick: the KEYBOARD page's capture, tested. */
static struct {
  unsigned at, hold;
  int key;
} rawscript[16];
static int rawscriptN = -1;
static void script_rawkeys(void) {
  if (rawscriptN < 0) {
    rawscriptN = 0;
    char buf[256], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_RAWKEY") ? getenv("POCKET_RAWKEY") : "");
    for (char *t = strtok_r(buf, ",", &save); t && rawscriptN < 16; t = strtok_r(NULL, ",", &save)) {
      unsigned at, hold = 2;
      int code;
      if (sscanf(t, "%u:%d:%u", &at, &code, &hold) >= 2)
        rawscript[rawscriptN++] = (typeof(rawscript[0])){at, hold, code};
    }
  }
  for (int i = 0; i < rawscriptN; i++) {
    if (tick == rawscript[i].at || tick == rawscript[i].at + rawscript[i].hold) {
      SDL_Event e = {0};
      e.type = tick == rawscript[i].at ? SDL_KEYDOWN : SDL_KEYUP;
      e.key.type = e.type;
      e.key.state = tick == rawscript[i].at ? SDL_PRESSED : SDL_RELEASED;
      e.key.keysym.sym = (SDL_Keycode)rawscript[i].key;
      SDL_PushEvent(&e);
    }
  }
}
/* POCKET_PAD="TICK:CONTROL[:HOLD]": real SDL controller button / trigger events for remapping tests. */
static struct {
  unsigned at, hold;
  int control;
} padscript[16];
static int padscriptN = -1;
static void script_rawpads(void) {
  if (padscriptN < 0) {
    padscriptN = 0;
    char buf[256], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_PAD") ? getenv("POCKET_PAD") : "");
    for (char *t = strtok_r(buf, ",", &save); t && padscriptN < 16; t = strtok_r(NULL, ",", &save)) {
      unsigned at, hold = 2;
      int control;
      if (sscanf(t, "%u:%d:%u", &at, &control, &hold) >= 2 && pd_pad_control_valid(control))
        padscript[padscriptN++] = (typeof(padscript[0])){at, hold, control};
    }
  }
  for (int i = 0; i < padscriptN; i++)
    if (tick == padscript[i].at || tick == padscript[i].at + padscript[i].hold) {
      bool downNow = tick == padscript[i].at;
      SDL_Event e = {0};
      if (padscript[i].control < SDL_CONTROLLER_BUTTON_MAX) {
        e.type = downNow ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        e.cbutton.type = e.type;
        e.cbutton.state = downNow ? SDL_PRESSED : SDL_RELEASED;
        e.cbutton.button = (Uint8)padscript[i].control;
      } else {
        e.type = SDL_CONTROLLERAXISMOTION;
        e.caxis.type = e.type;
        e.caxis.axis = (Uint8)(padscript[i].control == PD_PAD_TRIGGER_LEFT ? SDL_CONTROLLER_AXIS_TRIGGERLEFT
                                                                          : SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        e.caxis.value = downNow ? 32767 : 0;
      }
      SDL_PushEvent(&e);
    }
}
/* POCKET_STICK="TICK:AXIS:VALUE:HOLD" and POCKET_CLOSE="TICK": focused SDL input/window-event fixtures. */
static struct {
  unsigned at, hold;
  int axis, value;
} stickScript[8];
static int stickScriptN = -1, closeAt = -2;
static void script_stick_close(void) {
  if (stickScriptN < 0) {
    stickScriptN = 0;
    char buf[256], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_STICK") ? getenv("POCKET_STICK") : "");
    for (char *t = strtok_r(buf, ",", &save); t && stickScriptN < 8; t = strtok_r(NULL, ",", &save)) {
      unsigned at, hold;
      int axis, value;
      if (sscanf(t, "%u:%d:%d:%u", &at, &axis, &value, &hold) == 4)
        stickScript[stickScriptN++] = (typeof(stickScript[0])){at, hold, axis, value};
    }
    if (getenv("POCKET_CLOSE"))
      closeAt = atoi(getenv("POCKET_CLOSE"));
  }
  for (int i = 0; i < stickScriptN; i++)
    if (tick == stickScript[i].at || tick == stickScript[i].at + stickScript[i].hold) {
      SDL_Event e = {0};
      e.type = SDL_CONTROLLERAXISMOTION;
      e.caxis.type = e.type;
      e.caxis.axis = (Uint8)stickScript[i].axis;
      e.caxis.value = (Sint16)(tick == stickScript[i].at ? stickScript[i].value : 0);
      SDL_PushEvent(&e);
    }
  if (closeAt >= 0 && (unsigned)closeAt == tick) {
    SDL_Event e = {0};
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
    closeAt = -1;
  }
}
static void events(void) {
  pressed = 0;
  lastKey = 0;
  lastPadControl = -1;
  script_stick_close();
  bool modal = textPrompt.active;
  if (modal)
    script_text();
  else {
    script_rawkeys();
    script_rawpads();
  }
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) {
      if (library_confirm_close()) {
        textPrompt.active = false;
        continue;
      }
      running = 0;
      if (modal)
        textPrompt.active = false;
      continue;
    }
    if (modal) {
      if (e.type == SDL_QUIT) {
        running = 0;
        textPrompt.active = false;
      } else if (e.type == SDL_TEXTINPUT) {
        size_t used = strlen(textPrompt.text), add = strlen(e.text.text);
        if (used + add < sizeof textPrompt.text && text_characters(textPrompt.text) + text_characters(e.text.text) <= textPrompt.max)
          memcpy(textPrompt.text + used, e.text.text, add + 1);
      } else if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
          textPrompt.accepted = textPrompt.text[0] != 0;
          textPrompt.active = false;
        } else if (e.key.keysym.sym == SDLK_ESCAPE)
          textPrompt.active = false;
        else if (e.key.keysym.sym == SDLK_BACKSPACE) {
          size_t n = strlen(textPrompt.text);
          if (n) {
            n--;
            while (n && (((unsigned char)textPrompt.text[n] & 0xC0) == 0x80))
              n--;
            textPrompt.text[n] = 0;
          }
        } else if (e.key.keysym.sym == SDLK_DELETE)
          textPrompt.text[0] = 0;
      } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A || e.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
          textPrompt.accepted = textPrompt.text[0] != 0;
          textPrompt.active = false;
        } else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B || e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
          textPrompt.active = false;
      }
      continue;
    }
    if (e.type == SDL_QUIT)
      running = 0;
    if (e.type == SDL_JOYDEVICEADDED && !SDL_IsGameController(e.jdevice.which))
      controller_added(e.jdevice.which);
    if (e.type == SDL_AUDIODEVICEREMOVED && !e.adevice.iscapture && audio && e.adevice.which == audio)
      audioLost = true;
    if (e.type == SDL_CONTROLLERDEVICEADDED)
      controller_added(e.cdevice.which);
    if (e.type == SDL_CONTROLLERDEVICEREMOVED)
      controller_removed(e.cdevice.which);
    if (e.type == SDL_KEYDOWN && kbCapture >= 0)
      lastKey = (int)e.key.keysym.sym; // the KEYBOARD page's next key: the game sees nothing of it
    else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat)
      key(keyboard(e.key.keysym.sym), e.type == SDL_KEYDOWN);
    if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
      int control = pd_pad_button_control(e.cbutton.button);
      if (e.type == SDL_CONTROLLERBUTTONDOWN && padCapture >= 0)
        lastPadControl = control;
      pad_control(control, e.type == SDL_CONTROLLERBUTTONDOWN);
    }
    if (e.type == SDL_CONTROLLERAXISMOTION) {
      int trigger = pd_pad_axis_control(e.caxis.axis);
      if (trigger >= 0) {
        bool on = e.caxis.value > 16000;
        if (on && padCapture >= 0)
          lastPadControl = trigger;
        pad_control(trigger, on);
      }
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
        stick_dir(SLEFT, LEFT, e.caxis.value < -16000);
        stick_dir(SRIGHT, RIGHT, e.caxis.value > 16000);
      }
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
        stick_dir(SUP, UP, e.caxis.value < -16000);
        stick_dir(SDOWN, DOWN, e.caxis.value > 16000);
      }
    }
    if (!controller && rawTrimui && (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP))
      key(face(joy(e.jbutton.button)), e.type == SDL_JOYBUTTONDOWN);
    if (!controller && rawTrimui && e.type == SDL_JOYHATMOTION) {
      key(UP, e.jhat.value & SDL_HAT_UP);
      key(DOWN, e.jhat.value & SDL_HAT_DOWN);
      key(LEFT, e.jhat.value & SDL_HAT_LEFT);
      key(RIGHT, e.jhat.value & SDL_HAT_RIGHT);
    }
    if (!controller && rawTrimui && e.type == SDL_JOYAXISMOTION) {
      if (e.jaxis.axis == 2)
        key(L2, e.jaxis.value > 16000);
      if (e.jaxis.axis == 5)
        key(R2, e.jaxis.value > 16000);
    }
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
      mouseWindowX = e.button.x, mouseWindowY = e.button.y;
      pointer_at(e.button.x, e.button.y);
      pointerHeld = e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT;
      if (pointerHeld) {
        pointerPulse = 1; // a click the core sees even when press and release fall in one frame
        libraryClick = true;
      }
    }
#ifndef _WIN32
    // The cursor shows while a real mouse is used (not a touch screen's mouse events) and goes at the next key / button.
    if ((e.type == SDL_MOUSEMOTION && e.motion.which != SDL_TOUCH_MOUSEID) ||
        ((e.type == SDL_MOUSEBUTTONDOWN) && e.button.which != SDL_TOUCH_MOUSEID))
      SDL_ShowCursor(SDL_ENABLE);
    else if (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_JOYBUTTONDOWN)
      SDL_ShowCursor(SDL_DISABLE);
#endif
    if (e.type == SDL_MOUSEMOTION) {
      mouseWindowX = e.motion.x, mouseWindowY = e.motion.y;
      pointer_at(e.motion.x, e.motion.y);
    }
    if (e.type == SDL_MOUSEWHEEL) {
      int steps = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
      wheelSteps += steps;
      libraryWheel += steps;
    }
    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
      inputReset = true;
      held = pressed = padHeld = 0;
      pointerHeld = false; // its button-up may go to the window that took the focus
      pointerPulse = 0;
      pointerDown = false;
      libraryClick = false;
      libraryWheel = 0;
    }
    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
      outputChanged = true;
  }
  if (modal)
    tick++;
  else {
    script_mouse();
    script_resize();
    script_keys();
  }
}
/* POCKET_MOUSE="TICK:X:Y[:c],...": where the cursor is (window pixels) from that tick on, ',c' clicking then, for
 * tests; the same tick numbering as POCKET_KEYS. */
static struct {
  unsigned at;
  int x, y, wheel;
  bool click;
} mscript[16];
static int mscriptN = -1;
static void script_mouse(void) {
  if (mscriptN < 0) {
    mscriptN = 0;
    char buf[512], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_MOUSE") ? getenv("POCKET_MOUSE") : "");
    for (char *t = strtok_r(buf, ",", &save); t && mscriptN < 16; t = strtok_r(NULL, ",", &save)) {
      unsigned at;
      int x, y;
      char c = 0;
      if (sscanf(t, "%u:%d:%d:%c", &at, &x, &y, &c) >= 3)
        mscript[mscriptN] = (typeof(mscript[0])){at, x, y, c == 'd' ? -1 : c == 'u' ? 1 : 0, c == 'c'}, mscriptN++;
    }
  }
  for (int i = 0; i < mscriptN; i++)
    if (tick == mscript[i].at) {
      mouseWindowX = mscript[i].x, mouseWindowY = mscript[i].y;
      pointer_at(mscript[i].x, mscript[i].y);
      if (mscript[i].click) {
        pointerPulse = 1;
        libraryClick = true;
      }
      wheelSteps += mscript[i].wheel;
      libraryWheel += mscript[i].wheel;
    }
}
/* POCKET_RESIZE="TICK:WxH,...": resizes the window at that tick, as a dragged edge or fullscreen would, for tests:
 * the core lays its canvas out for the new panel. */
static struct {
  unsigned at;
  int w, h;
} rscript[8];
static int rscriptN = -1;
static void script_resize(void) {
  if (rscriptN < 0) {
    rscriptN = 0;
    char buf[128], *save = NULL;
    snprintf(buf, sizeof buf, "%s", getenv("POCKET_RESIZE") ? getenv("POCKET_RESIZE") : "");
    for (char *t = strtok_r(buf, ",", &save); t && rscriptN < 8; t = strtok_r(NULL, ",", &save)) {
      unsigned at;
      int w, h;
      if (sscanf(t, "%u:%dx%d", &at, &w, &h) == 3 && w > 0 && h > 0)
        rscript[rscriptN++] = (typeof(rscript[0])){at, w, h};
    }
  }
  for (int i = 0; i < rscriptN; i++)
    if (tick == rscript[i].at && window) {
      SDL_SetWindowSize(window, rscript[i].w, rscript[i].h);
      outputChanged = true;
    }
}
static void repeat_directions(void) {
  for (int k = RIGHT; k <= DOWN; k++)
    if ((held & (1ull << k)) && SDL_GetTicks() >= repeatAt[k]) {
      pressed |= 1ull << k;
      repeatAt[k] = SDL_GetTicks() + 90;
    }
}
static bool down(int k) { return (pressed & (1ull << k)) != 0; }

/* Modifier hotkeys complement the controller page's direct bindings. FF and rewind act while held; state, undo and
 * slot actions fire on the press. Their buttons never reach the game while completing the chord. */
enum { HK_FF, HK_REWIND, HK_SAVE, HK_LOAD, HK_UNDO_SAVE, HK_UNDO_LOAD, HK_SLOT_NEXT, HK_SLOT_PREV, HK_ACTIONS };
static const char *const hotkeyIds[HK_ACTIONS] = {"ff", "rewind", "savestate", "loadstate",
                                                  "undosave", "undoload", "slotnext", "slotprev"};
static unsigned hotkeyMask;
// The game's own buttons and the menu's stay theirs: A, B, START, SELECT, the D-pad (and the stick's), MENU.
static bool hotkey_assignable(int k) {
  return k == L || k == R || k == X || k == Y || k == L2 || k == R2 || k == L3 || k == R3 || (k >= P1 && k <= P4);
}
// START+SELECT opens the host menu too, unless POCKET_MENU_COMBO=off (PortMaster: there it is the port's exit).
static bool comboMenu = true;
static const struct {
  const char *name;
  int key;
} keyNames[] = {{"a", A},       {"b", B},         {"x", X},         {"y", Y},           {"l1", L},
                {"r1", R},      {"l2", L2},       {"r2", R2},       {"up", UP},         {"down", DOWN},
                {"left", LEFT}, {"right", RIGHT}, {"menu", MENU},   {"select", SELECT}, {"start", START}, {"l3", L3},
                {"sup", SUP},   {"sdown", SDOWN}, {"sleft", SLEFT}, {"sright", SRIGHT}, {"r3", R3},
                {"p1", P1},     {"p2", P2},       {"p3", P3},       {"p4", P4}, {"ff", FFKEY},
                {"rewind", RWKEY}, {"savestate", SAVEKEY}, {"loadstate", LOADKEY}, {"undosave", UNSAVEKEY},
                {"undoload", UNLOADKEY}, {"slotnext", NEXTKEY}, {"slotprev", PREVKEY}};
static int key_named(const char *s) {
  for (size_t i = 0; i < sizeof keyNames / sizeof keyNames[0]; i++)
    if (!strcmp(s, keyNames[i].name))
      return keyNames[i].key;
  return -1;
}
static const char *key_name(int k) {
  for (size_t i = 0; i < sizeof keyNames / sizeof keyNames[0]; i++)
    if (keyNames[i].key == k)
      return keyNames[i].name;
  return "off";
}
/* ---- the host's settings (DATA/host.ini): what its menu sets ---- */
static struct {
  int ffSpeed; // 0 = MAX (as many frames as fit), else 2..8
  bool rewind;
  int displayMode; // enum pocket_display_shader_mode; legacy lcd=1 maps to LCD
  bool swapAB; // the controller's A / B and X / Y the other way round (devices disagree on which is SDL's "A")
  int hotkey[HK_ACTIONS]; // the HOTKEYS page's button per action, -1 none
  int dsHotkey[HK_ACTIONS];
  int modifier;
  bool fullscreen; // the WINDOW setting
  int panel;       // the PANEL setting: 0 = the window's size, else a preset the core lays out for
  bool autoResume; // a separate clean-exit state, restored only while its battery image still matches
} hostSet = {.hotkey = {R2, L2, -1, -1, -1, -1, -1, -1},
             .dsHotkey = {R2, L2, -1, -1, -1, -1, -1, -1},
             .modifier = SELECT};
static char hostIni[PATH_MAX];
/* A DS game's L2 / R2 are its companion and screen buttons: with no modifier held for them, its FAST FORWARD and REWIND
 * go back to the stick clicks (R3 / L3), and come back to R2 / L2 when a modifier is picked again. */
static void ds_hotkeys_follow_modifier(void) {
  int *ds = hostSet.dsHotkey;
  if (hostSet.modifier < 0 && ds[HK_FF] == R2 && ds[HK_REWIND] == L2) {
    ds[HK_FF] = R3;
    ds[HK_REWIND] = L3;
  } else if (hostSet.modifier >= 0 && ds[HK_FF] == R3 && ds[HK_REWIND] == L3) {
    ds[HK_FF] = R2;
    ds[HK_REWIND] = L2;
  }
}
/* A new modifier can't also be a hotkey's own button (that hotkey would never fire): the hotkey lets it go. */
static void modifier_takes(int k) {
  for (int i = 0; i < HK_ACTIONS; i++) {
    if (hostSet.hotkey[i] == k)
      hostSet.hotkey[i] = -1;
    if (hostSet.dsHotkey[i] == k)
      hostSet.dsHotkey[i] = -1;
  }
}
static void host_load(void) {
  pd_pad_defaults(&padMap, faceLabels);
  FILE *f = fopen(hostIni, "r");
  char line[128];
  int v;
  bool modifierSaved = false;
  while (f && fgets(line, sizeof line, f)) {
    if (sscanf(line, "ff_speed=%d", &v) == 1)
      hostSet.ffSpeed = v == 0 || (v >= 2 && v <= 8) ? v : 0;
    if (sscanf(line, "rewind=%d", &v) == 1)
      hostSet.rewind = v != 0;
    if (sscanf(line, "lcd=%d", &v) == 1)
      hostSet.displayMode = v ? POCKET_DISPLAY_SHADER_LCD : POCKET_DISPLAY_SHADER_NONE;
    if (sscanf(line, "display_filter=%d", &v) == 1 && v >= 0 && v <= POCKET_DISPLAY_SHADER_EDGE_SMOOTH)
      hostSet.displayMode = v;
    if (sscanf(line, "swap_ab=%d", &v) == 1)
      hostSet.swapAB = v != 0;
    if (sscanf(line, "fullscreen=%d", &v) == 1)
      hostSet.fullscreen = v != 0;
    if (sscanf(line, "panel=%d", &v) == 1 && v >= 0 && v < PD_PANEL_PRESETS)
      hostSet.panel = v;
    if (sscanf(line, "auto_resume=%d", &v) == 1)
      hostSet.autoResume = v != 0;
    char modifier[16];
    if (sscanf(line, "hk_modifier=%15s", modifier) == 1) {
      int k = key_named(modifier);
      if (k == SELECT || k == START || hotkey_assignable(k) || !strcmp(modifier, "off")) {
        hostSet.modifier = k;
        modifierSaved = true;
      }
    }
    for (int i = 0; i < HK_ACTIONS; i++) {
      char key[32], name[16];
      snprintf(key, sizeof key, "hk_%s=%%15s", hotkeyIds[i]);
      if (sscanf(line, key, name) == 1)
        hostSet.hotkey[i] = hotkey_assignable(key_named(name)) ? key_named(name) : -1;
      snprintf(key, sizeof key, "ds_hk_%s=%%15s", hotkeyIds[i]);
      if (sscanf(line, key, name) == 1)
        hostSet.dsHotkey[i] = hotkey_assignable(key_named(name)) ? key_named(name) : -1;
    }
    for (int i = 0; i < KB_COUNT; i++) {
      char key[40];
      snprintf(key, sizeof key, "kb_%s=%%d", pd_pad_action_id(i));
      if (sscanf(line, key, &v) == 1 && v >= 0)
        kbKeys[i] = v;
    }
    for (int i = 0; i < PD_PAD_CONTROLS; i++) {
      const char *id = pd_pad_control_id(i);
      if (!id)
        continue;
      char key[48], name[16];
      snprintf(key, sizeof key, "pad_%s=%%15s", id);
      if (sscanf(line, key, name) == 1) {
        int action = pd_pad_action_named(name);
        if (action >= 0)
          pd_pad_bind(&padMap, i, action);
        else if (!strcmp(name, "off"))
          padMap.action[i] = -1;
      }
    }
  }
  if (f)
    fclose(f);
  if (!modifierSaved && hostSet.dsHotkey[HK_FF] == R3 && hostSet.dsHotkey[HK_REWIND] == L3) {
    hostSet.dsHotkey[HK_FF] = R2;
    hostSet.dsHotkey[HK_REWIND] = L2;
  }
  ds_hotkeys_follow_modifier();
  swapFace = hostSet.swapAB;
}
static void host_save(void) {
  char text[16384];
  size_t used = 0;
#define HOST_LINE(...)                                                                                                  \
  do {                                                                                                                  \
    int n = snprintf(text + used, sizeof text - used, __VA_ARGS__);                                                     \
    if (n < 0 || (size_t)n >= sizeof text - used)                                                                       \
      return;                                                                                                           \
    used += (size_t)n;                                                                                                  \
  } while (0)
  HOST_LINE("ff_speed=%d\nrewind=%d\nlcd=%d\nswap_ab=%d\n", hostSet.ffSpeed, hostSet.rewind,
            hostSet.displayMode == POCKET_DISPLAY_SHADER_LCD, hostSet.swapAB);
  HOST_LINE("display_filter=%d\nfullscreen=%d\npanel=%d\nauto_resume=%d\n", hostSet.displayMode, hostSet.fullscreen,
            hostSet.panel, hostSet.autoResume);
  HOST_LINE("hk_modifier=%s\n", key_name(hostSet.modifier));
  for (int i = 0; i < HK_ACTIONS; i++) {
    HOST_LINE("hk_%s=%s\n", hotkeyIds[i], key_name(hostSet.hotkey[i]));
    HOST_LINE("ds_hk_%s=%s\n", hotkeyIds[i], key_name(hostSet.dsHotkey[i]));
  }
  for (int i = 0; i < KB_COUNT; i++)
    HOST_LINE("kb_%s=%d\n", pd_pad_action_id(i), kbKeys[i]);
  for (int i = 0; i < PD_PAD_CONTROLS; i++)
    if (pd_pad_control_id(i))
      HOST_LINE("pad_%s=%s\n", pd_pad_control_id(i), pd_pad_action_id(pd_pad_action(&padMap, i)));
  pd_state_write_file(hostIni, text, used, true);
#undef HOST_LINE
}
// A hotkey's button as the HOTKEYS page shows it.
static const char *hotkey_label(int k) {
  static const char *const labels[] = {[START] = "START", [SELECT] = "SELECT", [L] = "L1", [R] = "R1",  [X] = "X",         [Y] = "Y",         [L2] = "L2",
                                       [R2] = "R2", [L3] = "L3", [R3] = "R3",       [P1] = "PADDLE 1", [P2] = "PADDLE 2",
                                       [P3] = "PADDLE 3", [P4] = "PADDLE 4"};
  return k >= 0 && k < (int)(sizeof labels / sizeof labels[0]) && labels[k] ? labels[k] : "OFF";
}
/* The actions held this frame; *fired: those whose button went down now. The keyboard's own keys count too. */
static unsigned hotkeys_held(const int *hotkeys, unsigned modifier, unsigned *latched, unsigned *fired) {
  unsigned on = 0;
  *fired = 0;
  hotkeyMask = 0;
  for (int i = 0; i < HK_ACTIONS; i++) {
    int k = hotkeys[i];
    // REWIND's button is the game's while REWIND is off (L2 to start with: the core's own buttons may be on it).
    if (k < 0 || (i == HK_REWIND && !hostSet.rewind))
      continue;
    unsigned bit = 1u << k;
    if (bit == modifier)
      continue;
    if (modifier) {
      if ((held & modifier) && (pressed & bit))
        *latched |= bit;
      if (!(*latched & bit) || !(held & modifier))
        continue;
    }
    hotkeyMask |= bit;
    // pressed too: a tap shorter than a frame is let go before this sees it.
    if (!((held | pressed) & bit))
      continue;
    on |= 1u << i;
    if (pressed & bit)
      *fired |= 1u << i;
  }
  if (held & (1ull << FFKEY))
    on |= 1u << HK_FF;
  if (held & (1ull << RWKEY))
    on |= 1u << HK_REWIND;
  if (down(SAVEKEY))
    *fired |= 1u << HK_SAVE;
  if (down(LOADKEY))
    *fired |= 1u << HK_LOAD;
  if (down(UNSAVEKEY))
    *fired |= 1u << HK_UNDO_SAVE;
  if (down(UNLOADKEY))
    *fired |= 1u << HK_UNDO_LOAD;
  if (down(NEXTKEY))
    *fired |= 1u << HK_SLOT_NEXT;
  if (down(PREVKEY))
    *fired |= 1u << HK_SLOT_PREV;
  return on;
}

/* ---- files ---- */
static bool exists(const char *p) { return access(p, F_OK) == 0; }
static unsigned char *read_file(const char *path, size_t *size, size_t max) {
  return pd_state_read_file(path, size, max);
}
// The same unique-temporary, synced, atomic publication path serves states, settings and battery saves.
static bool write_file(const char *path, const void *d, size_t n, bool sync) {
  return pd_state_write_file(path, d, n, sync);
}
/* A copy of the battery save before the game opens, like Android's and as host_io.c's pocket_save_backup names them (the
 * DS path's, and the hosts' before this one): DIR/STEM.backup-YYYYmmdd-HHMMSS.EXT, a new one only when the save data
 * differs from the newest backup's, the newest 10 kept. Every save chip is a multiple of 512 bytes, so what is past the
 * last whole 512 is mGBA's clock footer, which moves every session: a save where only the clock moved is the same save.
 * A save that can't be backed up keeps the game closed. */
static bool same_save(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
  size_t n = an - an % 512;
  if (!n || n != bn - bn % 512)
    return an == bn && !memcmp(a, b, an);
  return !memcmp(a, b, n);
}
static int newest_first(const void *a, const void *b) { return -strcmp(*(char *const *)a, *(char *const *)b); }
static bool save_backup(const char *sav, const char *dir) {
  size_t n;
  unsigned char *d = read_file(sav, &n, 64u << 20);
  if (!d)
    return !exists(sav); // nothing to keep yet
  if (!n) {
    free(d);
    return true;
  }
  pd_mkdir(dir);
  const char *file = strrchr(sav, '/');
  file = file ? file + 1 : sav;
  const char *dot = strrchr(file, '.');
  char stem[512], ext[32] = "sav";
  snprintf(stem, sizeof stem, "%.*s.backup-", dot && dot != file ? (int)(dot - file) : (int)strlen(file), file);
  if (dot && dot != file)
    snprintf(ext, sizeof ext, "%s", dot + 1);
  char *list[256];
  int count = 0;
  DIR *dd = opendir(dir);
  for (struct dirent *e; dd && (e = readdir(dd)) && count < 256;)
    if (!strncmp(e->d_name, stem, strlen(stem)) && !strstr(e->d_name, ".tmp"))
      list[count++] = strdup(e->d_name);
  if (dd)
    closedir(dd);
  qsort(list, count, sizeof *list, newest_first);
  bool write = true, ok = true;
  char path[PATH_MAX * 4 + 64];
  if (count) {
    size_t m;
    snprintf(path, sizeof path, "%s/%s", dir, list[0]);
    unsigned char *last = read_file(path, &m, 64u << 20);
    write = !last || !same_save(last, m, d, n);
    free(last);
  }
  if (write) {
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(path, sizeof path, "%s/%s%s.%s", dir, stem, stamp, ext);
    ok = write_file(path, d, n, true);
  }
  // The newest 10 stay (a new one counts as the newest).
  for (int i = ok && write ? 9 : 10; ok && i < count; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, list[i]);
    unlink(path);
  }
  for (int i = 0; i < count; i++)
    free(list[i]);
  free(d);
  return ok;
}

/* ---- the core ---- */
static struct {
  void *dl;
  void (*init)(void);
  void (*deinit)(void);
  void (*set_environment)(retro_environment_t);
  void (*set_video_refresh)(retro_video_refresh_t);
  void (*set_audio_sample)(retro_audio_sample_t);
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*set_input_poll)(retro_input_poll_t);
  void (*set_input_state)(retro_input_state_t);
  void (*get_system_info)(struct retro_system_info *);
  void (*get_system_av_info)(struct retro_system_av_info *);
  bool (*load_game)(const struct retro_game_info *);
  void (*unload_game)(void);
  void (*run)(void);
  size_t (*serialize_size)(void);
  bool (*serialize)(void *, size_t);
  bool (*unserialize)(const void *, size_t);
  void *(*get_memory_data)(unsigned);
  size_t (*get_memory_size)(unsigned);
} core;
static bool core_open(const char *path) {
  core.dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!core.dl) {
    fprintf(stderr, "%s\n", dlerror());
    return false;
  }
#define SYM(field, name)                                                                                       \
  if (!(*(void **)&core.field = dlsym(core.dl, name))) {                                                       \
    fprintf(stderr, "%s: no %s\n", path, name);                                                                \
    dlclose(core.dl);                                                                                          \
    core.dl = NULL;                                                                                            \
    return false;                                                                                              \
  }
  SYM(init, "retro_init")
  SYM(deinit, "retro_deinit")
  SYM(set_environment, "retro_set_environment")
  SYM(set_video_refresh, "retro_set_video_refresh")
  SYM(set_audio_sample, "retro_set_audio_sample")
  SYM(set_audio_sample_batch, "retro_set_audio_sample_batch")
  SYM(set_input_poll, "retro_set_input_poll")
  SYM(set_input_state, "retro_set_input_state")
  SYM(get_system_info, "retro_get_system_info")
  SYM(get_system_av_info, "retro_get_system_av_info")
  SYM(load_game, "retro_load_game")
  SYM(unload_game, "retro_unload_game")
  SYM(run, "retro_run")
  SYM(serialize_size, "retro_serialize_size")
  SYM(serialize, "retro_serialize")
  SYM(unserialize, "retro_unserialize")
  SYM(get_memory_data, "retro_get_memory_data")
  SYM(get_memory_size, "retro_get_memory_size")
#undef SYM
  return true;
}

/* The core's options: their defaults as it declares them, and the panel this host has. */
static struct {
  const char *key, *value;
  char legacyDefault[128];
} options[160];
static int optionN;
static char panelValue[32];
static char windowValue[32];   // the window the picture is drawn in, whatever PANEL preset is held
static bool optionsChanged;
static void options_from(const struct retro_core_option_v2_definition *d) {
  for (optionN = 0; d && d->key && optionN < (int)(sizeof options / sizeof options[0]); d++, optionN++) {
    options[optionN].key = d->key;
    options[optionN].value = d->default_value;
  }
}
static void options_from_v1(const struct retro_core_option_definition *d) {
  for (optionN = 0; d && d->key && optionN < (int)(sizeof options / sizeof options[0]); d++, optionN++) {
    options[optionN].key = d->key;
    options[optionN].value = d->default_value;
  }
}

static void options_from_variables(const struct retro_variable *v) {
  for (optionN = 0; v && v->key && optionN < (int)(sizeof options / sizeof options[0]); v++) {
    const char *first = v->value ? strchr(v->value, ';') : NULL;
    if (!first)
      continue;
    first++;
    first += strspn(first, " ");
    size_t len = strcspn(first, "|");
    if (len >= sizeof options[0].legacyDefault)
      continue;
    options[optionN].key = v->key;
    memcpy(options[optionN].legacyDefault, first, len);
    options[optionN].legacyDefault[len] = '\0';
    options[optionN].value = options[optionN].legacyDefault;
    optionN++;
  }
}

static enum retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_system_av_info av;
static SDL_Texture *frameTexture;
static int textureW, textureH, frameW, frameH;
static unsigned char *frameCopy;
static size_t frameCopyCapacity, frameCopyStride;
static bool frameCopyValid;
static enum retro_pixel_format textureFormat; // what frameTexture was made for: a core of another format needs a new one
static double srcRate = 32768, rateTrim = 1;   // the core's audio rate (SET_SYSTEM_AV_INFO can change it), the pacing's trim
static bool ffNow, rewindingNow, ffQuick; // ffQuick: the quick menu's FF toggle, until it is turned off again
static int ffSpeedNow = 1;
static void core_log(enum retro_log_level level, const char *fmt, ...) {
  if (level < RETRO_LOG_WARN && !getenv("POCKET_CORE_LOG"))
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
static void frame_texture(void) {
  int w = (int)av.geometry.max_width, h = (int)av.geometry.max_height;
  if (frameTexture && w <= textureW && h <= textureH && pixelFormat == textureFormat)
    return;
  if (frameTexture)
    SDL_DestroyTexture(frameTexture);
  frameTexture = SDL_CreateTexture(renderer,
                                   pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? SDL_PIXELFORMAT_ARGB8888
                                   : pixelFormat == RETRO_PIXEL_FORMAT_RGB565 ? SDL_PIXELFORMAT_RGB565
                                                                              : SDL_PIXELFORMAT_ARGB1555,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
  textureW = w;
  textureH = h;
  textureFormat = pixelFormat;
  if (frameTexture) {
    SDL_SetTextureBlendMode(frameTexture, SDL_BLENDMODE_NONE);
    // Pixel art stays pixel art: the picture and the core's touch panel both scale nearest, so a fractional window size
    // never blurs them. The hint covers backends that ignore the per-texture mode; the mode covers the rest.
    SDL_SetTextureScaleMode(frameTexture, SDL_ScaleModeNearest);
  } else
    fprintf(stderr, "Texture: %s\n", SDL_GetError());
}
static bool environment(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    v->value = NULL;
    if (!strcmp(v->key, "pokedaisy_ds_select_modifier"))
      v->value = hostSet.modifier == SELECT ? "enabled" : "disabled";
    if (!strcmp(v->key, "pokedaisy_panel"))
      v->value = panelValue;
    if (!strcmp(v->key, "pokedaisy_window"))
      v->value = windowValue;
    // ASPECT is the companion's own (the app's SETTINGS > SCREEN): auto leaves its file's setting be.
    if (!strcmp(v->key, "pokedaisy_aspect"))
      v->value = "auto";
    for (int i = 0; i < optionN && !v->value; i++)
      if (!strcmp(options[i].key, v->key))
        v->value = options[i].value;
    return v->value != NULL;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool *)data = optionsChanged;
    optionsChanged = false;
    return true;
  case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
    *(unsigned *)data = 2;
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    options_from(data ? ((const struct retro_core_options_v2 *)data)->definitions : NULL);
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
    const struct retro_core_options_v2_intl *i = data;
    options_from(i && i->us ? i->us->definitions : NULL);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    options_from_v1(data);
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    options_from_v1(data ? ((const struct retro_core_options_intl *)data)->us : NULL);
    return true;
  case RETRO_ENVIRONMENT_SET_VARIABLES:
    options_from_variables(data);
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
    enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
    if (f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_0RGB1555)
      return false;
    pixelFormat = f;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *(const char **)data = savesDir;
    return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    *(const char **)data = root;
    return true;
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
    av.geometry = *(const struct retro_game_geometry *)data;
    if (renderer)
      frame_texture();
    return true;
  case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    av = *(const struct retro_system_av_info *)data;
    if (av.timing.sample_rate > 0)
      srcRate = av.timing.sample_rate; // the audio resampler follows the core's new rate
    if (renderer)
      frame_texture();
    return true;
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    *(bool *)data = true;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    return true;
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    ((struct retro_log_callback *)data)->log = core_log;
    return true;
  case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
    if (data)
      *(int *)data = 3;
    return true;
  case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
    *(bool *)data = ffNow;
    return true;
  case PD_ENV_SET_FAST_FORWARD:
    ffQuick = data && *(const bool *)data;
    return true;
  case PD_ENV_SET_TOUCH_PANEL: {
    const struct pd_touch_panel *p = data;
    panelPending = p && p->active && p->pixels && p->src_w > 0 && p->src_h > 0 && p->w > 0 && p->h > 0;
    if (panelPending)
      panelMsg = *p;
    return true;
  }
  case PD_ENV_SET_HUD: {
    const struct pd_hud *h = data;
    hudPending = h && h->active && h->pixels && h->src_w > 0 && h->src_h > 0 && h->w > 0 && h->h > 0;
    hudHave = true;
    if (hudPending)
      hudMsg = *h;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
    struct retro_throttle_state *t = data;
    t->mode = rewindingNow ? RETRO_THROTTLE_REWINDING
              : ffNow      ? RETRO_THROTTLE_FAST_FORWARD
                           : RETRO_THROTTLE_VSYNC;
    t->rate = (float)(av.timing.fps * (ffNow ? ffSpeedNow : 1));
    return true;
  }
  default:
    return false;
  }
}

/* ---- video ---- */
static void frame_copy(const void *data, unsigned w, unsigned h, size_t pitch) {
  size_t bpp = pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2, row = (size_t)w * bpp;
  if (!w || !h || row / bpp != w || pitch < row || (h && row > SIZE_MAX / h)) {
    frameCopyValid = false;
    return;
  }
  size_t bytes = row * h;
  if (frameCopyCapacity < bytes) {
    unsigned char *grown = realloc(frameCopy, bytes);
    if (!grown) {
      frameCopyValid = false;
      return;
    }
    frameCopy = grown;
    frameCopyCapacity = bytes;
  }
  for (unsigned y = 0; y < h; y++)
    memcpy(frameCopy + (size_t)y * row, (const unsigned char *)data + (size_t)y * pitch, row);
  frameCopyStride = row;
  frameCopyValid = true;
}
static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch) {
  panelActive = panelPending;
  panelPending = false;
  if (hudHave) // a frame the core only repeats keeps the HUD where it was
    hudActive = hudPending;
  hudPending = hudHave = false;
  if (!data || !frameTexture) // a dupe: the last frame stays
    return;
  SDL_Rect r = {0, 0, (int)w, (int)h};
  SDL_UpdateTexture(frameTexture, &r, data, (int)pitch);
  frame_copy(data, w, h, pitch);
  if ((int)w != frameW || (int)h != frameH) // the core's canvas: it follows the aspect and the panel
    fprintf(stderr, "Frame: %ux%u\n", w, h);
  frameW = (int)w;
  frameH = (int)h;
}
/* The frame centred, at the largest scale the screen holds. The core sizes its canvas to the panel, so this is a whole
 * scale normally; where a whole one would leave the picture short of the screen (a canvas whose aspect isn't the
 * window's, or an odd panel) it fits instead, so the picture fills the screen rather than sitting in bars. */
static SDL_Rect frame_place(int ow, int oh) {
  SDL_Rect r = {0, 0, ow, oh};
  if (frameW <= 0 || frameH <= 0)
    return r;
  double fit = ow / (double)frameW < oh / (double)frameH ? ow / (double)frameW : oh / (double)frameH;
  int whole = (int)fit;
  double s = whole >= 1 && whole / fit >= 0.98 ? whole : fit; // whole pixels only when they fill the screen
  r.w = (int)(frameW * s + 0.5);
  r.h = (int)(frameH * s + 0.5);
  if (s == whole) {
    // A whole scale a pixel or three short of the screen would show as a thin bar down one side: cover it instead (the
    // stretch is a fraction of a pixel in a texel, invisible next to the bar).
    if (r.w < ow && ow - r.w <= 4)
      r.w = ow;
    if (r.h < oh && oh - r.h <= 4)
      r.h = oh;
  }
  r.x = (ow - r.w) / 2;
  r.y = (oh - r.h) / 2;
  return r;
}

/* The overlay (notices, the host menu, the game list): a canvas at the largest whole scale that keeps it at least
 * 320x240 (as the companion's), KEY_COLOR see-through. */
static uint32_t overlayPx[OVERLAY_MAX_W * OVERLAY_MAX_H];
static struct pd_canvas overlay;
static SDL_Texture *overlayTexture;
static int overlayTexW, overlayTexH;
static void overlay_begin(void) {
  int ow = W, oh = H;
  SDL_GetRendererOutputSize(renderer, &ow, &oh);
  int s = ow / 320 < oh / 240 ? ow / 320 : oh / 240;
  if (s < 1)
    s = 1;
  int w = ow / s, h = oh / s;
  w = w > OVERLAY_MAX_W ? OVERLAY_MAX_W : w;
  h = h > OVERLAY_MAX_H ? OVERLAY_MAX_H : h;
  pd_canvas_init(&overlay, w, h, overlayPx);
  pd_fill(&overlay, 0, 0, w, h, KEY_COLOR);
}
static void overlay_draw(bool opaque) {
  if (!overlayTexture || overlayTexW != overlay.w || overlayTexH != overlay.h) {
    if (overlayTexture)
      SDL_DestroyTexture(overlayTexture);
    overlayTexture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, overlay.w, overlay.h);
    overlayTexW = overlay.w;
    overlayTexH = overlay.h;
    if (!overlayTexture)
      return;
    SDL_SetTextureBlendMode(overlayTexture, SDL_BLENDMODE_BLEND);
  }
  int n = overlay.w * overlay.h;
  for (int i = 0; i < n; i++) {
    uint32_t p = overlayPx[i] & 0xFFFFFF;
    overlayPx[i] = p == KEY_COLOR && !opaque ? 0 : 0xFF000000u | p;
  }
  SDL_UpdateTexture(overlayTexture, NULL, overlayPx, overlay.w * 4);
  int ow = W, oh = H;
  SDL_GetRendererOutputSize(renderer, &ow, &oh);
  int s = ow / overlay.w < oh / overlay.h ? ow / overlay.w : oh / overlay.h;
  if (s < 1)
    s = 1;
  SDL_Rect r = {(ow - overlay.w * s) / 2, (oh - overlay.h * s) / 2, overlay.w * s, overlay.h * s};
  SDL_RenderCopy(renderer, overlayTexture, NULL, &r);
}
static bool overlay_notice(void) {
  if (SDL_GetTicks() >= messageUntil)
    return false;
  pd_fill(&overlay, 4, overlay.h - 30, overlay.w - 8, 24, 0x181818);
  pd_text_fit(&overlay, 10, overlay.h - 26, overlay.w - 20, message, 0xffffff, PD_NO_SHADOW);
  return true;
}

static const char *const displayModeNames[] = {"OFF", "LCD", "SCANLINES", "CRT", "GBA COLOR", "EDGE SMOOTH"};
static struct pocket_display_shader *displayShader;
static int displayShaderMode = -1, displayErrorMode = -1;
static uint8_t *displayInput;
static size_t displayInputCapacity;
static SDL_Texture *displayTexture;
static int displayTextureW, displayTextureH;
static double presentMs, filterMs;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
#define PD_SHADER_PIXEL_FORMAT SDL_PIXELFORMAT_RGBA8888
#else
#define PD_SHADER_PIXEL_FORMAT SDL_PIXELFORMAT_ABGR8888
#endif
static SDL_Texture *lcdTexture;
static int lcdW, lcdH;
static void display_filter_cleanup(void) {
  pocket_display_shader_destroy(displayShader);
  displayShader = NULL;
  displayShaderMode = -1;
  SDL_DestroyTexture(displayTexture);
  displayTexture = NULL;
  displayTextureW = displayTextureH = 0;
  SDL_DestroyTexture(lcdTexture);
  lcdTexture = NULL;
  lcdW = lcdH = 0;
  free(displayInput);
  displayInput = NULL;
  displayInputCapacity = 0;
}
/* Filter only the core's own frame. The panel and HUD keep their native textures and coordinates, and SDL performs the
 * final scale to frameRect. CRT uses no more than a 2x source buffer, never the output window's dimensions. */
static bool display_filter_render(int ow, int oh) {
  if (!hostSet.displayMode || !frameTexture || frameW <= 0 || frameH <= 0 || !frameCopyValid ||
      hostSet.displayMode == POCKET_DISPLAY_SHADER_LCD) {
    if (displayShader || displayTexture)
      display_filter_cleanup();
    return false; // LCD has its exact legacy output-space pass below the panel and HUD
  }
  uint64_t started = SDL_GetPerformanceCounter();
  if (displayShaderMode != hostSet.displayMode) {
    pocket_display_shader_destroy(displayShader);
    struct pocket_display_shader_config config = {(enum pocket_display_shader_mode)hostSet.displayMode, false};
    displayShader = pocket_display_shader_create(config);
    displayShaderMode = displayShader ? hostSet.displayMode : -1;
  }
  size_t bytes = (size_t)frameW * frameH * 4;
  if (!displayShader || bytes / 4 / (size_t)frameH != (size_t)frameW)
    goto unavailable;
  if (displayInputCapacity < bytes) {
    uint8_t *grown = realloc(displayInput, bytes);
    if (!grown)
      goto unavailable;
    displayInput = grown;
    displayInputCapacity = bytes;
  }
  for (int y = 0; y < frameH; y++) {
    const unsigned char *src = frameCopy + (size_t)y * frameCopyStride;
    uint8_t *dst = displayInput + (size_t)y * frameW * 4;
    for (int x = 0; x < frameW; x++, dst += 4) {
      uint32_t color;
      if (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        memcpy(&color, src + x * 4, 4);
        dst[0] = (uint8_t)(color >> 16);
        dst[1] = (uint8_t)(color >> 8);
        dst[2] = (uint8_t)color;
      } else {
        uint16_t p;
        memcpy(&p, src + x * 2, 2);
        if (pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
          dst[0] = (uint8_t)((p >> 11) * 255 / 31);
          dst[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
          dst[2] = (uint8_t)((p & 31) * 255 / 31);
        } else {
          dst[0] = (uint8_t)(((p >> 10) & 31) * 255 / 31);
          dst[1] = (uint8_t)(((p >> 5) & 31) * 255 / 31);
          dst[2] = (uint8_t)((p & 31) * 255 / 31);
        }
      }
      dst[3] = 255;
    }
  }
  int scale = hostSet.displayMode == POCKET_DISPLAY_SHADER_SCANLINES || hostSet.displayMode == POCKET_DISPLAY_SHADER_EDGE_SMOOTH
                  ? 2
                  : 1;
  if (hostSet.displayMode == POCKET_DISPLAY_SHADER_CRT && frameW <= 480 && frameH <= 270 && ow >= frameW * 2 &&
      oh >= frameH * 2)
    scale = 2;
  int dw = frameW * scale, dh = frameH * scale;
  struct pocket_display_shader_frame out;
  if (pocket_display_shader_process(displayShader, displayInput, (size_t)frameW, (size_t)frameH, (size_t)frameW * 4,
                                    (size_t)dw, (size_t)dh, &out) != POCKET_DISPLAY_SHADER_OK)
    goto unavailable;
  if (!displayTexture || displayTextureW != dw || displayTextureH != dh) {
    SDL_DestroyTexture(displayTexture);
    displayTexture = SDL_CreateTexture(renderer, PD_SHADER_PIXEL_FORMAT, SDL_TEXTUREACCESS_STREAMING, dw, dh);
    displayTextureW = displayTexture ? dw : 0;
    displayTextureH = displayTexture ? dh : 0;
    if (displayTexture) {
      SDL_SetTextureBlendMode(displayTexture, SDL_BLENDMODE_NONE);
      SDL_SetTextureScaleMode(displayTexture, SDL_ScaleModeNearest);
      fprintf(stderr, "Display filter: %s %dx%d (source %dx%d)\n", displayModeNames[hostSet.displayMode], dw, dh, frameW,
              frameH);
    }
  }
  if (!displayTexture || SDL_UpdateTexture(displayTexture, NULL, out.pixels, (int)out.stride) < 0)
    goto unavailable;
  SDL_Rect src = {0, 0, dw, dh};
  SDL_FRect dst = {(float)frameRect.x, (float)frameRect.y, (float)frameRect.w, (float)frameRect.h};
  bool ok = SDL_RenderCopyF(renderer, displayTexture, &src, &dst) == 0;
  filterMs += (double)(SDL_GetPerformanceCounter() - started) * 1000.0 / SDL_GetPerformanceFrequency();
  return ok;

unavailable:
  filterMs += (double)(SDL_GetPerformanceCounter() - started) * 1000.0 / SDL_GetPerformanceFrequency();
  if (displayErrorMode != hostSet.displayMode) {
    displayErrorMode = hostSet.displayMode;
    notice("Display filter unavailable; using the normal picture");
  }
  return false;
}
static void render_legacy_lcd(void) {
  if (hostSet.displayMode != POCKET_DISPLAY_SHADER_LCD)
    return;
  int w, h;
  if (SDL_GetRendererOutputSize(renderer, &w, &h) < 0 || w <= 0 || h <= 0)
    return;
  if (!lcdTexture || lcdW != w || lcdH != h) {
    SDL_DestroyTexture(lcdTexture);
    lcdTexture = NULL;
    uint32_t *mask = malloc((size_t)w * h * 4);
    if (!mask)
      return;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        mask[y * w + x] = pocket_lcd_pixel((unsigned)x, (unsigned)y);
    lcdTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (lcdTexture && (SDL_UpdateTexture(lcdTexture, NULL, mask, w * 4) < 0 ||
                       SDL_SetTextureBlendMode(lcdTexture, SDL_BLENDMODE_MOD) < 0)) {
      SDL_DestroyTexture(lcdTexture);
      lcdTexture = NULL;
    }
    free(mask);
    lcdW = w;
    lcdH = h;
  }
  if (lcdTexture)
    SDL_RenderCopy(renderer, lcdTexture, NULL, NULL);
}
static void state_preview_draw(struct pd_canvas *c);
static void present_game(const struct pd_menu *menu) {
  int ow = W, oh = H;
  SDL_GetRendererOutputSize(renderer, &ow, &oh);
  SDL_RenderClear(renderer);
  frameRect = frame_place(ow, oh);
  if (frameTexture && frameW > 0 && !display_filter_render(ow, oh)) {
    SDL_Rect src = {0, 0, frameW, frameH};
    SDL_FRect dst = {(float)frameRect.x, (float)frameRect.y, (float)frameRect.w, (float)frameRect.h};
    SDL_RenderCopyF(renderer, frameTexture, &src, &dst);
  }
  // The core's touch panel: its full 256 x 192 bottom screen, scaled once, here, to the panel's place in the output.
  if (panelActive && panelMsg.pixels && frameW > 0 && frameH > 0) {
    if (!panelTexture || panelTexW != panelMsg.src_w || panelTexH != panelMsg.src_h) {
      if (panelTexture)
        SDL_DestroyTexture(panelTexture);
      panelTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        panelMsg.src_w, panelMsg.src_h);
      panelTexW = panelMsg.src_w;
      panelTexH = panelMsg.src_h;
      if (panelTexture) {
        SDL_SetTextureBlendMode(panelTexture, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(panelTexture, SDL_ScaleModeNearest);
      }
    }
    if (panelTexture) {
      SDL_UpdateTexture(panelTexture, NULL, panelMsg.pixels, panelMsg.src_w * 4);
      float sx = (float)frameRect.w / (float)frameW, sy = (float)frameRect.h / (float)frameH;
      SDL_FRect dst = {(float)frameRect.x + panelMsg.x * sx, (float)frameRect.y + panelMsg.y * sy,
                       panelMsg.w * sx, panelMsg.h * sy};
      SDL_RenderCopyF(renderer, panelTexture, NULL, &dst);
    }
  }
  // The core's field HUD, from its own 256 x 192 picture: the frame's canvas is scaled to the window, and at a ZOOM step a
  // HUD blended into the canvas would be resampled with the picture. Drawn here it keeps the window's pixels at every step.
  if (hudActive && hudMsg.pixels && hudMsg.src_w > 0 && hudMsg.src_h > 0 && frameW > 0 && frameH > 0) {
    if (!hudTexture || hudTexW != hudMsg.src_w || hudTexH != hudMsg.src_h) {
      if (hudTexture)
        SDL_DestroyTexture(hudTexture);
      hudTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, hudMsg.src_w,
                                     hudMsg.src_h);
      hudTexW = hudMsg.src_w;
      hudTexH = hudMsg.src_h;
      if (hudTexture) {
        SDL_SetTextureBlendMode(hudTexture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(hudTexture, SDL_ScaleModeNearest);
      }
    }
    if (hudTexture) {
      SDL_UpdateTexture(hudTexture, NULL, hudMsg.pixels, hudMsg.src_w * 4);
      const float sx = (float)frameRect.w / (float)frameW, sy = (float)frameRect.h / (float)frameH;
      const float bx = (float)frameRect.x + hudMsg.x * sx, by = (float)frameRect.y + hudMsg.y * sy;
      const float bw = hudMsg.w * sx, bh = hudMsg.h * sy;
      // SMALL / NORMAL: each quarter of the picture at its own corner of the box, a quarter of the box's size, as the core's
      // in-canvas blend places it. Drawn from the picture's own pixels in one go, a quarter is as sharp here as at 2x.
      if (hudMsg.pct >= 100) {
        SDL_FRect dst = {bx, by, bw, bh};
        SDL_RenderCopyF(renderer, hudTexture, NULL, &dst);
      } else {
        const float qw = bw * hudMsg.pct / 200.0f, qh = bh * hudMsg.pct / 200.0f;
        const float sw = (float)hudMsg.src_w / 2, sh = (float)hudMsg.src_h / 2;
        if (qw >= 1 && qh >= 1)
          for (int qy = 0; qy < 2; qy++)
            for (int qx = 0; qx < 2; qx++) {
              SDL_Rect src = {(int)(qx * sw), (int)(qy * sh), (int)sw, (int)sh};
              SDL_FRect dst = {qx ? bx + bw - qw : bx, qy ? by + bh - qh : by, qw, qh};
              SDL_RenderCopyF(renderer, hudTexture, &src, &dst);
            }
      }
    }
  }
  render_legacy_lcd();
  bool any = false;
  overlay_begin();
  if (menu) {
    pd_menu_draw((struct pd_menu *)menu, &overlay);
    state_preview_draw(&overlay);
    any = true;
  }
  any |= overlay_notice();
  if (any)
    overlay_draw(false);
  uint64_t t = SDL_GetPerformanceCounter();
  SDL_RenderPresent(renderer);
  presentMs += (double)(SDL_GetPerformanceCounter() - t) * 1000.0 / SDL_GetPerformanceFrequency();
}
static bool capture(const char *path) {
  int w = W, h = H;
  SDL_GetRendererOutputSize(renderer, &w, &h);
  uint32_t *px = malloc((size_t)w * h * 4);
  bool ok = px && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, px, w * 4) == 0;
  for (int i = 0; ok && i < w * h; i++)
    px[i] &= 0xFFFFFF;
  ok = ok && pd_png_write(path, w, h, px);
  free(px);
  return ok;
}

/* ---- audio: the core's samples resampled to the device, a little faster or slower to keep its queue near 60 ms
 * (the screen paces the game, not the sound card); fast-forward keeps every Nth sample, so it plays sped up. ---- */
static double resamplePos;
static int16_t prevL, prevR;
static unsigned audioSkip, audioKeepEvery = 1;
static bool muted;
static void audio_open(void) {
  SDL_AudioSpec want = {0};
  want.freq = OUT_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
  if (audio)
    SDL_PauseAudioDevice(audio, 0);
  else
    fprintf(stderr, "Audio unavailable: %s\n", SDL_GetError());
}
/* A handheld's ALSA device can go bad under the app (the screen turned off and on: "ALSA write failed
 * (unrecoverable)"), and SDL then closes it for good; it's reopened as soon as SDL says so, or within a second of it
 * no longer playing. */
static void audio_check(void) {
  static Uint32 next;
  bool stopped = audio && SDL_GetAudioDeviceStatus(audio) == SDL_AUDIO_STOPPED;
  if (!audioLost && !stopped)
    return;
  if (SDL_GetTicks() < next)
    return;
  next = SDL_GetTicks() + 1000;
  if (audio)
    SDL_CloseAudioDevice(audio);
  audio = 0;
  audio_open();
  audioLost = !audio;
  if (audio)
    fprintf(stderr, "Audio device reopened\n");
}
#ifdef POCKET_TEST_AUDIO
static unsigned long audioNonzero;
#endif
static size_t audio_batch(const int16_t *in, size_t frames) {
  static int16_t out[4096 * 2];
  if (!audio || muted)
    return frames;
  size_t n = 0;
  double step = srcRate * rateTrim / OUT_RATE;
  const bool room = SDL_GetQueuedAudioSize(audio) < OUT_RATE * 4 / 5; // never more than 200 ms behind
  for (size_t i = 0; i < frames; i++) {
    if (audioSkip++ % audioKeepEvery)
      continue;
    int16_t l = in[2 * i], r = in[2 * i + 1];
#ifdef POCKET_TEST_AUDIO
    audioNonzero += l || r;
#endif
    while (resamplePos < 1.0) {
      if (n == 4096) { // a batch longer than the buffer: send what's made and go on (never skip ahead in the input)
        if (room)
          SDL_QueueAudio(audio, out, (Uint32)(n * 4));
        n = 0;
      }
      out[2 * n] = (int16_t)(prevL + (l - prevL) * resamplePos);
      out[2 * n + 1] = (int16_t)(prevR + (r - prevR) * resamplePos);
      n++;
      resamplePos += step;
    }
    resamplePos -= 1.0;
    prevL = l;
    prevR = r;
  }
  if (n && room)
    SDL_QueueAudio(audio, out, (Uint32)(n * 4));
  return frames;
}
static void audio_sample(int16_t l, int16_t r) {
  int16_t s[2] = {l, r};
  audio_batch(s, 1);
}
static void audio_trim(double gameHz, double paceHz) {
  double rate = paceHz > 0 && gameHz > 0 ? paceHz / gameHz : 1;
  if (audio) {
    double queued = SDL_GetQueuedAudioSize(audio) / 4.0, target = OUT_RATE * 0.06;
    double d = (target - queued) / target;
    d = d > 1 ? 1 : d < -1 ? -1 : d;
    rate *= 1 - 0.005 * d; // short of the target: a little more output per input sample
  }
  rateTrim = rate;
}

/* ---- input to the core ---- */
static unsigned gameKeys; // this frame's joypad, in RETRO_DEVICE_ID_JOYPAD bits
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)index;
  if (port)
    return 0;
  if (device == RETRO_DEVICE_JOYPAD)
    return id == RETRO_DEVICE_ID_JOYPAD_MASK ? (int16_t)gameKeys : (int16_t)((gameKeys >> id) & 1);
  if (device == RETRO_DEVICE_POINTER)
    return id == RETRO_DEVICE_ID_POINTER_X         ? (int16_t)pointerX
           : id == RETRO_DEVICE_ID_POINTER_Y       ? (int16_t)pointerY
           : id == RETRO_DEVICE_ID_POINTER_PRESSED ? pointerDown
                                                   : 0;
  if (device == RETRO_DEVICE_MOUSE)
    return id == RETRO_DEVICE_ID_MOUSE_WHEELUP     ? (int16_t)(wheelSteps > 0 ? wheelSteps : 0)
           : id == RETRO_DEVICE_ID_MOUSE_WHEELDOWN ? (int16_t)(wheelSteps < 0 ? -wheelSteps : 0)
                                                   : 0;
  return 0;
}
static unsigned joypad(unsigned keys) {
  static const struct {
    int key;
    unsigned id;
  } map[] = {{A, RETRO_DEVICE_ID_JOYPAD_A},           {B, RETRO_DEVICE_ID_JOYPAD_B},
             {SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT}, {START, RETRO_DEVICE_ID_JOYPAD_START},
             {RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},   {LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
             {UP, RETRO_DEVICE_ID_JOYPAD_UP},         {DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
             {R, RETRO_DEVICE_ID_JOYPAD_R},           {L, RETRO_DEVICE_ID_JOYPAD_L},
             {X, RETRO_DEVICE_ID_JOYPAD_X},           {Y, RETRO_DEVICE_ID_JOYPAD_Y},
             {L2, RETRO_DEVICE_ID_JOYPAD_L2},         {R2, RETRO_DEVICE_ID_JOYPAD_R2},
             {L3, RETRO_DEVICE_ID_JOYPAD_L3},         {R3, RETRO_DEVICE_ID_JOYPAD_R3}};
  unsigned bits = 0;
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
    if (keys & (1u << map[i].key))
      bits |= 1u << map[i].id;
  return bits;
}

/* ---- rewind: a state every 2 frames, kept as the newest whole state plus the differences back from it (XOR,
 * zero runs packed), within a memory budget (POCKET_REWIND_MB, default 32): about 20 s on a GBA game. ---- */
#define REWIND_CAP 4096
static unsigned char *rewindNow, *rewindTmp;
static size_t rewindSize, rewindBytes, rewindBudget;
static struct {
  unsigned char *d;
  size_t n;
} rewindRing[REWIND_CAP];
static int rewindHead, rewindCount, rewindTick;
static void rewind_clear(void) {
  for (int i = 0; i < rewindCount; i++)
    free(rewindRing[(rewindHead + i) % REWIND_CAP].d);
  rewindHead = rewindCount = 0;
  rewindBytes = 0;
  free(rewindNow);
  free(rewindTmp);
  rewindNow = rewindTmp = NULL;
  rewindSize = 0;
}
// a ^ b packed: (u32 zeros, u32 literal count, literal bytes)...
static size_t delta_pack(const unsigned char *a, const unsigned char *b, size_t n, unsigned char *out) {
  size_t o = 0, i = 0;
  while (i < n) {
    size_t z = i;
    while (z < n && a[z] == b[z])
      z++;
    size_t lit = z;
    while (lit < n && (a[lit] != b[lit] || (lit + 1 < n && a[lit + 1] != b[lit + 1])))
      lit++;
    uint32_t zeros = (uint32_t)(z - i), count = (uint32_t)(lit - z);
    memcpy(out + o, &zeros, 4);
    memcpy(out + o + 4, &count, 4);
    o += 8;
    for (size_t k = z; k < lit; k++)
      out[o++] = a[k] ^ b[k];
    i = lit;
  }
  return o;
}
static void delta_apply(unsigned char *s, const unsigned char *d, size_t dn) {
  size_t i = 0, o = 0;
  while (o + 8 <= dn) {
    uint32_t zeros, count;
    memcpy(&zeros, d + o, 4);
    memcpy(&count, d + o + 4, 4);
    o += 8;
    i += zeros;
    for (uint32_t k = 0; k < count; k++)
      s[i++] ^= d[o++];
  }
}
static void rewind_push(void) {
  size_t size = core.serialize_size();
  if (!size)
    return;
  if (size != rewindSize) {
    rewind_clear();
    rewindNow = malloc(size);
    rewindTmp = malloc(size * 3 + 64); // the worst packing: 9 bytes per 3 (1 literal + its header)
    if (!rewindNow || !rewindTmp || !core.serialize(rewindNow, size)) {
      rewind_clear();
      return;
    }
    rewindSize = size;
    return;
  }
  static unsigned char *next;
  static size_t nextSize;
  if (nextSize != size) {
    free(next);
    next = malloc(size);
    nextSize = next ? size : 0;
  }
  if (!next || !core.serialize(next, size))
    return;
  size_t dn = delta_pack(rewindNow, next, size, rewindTmp);
  unsigned char *d = malloc(dn);
  if (!d)
    return;
  memcpy(d, rewindTmp, dn);
  while (rewindCount && (rewindCount == REWIND_CAP || rewindBytes + dn > rewindBudget)) {
    free(rewindRing[rewindHead].d);
    rewindBytes -= rewindRing[rewindHead].n;
    rewindHead = (rewindHead + 1) % REWIND_CAP;
    rewindCount--;
  }
  int at = (rewindHead + rewindCount++) % REWIND_CAP;
  rewindRing[at].d = d;
  rewindRing[at].n = dn;
  rewindBytes += dn;
  memcpy(rewindNow, next, size);
}
// One state back: false when there's none left.
static bool rewind_pop(void) {
  if (!rewindCount || !rewindNow)
    return false;
  int at = (rewindHead + --rewindCount) % REWIND_CAP;
  delta_apply(rewindNow, rewindRing[at].d, rewindRing[at].n);
  rewindBytes -= rewindRing[at].n;
  free(rewindRing[at].d);
  return core.unserialize(rewindNow, rewindSize);
}

/* ---- pacing: the screen's refresh when it's the game's (58-62 Hz and presents wait for it), else the timer ---- */
static bool vsyncPaced;
static int screenHz;
static void pacing_start(double gameHz) {
  SDL_DisplayMode mode = {0};
  screenHz = SDL_GetCurrentDisplayMode(0, &mode) == 0 ? mode.refresh_rate : 0;
  SDL_RendererInfo info;
  vsyncPaced = !getenv("POCKET_NO_VSYNC") && screenHz >= 58 && screenHz <= 62 &&
               SDL_GetRendererInfo(renderer, &info) == 0 && (info.flags & SDL_RENDERER_PRESENTVSYNC) && gameHz > 0;
}
// A refresh-paced loop whose presents don't wait (a driver ignoring vsync) goes back to the timer.
static void pacing_check(double frameMs) {
  static double sum;
  static int count;
  if (!vsyncPaced)
    return;
  sum += frameMs;
  if (++count < 120)
    return;
  if (sum / count < 12.0) {
    vsyncPaced = false;
    fprintf(stderr, "Presents don't wait for the refresh (%.1f ms apart): pacing by the timer\n", sum / count);
  }
  sum = 0;
  count = 0;
}
/* POCKET_PERF=1: every 5 s, how the frames went. */
static double emuMs;
static void perf_frame(uint64_t start, double hz) {
  static int on = -1, n, late;
  static double sum, worst, presents, emus, filters;
  static uint64_t windowStart;
  if (on < 0)
    on = getenv("POCKET_PERF") != NULL;
  if (!on)
    return;
  uint64_t end = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
  double work = (double)(end - start) * 1000.0 / f;
  if (!windowStart)
    windowStart = end;
  n++;
  sum += work;
  presents += presentMs;
  emus += emuMs;
  filters += filterMs;
  presentMs = emuMs = filterMs = 0;
  worst = work > worst ? work : worst;
  late += work > 1000.0 / hz;
  if (end - windowStart >= 5 * f) {
    fprintf(stderr,
            "perf: %d frames in %.1f s, core %.2f ms, work %.2f ms avg (%.2f filter, %.2f present) / %.2f worst, "
            "%d late, screen %d Hz, %s, audio queued %u B\n",
            n, (double)(end - windowStart) / f, emus / n, sum / n, filters / n, presents / n, worst, late, screenHz,
            vsyncPaced ? "paced by the screen" : "paced by the timer", audio ? SDL_GetQueuedAudioSize(audio) : 0);
    n = late = 0;
    sum = worst = presents = emus = filters = 0;
    windowStart = end;
  }
}

/* ---- a game ---- */
static void panel_follow(void) {
  int ow = W, oh = H;
  SDL_GetRendererOutputSize(renderer, &ow, &oh);
  char v[32];
  const char *fixed = panel_preset(hostSet.panel); // the PANEL preset: the screen the core lays out for
  if (fixed)
    snprintf(v, sizeof v, "%s", fixed);
  else
    snprintf(v, sizeof v, "%dx%d", ow, oh);
  snprintf(windowValue, sizeof windowValue, "%dx%d", ow, oh);
  if (strcmp(v, panelValue)) {
    snprintf(panelValue, sizeof panelValue, "%s", v);
    optionsChanged = true;
  }
}
static char stateBase[PATH_MAX * 2], stateRom[256], stateCore[16];
static int stateSlot = 1;
static bool state_named(const char *suffix, char *out, size_t size) {
  int n = snprintf(out, size, "%s%s", stateBase, suffix);
  return n >= 0 && (size_t)n < size;
}
static bool state_slot_file(int slot, char *out, size_t size) {
  return pd_state_slot_path(root, stateRom, stateCore, slot, out, size);
}
static void state_select(int slot) {
  stateSlot = (slot + PD_STATE_SLOTS - 1) % PD_STATE_SLOTS + 1;
  char path[PATH_MAX * 2 + 32], text[8];
  int n = snprintf(text, sizeof text, "%d\n", stateSlot);
  if (state_named(".selected", path, sizeof path) && n > 0)
    write_file(path, text, (size_t)n, true);
}
static void state_selected_load(void) {
  char path[PATH_MAX * 2 + 32];
  size_t n;
  unsigned char *text = state_named(".selected", path, sizeof path) ? read_file(path, &n, 8) : NULL;
  if (text) {
    char value[8] = {0};
    memcpy(value, text, n < sizeof value - 1 ? n : sizeof value - 1);
    int slot = atoi(value);
    if (slot >= 1 && slot <= PD_STATE_SLOTS)
      stateSlot = slot;
  }
  free(text);
}
static unsigned char *state_serialize(size_t *size) {
  size_t n = core.serialize_size();
  unsigned char *data = n && n <= PD_STATE_FILE_MAX ? malloc(n) : NULL;
  if (!data || !core.serialize(data, n)) {
    free(data);
    *size = 0;
    return NULL;
  }
  *size = n;
  return data;
}
static bool state_load_file(const char *path) {
  size_t n;
  unsigned char *data = read_file(path, &n, PD_STATE_FILE_MAX);
  /* armGBA accepts several historical layouts. The core validates them; an exact current-size check would reject them. */
  bool ok = data && n && core.unserialize(data, n);
  free(data);
  if (ok)
    rewind_clear();
  return ok;
}
static void core_callbacks(void) {
  core.set_video_refresh(video_refresh);
  core.set_audio_sample(audio_sample);
  core.set_audio_sample_batch(audio_batch);
  core.set_input_poll(input_poll);
  core.set_input_state(input_state);
}
/* A failed automatic deserialization may have touched the core. Reboot it before exposing the game or its battery. */
static bool core_restart(const struct retro_game_info *info) {
  core.unload_game();
  core.deinit();
  pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
  core.set_environment(environment);
  core.init();
  core_callbacks();
  if (!core.load_game(info))
    return false;
  core.get_system_av_info(&av);
  frameW = frameH = 0;
  frameCopyValid = false;
  frame_texture();
  return true;
}

#define STATE_THUMB_W 96
#define STATE_THUMB_H 72
struct state_thumb_header {
  char magic[4];
  uint32_t w, h;
};
static bool statePreviewMenu, statePreviewHave;
static int statePreviewW, statePreviewH;
static uint32_t statePreviewPixels[STATE_THUMB_W * STATE_THUMB_H];
static void *state_thumbnail(size_t *outSize) {
  *outSize = 0;
  if (!frameCopyValid || frameW <= 0 || frameH <= 0)
    return NULL;
  double scale = STATE_THUMB_W / (double)frameW;
  if (scale > STATE_THUMB_H / (double)frameH)
    scale = STATE_THUMB_H / (double)frameH;
  if (scale > 1)
    scale = 1;
  int w = frameW * scale + 0.5, h = frameH * scale + 0.5;
  if (w < 1)
    w = 1;
  if (h < 1)
    h = 1;
  size_t size = sizeof(struct state_thumb_header) + (size_t)w * h * 4;
  unsigned char *out = malloc(size);
  if (!out)
    return NULL;
  struct state_thumb_header header = {{'P', 'D', 'T', '1'}, (uint32_t)w, (uint32_t)h};
  memcpy(out, &header, sizeof header);
  uint32_t *thumb = (uint32_t *)(out + sizeof header);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const unsigned char *src = frameCopy + (size_t)(y * frameH / h) * frameCopyStride +
                                 x * frameW / w * (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2);
      uint32_t color;
      if (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        memcpy(&color, src, 4);
        color &= 0xFFFFFF;
      } else {
        uint16_t p;
        memcpy(&p, src, 2);
        if (pixelFormat == RETRO_PIXEL_FORMAT_RGB565)
          color = ((p >> 11) * 255 / 31) << 16 | (((p >> 5) & 63) * 255 / 63) << 8 | (p & 31) * 255 / 31;
        else
          color = (((p >> 10) & 31) * 255 / 31) << 16 | (((p >> 5) & 31) * 255 / 31) << 8 | (p & 31) * 255 / 31;
      }
      thumb[y * w + x] = color;
    }
  *outSize = size;
  return out;
}
static void state_preview_load(int slot) {
  statePreviewHave = false;
  char state[PATH_MAX * 2 + 32], path[PATH_MAX * 2 + 48];
  if (!state_slot_file(slot, state, sizeof state) || !pd_state_sidecar(state, ".thumb", path, sizeof path))
    return;
  size_t n;
  unsigned char *data = read_file(path, &n, sizeof(struct state_thumb_header) + sizeof statePreviewPixels);
  if (!data || n < sizeof(struct state_thumb_header)) {
    free(data);
    return;
  }
  struct state_thumb_header header;
  memcpy(&header, data, sizeof header);
  bool valid = !memcmp(header.magic, "PDT1", 4) && header.w && header.h && header.w <= STATE_THUMB_W &&
               header.h <= STATE_THUMB_H && n == sizeof header + (size_t)header.w * header.h * 4;
  if (valid) {
    statePreviewW = (int)header.w;
    statePreviewH = (int)header.h;
    memcpy(statePreviewPixels, data + sizeof header, (size_t)header.w * header.h * 4);
    statePreviewHave = true;
  }
  free(data);
}
static void state_preview_draw(struct pd_canvas *c) {
  if (!statePreviewMenu || c->w < 240 || c->h < 144)
    return;
  int x = c->w - STATE_THUMB_W - 14, y = 43;
  pd_fill(c, x - 2, y - 2, STATE_THUMB_W + 4, STATE_THUMB_H + 4, PD_VALUE);
  pd_fill(c, x - 1, y - 1, STATE_THUMB_W + 2, STATE_THUMB_H + 2, 0x101010);
  if (statePreviewHave)
    for (int py = 0; py < statePreviewH; py++)
      for (int px = 0; px < statePreviewW; px++)
        pd_fill(c, x + px, y + py, 1, 1, statePreviewPixels[py * statePreviewW + px]);
  else
    pd_text_fit(c, x + 4, y + STATE_THUMB_H / 2, STATE_THUMB_W - 8, "NO PREVIEW", PD_MUTED, PD_NO_SHADOW);
}
/* The .sav: the core's save memory, then its cartridge clock (RETRO_MEMORY_RTC: a Game Boy MBC3's, Crystal's) right
 * after it - mGBA's own layout, which the old host wrote too. NULL out: the size. */
static size_t save_image(unsigned char *out) {
  void *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM), *rtc = core.get_memory_data(RETRO_MEMORY_RTC);
  size_t n = mem ? core.get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0, rn = rtc ? core.get_memory_size(RETRO_MEMORY_RTC) : 0;
  if (out && n)
    memcpy(out, mem, n);
  if (out && n && rn)
    memcpy(out + n, rtc, rn);
  return n ? n + rn : 0;
}
// The file's bytes into the core: the save memory, and what follows it into the clock (a GBA .sav's 16-byte clock
// footer from mGBA finds no clock memory on armGBA, so a dead battery recorded there never comes back).
static void save_load(const unsigned char *d, size_t fn) {
  void *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM), *rtc = core.get_memory_data(RETRO_MEMORY_RTC);
  size_t n = mem ? core.get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0, rn = rtc ? core.get_memory_size(RETRO_MEMORY_RTC) : 0;
  if (!n)
    return;
  memcpy(mem, d, fn < n ? fn : n);
  if (rn && fn > n)
    memcpy(rtc, d + n, fn - n < rn ? fn - n : rn);
}
struct save_baseline {
  unsigned char *data;
  size_t size;
};
static bool sram_write(const char *sav, struct save_baseline *copy, bool sync) {
  static unsigned char *now;
  static size_t nowSize;
  size_t n = save_image(NULL);
  if (!n)
    return true;
  if (n > nowSize) {
    free(now);
    now = malloc(n);
    nowSize = now ? n : 0;
    if (!now)
      return false;
  }
  save_image(now);
  if (copy->data && n == copy->size && !memcmp(now, copy->data, n) && exists(sav))
    return true;
  if (!write_file(sav, now, n, sync)) {
    notice("Save write failed");
    return false;
  }
  unsigned char *previous = copy->data;
  size_t previousSize = copy->size;
  copy->data = now;
  copy->size = n;
  now = previous;
  nowSize = previousSize;
  return true;
}
static void sram_refresh(struct save_baseline *copy) {
  free(copy->data);
  copy->data = NULL;
  copy->size = 0;
  size_t n = save_image(NULL);
  if (n) {
    copy->data = malloc(n);
    if (copy->data)
      copy->size = save_image(copy->data);
  }
}
static void battery_reload(const char *sav, struct save_baseline *copy) {
  size_t n = save_image(NULL), fn;
  unsigned char *data = n ? read_file(sav, &fn, PD_STATE_FILE_MAX) : NULL;
  if (data)
    save_load(data, fn);
  free(data);
  sram_refresh(copy);
}
static bool state_save_slot(int slot) {
  char path[PATH_MAX * 2 + 32];
  size_t n, thumbSize;
  unsigned char *data = state_slot_file(slot, path, sizeof path) ? state_serialize(&n) : NULL;
  void *thumb = data ? state_thumbnail(&thumbSize) : NULL;
  bool ok = data && pd_state_slot_save(path, data, n, thumb, thumbSize);
  free(thumb);
  free(data);
  return ok;
}
static bool state_before_load(const char *sav, const char *backups, struct save_baseline *copy) {
  return sram_write(sav, copy, true) && save_backup(sav, backups);
}
static bool state_load_slot(int slot, const char *sav, const char *backups, struct save_baseline *copy) {
  char path[PATH_MAX * 2 + 32], undo[PATH_MAX * 2 + 48];
  if (!state_slot_file(slot, path, sizeof path) || !state_named(".undo-load.ss", undo, sizeof undo) ||
      !state_before_load(sav, backups, copy))
    return false;
  size_t n;
  unsigned char *snapshot = state_serialize(&n);
  /* No load without a committed way back to the live state it replaces. */
  bool ok = snapshot && write_file(undo, snapshot, n, true);
  free(snapshot);
  if (ok)
    ok = state_load_file(path);
  if (ok)
    sram_refresh(copy);
  return ok;
}
static bool state_undo_load(const char *sav, const char *backups, struct save_baseline *copy) {
  char undo[PATH_MAX * 2 + 48];
  if (!state_named(".undo-load.ss", undo, sizeof undo) || !pd_state_exists(undo) ||
      !state_before_load(sav, backups, copy))
    return false;
  bool ok = state_load_file(undo);
  if (ok)
    sram_refresh(copy);
  return ok;
}
static uint64_t state_hash(const void *bytes, size_t size) {
  const unsigned char *p = bytes;
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; i++) {
    hash ^= p[i];
    hash *= 1099511628211ull;
  }
  return hash;
}
static bool state_resume_fail(const char *stage) {
  const char *fail = getenv("POCKET_RESUME_FAIL");
  return fail && !strcmp(fail, stage);
}
static bool state_resume_write(const char *sav) {
  char resume[PATH_MAX * 2 + 48], battery[PATH_MAX * 2 + 48], fresh[PATH_MAX * 2 + 48], ready[PATH_MAX * 2 + 48];
  if (!state_named(".resume.ss", resume, sizeof resume) || !state_named(".resume.sav", battery, sizeof battery) ||
      !state_named(".fresh", fresh, sizeof fresh) || !state_named(".resume.ready", ready, sizeof ready))
    return false;
  size_t n, thumbSize, batterySize = save_image(NULL), fileSize = 0;
  unsigned char *data = state_serialize(&n);
  void *thumb = data ? state_thumbnail(&thumbSize) : NULL;
  unsigned char *batteryData = data && batterySize ? read_file(sav, &fileSize, PD_STATE_FILE_MAX) : NULL;
  bool ok = data && (!batterySize || (batteryData && fileSize == batterySize));
  if (ok && !state_resume_fail("battery"))
    ok = write_file(battery, batteryData, fileSize, true);
  else if (state_resume_fail("battery"))
    ok = false;
  if (ok && !state_resume_fail("state"))
    ok = write_file(resume, data, n, true);
  else if (state_resume_fail("state"))
    ok = false;
  char thumbPath[PATH_MAX * 2 + 64];
  if (ok && pd_state_sidecar(resume, ".thumb", thumbPath, sizeof thumbPath) &&
      (!thumb || !write_file(thumbPath, thumb, thumbSize, true)))
    pd_state_remove_file(thumbPath);
  /* The content hashes are the commit. Any older manifest left by a partial update rejects the mixed bundle. */
  if (ok && !state_resume_fail("ready")) {
    char text[96];
    int len = snprintf(text, sizeof text, "PDR1 %016llx %016llx\n", (unsigned long long)state_hash(data, n),
                       (unsigned long long)state_hash(batteryData, fileSize));
    ok = len > 0 && (size_t)len < sizeof text && write_file(ready, text, (size_t)len, true);
  } else if (state_resume_fail("ready"))
    ok = false;
  if (ok)
    pd_state_remove_file(fresh);
  free(batteryData);
  free(thumb);
  free(data);
  return ok;
}
enum resume_status { RESUME_MISSING, RESUME_FRESH, RESUME_MISMATCH, RESUME_READY };
static bool state_resume_complete(const char *resume, const char *battery, const char *ready) {
  size_t stateSize, batterySize, textSize;
  unsigned char *state = read_file(resume, &stateSize, PD_STATE_FILE_MAX);
  unsigned char *batteryData = read_file(battery, &batterySize, PD_STATE_FILE_MAX);
  unsigned char *text = pd_state_exists(ready) ? read_file(ready, &textSize, 96) : NULL;
  bool complete;
  if (text) {
    char manifest[97] = {0};
    memcpy(manifest, text, textSize < sizeof manifest - 1 ? textSize : sizeof manifest - 1);
    unsigned long long stateHash, batteryHash;
    char tail;
    complete = state && batteryData && sscanf(manifest, "PDR1 %16llx %16llx %c", &stateHash, &batteryHash, &tail) == 2 &&
               stateHash == state_hash(state, stateSize) && batteryHash == state_hash(batteryData, batterySize);
  } else {
    /* This host's original bundle wrote the battery first. Reject the partial-new-battery ordering. */
    struct stat stateInfo, batteryInfo;
    complete = state && batteryData && !stat(resume, &stateInfo) && !stat(battery, &batteryInfo) &&
               stateInfo.st_mtime >= batteryInfo.st_mtime;
  }
  free(text);
  free(batteryData);
  free(state);
  return complete;
}
static enum resume_status state_resume_status(const char *sav) {
  char resume[PATH_MAX * 2 + 48], battery[PATH_MAX * 2 + 48], fresh[PATH_MAX * 2 + 48], ready[PATH_MAX * 2 + 48];
  if (!state_named(".resume.ss", resume, sizeof resume) || !state_named(".resume.sav", battery, sizeof battery) ||
      !state_named(".fresh", fresh, sizeof fresh) || !state_named(".resume.ready", ready, sizeof ready) ||
      !pd_state_exists(resume))
    return RESUME_MISSING;
  if (pd_state_exists(fresh))
    return RESUME_FRESH;
  if (!state_resume_complete(resume, battery, ready))
    return RESUME_MISMATCH;
  size_t expectedSize, currentSize = 0, memorySize = save_image(NULL);
  unsigned char *expected = read_file(battery, &expectedSize, PD_STATE_FILE_MAX);
  unsigned char *current = memorySize ? read_file(sav, &currentSize, PD_STATE_FILE_MAX) : NULL;
  bool match = expected && (memorySize ? current && same_save(expected, expectedSize, current, currentSize)
                                       : expectedSize == 0);
  free(current);
  free(expected);
  return match ? RESUME_READY : RESUME_MISMATCH;
}
static bool state_import_save(const char *from, const char *sav) {
  void *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM), *rtc = core.get_memory_data(RETRO_MEMORY_RTC);
  size_t n = mem ? core.get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0;
  size_t rn = rtc ? core.get_memory_size(RETRO_MEMORY_RTC) : 0;
  size_t fn;
  unsigned char *data = read_file(from, &fn, PD_STATE_FILE_MAX);
  bool compatible = data && n && (fn == n || (rn && fn == n + rn) || (!rn && fn > n && fn - n <= 64));
  char fresh[PATH_MAX * 2 + 48];
  bool ok = compatible && state_named(".fresh", fresh, sizeof fresh) && write_file(fresh, "fresh\n", 6, true) &&
            write_file(sav, data, fn, true);
  free(data);
  return ok;
}
// The settings the old host kept in DATA/settings.ini become the core's (SAVES/pokedaisy/settings.ini), once.
static void settings_migrate(void) {
  char from[PATH_MAX + 16], dir[PATH_MAX + 16], to[PATH_MAX + 32];
  snprintf(from, sizeof from, "%s/settings.ini", root);
  snprintf(dir, sizeof dir, "%s/pokedaisy", savesDir);
  snprintf(to, sizeof to, "%s/settings.ini", dir);
  size_t n;
  unsigned char *d = exists(to) ? NULL : read_file(from, &n, 1u << 20);
  if (!d)
    return;
  pd_mkdir(dir);
  if (write_file(to, d, n, false))
    fprintf(stderr, "Settings moved from %s to %s\n", from, to);
  free(d);
}
static const char *core_for(const char *path) {
  const char *ext = strrchr(path, '.');
  if (ext && !strcasecmp(ext, ".gba"))
    return "pokedaisy_gba_libretro" PD_CORE_EXT;
  if (ext && (!strcasecmp(ext, ".gb") || !strcasecmp(ext, ".gbc")))
    return "pokedaisy_gbc_libretro" PD_CORE_EXT;
  if (ext && !strcasecmp(ext, ".nds"))
    return "pokedaisy_ds_libretro" PD_CORE_EXT;
  return NULL;
}
/* The game list shows the main Pokemon games only (the firmware's folders hold every other game too), told by the cart's
 * header: a GBA game code of FireRed / LeafGreen / Emerald / Ruby / Sapphire (the hacks keep theirs) or a hack's own,
 * a Game Boy title of Red / Blue / Green / Yellow / Gold / Silver / Crystal and their hacks (not Pinball, the card
 * games or Puzzle Challenge), a DS game code of Diamond / Pearl / Platinum / HeartGold / SoulSilver / Black / White /
 * their sequels. */
static bool pokemon_rom(const char *path) {
  unsigned char h[0x150];
  FILE *f = fopen(path, "rb");
  size_t n = f ? fread(h, 1, sizeof h, f) : 0;
  if (f)
    fclose(f);
  const char *ext = strrchr(path, '.');
  if (!ext)
    return false;
  if (!strcasecmp(ext, ".gba")) {
    // Retail's first three letters (the fourth is the language); a hack's own code whole (Monomyth, Ultra Violet).
    static const char *const codes[] = {"BPR", "BPG", "BPE", "AXV", "AXP", "MONE", "PUVV"};
    for (size_t i = 0; n >= 0xB0 && i < sizeof codes / sizeof *codes; i++)
      if (!memcmp(h + 0xAC, codes[i], strlen(codes[i])))
        return true;
    return false;
  }
  if (!strcasecmp(ext, ".nds")) {
    static const char *const codes[] = {"ADA", "APA", "CPU", "IPK", "IPG", "IRB", "IRA", "IRE", "IRD"};
    for (size_t i = 0; n >= 0x10 && i < sizeof codes / sizeof *codes; i++)
      if (!memcmp(h + 0x0C, codes[i], 3))
        return true;
    return false;
  }
  if (n < 0x144)
    return false;
  char title[17] = {0};
  memcpy(title, h + 0x134, 16);
  static const char *const spinOffs[] = {"POKECARD", "POKEPINBALL", "POKEMON PC", "POKEMONPC", "POKEMON-CG", "POKEMON PINB"};
  for (size_t i = 0; i < sizeof spinOffs / sizeof *spinOffs; i++)
    if (!strncmp(title, spinOffs[i], strlen(spinOffs[i])))
      return false;
  return !strncmp(title, "POKE", 4) || !strncmp(title, "PM_", 3) || !strncmp(title, "PK", 2);
}
#ifdef POCKET_DSPERATE
extern int pd_ds_run(const char *, const char *, int, const char *, SDL_Renderer *, bool (*)(void));
static bool ds_running(void) { return running != 0; }
#endif
// The host menu's rows, top to bottom (fast-forward is the FAST FORWARD hotkey's, held).
enum { M_RESUME, M_SAVE, M_LOAD, M_HOTKEYS, M_KEYBOARD, M_CONTROLLER, M_STATES, M_UNDO_SAVE, M_UNDO_LOAD,
       M_AUTO_RESUME, M_REWIND, M_FFSPEED, M_LCD, M_SWAP, M_QUIT, M_COUNT };
static void run_game(const char *path, int maxFrames, const char *shot, bool fromList) {
#ifdef POCKET_DSPERATE
  const char *ext = strrchr(path, '.');
  if (ext && !strcasecmp(ext, ".nds")) {
    if (audio) {
      SDL_CloseAudioDevice(audio);
      audio = 0;
    }
    appError = pd_ds_run(path, root, maxFrames, shot, renderer, ds_running);
    // The DS backend owns its device while running; the GB / GBA games get theirs back.
    audio_open();
    audioLost = !audio;
    held = pressed = padHeld = 0;
    return;
  }
#endif
  const char *coreName = core_for(path);
  char corePath[PATH_MAX * 2];
  const char *dir = getenv("POCKET_CORE_DIR") ? getenv("POCKET_CORE_DIR") : exeDir;
  snprintf(corePath, sizeof corePath, "%s/%s", dir, coreName ? coreName : "");
  if (!coreName || !core_open(corePath)) {
    appError = 1;
    notice(coreName ? "Core missing: put it beside " PD_HOST_NAME : "Unsupported ROM");
    return;
  }
  struct retro_system_info system = {0};
  core.get_system_info(&system);
  size_t romSize = 0;
  unsigned char *rom = system.need_fullpath ? NULL : read_file(path, &romSize, 64u << 20);
  if (!system.need_fullpath && (!rom || romSize < 0x150)) {
    free(rom);
    dlclose(core.dl);
    appError = 1;
    notice("Cannot read ROM");
    return;
  }
  // The ROM's file name: its whole name with the extension, without the folder. A Windows path may use either slash
  // (a command line launch or a shortcut passes backslashes), and the save's name follows the ROM's.
  const char *name = strrchr(path, '/');
#ifdef _WIN32
  const char *back = strrchr(path, '\\');
  if (back && (!name || back > name))
    name = back;
#endif
  name = name ? name + 1 : path;
  char sav[PATH_MAX * 2], ss[PATH_MAX * 2], backups[PATH_MAX * 2];
  if (saveFile) {
    snprintf(sav, sizeof sav, "%s", saveFile);
    snprintf(backups, sizeof backups, "%s/backups", root);
  } else {
    pd_save_path(savesDir, path, sav, sizeof sav);
    snprintf(backups, sizeof backups, "%s/pokedaisy-backups", savesDir);
  }
  // DS uses L2 for the Companion and R2 for screen switching. Keep the host's DS bindings separate.
  const bool dsGame = !strcmp(coreName, "pokedaisy_ds_libretro" PD_CORE_EXT);
  int *hotkeys = dsGame ? hostSet.dsHotkey : hostSet.hotkey;
  // armGBA's states aren't mGBA's: each core its own files. Slot 1 keeps the old single-state name.
  snprintf(stateRom, sizeof stateRom, "%s", name);
  snprintf(stateCore, sizeof stateCore, "%s", strstr(coreName, "_gba_") ? ".armgba" : "");
  snprintf(stateBase, sizeof stateBase, "%s/states/%s%s", root, name, stateCore);
  stateSlot = 1;
  state_selected_load();
  state_slot_file(1, ss, sizeof ss);
  if (strstr(coreName, "_gba_")) {
    // A GBA state from before armGBA is mGBA's: it can't load here, so say so instead of "No state saved yet".
    char old[PATH_MAX * 2 + 16];
    snprintf(old, sizeof old, "%s/states/%s.ss", root, name);
    if (exists(old) && !exists(ss))
      notice("Your older save state doesn't load in this version");
  }
  if (!save_backup(sav, backups)) {
    free(rom);
    dlclose(core.dl);
    appError = 1;
    notice("Save backup failed; game not opened");
    return;
  }
  settings_migrate();
  optionN = 0;
  pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
  panel_follow();
  optionsChanged = false;
  core.set_environment(environment);
  core.init();
  core_callbacks();
  struct retro_game_info info = {path, rom, romSize, NULL};
  if (!core.load_game(&info)) {
    core.deinit();
    dlclose(core.dl);
    free(rom);
    appError = 1;
    notice("ROM load failed");
    return;
  }
  core.get_system_av_info(&av);
  srcRate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 32768;
  double hz = av.timing.fps > 0 ? av.timing.fps : 59.7275;
  frameW = frameH = 0;
  frameCopyValid = false;
  frame_texture();
  // An import has already had the normal pre-open backup; validate its size before replacing the battery atomically.
  if (importFile && !state_import_save(importFile, sav)) {
    core.unload_game();
    core.deinit();
    dlclose(core.dl);
    memset(&core, 0, sizeof core);
    free(rom);
    appError = 1;
    notice("Save import failed: wrong file or save size");
    return;
  }
  // The battery save: the file's bytes into the core's, and written back whenever they change.
  struct save_baseline sramCopy = {0};
  battery_reload(sav, &sramCopy);
  if (hostSet.autoResume) {
    enum resume_status resume = state_resume_status(sav);
    if (resume == RESUME_READY) {
      char resumePath[PATH_MAX * 2 + 48];
      bool loaded = state_named(".resume.ss", resumePath, sizeof resumePath) && state_load_file(resumePath);
      if (loaded) {
        battery_reload(sav, &sramCopy); // the file's battery and clock win over the state's copies
        notice("Resumed from last session");
      } else {
        notice("Resume load failed; game restarted");
        if (!core_restart(&info)) {
          free(sramCopy.data);
          core.deinit();
          dlclose(core.dl);
          memset(&core, 0, sizeof core);
          free(rom);
          appError = 1;
          notice("Core restart failed");
          return;
        }
        srcRate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 32768;
        hz = av.timing.fps > 0 ? av.timing.fps : 59.7275;
        battery_reload(sav, &sramCopy);
      }
    } else if (resume == RESUME_MISMATCH)
      notice("Resume skipped: battery save changed");
    else if (resume == RESUME_FRESH)
      notice("Resume skipped: imported save");
  }
  int rewindMB = getenv("POCKET_REWIND_MB") ? atoi(getenv("POCKET_REWIND_MB")) : 32;
  rewindBudget = (size_t)(rewindMB > 0 ? rewindMB : 32) << 20; // a bad value (negative, 0) is the default
  rewindTick = 0;
  bool menuOpen = false, leave = false;
  struct pd_menu menu, hkMenu, slotMenu;
  static char rows[M_COUNT][48], hkRows[HK_ACTIONS + 1][48], slotRows[PD_STATE_SLOTS][48];
  static const char *rowPtrs[M_COUNT], *hkPtrs[HK_ACTIONS + 1], *slotPtrs[PD_STATE_SLOTS];
  static const char *const hkNames[HK_ACTIONS] = {"FAST FORWARD", "REWIND", "SAVE STATE", "LOAD STATE",
                                                  "UNDO SAVE", "UNDO LOAD", "SLOT NEXT", "SLOT PREV"};
  bool hkPage = false, kbPage = false, padPage = false, slotPage = false;
  unsigned latched = 0, menuConsumed = 0;
  bool modifierPending = false, modifierUsed = false;
  int pulseLeft = 0; // frames the modifier's own tap still reaches the game
  int capturing = -1; // the HOTKEYS row waiting for a button
  // LOAD STATE and QUIT ask first, CANCEL on top (as the hosts before this one): a stray A never loses the game's progress.
  struct pd_menu confirmMenu;
  static const char *confirmRows[2];
  int confirming = 0; // the menu row asked about (M_LOAD / M_QUIT), 0 = none
  int frames = 0;
  uint64_t due = SDL_GetPerformanceCounter(), lastStart = 0, lastFrame = due;
  ffQuick = false; // a new game starts at normal speed
  pacing_start(hz);
  while (running && !leave) {
    uint64_t frameStart = SDL_GetPerformanceCounter();
    // Back from a pause (a stopped process, sleep): no catching up.
    if (lastStart && (frameStart - lastStart) * 4 > SDL_GetPerformanceFrequency())
      due = frameStart;
    lastStart = frameStart;
    events();
    audio_check();
    repeat_directions();
    if (outputChanged) {
      outputChanged = false;
      panel_follow();
    }
    const unsigned menuChord = (1u << START) | (1u << SELECT);
    const bool menuChordDown = (held & menuChord) == menuChord;
    bool menuKey = padCapture < 0 && (down(MENU) || (comboMenu && menuChordDown && (pressed & menuChord)));
    snprintf(rows[M_RESUME], 48, "RESUME");
    snprintf(rows[M_SAVE], 48, "SAVE STATE  SLOT %d", stateSlot);
    snprintf(rows[M_LOAD], 48, "LOAD STATE  SLOT %d", stateSlot);
    if (hostSet.ffSpeed)
      snprintf(rows[M_FFSPEED], 48, "FF SPEED  %dx", hostSet.ffSpeed);
    else
      snprintf(rows[M_FFSPEED], 48, "FF SPEED  MAX");
    snprintf(rows[M_REWIND], 48, "REWIND  %s", hostSet.rewind ? "ON" : "OFF");
    snprintf(rows[M_AUTO_RESUME], 48, "AUTO RESUME  %s", hostSet.autoResume ? "ON" : "OFF");
    snprintf(rows[M_LCD], 48, "DISPLAY FILTER  %s", displayModeNames[hostSet.displayMode]);
    snprintf(rows[M_SWAP], 48, "A/B BUTTONS  %s", hostSet.swapAB ? "SWAPPED" : "NORMAL");
    snprintf(rows[M_HOTKEYS], 48, "HOTKEYS");
    snprintf(rows[M_KEYBOARD], 48, "KEYBOARD");
    snprintf(rows[M_CONTROLLER], 48, "CONTROLLER");
    snprintf(rows[M_STATES], 48, "STATE SLOTS");
    snprintf(rows[M_UNDO_SAVE], 48, "UNDO SAVE");
    snprintf(rows[M_UNDO_LOAD], 48, "UNDO LOAD");
    snprintf(rows[M_QUIT], 48, fromList ? "QUIT TO GAME LIST" : "QUIT");
    for (int i = 0; i < M_COUNT; i++)
      rowPtrs[i] = rows[i];
    for (int i = 0; i < HK_ACTIONS; i++) {
      snprintf(hkRows[i], 48, "%s  %s", hkNames[i], capturing == i ? "..." : hotkey_label(hotkeys[i]));
      hkPtrs[i] = hkRows[i];
    }
    snprintf(hkRows[HK_ACTIONS], 48, "MODIFIER  %s",
             capturing == HK_ACTIONS ? "..." : hotkey_label(hostSet.modifier));
    hkPtrs[HK_ACTIONS] = hkRows[HK_ACTIONS];
    for (int i = 0; i < PD_STATE_SLOTS; i++) {
      char path[PATH_MAX * 2 + 32];
      bool present = state_slot_file(i + 1, path, sizeof path) && pd_state_exists(path);
      snprintf(slotRows[i], 48, "SLOT %d  %s%s", i + 1, present ? "SAVED" : "EMPTY", stateSlot == i + 1 ? "  *" : "");
      slotPtrs[i] = slotRows[i];
    }
    kb_menu_build();
    pad_menu_build();
    menuConsumed &= (unsigned)held;
    if (inputReset || menuKey || menuChordDown) {
      modifierPending = modifierUsed = false;
      pulseLeft = 0;
      inputReset = false;
      if (menuKey || menuChordDown)
        menuConsumed |= held;
    }
    if (menuKey) {
      menuOpen = !menuOpen;
      hkPage = kbPage = padPage = slotPage = false;
      confirming = 0;
      capturing = -1;
      kbCapture = padCapture = -1;
      if (menuOpen) {
        pd_menu_init(&menu, "POKEDAISY", rowPtrs, M_COUNT);
        menu.overlay = true;
      }
    }
    unsigned hkFired = 0, hkOn = 0;
    hotkeyMask = 0;
    unsigned modifier = hostSet.modifier < 0 ? 0 : 1u << hostSet.modifier;
    latched &= held;
    unsigned modifierPulse = 0;
    if (!menuOpen && !menuKey && !menuChordDown) {
      if (pressed & modifier & ~menuConsumed)
        modifierPending = true;
      if (!(menuConsumed & modifier))
        hkOn = hotkeys_held(hotkeys, modifier, &latched, &hkFired);
      // The DS core handles its stylus shortcuts with Select held.
      const unsigned stylus = (1u << UP) | (1u << DOWN) | (1u << LEFT) | (1u << RIGHT) | (1u << A) | (1u << Y);
      bool dsStylus = dsGame && hostSet.modifier == SELECT && (held & modifier)
                      && (held & stylus & ~latched & ~menuConsumed) && !(latched & hotkeyMask);
      modifierUsed |= !!(latched & hotkeyMask) || dsStylus;
      if (!(held & modifier)) {
        if (modifierPending && !modifierUsed)
          pulseLeft = MODIFIER_TAP_FRAMES;
        modifierPending = modifierUsed = false;
      } else
        pulseLeft = 0; // held again: it's the modifier now, not a tap
      if (pulseLeft > 0)
        modifierPulse = modifier;
      if (!dsStylus)
        hotkeyMask |= modifier;
    } else
      pulseLeft = 0;
    hotkeyMask |= latched | menuConsumed;
    if (hkFired & (1u << HK_SAVE)) {
      bool ok = state_save_slot(stateSlot);
      char text[64];
      snprintf(text, sizeof text, ok ? "State saved in slot %d" : "State save failed (slot %d)", stateSlot);
      notice(text);
    }
    if (hkFired & (1u << HK_LOAD)) {
      char path[PATH_MAX * 2 + 32];
      bool present = state_slot_file(stateSlot, path, sizeof path) && pd_state_exists(path);
      notice(!present ? "No state in this slot"
                      : state_load_slot(stateSlot, sav, backups, &sramCopy) ? "State loaded" : "State load failed");
    }
    if (hkFired & (1u << HK_UNDO_SAVE)) {
      char path[PATH_MAX * 2 + 32];
      notice(state_slot_file(stateSlot, path, sizeof path) && pd_state_undo_save(path) ? "Save undone" : "Nothing to undo");
    }
    if (hkFired & (1u << HK_UNDO_LOAD))
      notice(state_undo_load(sav, backups, &sramCopy) ? "Load undone" : "Nothing to undo");
    if (hkFired & (1u << HK_SLOT_NEXT))
      state_select(stateSlot + 1);
    if (hkFired & (1u << HK_SLOT_PREV))
      state_select(stateSlot - 1);
    if (hkFired & ((1u << HK_SLOT_NEXT) | (1u << HK_SLOT_PREV))) {
      char text[32];
      snprintf(text, sizeof text, "State slot %d", stateSlot);
      notice(text);
    }
    if ((hkFired & (1u << HK_REWIND)) && !hostSet.rewind)
      notice("REWIND is off: turn it on in the menu");
    const bool menuFrame = menuOpen; // the frame that closes the menu is still the menu's
    if (menuOpen && padCapture >= 0) {
      if (down(B)) {
        padCapture = -1;
      } else if (down(SELECT)) {
        pd_pad_clear(&padMap, padCapture);
        host_save();
        padCapture = -1;
        input_mapping_reset();
      } else if (lastPadControl >= 0) {
        pd_pad_bind(&padMap, lastPadControl, padCapture);
        host_save();
        padCapture = -1;
        input_mapping_reset();
      }
      padMenu.title = padCapture >= 0 ? "PRESS A BUTTON / TRIGGER" : "CONTROLLER";
    } else if (menuOpen && capturing >= 0) {
      // The next assignable button is the hotkey (taken from any other action that had it); B cancels, SELECT = OFF.
      if (down(B))
        capturing = -1;
      else if (down(SELECT)) {
        if (capturing == HK_ACTIONS) {
          hostSet.modifier = hostSet.modifier == SELECT ? -1 : SELECT;
          if (hostSet.modifier >= 0)
            modifier_takes(hostSet.modifier);
          ds_hotkeys_follow_modifier();
        } else
          hotkeys[capturing] = -1;
        host_save();
        capturing = -1;
      } else
        for (int k = 0; k < NKEYS; k++)
          if (down(k) && (hotkey_assignable(k) || (capturing == HK_ACTIONS && k == START))) {
            if (capturing == HK_ACTIONS) {
              hostSet.modifier = k;
              modifier_takes(k);
              ds_hotkeys_follow_modifier();
            } else if (k == hostSet.modifier) {
              notice("That button is the MODIFIER");
              capturing = -1;
              break;
            } else {
              for (int j = 0; j < HK_ACTIONS; j++)
                if (hotkeys[j] == k)
                  hotkeys[j] = -1;
              hotkeys[capturing] = k;
            }
            host_save();
            capturing = -1;
            break;
          }
      hkMenu.title = capturing >= 0 ? "PRESS A BUTTON" : "HOTKEYS";
    } else if (menuOpen && confirming) {
      if (down(UP))
        pd_menu_move(&confirmMenu, -1);
      if (down(DOWN))
        pd_menu_move(&confirmMenu, 1);
      if (down(B))
        confirming = 0;
      if (down(A)) {
        if (confirmMenu.selected == 1 && confirming == M_LOAD) {
          notice(state_load_slot(stateSlot, sav, backups, &sramCopy) ? "State loaded" : "State load failed");
          menuOpen = false;
        } else if (confirmMenu.selected == 1 && confirming == M_QUIT) {
          leave = true;
        }
        confirming = 0;
      }
    } else if (menuOpen && hkPage) {
      if (down(UP))
        pd_menu_move(&hkMenu, -1);
      if (down(DOWN))
        pd_menu_move(&hkMenu, 1);
      if (down(B))
        hkPage = false;
      if (down(A)) {
        capturing = hkMenu.selected;
        hkMenu.title = "PRESS A BUTTON";
        notice(capturing == HK_ACTIONS ? "SELECT: select / off   B: cancel" : "B: cancel   SELECT: off");
      }
    } else if (menuOpen && kbCapture >= 0) {
      // Escape cancels and Backspace turns the row off, rather than capturing the keyboard's own menu/select keys.
      if (lastKey == SDLK_ESCAPE)
        lastKey = 0, pressed |= 1ull << B;
      else if (lastKey == SDLK_BACKSPACE || lastKey == SDLK_DELETE)
        lastKey = 0, pressed |= 1ull << SELECT;
      if (lastKey) {
        for (int i = 0; i < KB_COUNT; i++)
          if (kbKeys[i] == lastKey)
            kbKeys[i] = SDLK_UNKNOWN;
        kbKeys[kbCapture] = lastKey;
        host_save();
        kbCapture = -1;
        kbMenu.title = "KEYBOARD";
      } else if (down(B)) {
        kbCapture = -1;
        kbMenu.title = "KEYBOARD";
      } else if (down(SELECT)) {
        kbKeys[kbCapture] = SDLK_UNKNOWN;
        host_save();
        kbCapture = -1;
        kbMenu.title = "KEYBOARD";
      }
    } else if (menuOpen && kbPage) {
      if (down(UP))
        pd_menu_move(&kbMenu, -1);
      if (down(DOWN))
        pd_menu_move(&kbMenu, 1);
      if (down(B))
        kbPage = false;
      if (down(A)) {
        kbCapture = kbMenu.selected;
        held = pressed = 0;
        kbMenu.title = "PRESS A KEY";
        notice("Press a key. Esc: cancel   Backspace: off");
      }
    } else if (menuOpen && padPage) {
      if (down(UP))
        pd_menu_move(&padMenu, -1);
      if (down(DOWN))
        pd_menu_move(&padMenu, 1);
      if (down(B))
        padPage = false;
      if (down(A)) {
        padCapture = padMenu.selected;
        input_mapping_reset();
        padMenu.title = "PRESS A BUTTON / TRIGGER";
        notice("B: cancel   SELECT: off   Left stick stays a D-pad");
      }
    } else if (menuOpen && slotPage) {
      int before = slotMenu.selected;
      if (down(UP))
        pd_menu_move(&slotMenu, -1);
      if (down(DOWN))
        pd_menu_move(&slotMenu, 1);
      if (before != slotMenu.selected) {
        state_select(slotMenu.selected + 1);
        state_preview_load(stateSlot);
      }
      if (down(B))
        slotPage = false;
      if (down(A)) {
        state_select(slotMenu.selected + 1);
        slotPage = false;
      }
    } else if (menuOpen) {
      if (down(UP))
        pd_menu_move(&menu, -1);
      if (down(DOWN))
        pd_menu_move(&menu, 1);
      if ((menu.selected == M_SAVE || menu.selected == M_LOAD) && (down(LEFT) || down(RIGHT)))
        state_select(stateSlot + (down(RIGHT) ? 1 : -1));
      if (down(B))
        menuOpen = false;
      if (down(A)) {
        switch (menu.selected) {
        case M_RESUME:
          menuOpen = false;
          break;
        case M_SAVE: {
          bool ok = state_save_slot(stateSlot);
          char text[64];
          snprintf(text, sizeof text, ok ? "State saved in slot %d" : "State save failed (slot %d)", stateSlot);
          notice(text);
          menuOpen = false;
          break;
        }
        case M_LOAD: {
          char path[PATH_MAX * 2 + 32];
          if (!state_slot_file(stateSlot, path, sizeof path) || !pd_state_exists(path)) {
            notice("No state in this slot");
            menuOpen = false;
            break;
          }
          confirming = M_LOAD;
          confirmRows[0] = "CANCEL";
          confirmRows[1] = "LOAD STATE";
          pd_menu_init(&confirmMenu, "LOAD THE STATE?", confirmRows, 2);
          confirmMenu.overlay = true;
          break;
        }
        case M_FFSPEED:
          hostSet.ffSpeed = hostSet.ffSpeed == 0 ? 2 : hostSet.ffSpeed >= 8 ? 0 : hostSet.ffSpeed + 1;
          host_save();
          break;
        case M_REWIND:
          hostSet.rewind = !hostSet.rewind;
          if (!hostSet.rewind)
            rewind_clear();
          host_save();
          break;
        case M_AUTO_RESUME:
          hostSet.autoResume = !hostSet.autoResume;
          host_save();
          break;
        case M_LCD:
          hostSet.displayMode = (hostSet.displayMode + 1) % (POCKET_DISPLAY_SHADER_EDGE_SMOOTH + 1);
          host_save();
          break;
        case M_SWAP:
          hostSet.swapAB = swapFace = !hostSet.swapAB;
          input_mapping_reset();
          host_save();
          break;
        case M_HOTKEYS:
          hkPage = true;
          pd_menu_init(&hkMenu, "HOTKEYS", hkPtrs, HK_ACTIONS + 1);
          hkMenu.overlay = true;
          break;
        case M_KEYBOARD:
          kbPage = true;
          kb_menu_open();
          break;
        case M_CONTROLLER:
          padPage = true;
          pad_menu_open();
          break;
        case M_STATES:
          slotPage = true;
          pd_menu_init(&slotMenu, "STATE SLOTS", slotPtrs, PD_STATE_SLOTS);
          slotMenu.selected = stateSlot - 1;
          slotMenu.overlay = true;
          state_preview_load(stateSlot);
          break;
        case M_UNDO_SAVE: {
          char statePath[PATH_MAX * 2 + 32];
          notice(state_slot_file(stateSlot, statePath, sizeof statePath) && pd_state_undo_save(statePath)
                     ? "Save undone"
                     : "Nothing to undo");
          break;
        }
        case M_UNDO_LOAD: {
          bool ok = state_undo_load(sav, backups, &sramCopy);
          notice(ok ? "Load undone" : "Nothing to undo");
          if (ok)
            menuOpen = false;
          break;
        }
        case M_QUIT:
          confirming = M_QUIT;
          confirmRows[0] = "CANCEL";
          confirmRows[1] = fromList ? "QUIT TO GAME LIST" : "QUIT";
          pd_menu_init(&confirmMenu, "LEAVE THE GAME?", confirmRows, 2);
          confirmMenu.overlay = true;
          break;
        }
      }
    }
    if (menuFrame) {
      muted = true;
      gameKeys = 0;
      statePreviewMenu = menuOpen && slotPage;
      present_game(menuOpen ? (confirming ? &confirmMenu
                              : hkPage    ? &hkMenu
                              : kbPage    ? &kbMenu
                              : padPage   ? &padMenu
                              : slotPage  ? &slotMenu
                                          : &menu)
                            : NULL);
      if (maxFrames && ++frames >= maxFrames) { // a scripted run can end, and be captured, with the menu up
        if (shot && !capture(shot))
          appError = 1;
        break;
      }
      if (!vsyncPaced && !maxFrames)
        SDL_Delay(16);
      due = SDL_GetPerformanceCounter();
      continue;
    }
    muted = false;
    bool rewinding = (hkOn & (1u << HK_REWIND)) && hostSet.rewind;
    bool ff = !rewinding && ((hkOn & (1u << HK_FF)) || ffQuick);
    int runs = 1;
    if (ff) {
      double cost = emuMs > 0 ? emuMs : 4;
      runs = hostSet.ffSpeed ? hostSet.ffSpeed : (int)(800.0 / hz / cost);
      runs = runs < 2 ? 2 : runs > 30 ? 30 : runs;
    }
    ffNow = ff;
    ffSpeedNow = runs;
    rewindingNow = rewinding;
    audioKeepEvery = (unsigned)runs;
    // The mouse as the app's touch screen: a press lasts a frame at least, so a quick click still taps.
    pointerDown = pointerHeld || pointerPulse > 0;
    if (pointerPulse > 0)
      pointerPulse--;
    unsigned gameHeld = (unsigned)held;
    gameKeys = joypad((gameHeld & ~hotkeyMask & ~((1u << MENU) | (1u << FFKEY) | (1u << RWKEY))) | modifierPulse);
    uint64_t t = SDL_GetPerformanceCounter();
    if (rewinding) {
      muted = true;
      if (rewind_pop())
        core.run();
      else
        rewinding = false;
    } else {
      for (int i = 0; i < runs && running; i++) {
        core.run();
        wheelSteps = 0; // the core has read this frame's notches
        if (modifierPulse && --pulseLeft <= 0) {
          gameKeys &= ~joypad(modifierPulse);
          modifierPulse = 0;
        }
        if (hostSet.rewind && !(++rewindTick & 1))
          rewind_push();
      }
    }
    emuMs = (double)(SDL_GetPerformanceCounter() - t) * 1000.0 / SDL_GetPerformanceFrequency() / runs;
    if (++frames % 120 == 0)
      sram_write(sav, &sramCopy, true);
    present_game(NULL);
    if (maxFrames && frames >= maxFrames) {
      if (shot && !capture(shot))
        appError = 1;
      break;
    }
    uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    pacing_check((double)(now - lastFrame) * 1000.0 / f);
    lastFrame = now;
    audio_trim(hz, vsyncPaced ? screenHz : hz);
    if (!vsyncPaced && !maxFrames) {
      due += (uint64_t)(f / hz);
      if (now < due)
        SDL_Delay((Uint32)((due - now) * 1000 / f));
      else if (now - due > f / 10)
        due = now; // far behind (a stall): start again rather than run to catch up
    }
    perf_frame(frameStart, hz);
  }
  bool batteryOkay = sram_write(sav, &sramCopy, true);
  if (hostSet.autoResume && batteryOkay)
    notice(state_resume_write(sav) ? "Resume state saved" : "Resume save failed");
  statePreviewMenu = false;
  free(sramCopy.data);
  rewind_clear();
  core.unload_game();
  core.deinit();
  dlclose(core.dl);
  memset(&core, 0, sizeof core);
  free(rom);
  if (audio)
    SDL_ClearQueuedAudio(audio);
  muted = false;
  held = pressed = padHeld = 0;
}

// By the file name, whichever folder it is in.
static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}
/* ---- the library (library.c): the host's own SETTINGS rows, and its canvases and covers on the screen ---- */
static bool libraryKeyboard, libraryController;
static const char *lib_label(int row) {
  static const char *const LABELS[] = {"DISPLAY FILTER", "FF SPEED", "REWIND", "AUTO RESUME", "A/B BUTTONS", "CONTROLLER", "KEYBOARD"};
  return LABELS[row];
}
static const char *lib_group(int row) { return row == 0 ? "SCREEN" : row < 3 ? "FF / SPEED" : row == 3 ? "SAVES" : "CONTROLS"; }
static void lib_value(int row, char *out, int len) {
  if (row == 0)
    snprintf(out, (size_t)len, "%s", displayModeNames[hostSet.displayMode]);
  else if (row == 1 && hostSet.ffSpeed)
    snprintf(out, (size_t)len, "%dx", hostSet.ffSpeed);
  else if (row == 1)
    snprintf(out, (size_t)len, "MAX");
  else if (row == 2)
    snprintf(out, (size_t)len, "%s", hostSet.rewind ? "ON" : "OFF");
  else if (row == 3)
    snprintf(out, (size_t)len, "%s", hostSet.autoResume ? "ON" : "OFF");
  else if (row == 4)
    snprintf(out, (size_t)len, "%s", hostSet.swapAB ? "SWAPPED" : "NORMAL");
  else
    snprintf(out, (size_t)len, "EDIT");
}
static void lib_step(int row, int dir) {
  if (row == 0)
    hostSet.displayMode = (hostSet.displayMode + dir + POCKET_DISPLAY_SHADER_EDGE_SMOOTH + 1) %
                          (POCKET_DISPLAY_SHADER_EDGE_SMOOTH + 1);
  else if (row == 1) {
    // MAX, 2x ... 8x, round.
    int v = hostSet.ffSpeed ? hostSet.ffSpeed - 1 : 0; // 0 MAX, 1..7 = 2x..8x
    v = (v + dir + 8) % 8;
    hostSet.ffSpeed = v ? v + 1 : 0;
  } else if (row == 2)
    hostSet.rewind = !hostSet.rewind;
  else if (row == 3)
    hostSet.autoResume = !hostSet.autoResume;
  else if (row == 4)
    hostSet.swapAB = swapFace = !hostSet.swapAB;
  else {
    if (row == 5)
      libraryController = true;
    else
      libraryKeyboard = true;
    return;
  }
  host_save();
}
static void library_present(struct pd_menu *page);
static bool save_text_edit(void *ctx, const char *hint, const char *initial, int max, char *out, size_t outLen) {
  (void)ctx;
  if (!initial || max < 1 || max >= (int)sizeof textPrompt.text || !out || outLen < 2)
    return false;
  memset(&textPrompt, 0, sizeof textPrompt);
  textPrompt.active = true;
  textPrompt.max = max;
  textScript = -1;
  snprintf(textPrompt.hint, sizeof textPrompt.hint, "%s", hint ? hint : "Name");
  snprintf(textPrompt.text, sizeof textPrompt.text, "%s", initial);
  out[0] = 0;
  SDL_StartTextInput();
  while (running && textPrompt.active) {
    events();
    if (textPrompt.active)
      library_present(NULL);
    SDL_Delay(16);
  }
  textPrompt.active = false;
  SDL_StopTextInput();
  if (textPrompt.accepted)
    snprintf(out, outLen, "%s", textPrompt.text);
  pressed = held = 0;
  libraryClick = false;
  libraryWheel = 0;
  fprintf(stderr, "Save tools: text %s\n", textPrompt.accepted ? "accepted" : "cancelled");
  return textPrompt.accepted && out[0];
}
#ifndef POCKET_VERSION
#define POCKET_VERSION "dev"
#endif
static const struct lib_host libraryHost = {7, lib_label, lib_group, lib_value, lib_step, POCKET_VERSION, save_text_edit, NULL};

/* The library at a finer grain than the overlay (its 320x240 is the companion's): about 640x480 on a 1280x960 panel. Its
 * page, then the covers at the screen's own resolution, then a second canvas with the legend and the menus. */
static uint32_t libraryTopPx[OVERLAY_MAX_W * OVERLAY_MAX_H];
static SDL_Texture *libraryTopTexture, *coverTextures[1024];
static unsigned coverSerials[1024];
static int libraryTopW, libraryTopH;
/* The same viewport library_present draws: window pixels, the scale, the canvas and its top-left corner. */
static void library_viewport(int *ow, int *oh, int *scale, int *w, int *h, int *x0, int *y0) {
  int width = W, height = H;
  SDL_GetRendererOutputSize(renderer, &width, &height);
  int s = width / 560 < height / 400 ? width / 560 : height / 400;
  if (s < 1)
    s = 1;
  int cw = width / s, ch = height / s;
  cw = cw > OVERLAY_MAX_W ? OVERLAY_MAX_W : cw;
  ch = ch > OVERLAY_MAX_H ? OVERLAY_MAX_H : ch;
  *ow = width;
  *oh = height;
  *scale = s;
  *w = cw;
  *h = ch;
  *x0 = (width - cw * s) / 2;
  *y0 = (height - ch * s) / 2;
}
/* The KEYBOARD page over the game list: its place in the list's canvas (a 320 x 240 page, scaled up whole) and a window
 * click mapped into that page, for the page's rows. */
static int keyPageScale = 1, keyPageX, keyPageY;
static void key_page_place(int w, int h) {
  keyPageScale = w / 320 < h / 240 ? w / 320 : h / 240;
  keyPageScale = keyPageScale < 1 ? 1 : keyPageScale;
  keyPageX = (w - 320 * keyPageScale) / 2;
  keyPageY = (h - 240 * keyPageScale) / 2;
}
static bool key_page_pointer(int *px, int *py) {
  int ow, oh, s, w, h, x0, y0;
  library_viewport(&ow, &oh, &s, &w, &h, &x0, &y0);
  key_page_place(w, h);
  int cx = (mouseWindowX - x0) / s, cy = (mouseWindowY - y0) / s;
  if (cx < keyPageX || cy < keyPageY || cx >= keyPageX + 320 * keyPageScale || cy >= keyPageY + 240 * keyPageScale)
    return false;
  *px = (cx - keyPageX) / keyPageScale;
  *py = (cy - keyPageY) / keyPageScale;
  return true;
}
static void library_textures_free(void) {
  for (int i = 0; i < 1024; i++)
    if (coverTextures[i]) {
      SDL_DestroyTexture(coverTextures[i]);
      coverTextures[i] = NULL;
    }
  if (libraryTopTexture)
    SDL_DestroyTexture(libraryTopTexture);
  libraryTopTexture = NULL;
}
static void library_present(struct pd_menu *page) {
  int ow, oh, s, w, h, x0, y0;
  library_viewport(&ow, &oh, &s, &w, &h, &x0, &y0);
  SDL_RenderClear(renderer);
  pd_canvas_init(&overlay, w, h, overlayPx);
  library_draw(&overlay);
  if (page) {
    // The KEYBOARD page over the list, the list dimmed behind it as the game list's own windows dim it. The page is
    // drawn at the host menu's own density (320x240) and scaled up whole, so its text is as large as in a game.
    pd_unclip(&overlay); // the list's own drawing may have left a clip: the dim covers the whole picture
    pd_dim(&overlay, 150);
    static uint32_t pagePx[320 * 240];
    struct pd_canvas pc;
    pd_canvas_init(&pc, 320, 240, pagePx);
    pd_fill(&pc, 0, 0, 320, 240, KEY_COLOR);
    pd_menu_draw(page, &pc);
    key_page_place(overlay.w, overlay.h);
    const int sc = keyPageScale, ox = keyPageX, oy = keyPageY;
    for (int y = 0; y < 240 * sc; y++)
      for (int x = 0; x < 320 * sc; x++) {
        uint32_t p = pagePx[(y / sc) * 320 + x / sc] & 0xFFFFFFu;
        const int dx = ox + x, dy = oy + y; // a window smaller than the page cuts it, never writes past the canvas
        if (p != (KEY_COLOR & 0xFFFFFFu) && dx >= 0 && dy >= 0 && dx < overlay.w && dy < overlay.h)
          overlay.px[dy * overlay.w + dx] = 0xFF000000u | p;
      }
  }
  overlay_draw(true);
  if (page) { // no legend or rail over the page
    SDL_RenderPresent(renderer);
    return;
  }
  struct lib_cover covers[64];
  int n = library_covers(covers, 64);
  for (int i = 0; i < n; i++) {
    const struct lib_cover *cv = &covers[i];
    if (cv->game < 0 || cv->game >= 1024)
      continue;
    SDL_Texture **t = &coverTextures[cv->game];
    if (!*t || coverSerials[cv->game] != cv->serial + 1) {
      if (!*t) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, cv->size, cv->size);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        if (*t)
          SDL_SetTextureBlendMode(*t, SDL_BLENDMODE_BLEND);
      }
      if (*t)
        SDL_UpdateTexture(*t, NULL, cv->px, cv->size * 4);
      coverSerials[cv->game] = cv->serial + 1;
    }
    if (!*t)
      continue;
    SDL_Rect clip = {x0 + cv->clipX * s, y0 + cv->clipY * s, cv->clipW * s, cv->clipH * s};
    SDL_Rect dst = {x0 + cv->x * s, y0 + cv->y * s, cv->w * s, cv->h * s};
    SDL_RenderSetClipRect(renderer, &clip);
    SDL_RenderCopy(renderer, *t, NULL, &dst);
    SDL_RenderSetClipRect(renderer, NULL);
  }
  if (library_dimmed()) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_RenderFillRect(renderer, NULL);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  }
  struct pd_canvas top;
  pd_canvas_init(&top, w, h, libraryTopPx);
  library_draw_top(&top, KEY_COLOR);
  if (textPrompt.active) {
    int pw = w < 280 ? w : 280, ph = 92, px = (w - pw) / 2, py = (h - ph) / 2;
    int f = pd_title_box(&top, px, py, pw, ph, PD_TITLE_FILL);
    pd_text_fit(&top, px + f + 8, py + f + 5, pw - 2 * f - 16, textPrompt.hint, PD_VALUE, PD_VALUE_SHADOW);
    char typed[65];
    snprintf(typed, sizeof typed, "%s_", textPrompt.text);
    pd_text_fit(&top, px + f + 8, py + f + 28, pw - 2 * f - 16, typed, PD_LABEL, PD_LABEL_SHADOW);
    pd_text_fit(&top, px + f + 8, py + ph - f - 20, pw - 2 * f - 16, "ENTER: OK   ESC: CANCEL", PD_MUTED,
                PD_MUTED_SHADOW);
  }
  if (!libraryTopTexture || libraryTopW != w || libraryTopH != h) {
    if (libraryTopTexture)
      SDL_DestroyTexture(libraryTopTexture);
    libraryTopTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (libraryTopTexture)
      SDL_SetTextureBlendMode(libraryTopTexture, SDL_BLENDMODE_BLEND);
    libraryTopW = w;
    libraryTopH = h;
  }
  for (int i = 0; i < w * h; i++) {
    uint32_t p = libraryTopPx[i] & 0xFFFFFF;
    libraryTopPx[i] = p == KEY_COLOR ? 0 : 0xFF000000u | p;
  }
  if (libraryTopTexture) {
    SDL_UpdateTexture(libraryTopTexture, NULL, libraryTopPx, w * 4);
    SDL_Rect r = {x0, y0, w * s, h * s};
    SDL_RenderCopy(renderer, libraryTopTexture, NULL, &r);
  }
  SDL_RenderPresent(renderer);
}
/* By file name; the same file in two folders: DATA/roms' first (the library keeps the first), then by path. */
static bool in_data_roms(const char *p) {
  size_t n = strlen(root);
  return !strncmp(p, root, n) && !strncmp(p + n, "/roms/", 6);
}
static int cmp(const void *a, const void *b) {
  const char *x = *(char *const *)a, *y = *(char *const *)b;
  int c = strcasecmp(base_name(x), base_name(y));
  if (!c)
    c = strcmp(base_name(x), base_name(y));
  if (!c && in_data_roms(x) != in_data_roms(y))
    c = in_data_roms(x) ? -1 : 1;
  return c ? c : strcmp(x, y);
}
static void exe_dir(const char *argv0) {
  char p[PATH_MAX];
#ifdef _WIN32
  long n = pd_exe_path(p, sizeof p); // GetModuleFileNameA: Windows has no /proc/self/exe
#else
  ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
#endif
  if (n > 0)
    p[n] = 0;
  else
    snprintf(p, sizeof p, "%s", argv0);
  char *slash = strrchr(p, '/');
  if (slash)
    *slash = 0;
  else
    snprintf(p, sizeof p, ".");
#ifdef _WIN32
  for (char *c = p; *c; c++) // one separator throughout: base_name and the path joins below use '/'
    if (*c == '\\')
      *c = '/';
#endif
  snprintf(exeDir, sizeof exeDir, "%s", p);
}
int main(int argc, char **argv) {
  const char *rom = NULL, *shot = NULL;
  int frames = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--data") && i + 1 < argc)
      root = argv[++i];
    else if (!strcmp(argv[i], "--save") && i + 1 < argc)
      saveFile = argv[++i];
    else if ((!strcmp(argv[i], "--import-save") || !strcmp(argv[i], "--restore-save")) && i + 1 < argc)
      importFile = argv[++i];
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
      frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
      shot = argv[++i];
    else
      rom = argv[i];
  }
  exe_dir(argv[0]);
  signal(SIGINT, stop);
  signal(SIGTERM, stop);
#ifdef SIGHUP // a closed terminal or session: the save goes to the card like a quit (no SIGHUP on Windows)
  signal(SIGHUP, stop);
#endif
  pd_mkdir(root);
  char path[PATH_MAX * 2];
  const char *dirs[] = {"roms", "saves", "states"};
  for (int i = 0; i < 3; i++) {
    snprintf(path, sizeof path, "%s/%s", root, dirs[i]);
    pd_mkdir(path);
  }
  snprintf(savesDir, sizeof savesDir, "%s/saves", root);
  snprintf(hostIni, sizeof hostIni, "%s/host.ini", root);
  faceLabels = getenv("POCKET_FACE_LABELS") && !strcmp(getenv("POCKET_FACE_LABELS"), "1");
  host_load();
  if (getenv("POCKET_RESUME"))
    hostSet.autoResume = strcmp(getenv("POCKET_RESUME"), "0") != 0;
  comboMenu = !(getenv("POCKET_MENU_COMBO") && !strcmp(getenv("POCKET_MENU_COMBO"), "off"));
  // A window manager can give another window the focus over the game (plorpOS's in-game menu on the GKD): the
  // controller has to keep reaching the host then.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  SDL_DisplayMode display = {0};
  // Fullscreen with POCKET_FULLSCREEN, and under gamescope (the Steam Deck's Game Mode): it scales a window to the
  // screen keeping its shape, so a window there was letterboxed and EXPAND couldn't reach the edges.
  const char *desktop = getenv("XDG_CURRENT_DESKTOP");
  bool fullscreen = hostSet.fullscreen || getenv("POCKET_FULLSCREEN") != NULL || (desktop && strstr(desktop, "gamescope"));
  if (fullscreen && SDL_GetCurrentDisplayMode(0, &display))
    fullscreen = false;
  // POCKET_WINDOW=WxH: a window the size of a device's panel, for trying a layout on a desktop.
  int winW = W, winH = H;
  const char *size = getenv("POCKET_WINDOW");
  if (size &&
      (sscanf(size, "%dx%d", &winW, &winH) != 2 || winW < 160 || winH < 144 || winW > 8192 || winH > 8192))
    winW = W, winH = H;
  window = SDL_CreateWindow(PD_HOST_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            fullscreen ? display.w : winW, fullscreen ? display.h : winH,
                            fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE);
#ifdef _WIN32
  set_window_icon(window);
#endif
  // Presents wait for the refresh (the pacing, above); a driver without it gets the timer.
  renderer = window ? SDL_CreateRenderer(window, -1, getenv("POCKET_NO_VSYNC") ? 0 : SDL_RENDERER_PRESENTVSYNC) : NULL;
  if (!renderer && window)
    renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    fprintf(stderr, "Video: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_RendererInfo rendererInfo;
  if (!SDL_GetRendererInfo(renderer, &rendererInfo))
    fprintf(stderr, "Video backend: %s\n", rendererInfo.name);
#ifdef _WIN32
  // The mouse taps the library and the app's own overlays: its cursor stays on screen.
  SDL_ShowCursor(SDL_ENABLE);
#else
  // A handheld (KMSDRM, gamescope) would draw an arrow over the game: hidden until the mouse moves (events_poll).
  SDL_ShowCursor(SDL_DISABLE);
#endif
  // SDL starts text input with the window, and Steam answers it with its on-screen keyboard: the host reads keys,
  // never text.
  SDL_StopTextInput();
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  audio_open();
  rawTrimui = getenv("POCKET_RAW_TRIMUI") && !strcmp(getenv("POCKET_RAW_TRIMUI"), "1");
  snprintf(path, sizeof path, "%s/gamecontrollerdb.txt", root);
  if (access(path, R_OK) == 0) {
    int added = SDL_GameControllerAddMappingsFromFile(path);
    fprintf(stderr, "Controller mappings from %s: %d\n", path, added);
  }
  for (int i = 0; i < SDL_NumJoysticks() && !controller; i++)
    if (SDL_IsGameController(i))
      controller_added(i);
  SDL_Joystick *js = !controller && rawTrimui && SDL_NumJoysticks() ? SDL_JoystickOpen(0) : NULL;
  if (rom)
    run_game(rom, frames, shot, false);
  else {
    /* DATA/roms, then the folders POCKET_ROM_DIRS names (colon-separated: the PortMaster launcher passes the
     * firmware's own gba / gb / gbc / nds folders), by file name. Those hold every other game too, so only their
     * Pokemon games are listed (pokemon_rom); DATA/roms lists whatever was put there. */
    static char *paths[1024];
    int count = 0;
    char dirs[4096];
    snprintf(dirs, sizeof dirs, "%s/roms%s%s", root, getenv("POCKET_ROM_DIRS") ? ":" : "",
             getenv("POCKET_ROM_DIRS") ? getenv("POCKET_ROM_DIRS") : "");
    char *save = NULL;
    bool firmware = false;
    for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save), firmware = true) {
      DIR *d = opendir(dir);
      struct dirent *e;
      while (d && (e = readdir(d)) && count < 1024) {
        if (core_for(e->d_name)) {
          snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
          if (!firmware || pokemon_rom(path))
            paths[count++] = strdup(path);
        }
      }
      if (d)
        closedir(d);
    }
    qsort(paths, count, sizeof(*paths), cmp);
    settings_migrate(); // before the library writes its first settings: an old DATA/settings.ini wins
    library_open(paths, count, root, savesDir, &libraryHost);
    int ticks = 0;
    bool keyPage = false, controllerPage = false;
    while (running && !library_quit()) {
      events();
      repeat_directions();
      static const int LIB_KEYS[][2] = {{A, LIB_A},       {B, LIB_B},         {X, LIB_X},           {Y, LIB_Y},
                                        {L, LIB_L},       {R, LIB_R},         {UP, LIB_UP},         {DOWN, LIB_DOWN},
                                        {LEFT, LIB_LEFT}, {RIGHT, LIB_RIGHT}, {START, LIB_START},   {SELECT, LIB_SELECT},
                                        {MENU, LIB_MENU}};
      unsigned libDown = 0;
      for (size_t i = 0; i < sizeof LIB_KEYS / sizeof *LIB_KEYS; i++)
        if (down(LIB_KEYS[i][0]))
          libDown |= (unsigned)LIB_KEYS[i][1];
      // START + SELECT (dev's quit chord) asks to close as START or Menu do.
      if ((down(START) && (held >> SELECT & 1)) || (down(SELECT) && (held >> START & 1)))
        libDown = (libDown & ~(unsigned)LIB_SELECT) | LIB_START;
      if (keyPage || controllerPage)
        libDown = 0;
      library_keys(libDown);
      library_tick();
      if (libraryKeyboard || libraryController) {
        keyPage = libraryKeyboard;
        controllerPage = libraryController;
        libraryKeyboard = libraryController = false;
        kbCapture = padCapture = -1;
        if (keyPage)
          kb_menu_open();
        else
          pad_menu_open();
        pressed = held = 0;
        libraryClick = false;
      }
      if (keyPage || controllerPage) {
        int px = 0, py = 0;
        bool onPage = libraryClick && key_page_pointer(&px, &py);
        struct pd_menu *page = keyPage ? &kbMenu : &padMenu;
        int *capture = keyPage ? &kbCapture : &padCapture;
        if (controllerPage && padCapture >= 0) {
          if (down(B))
            padCapture = -1;
          else if (down(SELECT)) {
            pd_pad_clear(&padMap, padCapture);
            host_save();
            padCapture = -1;
            input_mapping_reset();
          } else if (lastPadControl >= 0) {
            pd_pad_bind(&padMap, lastPadControl, padCapture);
            host_save();
            padCapture = -1;
            input_mapping_reset();
          }
        } else if (keyPage && kbCapture >= 0) {
          if (lastKey == SDLK_ESCAPE)
            lastKey = 0, pressed |= 1ull << B;
          else if (lastKey == SDLK_BACKSPACE || lastKey == SDLK_DELETE)
            lastKey = 0, pressed |= 1ull << SELECT;
          if (lastKey) {
            for (int i = 0; i < KB_COUNT; i++)
              if (kbKeys[i] == lastKey)
                kbKeys[i] = SDLK_UNKNOWN;
            kbKeys[kbCapture] = lastKey;
            host_save();
            kbCapture = -1;
          } else if (down(B))
            kbCapture = -1;
          else if (down(SELECT)) {
            kbKeys[kbCapture] = SDLK_UNKNOWN;
            host_save();
            kbCapture = -1;
          } else if (libraryClick && !onPage) {
            keyPage = false;
            kbCapture = -1;
          }
        } else if (down(B) || down(MENU)) {
          keyPage = controllerPage = false;
          kbCapture = padCapture = -1;
        } else if (down(UP) || down(DOWN)) {
          pd_menu_move(page, down(DOWN) ? 1 : -1);
        } else if (down(A)) {
          *capture = page->selected;
          pressed = held = 0;
        } else if (onPage) {
          pd_menu_touch(page, px, py, true);
          const int row = pd_menu_touch(page, px, py, false);
          if (row >= 0) {
            page->selected = row;
            *capture = row;
            pressed = held = 0;
          }
        } else if (libraryClick) {
          keyPage = controllerPage = false;
        }
        if (keyPage) {
          kbMenu.title = kbCapture >= 0 ? "PRESS A KEY" : "KEYBOARD";
          kb_menu_build();
        } else {
          padMenu.title = padCapture >= 0 ? "PRESS A BUTTON / TRIGGER" : "CONTROLLER";
          pad_menu_build();
        }
        libraryClick = false;
        libraryWheel = 0;
      } else if (libraryClick || libraryWheel) {
        int ow, oh, s, w, h, x0, y0;
        library_viewport(&ow, &oh, &s, &w, &h, &x0, &y0);
        int mx = (mouseWindowX - x0) / s, my = (mouseWindowY - y0) / s;
        if (mx >= 0 && my >= 0 && mx < w && my < h)
          library_pointer(mx, my, libraryClick, libraryWheel);
        libraryClick = false;
        libraryWheel = 0;
      }
      const char *game = library_picked();
      if (game) {
        char picked[PATH_MAX];
        snprintf(picked, sizeof picked, "%s", game);
        appError = 0;
        library_pause(true); // no hashing or cover downloads while a game plays
        run_game(picked, 0, NULL, true); // the last game's result is the exit status
        library_pause(false);
        libraryClick = false; // a click in the game is not one in the library
        libraryWheel = 0;
        library_played(picked);
      }
      library_present(keyPage ? &kbMenu : controllerPage ? &padMenu : NULL);
      if (frames && ++ticks >= frames) {
        if (shot && !capture(shot))
          appError = 1;
        break;
      }
      SDL_Delay(16);
    }
    library_close();
    library_textures_free();
    for (int i = 0; i < count; i++)
      free(paths[i]);
  }
  if (js)
    SDL_JoystickClose(js);
  if (controller)
    SDL_GameControllerClose(controller);
  if (audio)
    SDL_CloseAudioDevice(audio);
  if (frameTexture)
    SDL_DestroyTexture(frameTexture);
  if (overlayTexture)
    SDL_DestroyTexture(overlayTexture);
  display_filter_cleanup();
  free(frameCopy);
  frameCopy = NULL;
  frameCopyCapacity = frameCopyStride = 0;
  frameCopyValid = false;
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return appError;
}
