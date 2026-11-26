#include "pd_lr_gameplay.h"

#include "field_items.h"
#include "pd_dex.h"
#include "pd_fs.h"
#include "pd_map.h"
#include "pd_romart.h"
#include "pk_call.h"
#include "pk_wide.h"
#include "tm_teach.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRADE_MON_SIZE 100
#define EVO_MODE_TRADE 1

static struct pd_lr_gameplay* active;

static void tell(struct pd_lr_gameplay* g, const char* text) {
    if (text) g->env.notice(g->env.callbackCtx, text);
}

static bool battle_now(const struct pd_lr_gameplay* g) {
    return g->env.snap->inBattle || (*g->env.fieldFlags & PK_FIELD_BATTLE);
}

static bool native_busy(const struct pd_lr_gameplay* g) {
    return battle_now(g) || (g->env.wideKnown && pk_wide_scene() != PK_WIDE_SCENE_FIELD);
}

static bool item_can_use(int item) {
    return active && ((active->tmCfg && tm_teaches(active->tmCfg, item)) || (active->partyItemsOk && pd_items_can_use(item)));
}

static bool item_can_give(int item) {
    return active && active->partyItemsOk && pd_items_can_give(item);
}

// ROM options: apply only where every site still holds its expected bytes or our own, and verify the writes took.
static bool rom_patch(struct pd_lr_gameplay* g, const struct pd_rom_write* writes, int count, bool on) {
    uint8_t now[4];
    for (int i = 0; i < count; i++) {
        const struct pd_rom_write* w = &writes[i];
        if (!g->env.read(g->env.memoryCtx, w->addr, now, (size_t) w->len) ||
            (memcmp(now, w->expect, (size_t) w->len) && memcmp(now, w->value, (size_t) w->len)))
            return false;
    }
    for (int i = 0; i < count; i++) {
        const struct pd_rom_write* w = &writes[i];
        const uint8_t* value = on ? w->value : w->expect;
        if (!g->env.romWrite(g->env.memoryCtx, w->addr, value, (size_t) w->len) ||
            !g->env.read(g->env.memoryCtx, w->addr, now, (size_t) w->len) || memcmp(now, value, (size_t) w->len))
            return false;
    }
    return true;
}

static void options_sync(struct pd_lr_gameplay* g) {
    bool tms = g->env.settings->reusableTms && g->tmCount > 0;
    bool text = g->env.settings->instantText && g->textCount > 0;
    if (tms != g->tmApplied && rom_patch(g, g->tmWrites, g->tmCount, tms)) g->tmApplied = tms;
    if (text != g->textApplied && rom_patch(g, g->textWrites, g->textCount, text)) g->textApplied = text;
}

static void options_find(struct pd_lr_gameplay* g) {
    const struct pd_game* game = g->env.game;
    if (!game->cfg || game->cfg->gb || !g->env.rom) return;
    uint32_t n = 0;
    if (pd_romart_fact_get(game, "tms", &n) && n <= PD_TM_SITES_MAX) {
        uint32_t sites[PD_TM_SITES_MAX];
        bool all = true;
        for (int i = 0; i < PD_TM_SITES_MAX && (uint32_t) i < n && all; i++) {
            char name[8];
            snprintf(name, sizeof name, "tms%d", i);
            all = pd_romart_fact_get(game, name, &sites[i]);
        }
        if (all && n) g->tmCount = pd_reusable_tm_at(g->env.rom, g->env.romSize, sites, (int) n, g->tmWrites);
    } else {
        g->tmCount = pd_reusable_tm_find(g->env.rom, g->env.romSize, g->tmWrites);
        pd_romart_fact_set(game, "tms", (uint32_t) g->tmCount);
        for (int i = 0; i < g->tmCount; i++) {
            char name[8];
            snprintf(name, sizeof name, "tms%d", i);
            pd_romart_fact_set(game, name, g->tmWrites[i].addr - 6);
        }
    }
    bool builtIn = game->guide == PD_GUIDE_UNBOUND || game->guide == PD_GUIDE_RADICAL_RED ||
                   (game->title && !strncmp(game->title, "Pokémon SoulGold", strlen("Pokémon SoulGold")));
    g->env.host->tmsSupported = g->tmCount ? 1 : builtIn ? 2 : 0;
    g->textCount = pd_instant_text_find(g->env.rom, g->env.romSize, g->textWrites);
    g->env.host->textSupported = g->textCount > 0;
}

// NUZLOCKE: the shared tracker, persisted per ROM CRC in the same text format as the other hosts.
static char nuzText[48 * 1024];

static int nuz_rename(const char* from, const char* to) {
    return pd_rename(from, to);
}

