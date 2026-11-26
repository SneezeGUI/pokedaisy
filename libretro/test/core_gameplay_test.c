#define _GNU_SOURCE
#include "../pd_lr_gameplay.h"
#include "pd_center.h"
#include "pd_gb.h"
#include "pd_party.h"
#include "pk_wide.h"
#include "../../3ds/host/pd_rom_options.h"
#include <mgba-util/crc32.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t memoryImage[0x40000];
static uint8_t iwramImage[0x8000];
static int writeCount, setupCount, fadeOutCount, unlockCount, afterWriteCount;
static uint32_t lastWrite, lastCall, lastArgs[4];
static uint8_t lastBytes[32];
static size_t lastWriteLen;
static bool support = true;
static uint32_t fieldLockedResult;
static uint32_t personalities[6] = {0x11110000, 0x22220000, 0x33330000, 0x44440000, 0x55550000, 0x66660000};
static uint16_t partyMoves[6][4];

static bool test_read(void* ctx, uint32_t addr, void* out, size_t len) {
    (void)ctx;
    if (addr >= 0x02000000u && addr - 0x02000000u + len <= sizeof memoryImage) {
        memcpy(out, memoryImage + addr - 0x02000000u, len);
        return true;
    }
    if (addr >= 0x03000000u && addr - 0x03000000u + len <= sizeof iwramImage) {
        memcpy(out, iwramImage + addr - 0x03000000u, len);
        return true;
    }
    return false;
}

static bool test_write(void* ctx, uint32_t addr, const void* data, size_t len) {
    if (!test_read(ctx, addr, lastBytes, 0)) return false;
    writeCount++;
    if (addr == 0x03000100u) afterWriteCount++;
    lastWrite = addr;
    lastWriteLen = len;
    memset(lastBytes, 0, sizeof lastBytes);
    memcpy(lastBytes, data, len <= sizeof lastBytes ? len : sizeof lastBytes);
    if (addr >= 0x02000000u && addr - 0x02000000u + len <= sizeof memoryImage) {
        memcpy(memoryImage + addr - 0x02000000u, data, len);
        return true;
    }
    if (addr >= 0x03000000u && addr - 0x03000000u + len <= sizeof iwramImage) {
        memcpy(iwramImage + addr - 0x03000000u, data, len);
        return true;
    }
    return false;
}

static bool center_call(void* ctx, uint32_t fn, const uint32_t args[4], uint32_t* r0) {
    (void)ctx;
    lastCall = fn;
    memcpy(lastArgs, args, sizeof lastArgs);
    uint32_t result = 0;
    if (fn == 0x08000101u || fn == 0x08000081u) result = fieldLockedResult;
    else if (fn == 0x08000201u) {
        int slot = (args[0] - 0x02000000u) / 100;
        if (args[1] == 0) result = slot < 6 ? personalities[slot] : 0;
        else if (args[1] == 45) result = 0;
        else if (args[1] >= 13 && args[1] < 17) result = slot < 6 ? partyMoves[slot][args[1] - 13] : 0;
    } else if (fn == 0x0800CC1B4u) {
        memoryImage[0x370D0] = 1;
    } else if (fn == 0x08000301u || fn == 0x080000E1u) {
        setupCount++;
    } else if (fn == 0x08000401u) {
        fadeOutCount++;
    } else if (fn == 0x08000402u) {
        unlockCount++;
    }
    if (r0) *r0 = result;
    return support;
}

static bool supported(void* ctx) {
    (void)ctx;
    return support;
}

static struct pd_config center_config(void) {
    static struct pd_party_fns party;
    party = (struct pd_party_fns){.getMonData = 0x08000201u, .party = 0x02000000u, .partyCount = 0x02001000u};
    return (struct pd_config){.partyFns = &party, .fieldLocked = 0x08000101u, .pcSetupScript = 0x08000301u,
        .tmLock = 0x08000403u, .tmFade = 0x08000401u, .tmUnlock = 0x08000402u,
        .tmCleanup = 0x08000404u, .tmSetMainCallback2 = 0x08000405u};
}

