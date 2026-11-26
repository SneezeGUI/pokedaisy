#include "host_input.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void defaults(void) {
  struct pd_pad_map map;
  pd_pad_defaults(&map, false);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_A) == PD_ACT_B);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_B) == PD_ACT_A);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_DPAD_UP) == PD_ACT_UP);
  assert(pd_pad_action(&map, PD_PAD_TRIGGER_LEFT) == PD_ACT_L2);
  assert(pd_pad_action(&map, PD_PAD_TRIGGER_RIGHT) == PD_ACT_R2);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_LEFTSTICK) == PD_ACT_L3);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_RIGHTSTICK) == PD_ACT_R3);
  pd_pad_defaults(&map, true);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_A) == PD_ACT_A);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_B) == PD_ACT_B);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_X) == PD_ACT_X);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_Y) == PD_ACT_Y);
}

static void conflicts_and_clear(void) {
  struct pd_pad_map map;
  pd_pad_defaults(&map, false);
  assert(pd_pad_bind(&map, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PD_ACT_SAVE) == -1);
  assert(pd_pad_control_for(&map, PD_ACT_SAVE) == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  assert(pd_pad_bind(&map, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PD_ACT_SAVE) == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) == -1);
  assert(pd_pad_control_for(&map, PD_ACT_SAVE) == SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  assert(pd_pad_bind(&map, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PD_ACT_R1) == -1);
  assert(pd_pad_bind(&map, PD_PAD_TRIGGER_LEFT, PD_ACT_R1) == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) == -1);
  assert(pd_pad_action(&map, PD_PAD_TRIGGER_LEFT) == PD_ACT_R1);
  pd_pad_clear(&map, PD_ACT_SAVE);
  assert(pd_pad_control_for(&map, PD_ACT_SAVE) == -1);
  assert(pd_pad_action(&map, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) == -1);
  assert(pd_pad_bind(&map, -1, PD_ACT_A) == -1);
  assert(pd_pad_bind(&map, SDL_CONTROLLER_BUTTON_A, -1) == -1);
}

static void names(void) {
  for (int i = 0; i < PD_ACT_COUNT; i++) {
    assert(pd_pad_action_id(i));
    assert(pd_pad_action_named(pd_pad_action_id(i)) == i);
    assert(pd_pad_action_label(i));
  }
  for (int i = 0; i < PD_PAD_CONTROLS; i++) {
    const char *id = pd_pad_control_id(i);
    if (!id)
      continue;
    char label[64];
    pd_pad_control_label(i, label, sizeof label);
    assert(label[0]);
  }
  assert(!strcmp(pd_pad_control_id(PD_PAD_TRIGGER_LEFT), "triggerleft"));
  assert(!strcmp(pd_pad_control_id(PD_PAD_TRIGGER_RIGHT), "triggerright"));
  assert(pd_pad_axis_control(SDL_CONTROLLER_AXIS_LEFTX) == -1);
  assert(!pd_pad_control_valid(PD_PAD_CONTROLS));
}

int main(void) {
  defaults();
  conflicts_and_clear();
  names();
  puts("PASS controller defaults, all actions, conflict moves, clear and invalid bindings");
}