static void nuz_save(struct pd_lr_gameplay* g) {
    if (g->nuzPath[0]) pd_nuz_save_atomic(g->nuzPath, g->nuzOn, &g->nuz.run, nuz_rename);
}

static void nuz_load(struct pd_lr_gameplay* g) {
    const struct pd_game* game = g->env.game;
    g->nuzSupported = g->env.gba && game->cfg && !game->cfg->gb && !(game->title && strstr(game->title, "Rogue"));
    pd_nuz_reset(&g->nuz, NULL);
    if (g->nuzSupported && g->env.dataDir && *g->env.dataDir) {
        char dir[1000];
        int len = snprintf(dir, sizeof dir, "%s/nuzlocke", g->env.dataDir);
        if (len > 0 && len < (int) sizeof dir && (size_t) len + 13 < sizeof g->nuzPath) {
            pd_mkdir(dir);
            memcpy(g->nuzPath, dir, (size_t) len);
            snprintf(g->nuzPath + len, sizeof g->nuzPath - (size_t) len, "/%08X.txt", (unsigned) g->env.romCrc);
        }
    }
    FILE* f = g->nuzPath[0] ? fopen(g->nuzPath, "r") : NULL;
    if (f) {
        size_t n = fread(nuzText, 1, sizeof nuzText - 1, f);
        fclose(f);
        nuzText[n] = 0;
        g->nuzOn = !strncmp(nuzText, "on\t1", 4);
        struct pd_nuz_run run;
        pd_nuz_decode(&run, nuzText);
        pd_nuz_reset(&g->nuz, &run);
    }
    g->env.host->nuzlockeSupported = g->nuzSupported;
    g->env.host->nuzlockeOn = g->nuzOn;
    g->env.host->nuzRun = g->nuzSupported ? &g->nuz.run : NULL;
}

static int nuz_caught(void* ctx, int dex) {
    struct pd_lr_gameplay* g = ctx;
    return g->env.snap->dexOk && dex > 0 ? (pd_dex_caught(g->env.snap, dex) ? 1 : 0) : -1;
}

static void nuz_feed(struct pd_lr_gameplay* g) {
    const struct pd_snapshot* snap = g->env.snap;
    g->env.host->nuzArea = snap->mapsec;
    const char* place = pd_mapsec_name(g->env.game, snap->mapsec);
    snprintf(g->env.host->nuzAreaName, sizeof g->env.host->nuzAreaName, "%s", place ? place : "");
    if (!g->nuzOn || !g->nuzSupported) return;
    struct pd_nuz_sample sample;
    memset(&sample, 0, sizeof sample);
    sample.inBattle = snap->inBattle;
    sample.trainer = snap->inBattle ? (snap->isTrainer ? 1 : 0) : -1;
    sample.area = snap->mapsec;
    snprintf(sample.areaName, sizeof sample.areaName, "%s", g->env.host->nuzAreaName);
    const struct pd_battle_mon* foe = &snap->battlers[PD_POS_OPPONENT_LEFT];
    char name[40];
    const char* foeName = foe->species ? pd_species_name(g->env.game, foe->species, name, sizeof name) : NULL;
    if (foe->species && foe->level >= 1 && foe->level <= 100 && foeName && foeName[0]) {
        sample.foeSpecies = foe->species;
        int dex = pd_dex_national(g->env.game, foe->species);
        sample.foeDex = dex > 0 ? dex : foe->species;
        snprintf(sample.foeName, sizeof sample.foeName, "%s", foeName);
    }
    for (int i = 0; i < snap->partyCount && i < 6; i++) {
        const struct pd_mon* m = &snap->party[i];
        struct pd_nuz_party* p = &sample.party[sample.partyCount++];
        p->personality = m->personality;
        p->species = m->species;
        p->level = m->level;
        p->hp = m->hp;
        p->maxHp = m->maxHp;
        p->egg = m->egg || m->species == PD_SPECIES_EGG;
        if (m->nickname[0]) snprintf(p->name, sizeof p->name, "%s", m->nickname);
        else pd_species_name(g->env.game, m->species, p->name, sizeof p->name);
    }
    sample.menuUp = snap->battleState == PD_BATTLE_ACTION || snap->battleState == PD_BATTLE_MOVE ||
                    snap->battleState == PD_BATTLE_BAG_OPEN || snap->battleState == PD_BATTLE_PARTY_OPEN;
    sample.caught = nuz_caught;
    sample.ctx = g;
    struct pd_nuz_events events = {.count = 0};
    if (pd_nuz_feed(&g->nuz, &sample, &events)) nuz_save(g);
    if (g->nuz.run.warnings && events.count) tell(g, events.text[events.count - 1]);
}