static struct pd_center_found center_found(void) {
    return (struct pd_center_found){.heal = 0, .nickname = 158, .forget = 221, .relearnSpecial = -1,
        .hasRelearn = true, .relearn = {224, 0x080E47A1u, 0x03005020u, 0x0807DD39u},
        .otIdNotPlayers = 0x0800CC1B4u, .windowFrame = 0x080F6F14u,
        .var8004 = 0x020370C0u, .varResult = 0x020370D0u, .healSong = 256, .forgetSong = 270};
}

static struct pd_center_ops center_ops(void) {
    return (struct pd_center_ops){.read = test_read, .write = test_write, .call = center_call,
        .fieldSupported = supported, .pcSupported = supported, .tmSupported = supported};
}

static void reset_center(void) {
    memset(memoryImage, 0, sizeof memoryImage);
    memset(iwramImage, 0, sizeof iwramImage);
    memset(partyMoves, 0, sizeof partyMoves);
    memoryImage[0x1000] = 2;
    partyMoves[0][0] = 10;
    partyMoves[0][1] = 20;
    partyMoves[1][0] = 30;
    writeCount = setupCount = fadeOutCount = unlockCount = 0;
    lastWrite = lastCall = 0;
    support = true;
}

static void test_center_actions(void) {
    struct pd_center_ops ops = center_ops();
    struct pd_config cfg = center_config();
    struct pd_center_found found = center_found();
    reset_center();
    assert(pd_center_heal(&ops, &cfg, &found, false, true) == PD_CENTER_IN_BATTLE);
    assert(!writeCount && !setupCount);
    assert(pd_center_heal(&ops, &cfg, &found, false, false) == PD_CENTER_OK);
    const uint8_t heal[] = {0x69, 0x25, 0, 0, 0x31, 0, 1, 0x32, 0x6B, 0x02};
    assert(lastWrite == 0x0203FFD8u && lastWriteLen == sizeof heal && !memcmp(lastBytes, heal, sizeof heal));
    assert(lastCall == cfg.pcSetupScript && lastArgs[0] == 0x0203FFD8u && setupCount == 1);

    reset_center();
    assert(pd_center_nickname(&ops, &cfg, &found, 0, personalities[0], false, false) == PD_CENTER_TRADED);
    assert(lastCall == found.otIdNotPlayers && !setupCount);
    assert(pd_center_nickname(&ops, &cfg, &found, 0, 0, false, false) == PD_CENTER_PARTY_CHANGED);
    assert(!setupCount);

    reset_center();
    partyMoves[0][1] = 0;
    assert(pd_center_forget(&ops, &cfg, &found, 0, personalities[0], 0, 10, false, false, false) == PD_CENTER_LAST_MOVE);
    assert(!setupCount);

    reset_center();
    partyMoves[0][0] = 57;
    assert(pd_center_forget(&ops, &cfg, &found, 0, personalities[0], 0, 57, false, false, false) == PD_CENTER_LAST_SURFER);
    assert(!setupCount);
    assert(pd_center_forget(&ops, &cfg, &found, 0, personalities[0], 0, 57, true, false, false) == PD_CENTER_OK);
    const uint8_t forget[] = {0x69, 0x16, 4, 0x80, 0, 0, 0x16, 5, 0x80, 0, 0, 0x25, 0xDD, 0, 0x31, 0x0E, 1, 0x32, 0x6B, 0x02};
    assert(lastWriteLen == sizeof forget && !memcmp(lastBytes, forget, sizeof forget) && setupCount == 1);

    reset_center();
    assert(!pd_center_relearn_finish(&ops, &cfg, &found, 0, personalities[0], true));
    assert(!writeCount && !fadeOutCount && !unlockCount);
    assert(pd_center_relearn_finish(&ops, &cfg, &found, 0, personalities[0], false));
    uint32_t callback = 0;
    assert(test_read(NULL, found.relearn.fieldCallback, &callback, 4) && callback == found.relearn.continueScript);
    assert(lastCall == cfg.tmSetMainCallback2 && lastArgs[0] == found.relearn.init && setupCount == 1);

    reset_center();
    assert(pd_center_relearn_start(&ops, &cfg, &found, 0, personalities[0], false, false) == PD_CENTER_OK);
    assert(lastCall == cfg.tmFade && lastArgs[0] == 1);
    assert(pd_center_relearn_start(&ops, &cfg, &found, 0, personalities[0], true, false) == PD_CENTER_NOT_NOW);
}

