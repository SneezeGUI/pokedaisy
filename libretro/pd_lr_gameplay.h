#ifndef PD_LR_GAMEPLAY_H
#define PD_LR_GAMEPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pd_boxes.h"
#include "pd_center.h"
#include "pd_nuzlocke.h"
#include "pd_party.h"
#include "pd_ui.h"
#include "../3ds/host/pd_items.h"
#include "../3ds/host/pd_rom_options.h"

struct mCore;

struct pd_lr_gameplay_env {
    struct mCore* core;
    const struct pd_game* game;
    struct pd_settings* settings;
    struct pd_ui* ui;
    struct pd_snapshot* snap;
    struct pd_host_info* host;
    const uint8_t* rom;
    size_t romSize;
    uint32_t romCrc;
    const char* dataDir;
    bool gba, wideKnown, fieldKnown;
    int* fieldFlags;
    bool* companion;
    pd_read_fn read;
    pd_write_fn write;
    bool (*romWrite)(void* ctx, uint32_t addr, const void* data, size_t len);
    void* memoryCtx;
    void (*notice)(void* ctx, const char* text);
    void (*snapshot)(void* ctx);
    void* callbackCtx;
};

struct pd_lr_gameplay {
    struct pd_lr_gameplay_env env;
    struct pd_binput input;
    bool battleActions, partyItemsOk, tradeEvoOk;
    const struct pd_config* tmCfg;
    struct pd_boxes boxes;

    struct pd_rom_write tmWrites[PD_TM_SITES_MAX], textWrites[PD_TEXT_WRITES_MAX];
    int tmCount, textCount;
    bool tmApplied, textApplied;

    struct pd_nuz_tracker nuz;
    bool nuzOn, nuzSupported;
    int lastFieldFlags;
    bool haveFieldFlags;
    char nuzPath[1050];

    int itemAsk;
    struct pd_party_targets itemTargets;
    int boxAsk;
    bool swapAsk, evolveAsk;
    int evolveSlot, evolveTarget;
    uint32_t evolvePersonality;

    struct pd_center_found center;
    bool centerOk;
    int centerAsk, centerStage, centerWait, centerSlot, centerScriptAction;
    uint32_t centerPersonality, centerCallback;
    int centerMove;
    bool centerScriptSeenBusy;

    int tmStage, tmWait, tmItem;
    uint32_t tmCallback;
};

void pd_lr_gameplay_init(struct pd_lr_gameplay* g, const struct pd_lr_gameplay_env* env, bool polishedBattle);
void pd_lr_gameplay_jump(struct pd_lr_gameplay* g, bool polishedBattle);
void pd_lr_gameplay_frame(struct pd_lr_gameplay* g, int frames);
void pd_lr_gameplay_jobs(struct pd_lr_gameplay* g);
void pd_lr_gameplay_sync_host(struct pd_lr_gameplay* g);
bool pd_lr_gameplay_action(struct pd_lr_gameplay* g, enum pd_action action);

#endif