static void nuz_edit(struct pd_lr_gameplay* g) {
    struct pd_ui* ui = g->env.ui;
    struct pd_nuz_run* r = &g->nuz.run;
    switch ((enum pd_nuz_edit) ui->nuzEdit) {
    case PD_NUZ_EDIT_OUTCOME: pd_nuz_set_outcome(r, ui->nuzArea, ui->nuzValue); break;
    case PD_NUZ_EDIT_DEAD: pd_nuz_set_dead(r, ui->nuzPersonality, true, g->env.host->nuzAreaName); break;
    case PD_NUZ_EDIT_ALIVE: pd_nuz_set_dead(r, ui->nuzPersonality, false, ""); break;
    case PD_NUZ_EDIT_NEW_RUN: pd_nuz_new_run(r); break;
    case PD_NUZ_EDIT_DUPES: r->dupes = !r->dupes; break;
    case PD_NUZ_EDIT_WARNINGS: r->warnings = !r->warnings; break;
    default: return;
    }
    nuz_save(g);
}

// TRADE EVOS: only the config's verified functions, with each dry-run party record restored before it is shown.
static bool trade_evo_check(const struct pd_lr_gameplay* g) {
    const struct pd_config* cfg = g->env.game->cfg;
    if (!cfg || !cfg->tradeEvoTarget || !cfg->playerParty || !g->env.rom) return false;
    const uint32_t fns[4] = {cfg->tradeEvoTarget, cfg->tradeEvoScene, cfg->tradeEvoReturn, cfg->tradeEvoLock};
    for (int i = 0; i < 4; i++) {
        uint32_t off = (fns[i] & ~1u) - 0x08000000u;
        if (fns[i] < 0x08000000u || off + 32 > g->env.romSize || pd_crc32(g->env.rom + off, 32) != cfg->tradeEvoCrcs[i])
            return false;
    }
    return true;
}

static uint32_t game_call(struct pd_lr_gameplay* g, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                          bool* ok) {
    const uint32_t args[4] = {a0, a1, a2, a3};
    uint32_t r0 = 0;
    if (!pk_call(g->env.core, fn, args, &r0)) *ok = false;
    return r0;
}

static bool trade_evo_field(const struct pd_lr_gameplay* g) {
    int flags = *g->env.fieldFlags;
    return !g->env.snap->inBattle && !(flags & (PK_FIELD_BATTLE | PK_FIELD_LOCKED)) && (flags & PK_FIELD_KNOWN) &&
           (!g->env.wideKnown || pk_wide_scene() == PK_WIDE_SCENE_FIELD);
}

static void trade_evo_targets(struct pd_lr_gameplay* g, int frames) {
    struct pd_ui* ui = g->env.ui;
    bool want = g->tradeEvoOk && g->env.settings->tradeEvos && *g->env.companion && ui->tab == PD_TAB_PARTY && ui->summarySlot >= 0;
    if (!want) {
        memset(g->env.host->evolveTo, 0, sizeof g->env.host->evolveTo);
        return;
    }
    if (frames % 30 || !trade_evo_field(g) || !pk_call_safe(g->env.core)) return;
    const struct pd_config* cfg = g->env.game->cfg;
    for (int i = 0; i < 6; i++) {
        g->env.host->evolveTo[i] = 0;
        if (i >= g->env.snap->partyCount || g->env.snap->party[i].egg || !g->env.snap->party[i].species) continue;
        uint32_t mon = cfg->playerParty + (uint32_t) (i * TRADE_MON_SIZE);
        uint8_t saved[TRADE_MON_SIZE];
        if (!g->env.read(g->env.memoryCtx, mon, saved, sizeof saved)) continue;
        bool ok = true;
        uint32_t target = game_call(g, cfg->tradeEvoTarget, mon, EVO_MODE_TRADE, 0, 0, &ok) & 0xFFFF;
        g->env.write(g->env.memoryCtx, mon, saved, sizeof saved);
        if (ok) g->env.host->evolveTo[i] = (int) target;
    }
}

