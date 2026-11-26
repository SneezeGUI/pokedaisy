/* SDL controller controls mapped to game and host actions, persisted by the Linux host. */
#ifndef POCKET_HOST_INPUT_H
#define POCKET_HOST_INPUT_H

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>

enum pd_pad_action {
  PD_ACT_A,
  PD_ACT_B,
  PD_ACT_X,
  PD_ACT_Y,
  PD_ACT_L1,
  PD_ACT_R1,
  PD_ACT_L2,
  PD_ACT_R2,
  PD_ACT_L3,
  PD_ACT_R3,
  PD_ACT_START,
  PD_ACT_SELECT,
  PD_ACT_UP,
  PD_ACT_DOWN,
  PD_ACT_LEFT,
  PD_ACT_RIGHT,
  PD_ACT_MENU,
  PD_ACT_FF,
  PD_ACT_REWIND,
  PD_ACT_SAVE,
  PD_ACT_LOAD,
  PD_ACT_UNDO_SAVE,
  PD_ACT_UNDO_LOAD,
  PD_ACT_SLOT_NEXT,
  PD_ACT_SLOT_PREV,
  PD_ACT_REMAP_COUNT,
  /* Auxiliary controls remain available to the older modifier-hotkey page. */
  PD_ACT_P1 = PD_ACT_REMAP_COUNT,
  PD_ACT_P2,
  PD_ACT_P3,
  PD_ACT_P4,
  PD_ACT_COUNT
};

#define PD_PAD_TRIGGER_LEFT SDL_CONTROLLER_BUTTON_MAX
#define PD_PAD_TRIGGER_RIGHT (SDL_CONTROLLER_BUTTON_MAX + 1)
#define PD_PAD_CONTROLS (SDL_CONTROLLER_BUTTON_MAX + 2)

struct pd_pad_map {
  int action[PD_PAD_CONTROLS]; /* enum pd_pad_action, or -1 for off */
};

void pd_pad_defaults(struct pd_pad_map *map, bool faceLabels);
int pd_pad_button_control(int button);
int pd_pad_axis_control(int axis);
bool pd_pad_control_valid(int control);
bool pd_pad_action_valid(int action);
int pd_pad_action(const struct pd_pad_map *map, int control);
int pd_pad_control_for(const struct pd_pad_map *map, int action);
/* A moved action comes off its old control. Returns that control, or -1. */
int pd_pad_bind(struct pd_pad_map *map, int control, int action);
void pd_pad_clear(struct pd_pad_map *map, int action);
const char *pd_pad_action_id(int action);
const char *pd_pad_action_label(int action);
int pd_pad_action_named(const char *id);
const char *pd_pad_control_id(int control);
void pd_pad_control_label(int control, char *out, size_t size);

#endif
