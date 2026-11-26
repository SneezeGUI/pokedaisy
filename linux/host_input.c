#include "host_input.h"

#include <stdio.h>
#include <string.h>

static const char *const actionIds[PD_ACT_COUNT] = {
    "a", "b", "x", "y", "l1", "r1", "l2", "r2", "l3", "r3", "start", "select", "up", "down", "left", "right",
    "menu", "ff", "rewind", "savestate", "loadstate", "undosave", "undoload", "slotnext", "slotprev",
    "p1", "p2", "p3", "p4"};
static const char *const actionLabels[PD_ACT_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3", "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "MENU", "FAST FORWARD", "REWIND", "SAVE STATE", "LOAD STATE", "UNDO SAVE", "UNDO LOAD", "SLOT NEXT", "SLOT PREV",
    "PADDLE 1", "PADDLE 2", "PADDLE 3", "PADDLE 4"};

bool pd_pad_control_valid(int control) { return control >= 0 && control < PD_PAD_CONTROLS; }
bool pd_pad_action_valid(int action) { return action >= 0 && action < PD_ACT_COUNT; }
int pd_pad_button_control(int button) { return button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX ? button : -1; }
int pd_pad_axis_control(int axis) {
  return axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? PD_PAD_TRIGGER_LEFT
         : axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? PD_PAD_TRIGGER_RIGHT
                                                    : -1;
}

static void set_default(struct pd_pad_map *map, int control, int action) {
  if (pd_pad_control_valid(control))
    map->action[control] = action;
}

void pd_pad_defaults(struct pd_pad_map *map, bool faceLabels) {
  for (int i = 0; i < PD_PAD_CONTROLS; i++)
    map->action[i] = -1;
  set_default(map, SDL_CONTROLLER_BUTTON_A, faceLabels ? PD_ACT_A : PD_ACT_B);
  set_default(map, SDL_CONTROLLER_BUTTON_B, faceLabels ? PD_ACT_B : PD_ACT_A);
  set_default(map, SDL_CONTROLLER_BUTTON_X, faceLabels ? PD_ACT_X : PD_ACT_Y);
  set_default(map, SDL_CONTROLLER_BUTTON_Y, faceLabels ? PD_ACT_Y : PD_ACT_X);
  set_default(map, SDL_CONTROLLER_BUTTON_BACK, PD_ACT_SELECT);
  set_default(map, SDL_CONTROLLER_BUTTON_START, PD_ACT_START);
  set_default(map, SDL_CONTROLLER_BUTTON_GUIDE, PD_ACT_MENU);
  set_default(map, SDL_CONTROLLER_BUTTON_LEFTSTICK, PD_ACT_L3);
  set_default(map, SDL_CONTROLLER_BUTTON_RIGHTSTICK, PD_ACT_R3);
  set_default(map, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PD_ACT_L1);
  set_default(map, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PD_ACT_R1);
  set_default(map, SDL_CONTROLLER_BUTTON_DPAD_UP, PD_ACT_UP);
  set_default(map, SDL_CONTROLLER_BUTTON_DPAD_DOWN, PD_ACT_DOWN);
  set_default(map, SDL_CONTROLLER_BUTTON_DPAD_LEFT, PD_ACT_LEFT);
  set_default(map, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PD_ACT_RIGHT);
  set_default(map, PD_PAD_TRIGGER_LEFT, PD_ACT_L2);
  set_default(map, PD_PAD_TRIGGER_RIGHT, PD_ACT_R2);
  set_default(map, SDL_CONTROLLER_BUTTON_PADDLE1, PD_ACT_P1);
  set_default(map, SDL_CONTROLLER_BUTTON_PADDLE2, PD_ACT_P2);
  set_default(map, SDL_CONTROLLER_BUTTON_PADDLE3, PD_ACT_P3);
  set_default(map, SDL_CONTROLLER_BUTTON_PADDLE4, PD_ACT_P4);
}

int pd_pad_action(const struct pd_pad_map *map, int control) {
  return map && pd_pad_control_valid(control) ? map->action[control] : -1;
}