static void trade_evo_job(struct pd_lr_gameplay* g) {
    if (!g->evolveAsk || !pk_call_safe(g->env.core)) return;
    g->evolveAsk = false;
    const struct pd_config* cfg = g->env.game->cfg;
    const int slot = g->evolveSlot, expected = g->evolveTarget;
    if (!g->tradeEvoOk || slot < 0 || slot >= g->env.snap->partyCount || !g->evolvePersonality || expected <= 0) return;
    if (!trade_evo_field(g)) {
        tell(g, "Not now: close the game's menus first");
        return;
    }
    uint32_t mon = cfg->playerParty + (uint32_t) (slot * TRADE_MON_SIZE);
    uint8_t saved[TRADE_MON_SIZE];
    uint32_t personality = 0;
    if (!g->env.read(g->env.memoryCtx, mon, saved, sizeof saved)) {
        tell(g, "The party has changed. Try again.");
        return;
    }
    memcpy(&personality, saved, sizeof personality);
    if (personality != g->evolvePersonality) {
        tell(g, "The party has changed. Try again.");
        return;
    }
    bool ok = true;
    uint32_t target = game_call(g, cfg->tradeEvoTarget, mon, EVO_MODE_TRADE, 0, 0, &ok) & 0xFFFF;
    bool restored = g->env.write(g->env.memoryCtx, mon, saved, sizeof saved);
    if (!ok || !restored) {
        tell(g, "Couldn't evolve it");
        return;
    }
    if (target != (uint32_t) expected) {
        tell(g, "Its evolution changed. Try again.");
        return;
    }
    uint32_t back = cfg->tradeEvoReturn | 1u;
    uint8_t after[4] = {(uint8_t) back, (uint8_t) (back >> 8), (uint8_t) (back >> 16), (uint8_t) (back >> 24)};
    if (!g->env.write(g->env.memoryCtx, cfg->tradeEvoAfter, after, 4)) {
        tell(g, "Couldn't evolve it");
        return;
    }
    game_call(g, cfg->tradeEvoLock, 0, 0, 0, 0, &ok);
    game_call(g, cfg->tradeEvoScene, mon, target, 0, (uint32_t) slot, &ok);
    g->env.ui->summarySlot = -1;
    memset(g->env.host->evolveTo, 0, sizeof g->env.host->evolveTo);
    *g->env.companion = false;
}

// USE / GIVE / TAKE, TM teaching, party switching and the PC's moves. Each waits for a safe point between frames.
static void item_jobs(struct pd_lr_gameplay* g) {
    if (!g->itemAsk || !pk_call_safe(g->env.core)) return;
    int ask = g->itemAsk;
    g->itemAsk = 0;
    struct pd_ui* ui = g->env.ui;
    const struct pd_config* cfg = g->env.game->cfg;
    bool busy = native_busy(g);
    if (ask == 1 && !ui->itemGive && g->tmCfg && tm_teaches(g->tmCfg, ui->itemId)) {
        enum tm_result r = tm_start(g->env.core, g->tmCfg, ui->itemId, busy || (*g->env.fieldFlags & PK_FIELD_LOCKED));
        if (r == TM_STARTED) {
            g->tmStage = 1;
            g->tmWait = 0;
            g->tmItem = ui->itemId;
            g->tmCallback = tm_main_callback(g->env.core, g->tmCfg);
            *g->env.companion = false;
        } else {
            tell(g, tm_reason(r));
        }
    } else if (ask == 1 && g->partyItemsOk) {
        pd_items_targets(g->env.core, cfg, ui->itemId, ui->itemGive, busy, &g->itemTargets);
        if (g->itemTargets.result != PD_ITEM_USED) {
            tell(g, pd_item_outcome_text(g->itemTargets.result));
            return;
        }
        for (int i = 0; i < g->itemTargets.count; i++) {
            const struct pd_party_target* t = &g->itemTargets.slots[i];
            ui->itemSlots[i].ok = t->verdict == PD_SLOT_OK;
            ui->itemSlots[i].heldItem = t->heldItem;
            memcpy(ui->itemSlots[i].moves, t->moves, sizeof t->moves);
        }
        ui->itemPicking = true;
        ui->itemMoveSlot = -1;
    } else if (ask == 2 && g->partyItemsOk && ui->itemSlot >= 0 && ui->itemSlot < g->itemTargets.count) {
        const struct pd_party_target* t = &g->itemTargets.slots[ui->itemSlot];
        int swapped = 0;
        enum pd_item_outcome r = ui->itemGive
            ? pd_items_give(g->env.core, cfg, ui->itemId, ui->itemSlot, t->personality, busy, &swapped)
            : pd_items_use(g->env.core, cfg, ui->itemId, ui->itemSlot, t->personality, ui->itemMove, busy);
        tell(g, pd_item_outcome_text(r));
    } else if (ask == 3 && g->partyItemsOk && ui->summarySlot >= 0 && ui->summarySlot < g->env.snap->partyCount) {
        int held = 0;
        const struct pd_mon* m = &g->env.snap->party[ui->summarySlot];
        tell(g, pd_item_outcome_text(pd_items_take(g->env.core, cfg, ui->summarySlot, m->personality, busy, &held)));
    }
}

