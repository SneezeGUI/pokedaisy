// POKéMON CENTER anywhere: the app's PokemonCenter.kt in portable C. Discovery checks the game's own specials; actions
// run its scripts or screens between frames. Backends supply the already validated field / PC / TM code and game calls.
#ifndef PD_CENTER_H
#define PD_CENTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pd_binput.h"
#include "pd_game.h"

struct pd_center_relearn {
    int index;
    uint32_t init, fieldCallback, continueScript;
};

struct pd_center_found {
    uint32_t specials;
    int heal, nickname, forget, relearnSpecial; // -1 = this game has no such special
    struct pd_center_relearn relearn;
    bool hasRelearn;
    uint32_t otIdNotPlayers, windowFrame; // 0 = none / not needed
    uint32_t var8004, varResult;
    int healSong, forgetSong;
};

enum pd_center_result {
    PD_CENTER_OK, PD_CENTER_NOT_NOW, PD_CENTER_IN_BATTLE, PD_CENTER_EGG, PD_CENTER_TRADED,
    PD_CENTER_LAST_MOVE, PD_CENTER_LAST_SURFER, PD_CENTER_PARTY_CHANGED, PD_CENTER_FAILED,
};

struct pd_center_ops {
    void* ctx;
    pd_read_fn read;
    pd_write_fn write;
    bool (*call)(void* ctx, uint32_t fn, const uint32_t args[4], uint32_t* r0);
    bool (*fieldSupported)(void* ctx);
    bool (*pcSupported)(void* ctx);
    bool (*tmSupported)(void* ctx);
};

bool pd_center_find(const uint8_t* rom, size_t size, struct pd_center_found* out);

enum pd_center_result pd_center_heal(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                     const struct pd_center_found* found, bool busy, bool battle);
enum pd_center_result pd_center_nickname(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                         const struct pd_center_found* found, int slot, uint32_t personality,
                                         bool busy, bool battle);
enum pd_center_result pd_center_forget(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                       const struct pd_center_found* found, int slot, uint32_t personality,
                                       int moveIndex, int move, bool hmFree, bool busy, bool battle);
enum pd_center_result pd_center_relearn_script(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                               const struct pd_center_found* found, int slot, uint32_t personality,
                                               bool busy, bool battle);
enum pd_center_result pd_center_relearn_start(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                              const struct pd_center_found* found, int slot, uint32_t personality,
                                              bool busy, bool battle);
bool pd_center_relearn_finish(const struct pd_center_ops* ops, const struct pd_config* cfg,
                              const struct pd_center_found* found, int slot, uint32_t personality, bool leftField);
const char* pd_center_result_text(enum pd_center_result result);

#endif