static bool failSecondWrite, failReads;
static bool swap_read(void* ctx, uint32_t addr, void* out, size_t len) {
    (void)ctx;
    if (failReads) return false;
    memcpy(out, memoryImage + addr, len);
    return true;
}
static bool swap_write(void* ctx, uint32_t addr, const void* data, size_t len) {
    (void)ctx;
    if (failSecondWrite && addr == 110) return false;
    memcpy(memoryImage + addr, data, len);
    return true;
}

static void test_party_swap(void) {
    for (int i = 0; i < 256; i++) memoryImage[i] = (uint8_t)i;
    assert(pd_party_swap_records(10, 100, 2, 0, 1, swap_read, swap_write, NULL));
    for (int i = 0; i < 100; i++) {
        assert(memoryImage[10 + i] == (uint8_t)(110 + i));
        assert(memoryImage[110 + i] == (uint8_t)(10 + i));
    }
    assert(!pd_party_swap_records(10, 100, 2, 0, 0, swap_read, swap_write, NULL));
    failSecondWrite = true;
    for (int i = 0; i < 256; i++) memoryImage[i] = (uint8_t)i;
    assert(!pd_party_swap_records(10, 100, 2, 0, 1, swap_read, swap_write, NULL));
    for (int i = 0; i < 200; i++) assert(memoryImage[10 + i] == (uint8_t)(10 + i));
    failSecondWrite = false;
    failReads = true;
    memoryImage[0] = 0xA5;
    assert(!pd_party_swap_records(10, 100, 2, 0, 1, swap_read, swap_write, NULL));
    assert(memoryImage[0] == 0xA5);
    failReads = false;
}

static bool battle_read(void* ctx, uint32_t addr, void* out, size_t len) {
    (void)ctx;
    assert(addr + len <= sizeof memoryImage);
    memcpy(out, memoryImage + addr, len);
    return true;
}
static bool battle_write(void* ctx, uint32_t addr, const void* data, size_t len) {
    (void)ctx;
    assert(addr + len <= sizeof memoryImage);
    memcpy(memoryImage + addr, data, len);
    return true;
}

static void test_battle_actions(void) {
    struct pd_config cfg = {.gMain = 0x200, .battlerControllerFuncs = 0x300,
        .handleInputChooseAction = 0x08000101, .handleInputChooseMove = 0x08000201,
        .actionCursor = 0x100, .moveCursor = 0x110};
    struct pd_game game = {.cfg = &cfg};
    static const int cells[] = {0, 1, 2, 3};
    for (int action = PD_ACT_FIGHT; action <= PD_ACT_RUN; action++) {
        memset(memoryImage, 0, sizeof memoryImage);
        struct pd_binput input;
        pd_binput_init(&input, &game, battle_read, battle_write, NULL);
        assert(pd_binput_available(&input));
        memoryImage[0x200] = 2;
        uint32_t handler = cfg.handleInputChooseAction;
        memcpy(memoryImage + 0x300, &handler, sizeof handler);
        pd_binput_action(&input, (enum pd_battle_action)action);
        bool exclusive = false;
        uint32_t keys = pd_binput_tick(&input, &exclusive);
        assert(keys == PD_KEY_A && exclusive);
        if (cells[action]) assert(memoryImage[0x100] == cells[action]);
        if (action == PD_ACT_RUN) assert(input.runPresses > 0);
    }
    struct pd_binput unavailable;
    struct pd_config noCfg = {0};
    game.cfg = &noCfg;
    pd_binput_init(&unavailable, &game, battle_read, battle_write, NULL);
    assert(!pd_binput_available(&unavailable));
    bool exclusive = true;
    assert(!pd_binput_tick(&unavailable, &exclusive) && !exclusive);
}