static void tm_jobs(struct pd_lr_gameplay* g) {
    if (!g->tmCfg || g->tmStage != 1 || *g->env.companion || ++g->tmWait < TM_FADE_FRAMES || !pk_call_safe(g->env.core))
        return;
    g->tmStage = 0;
    bool left = tm_main_callback(g->env.core, g->tmCfg) != g->tmCallback;
    if (!tm_finish(g->env.core, g->tmCfg, g->tmItem, left) && !left) tell(g, tm_reason(TM_FAILED));
}

static void swap_job(struct pd_lr_gameplay* g) {
    if (!g->swapAsk || !pk_call_safe(g->env.core)) return;
    g->swapAsk = false;
    struct pd_ui* ui = g->env.ui;
    const struct pd_config* cfg = g->env.game->cfg;
    if (battle_now(g)) {
        tell(g, "Not in a battle");
        return;
    }
    if ((*g->env.fieldFlags & PK_FIELD_LOCKED) || (g->env.wideKnown && pk_wide_scene() != PK_WIDE_SCENE_FIELD)) {
        tell(g, "Not now");
        return;
    }
    int stride = cfg && cfg->partyLayout ? cfg->partyLayout->size : pd_party_layout_vanilla.size;
    if (!cfg || !pd_party_swap_records(cfg->playerParty, stride, g->env.snap->partyCount, ui->switchFrom, ui->switchTo,
                                       g->env.read, g->env.write, g->env.memoryCtx)) {
        tell(g, "Couldn't switch");
        return;
    }
    ui->summarySlot = ui->switchTo;
}

static void box_job(struct pd_lr_gameplay* g) {
    if (!g->boxAsk || !pk_call_safe(g->env.core)) return;
    int ask = g->boxAsk;
    g->boxAsk = 0;
    struct pd_ui* ui = g->env.ui;
    const struct pd_config* cfg = g->env.game->cfg;
    bool busy = native_busy(g);
    const struct pd_mon* boxMon = ui->boxSlot >= 0 && ui->boxSlot < PD_BOX_SLOTS ? &g->boxes.mons[ui->boxSlot] : NULL;
    const struct pd_mon* partyMon = ui->boxPartySlot >= 0 && ui->boxPartySlot < g->env.snap->partyCount
        ? &g->env.snap->party[ui->boxPartySlot] : NULL;
    char boxName[32] = "", partyName[32] = "", scratch[32];
    if (boxMon) {
        if (*boxMon->nickname) snprintf(boxName, sizeof boxName, "%s", boxMon->nickname);
        else snprintf(boxName, sizeof boxName, "%s", pd_species_name(g->env.game, boxMon->species, scratch, sizeof scratch));
    }
    if (partyMon) {
        if (*partyMon->nickname) snprintf(partyName, sizeof partyName, "%s", partyMon->nickname);
        else snprintf(partyName, sizeof partyName, "%s", pd_species_name(g->env.game, partyMon->species, scratch, sizeof scratch));
    }
    enum pd_box_outcome r = PD_BOX_FAILED;
    if (ask == PD_ACTION_BOX_WITHDRAW && boxMon)
        r = pd_box_withdraw(g->env.core, cfg, g->boxes.shown, ui->boxSlot, boxMon->personality, busy);
    else if (ask == PD_ACTION_BOX_SWAP && boxMon && partyMon)
        r = pd_box_swap(g->env.core, cfg, g->boxes.shown, ui->boxSlot, boxMon->personality, ui->boxPartySlot,
                        partyMon->personality, busy);
    else if (ask == PD_ACTION_BOX_DEPOSIT && partyMon)
        r = pd_box_deposit(g->env.core, cfg, ui->boxPartySlot, partyMon->personality, ui->boxTarget, busy);
    int target = ask == PD_ACTION_BOX_DEPOSIT ? ui->boxTarget : g->boxes.shown;
    const char* where = target >= 0 && target < PD_MAX_BOXES ? g->boxes.names[target] : "?";
    if (r != PD_BOX_DONE) snprintf(ui->boxMessage, sizeof ui->boxMessage, "%s", pd_box_outcome_text(r));
    else if (ask == PD_ACTION_BOX_WITHDRAW)
        snprintf(ui->boxMessage, sizeof ui->boxMessage, "%.10s joined the party. Save in the game to keep it.", boxName);
    else if (ask == PD_ACTION_BOX_SWAP)
        snprintf(ui->boxMessage, sizeof ui->boxMessage, "%.8s joined, %.8s went to %.8s. Save to keep it.", boxName, partyName, where);
    else
        snprintf(ui->boxMessage, sizeof ui->boxMessage, "%.10s was stored in %.8s. Save in the game to keep it.", partyName, where);
    if (r == PD_BOX_DONE) {
        ui->boxSummary = -1;
        ui->boxSlot = -1;
        ui->summarySlot = -1;
    }
    pd_boxes_read(&g->boxes, g->env.game, g->env.read, g->env.memoryCtx, ui->boxShown);
}