int pd_pad_control_for(const struct pd_pad_map *map, int action) {
  if (!map || !pd_pad_action_valid(action))
    return -1;
  for (int i = 0; i < PD_PAD_CONTROLS; i++)
    if (map->action[i] == action)
      return i;
  return -1;
}

int pd_pad_bind(struct pd_pad_map *map, int control, int action) {
  if (!map || !pd_pad_control_valid(control) || !pd_pad_action_valid(action))
    return -1;
  int previous = pd_pad_control_for(map, action);
  pd_pad_clear(map, action);
  map->action[control] = action;
  return previous == control ? -1 : previous;
}

void pd_pad_clear(struct pd_pad_map *map, int action) {
  if (!map)
    return;
  for (int i = 0; i < PD_PAD_CONTROLS; i++)
    if (map->action[i] == action)
      map->action[i] = -1;
}

const char *pd_pad_action_id(int action) { return pd_pad_action_valid(action) ? actionIds[action] : "off"; }
const char *pd_pad_action_label(int action) { return pd_pad_action_valid(action) ? actionLabels[action] : "OFF"; }
int pd_pad_action_named(const char *id) {
  for (int i = 0; i < PD_ACT_COUNT; i++)
    if (id && !strcmp(id, actionIds[i]))
      return i;
  return -1;
}

const char *pd_pad_control_id(int control) {
  static const char *const ids[SDL_CONTROLLER_BUTTON_MAX] = {
      [SDL_CONTROLLER_BUTTON_A] = "a",
      [SDL_CONTROLLER_BUTTON_B] = "b",
      [SDL_CONTROLLER_BUTTON_X] = "x",
      [SDL_CONTROLLER_BUTTON_Y] = "y",
      [SDL_CONTROLLER_BUTTON_BACK] = "back",
      [SDL_CONTROLLER_BUTTON_GUIDE] = "guide",
      [SDL_CONTROLLER_BUTTON_START] = "start",
      [SDL_CONTROLLER_BUTTON_LEFTSTICK] = "leftstick",
      [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "rightstick",
      [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "leftshoulder",
      [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "rightshoulder",
      [SDL_CONTROLLER_BUTTON_DPAD_UP] = "dpadup",
      [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = "dpaddown",
      [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = "dpadleft",
      [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = "dpadright",
      [SDL_CONTROLLER_BUTTON_MISC1] = "misc1",
      [SDL_CONTROLLER_BUTTON_PADDLE1] = "paddle1",
      [SDL_CONTROLLER_BUTTON_PADDLE2] = "paddle2",
      [SDL_CONTROLLER_BUTTON_PADDLE3] = "paddle3",
      [SDL_CONTROLLER_BUTTON_PADDLE4] = "paddle4",
      [SDL_CONTROLLER_BUTTON_TOUCHPAD] = "touchpad",
  };
  if (control == PD_PAD_TRIGGER_LEFT)
    return "triggerleft";
  if (control == PD_PAD_TRIGGER_RIGHT)
    return "triggerright";
  return pd_pad_control_valid(control) && control < SDL_CONTROLLER_BUTTON_MAX && ids[control] ? ids[control] : NULL;
}

void pd_pad_control_label(int control, char *out, size_t size) {
  if (control == PD_PAD_TRIGGER_LEFT || control == PD_PAD_TRIGGER_RIGHT) {
    snprintf(out, size, "%s", control == PD_PAD_TRIGGER_LEFT ? "LEFT TRIGGER" : "RIGHT TRIGGER");
    return;
  }
  const char *name = pd_pad_control_valid(control) && control < SDL_CONTROLLER_BUTTON_MAX
                         ? SDL_GameControllerGetStringForButton((SDL_GameControllerButton)control)
                         : NULL;
  if (!name) {
    snprintf(out, size, "OFF");
    return;
  }
  size_t i = 0;
  for (; name[i] && i + 1 < size; i++)
    out[i] = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 'a' + 'A') : name[i] == '_' ? ' ' : name[i];
  out[i] = 0;
}