static bool callSafe = true;
static int wideScene, sceneCount, sceneSlot, targetCallCount, noticeCount;
static uint32_t tradeTarget;
static char noticeText[128];
static uint8_t fakeRom[256];

int pk_call_safe(struct mCore* core) {
    (void)core;
    return callSafe;
}

int pk_wide_scene(void) {
    return wideScene;
}

int pk_call(struct mCore* core, uint32_t fn, const uint32_t args[4], uint32_t* r0) {
    (void)core;
    if (fn == 0x08001001u) {
        targetCallCount++;
        if (r0) *r0 = tradeTarget;
        return true;
    }
    if (fn == 0x08005001u) {
        sceneCount++;
        sceneSlot = (int)args[2];
        return true;
    }
    return center_call(NULL, fn, args, r0);
}

static void adapter_notice(void* ctx, const char* text) {
    (void)ctx;
    noticeCount++;
    snprintf(noticeText, sizeof noticeText, "%s", text);
}

static struct pd_config adapter_config(void) {
    struct pd_config cfg = center_config();
    memset(fakeRom, 0, sizeof fakeRom);
    const uint32_t fieldFns[5] = {0x08000001u, 0x08000021u, 0x08000041u, 0x08000061u, 0x08000081u};
    cfg.fieldVarGet = fieldFns[0]; cfg.fieldVarSet = fieldFns[1]; cfg.fieldCheckBag = fieldFns[2];
    cfg.fieldRemoveBag = fieldFns[3]; cfg.fieldLocked = fieldFns[4];
    for (int i = 0; i < 5; i++) cfg.fieldCrcs[i] = doCrc32(fakeRom + i * 32, 32);
    cfg.pcSpecials = 0x080000A0u;
    cfg.pcSpecial = 0;
    cfg.pcFn = 0x080000C1u;
    cfg.pcSetupScript = 0x080000E1u;
    memcpy(fakeRom + 0xA0, &cfg.pcFn, sizeof cfg.pcFn);
    cfg.pcCrcs[0] = doCrc32(fakeRom + 0xE0, 32);
    cfg.pcCrcs[1] = doCrc32(fakeRom + 0xC0, 32);
    cfg.playerParty = 0x02000000u;
    cfg.tradeEvoTarget = 0x08001001u;
    cfg.tradeEvoScene = 0x08005001u;
    cfg.tradeEvoAfter = 0x03000100u;
    cfg.tradeEvoReturn = 0x08003000u;
    cfg.tradeEvoLock = 0x08004001u;
    return cfg;
}

static void adapter_reset(struct pd_lr_gameplay* g) {
    reset_center();
    static struct pd_config cfg;
    cfg = adapter_config();
    static struct pd_game game;
    static struct pd_settings settings;
    static struct pd_ui ui;
    static struct pd_snapshot snap;
    static struct pd_host_info host;
    static int flags;
    static bool companion;
    game = (struct pd_game){.cfg = &cfg, .rom = fakeRom, .romSize = sizeof fakeRom};
    settings = (struct pd_settings){0};
    ui = (struct pd_ui){.summarySlot = 0, .centerMove = 0};
    snap = (struct pd_snapshot){.partyCount = 2};
    snap.party[0].personality = personalities[0];
    snap.party[0].moves[0] = 10;
    snap.party[0].moves[1] = 20;
    snap.party[1].personality = personalities[1];
    host = (struct pd_host_info){0};
    host.evolveTo[0] = 200;
    flags = PK_FIELD_KNOWN;
    companion = true;
    memset(g, 0, sizeof *g);
    g->env = (struct pd_lr_gameplay_env){.game = &game, .settings = &settings, .ui = &ui, .snap = &snap, .host = &host,
        .rom = fakeRom, .romSize = sizeof fakeRom, .gba = true, .fieldFlags = &flags, .companion = &companion,
        .read = test_read, .write = test_write, .notice = adapter_notice};
    g->tradeEvoOk = g->centerOk = true;
    g->center = center_found();
    memcpy(memoryImage, &personalities[0], sizeof personalities[0]);
    memcpy(memoryImage + 100, &personalities[1], sizeof personalities[1]);
    callSafe = true;
    wideScene = PK_WIDE_SCENE_FIELD;
    tradeTarget = 200;
    sceneCount = sceneSlot = targetCallCount = noticeCount = afterWriteCount = 0;
    noticeText[0] = 0;
    fieldLockedResult = 0;
}