// Center operations use the same pk_call and ROM validators as the existing field / TM adapters.
static bool center_call(void* ctx, uint32_t fn, const uint32_t args[4], uint32_t* r0) {
    struct pd_lr_gameplay* g = ctx;
    return pk_call(g->env.core, fn, args, r0) != 0;
}

static bool center_field_supported(void* ctx) {
    struct pd_lr_gameplay* g = ctx;
    return field_items_supported(g->env.game->cfg, g->env.rom, g->env.romSize);
}

static bool center_pc_supported(void* ctx) {
    struct pd_lr_gameplay* g = ctx;
    return pc_supported(g->env.game->cfg, g->env.rom, g->env.romSize);
}

static bool center_tm_supported(void* ctx) {
    struct pd_lr_gameplay* g = ctx;
    return g->tmCfg != NULL;
}

static struct pd_center_ops center_ops(struct pd_lr_gameplay* g) {
    return (struct pd_center_ops){g, g->env.read, g->env.write, center_call, center_field_supported,
                                  center_pc_supported, center_tm_supported};
}

static void center_result(struct pd_lr_gameplay* g, enum pd_center_result r, const char* success) {
    if (r == PD_CENTER_OK && success) tell(g, success);
    else if (r != PD_CENTER_OK) tell(g, pd_center_result_text(r));
}

static void center_script_job(struct pd_lr_gameplay* g) {
    if (g->centerStage != 2 || *g->env.companion) return;
    if (++g->centerWait > 240 && !g->centerScriptSeenBusy) {
        g->centerStage = 0;
        tell(g, pd_center_result_text(PD_CENTER_FAILED));
        return;
    }
    if (!pk_call_safe(g->env.core)) return;
    uint32_t locked = 1;
    if (!center_call(g, g->env.game->cfg->fieldLocked, (const uint32_t[]){0, 0, 0, 0}, &locked)) return;
    if (locked) {
        g->centerScriptSeenBusy = true;
        return;
    }
    if (!g->centerScriptSeenBusy) return;
    int action = g->centerScriptAction;
    g->centerStage = 0;
    if (action == PD_ACTION_CENTER_HEAL) tell(g, "Your POKéMON are fully healed.");
    else if (action == PD_ACTION_CENTER_FORGET) tell(g, "Move forgotten.");
}

static void center_jobs(struct pd_lr_gameplay* g) {
    struct pd_center_ops ops = center_ops(g);
    if (g->centerStage == 1) {
        if (*g->env.companion || ++g->centerWait < TM_FADE_FRAMES || !pk_call_safe(g->env.core)) return;
        g->centerStage = 0;
        bool left = tm_main_callback(g->env.core, g->tmCfg) != g->centerCallback;
        if (!pd_center_relearn_finish(&ops, g->env.game->cfg, &g->center, g->centerSlot, g->centerPersonality, left) && !left)
            tell(g, pd_center_result_text(PD_CENTER_FAILED));
        return;
    }
    if (g->centerStage == 2) {
        center_script_job(g);
        return;
    }
    if (!g->centerAsk || !pk_call_safe(g->env.core)) return;
    int ask = g->centerAsk;
    g->centerAsk = 0;
    bool busy = native_busy(g), battle = battle_now(g);
    const struct pd_config* cfg = g->env.game->cfg;
    struct pd_ui* ui = g->env.ui;
    enum pd_center_result r = PD_CENTER_FAILED;
    if (ask == PD_ACTION_CENTER_HEAL) {
        r = pd_center_heal(&ops, cfg, &g->center, busy, battle);
        center_result(g, r, NULL);
        if (r == PD_CENTER_OK) {
            g->centerStage = 2;
            g->centerWait = 0;
            g->centerScriptAction = ask;
            g->centerScriptSeenBusy = false;
            *g->env.companion = false;
        }
    } else if (ask == PD_ACTION_CENTER_NICKNAME) {
        r = pd_center_nickname(&ops, cfg, &g->center, ui->summarySlot, g->centerPersonality, busy, battle);
        center_result(g, r, NULL);
        if (r == PD_CENTER_OK) *g->env.companion = false;
    } else if (ask == PD_ACTION_CENTER_FORGET) {
        int move = g->centerMove >= 0 && g->centerMove < PD_NUM_MOVES && ui->summarySlot >= 0 &&
                   ui->summarySlot < g->env.snap->partyCount ? g->env.snap->party[ui->summarySlot].moves[g->centerMove] : 0;
        r = pd_center_forget(&ops, cfg, &g->center, ui->summarySlot, g->centerPersonality, g->centerMove,
                             move, false, busy, battle);
        center_result(g, r, NULL);
        if (r == PD_CENTER_OK) {
            g->centerStage = 2;
            g->centerWait = 0;
            g->centerScriptAction = ask;
            g->centerScriptSeenBusy = false;
            *g->env.companion = false;
        }
    } else if (ask == PD_ACTION_CENTER_RELEARN && g->center.relearnSpecial >= 0) {
        r = pd_center_relearn_script(&ops, cfg, &g->center, ui->summarySlot, g->centerPersonality, busy, battle);
        center_result(g, r, NULL);
        if (r == PD_CENTER_OK) *g->env.companion = false;
    } else if (ask == PD_ACTION_CENTER_RELEARN) {
        r = pd_center_relearn_start(&ops, cfg, &g->center, ui->summarySlot, g->centerPersonality, busy, battle);
        center_result(g, r, NULL);
        if (r == PD_CENTER_OK) {
            g->centerStage = 1;
            g->centerWait = 0;
            g->centerSlot = ui->summarySlot;
            g->centerCallback = tm_main_callback(g->env.core, g->tmCfg);
            *g->env.companion = false;
        }
    }
}

void pd_lr_gameplay_sync_host(struct pd_lr_gameplay* g) {
    struct pd_host_info* host = g->env.host;
    host->canTake = g->partyItemsOk && pd_items_can_take();
    host->canSwitchParty = g->env.gba && g->env.game->cfg && g->env.game->cfg->playerParty;
    host->centerServices = 0;
    if (g->centerOk && center_pc_supported(g)) {
        host->centerServices |= PD_CENTER_HEAL;
        if (g->partyItemsOk) {
            if (g->center.nickname >= 0) host->centerServices |= PD_CENTER_NICKNAME;
            if (g->center.forget >= 0) host->centerServices |= PD_CENTER_FORGET;
            if ((g->center.hasRelearn && g->tmCfg) || g->center.relearnSpecial >= 0)
                host->centerServices |= PD_CENTER_RELEARN;
        }
    }
    host->nuzlockeOn = g->nuzOn;
}

void pd_lr_gameplay_init(struct pd_lr_gameplay* g, const struct pd_lr_gameplay_env* env, bool polishedBattle) {
    memset(g, 0, sizeof *g);
    g->env = *env;
    active = g;
    pd_binput_init(&g->input, g->env.game, g->env.read, g->env.gba ? g->env.write : NULL, g->env.memoryCtx);
    g->battleActions = g->env.gba ? pd_binput_available(&g->input) : polishedBattle;
    if (g->battleActions) g->env.ui->battleInput = &g->input;
    g->env.host->hasGameplayRows = true;
    if (!g->env.gba) return;

    const struct pd_config* cfg = g->env.game->cfg;
    g->partyItemsOk = cfg && field_items_supported(cfg, g->env.rom, g->env.romSize) &&
                      pd_items_setup(cfg, g->env.rom, g->env.romSize);
    g->tmCfg = cfg && tm_supported(cfg, g->env.rom, g->env.romSize) ? cfg : NULL;
    if (g->partyItemsOk || g->tmCfg) {
        g->env.host->itemCanUse = item_can_use;
        g->env.host->itemCanGive = item_can_give;
    }
    if (cfg && pd_boxes_supported(g->env.game)) g->env.ui->boxes = &g->boxes;
    g->env.ui->boxMoves = g->env.ui->boxes && g->partyItemsOk &&
        pd_box_moves_ok(cfg, g->env.game->entry ? g->env.game->entry->boxSwap : NULL, g->env.rom, g->env.romSize);
    g->env.ui->boxSummary = g->env.ui->boxSlot = -1;
    options_find(g);
    options_sync(g);
    g->tradeEvoOk = trade_evo_check(g);
    g->env.host->tradeEvoSupported = g->tradeEvoOk;
    nuz_load(g);
    g->centerOk = pd_center_find(g->env.rom, g->env.romSize, &g->center);
    pd_lr_gameplay_sync_host(g);
}

void pd_lr_gameplay_jump(struct pd_lr_gameplay* g, bool polishedBattle) {
    pd_binput_init(&g->input, g->env.game, g->env.read, g->env.gba ? g->env.write : NULL, g->env.memoryCtx);
    g->battleActions = g->env.gba ? pd_binput_available(&g->input) : polishedBattle;
    g->env.ui->battleInput = g->battleActions ? &g->input : NULL;
    g->env.ui->battleChoosingMove = g->env.ui->battleChoosingMon = false;
    g->itemAsk = g->boxAsk = g->centerAsk = 0;
    g->swapAsk = g->evolveAsk = false;
    g->tmStage = g->centerStage = 0;
    g->haveFieldFlags = false;
}