static void test_delayed_evolution(void) {
    struct pd_lr_gameplay g;
    adapter_reset(&g);
    callSafe = false;
    assert(pd_lr_gameplay_action(&g, PD_ACTION_TRADE_EVOLVE));
    assert(g.evolveAsk && g.evolveSlot == 0 && g.evolvePersonality == personalities[0] && g.evolveTarget == 200);
    g.env.ui->summarySlot = 1; // A selection change alone must not redirect the captured request.
    pd_lr_gameplay_jobs(&g);
    assert(!targetCallCount && !sceneCount && !afterWriteCount);
    callSafe = true;
    pd_lr_gameplay_jobs(&g);
    assert(targetCallCount == 1 && sceneCount == 1 && sceneSlot == 0 && afterWriteCount == 1);

    adapter_reset(&g);
    callSafe = false;
    assert(pd_lr_gameplay_action(&g, PD_ACTION_TRADE_EVOLVE));
    memcpy(memoryImage, &personalities[1], sizeof personalities[1]); // A party swap changed the captured slot's identity.
    callSafe = true;
    pd_lr_gameplay_jobs(&g);
    assert(!targetCallCount && !sceneCount && !afterWriteCount);
    assert(*g.env.companion && strstr(noticeText, "party has changed"));

    adapter_reset(&g);
    uint8_t before[100];
    memcpy(before, memoryImage, sizeof before);
    tradeTarget = 201;
    assert(pd_lr_gameplay_action(&g, PD_ACTION_TRADE_EVOLVE));
    pd_lr_gameplay_jobs(&g);
    assert(targetCallCount == 1 && !sceneCount && !afterWriteCount);
    assert(!memcmp(before, memoryImage, sizeof before)); // The target dry run was restored before cancellation.
    assert(strstr(noticeText, "evolution changed"));
}

static void test_queued_center_scripts(void) {
    struct pd_lr_gameplay g;
    adapter_reset(&g);
    assert(pd_lr_gameplay_action(&g, PD_ACTION_CENTER_HEAL));
    pd_lr_gameplay_jobs(&g);
    assert(setupCount == 1 && g.centerStage == 2 && !*g.env.companion && !noticeCount);
    *g.env.companion = true;
    assert(pd_lr_gameplay_action(&g, PD_ACTION_CENTER_HEAL));
    pd_lr_gameplay_jobs(&g);
    assert(setupCount == 1 && g.centerStage == 2 && !noticeCount); // Reopening cannot complete or replace the pending script.
    *g.env.companion = false;
    fieldLockedResult = 1;
    pd_lr_gameplay_jobs(&g);
    assert(g.centerScriptSeenBusy && !noticeCount);
    fieldLockedResult = 0;
    pd_lr_gameplay_jobs(&g);
    assert(!g.centerStage && noticeCount == 1 && !strcmp(noticeText, "Your POKéMON are fully healed."));
    assert(pd_lr_gameplay_action(&g, PD_ACTION_CENTER_HEAL));
    pd_lr_gameplay_jobs(&g);
    assert(setupCount == 2 && g.centerStage == 2); // A later request is allowed only after the first completed.

    adapter_reset(&g);
    assert(pd_lr_gameplay_action(&g, PD_ACTION_CENTER_FORGET));
    pd_lr_gameplay_jobs(&g);
    assert(setupCount == 1 && g.centerStage == 2 && !*g.env.companion && !noticeCount);
    assert(pd_lr_gameplay_action(&g, PD_ACTION_CENTER_FORGET));
    pd_lr_gameplay_jobs(&g);
    assert(setupCount == 1 && !noticeCount);
    fieldLockedResult = 1;
    pd_lr_gameplay_jobs(&g);
    fieldLockedResult = 0;
    pd_lr_gameplay_jobs(&g);
    assert(!g.centerStage && noticeCount == 1 && !strcmp(noticeText, "Move forgotten."));
}