void pd_lr_gameplay_frame(struct pd_lr_gameplay* g, int frames) {
    int flags = g->env.gba ? *g->env.fieldFlags : 0;
    bool stateChanged = g->env.gba && g->input.state != g->env.snap->battleState;
    bool battleTransition = g->haveFieldFlags && ((g->lastFieldFlags ^ flags) & PK_FIELD_BATTLE);
    bool urgent = stateChanged || battleTransition;
    bool tracking = g->nuzOn && g->nuzSupported && (frames % 10 == 0 || urgent);
    if (frames % 30 == 0 || tracking || urgent) {
        g->env.snapshot(g->env.callbackCtx);
        if (g->nuzSupported && (tracking || urgent || !g->nuzOn)) nuz_feed(g);
    }
    g->lastFieldFlags = flags;
    g->haveFieldFlags = g->env.gba;
    struct pd_ui* ui = g->env.ui;
    if (g->env.gba && ui->boxes && *g->env.companion &&
        (ui->tab == PD_TAB_BOXES || ui->overlay == PD_OVERLAY_PICK_BOX) &&
        (frames % 30 == 0 || ui->boxShown != g->boxes.shown))
        pd_boxes_read(&g->boxes, g->env.game, g->env.read, g->env.memoryCtx, ui->boxShown);
    if (g->env.gba) trade_evo_targets(g, frames);
}

void pd_lr_gameplay_jobs(struct pd_lr_gameplay* g) {
    if (!g->env.gba) return;
    tm_jobs(g);
    center_jobs(g);
    item_jobs(g);
    box_job(g);
    swap_job(g);
    trade_evo_job(g);
}

bool pd_lr_gameplay_action(struct pd_lr_gameplay* g, enum pd_action action) {
    struct pd_ui* ui = g->env.ui;
    if (!g->env.gba && action != PD_ACTION_SETTINGS_CHANGED) return false;
    switch (action) {
    case PD_ACTION_SETTINGS_CHANGED:
        options_sync(g);
        return true;
    case PD_ACTION_ITEM_PICK:
        if (g->partyItemsOk || (g->tmCfg && !ui->itemGive && tm_teaches(g->tmCfg, ui->itemId))) g->itemAsk = 1;
        return true;
    case PD_ACTION_ITEM_APPLY:
        if (g->partyItemsOk) g->itemAsk = 2;
        return true;
    case PD_ACTION_ITEM_TAKE:
        if (g->partyItemsOk) g->itemAsk = 3;
        return true;
    case PD_ACTION_BOX_WITHDRAW:
    case PD_ACTION_BOX_SWAP:
    case PD_ACTION_BOX_DEPOSIT:
        if (ui->boxMoves) g->boxAsk = action;
        return true;
    case PD_ACTION_PARTY_SWAP:
        if (g->env.host->canSwitchParty) g->swapAsk = true;
        return true;
    case PD_ACTION_TRADE_EVOLVE:
        if (g->tradeEvoOk && !g->evolveAsk && ui->summarySlot >= 0 && ui->summarySlot < g->env.snap->partyCount &&
            g->env.snap->party[ui->summarySlot].personality && g->env.host->evolveTo[ui->summarySlot] > 0) {
            g->evolveSlot = ui->summarySlot;
            g->evolvePersonality = g->env.snap->party[ui->summarySlot].personality;
            g->evolveTarget = g->env.host->evolveTo[ui->summarySlot];
            g->evolveAsk = true;
        }
        return true;
    case PD_ACTION_NUZLOCKE_TOGGLE:
        if (g->nuzSupported) {
            g->nuzOn = !g->nuzOn;
            g->env.host->nuzlockeOn = g->nuzOn;
            nuz_save(g);
        }
        return true;
    case PD_ACTION_NUZLOCKE_EDIT:
        if (g->nuzSupported) nuz_edit(g);
        return true;
    case PD_ACTION_CENTER_HEAL:
    case PD_ACTION_CENTER_NICKNAME:
    case PD_ACTION_CENTER_FORGET:
    case PD_ACTION_CENTER_RELEARN:
        if (g->centerOk && !g->centerAsk && !g->centerStage && ui->summarySlot >= 0 &&
            ui->summarySlot < g->env.snap->partyCount) {
            g->centerPersonality = g->env.snap->party[ui->summarySlot].personality;
            g->centerMove = ui->centerMove;
            g->centerAsk = action;
        }
        return true;
    default:
        return false;
    }
}