static bool renameFails;
static int test_rename(const char* from, const char* to) {
    return renameFails ? -1 : rename(from, to);
}

static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(!fclose(f));
}

static void read_text(const char* path, char* out, size_t size) {
    FILE* f = fopen(path, "r");
    assert(f);
    size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
}

static void test_nuz_atomic_replace(void) {
    char dir[] = "/tmp/pd-nuz-XXXXXX";
    assert(mkdtemp(dir));
    char path[256], temp[300], text[256];
    snprintf(path, sizeof path, "%s/run.txt", dir);
    snprintf(temp, sizeof temp, "%s.tmp", path);
    write_text(path, "old-run\n");
    struct pd_nuz_run run = {0};
    renameFails = true;
    assert(!pd_nuz_save_atomic(path, true, &run, test_rename));
    read_text(path, text, sizeof text);
    assert(!strcmp(text, "old-run\n"));
    read_text(temp, text, sizeof text);
    assert(!strncmp(text, "on\t1\n", 4)); // Both the old run and the new temp survived the failed rename.
    renameFails = false;
    assert(pd_nuz_save_atomic(path, true, &run, test_rename));
    read_text(path, text, sizeof text);
    assert(!strncmp(text, "on\t1\n", 4));
    assert(access(temp, F_OK) != 0);
    remove(path);
    remove(temp);
    rmdir(dir);
}

static void scan_rom(const char* path, int kind) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("SKIP retail ROM %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    uint8_t* rom = malloc((size_t)size);
    assert(rom && fread(rom, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    struct pd_center_found found;
    assert(pd_center_find(rom, (size_t)size, &found));
    assert(found.specials && found.heal == 0 && found.forget >= 0 && found.var8004);
    if (kind == 0) {
        assert(found.specials == 0x0815FDD8u && found.nickname == 158 && found.forget == 221);
        assert(found.hasRelearn && found.relearn.index == 224 && found.relearn.init == 0x080E47A1u);
        assert(found.relearn.fieldCallback == 0x03005020u && found.relearn.continueScript == 0x0807DD39u);
        assert(found.var8004 == 0x020370C0u && found.varResult == 0x020370D0u);
        assert(found.otIdNotPlayers == 0x080CC1B4u && found.windowFrame == 0x080F6F14u);
        assert(found.healSong == 256 && found.forgetSong == 270);
    } else if (kind == 1) {
        assert(found.nickname == 161 && found.forget == 224);
        assert(found.hasRelearn && found.relearn.index == 227 && found.relearn.init == 0x081606A1u);
        assert(found.relearn.fieldCallback == 0x03005DACu && found.relearn.continueScript == 0x080AF169u);
        assert(found.var8004 == 0x020375E0u && !found.windowFrame && found.healSong == 368 && found.forgetSong == 378);
    } else {
        assert(found.nickname == -1);
    }
    if (kind < 2) {
        struct pd_rom_write tms[PD_TM_SITES_MAX], text[PD_TEXT_WRITES_MAX];
        int tmCount = pd_reusable_tm_find(rom, (size_t)size, tms);
        assert(tmCount == (kind == 0 ? 3 : 1));
        assert(pd_instant_text_find(rom, (size_t)size, text) > 0);
        printf("PASS gameplay option scan: %d TM site(s), instant text\n", tmCount);
    }
    printf("PASS Center scan %s\n", path);
    free(rom);
}

int main(int argc, char** argv) {
    test_party_swap();
    test_center_actions();
    test_battle_actions();
    test_delayed_evolution();
    test_queued_center_scripts();
    test_nuz_atomic_replace();
    uint8_t empty[0x10000] = {0};
    struct pd_center_found none;
    assert(!pd_center_find(empty, sizeof empty, &none));
    if (argc > 1) scan_rom(argv[1], 0);
    if (argc > 2) scan_rom(argv[2], 1);
    if (argc > 3) scan_rom(argv[3], 2);
    puts("core gameplay tests passed");
    return 0;
}
