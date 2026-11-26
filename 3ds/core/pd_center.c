#include "pd_center.h"

#include <string.h>

#define ROM_BASE 0x08000000u
#define ANY -1
#define ARRAY_LEN(a) ((int) (sizeof(a) / sizeof(*(a))))
#define MON_PERSONALITY 0
#define MON_MOVE1 13
#define MON_IS_EGG 45
#define MOVE_SURF 57
#define CENTER_SCRIPT 0x0203FFD8u

static const int HEAL[] = {
    0xf0,0xb5,0x57,0x46,0x4e,0x46,0x45,0x46,0xe0,0xb4,0x81,0xb0,0x00,0x20,0x80,0x46,0x2a,0x48,0x00,0x78,
    0x80,0x45,0x49,0xd2,0x29,0x49,0x8a,0x46,0x6e,0x46,0x64,0x20,0x44,0x46,0x44,0x43,0x54,0x44,0x20,0x1c,
};
static const int NICKNAME[] = {
    0x70,0xb5,0x56,0x46,0x4d,0x46,0x44,0x46,0x70,0xb4,0x82,0xb0,0x26,0x48,0x81,0x46,0x00,0x88,0x64,0x21,
    0x88,0x46,0x41,0x46,0x41,0x43,0x08,0x1c,0x23,0x4e,0x80,0x19,0x23,0x4a,0x02,0x21,ANY,ANY,ANY,ANY,
    0x49,0x46,0x08,0x88,0x41,0x46,0x41,0x43,0x08,0x1c,0x80,0x19,0x1f,0x49,0x8a,0x46,0x02,0x21,0x52,0x46,
    ANY,ANY,ANY,ANY,
};
static const int FORGET[] = {
    0x70,0xb5,0x46,0x46,0x40,0xb4,0x16,0x48,0x80,0x46,0x00,0x88,0x64,0x26,0x70,0x43,0x14,0x4d,0x40,0x19,
    0x14,0x4c,0x22,0x78,0x00,0x21,ANY,ANY,ANY,ANY,0x41,0x46,0x08,0x88,0x70,0x43,0x40,0x19,0x21,0x78,
    ANY,ANY,ANY,ANY,0x24,0x88,0x02,0x2c,0x10,0xd8,0x0b,0x48,0x01,0x88,0x64,0x20,
};
static const int RELEARN[] = {
    0x00,0xb5,0x81,0xb0,ANY,ANY,ANY,ANY,0x07,0x48,0x0a,0x21,ANY,ANY,ANY,ANY,0x01,0x20,0x40,0x42,
    0x00,0x21,0x00,0x91,0x00,0x22,0x10,0x23,ANY,ANY,ANY,ANY,
};
static const int RELEARN_TASK[] = {
    0x10,0xb5,0x00,0x06,0x04,0x0e,0x09,0x48,0xc1,0x79,0x80,0x20,0x08,0x40,0x00,0x28,0x08,0xd1,0x07,0x48,
    ANY,ANY,ANY,ANY,0x06,0x49,0x07,0x48,0x08,0x60,0x20,0x1c,ANY,ANY,ANY,ANY,0x10,0xbc,0x01,0xbc,
};
static const int WINDOW_FRAME[] = {
    0x00,0xb5,0x07,0x48,0x00,0x78,0x02,0x28,0x0e,0xd1,0x06,0x4a,0x10,0x78,0x04,0x21,0x08,0x43,0x10,0x70,
    0x80,0x21,0x89,0x00,0x00,0x20,ANY,ANY,ANY,ANY,0x0b,0xe0,ANY,ANY,ANY,ANY,ANY,ANY,ANY,ANY,
    ANY,ANY,ANY,ANY,0x80,0x21,0x89,0x00,0x00,0x20,0xf0,0x22,ANY,ANY,ANY,ANY,0x85,0x21,0x89,0x00,0x00,0x20,
    0xe0,0x22,ANY,ANY,ANY,ANY,
};
static const int OT_ID[] = {
    0x10,0xb5,ANY,ANY,ANY,ANY,0x04,0x1c,0x07,0x48,0x01,0x88,0x64,0x20,0x48,0x43,0x06,0x49,0x40,0x18,
    0x01,0x21,0x00,0x22,ANY,ANY,ANY,ANY,0x84,0x42,0x09,0xd1,0x03,0x49,0x00,0x20,
};

static uint16_t u16(const uint8_t* p) {
    return (uint16_t) (p[0] | p[1] << 8);
}

static uint32_t u32(const uint8_t* p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static bool word_at(const uint8_t* rom, size_t size, size_t at, uint32_t* out) {
    if (at > size || size - at < 4) return false;
    *out = u32(rom + at);
    return true;
}

static bool thumb_rom(uint32_t word, size_t size) {
    return (word & 1) && word >= ROM_BASE + 1 && word - ROM_BASE < size;
}

static long fn_offset(uint32_t word, size_t size) {
    if (!thumb_rom(word, size)) return -1;
    return (long) ((word & ~1u) - ROM_BASE);
}

static bool matches(const uint8_t* rom, size_t size, long at, const int* sig, int count) {
    if (at < 0 || (size_t) at > size || size - (size_t) at < (size_t) count) return false;
    for (int i = 0; i < count; i++)
        if (sig[i] != ANY && rom[at + i] != sig[i]) return false;
    return true;
}

static int only(const uint8_t* rom, size_t size, size_t table, int entries, const int* sig, int count) {
    int found = -1;
    for (int i = 0; i < entries; i++) {
        uint32_t word;
        if (!word_at(rom, size, table + (size_t) i * 4, &word)) return -1;
        if (!matches(rom, size, fn_offset(word, size), sig, count)) continue;
        if (found >= 0) return -1;
        found = i;
    }
    return found;
}

static uint32_t literal(const uint8_t* rom, size_t size, long at) {
    uint16_t ins;
    uint32_t word;
    if (at < 0 || (size_t) at > size || size - (size_t) at < 2) return 0;
    ins = u16(rom + at);
    size_t literalAt = (((size_t) at + 4) & ~(size_t) 3) + ((size_t) (ins & 0xFF) << 2);
    return word_at(rom, size, literalAt, &word) ? word : 0;
}

static long scan_one(const uint8_t* rom, size_t size, const int* sig, int count) {
    long found = -1;
    if (size < 2) return -1;
    for (size_t at = 0; at + (size_t) count <= size; at += 2) {
        if (rom[at] != sig[0] || rom[at + 1] != sig[1] || !matches(rom, size, (long) at, sig, count)) continue;
        if (found >= 0) return -1;
        found = (long) at;
    }
    return found;
}

bool pd_center_find(const uint8_t* rom, size_t size, struct pd_center_found* out) {
    memset(out, 0, sizeof(*out));
    out->nickname = out->forget = out->relearnSpecial = -1;
    if (!rom || size < 0x1000) return false;
    size_t table = 0;
    int tableCount = 0, tables = 0;
    for (size_t at = 0; at + 4 <= size; at += 4) {
        uint32_t word = u32(rom + at);
        if (!matches(rom, size, fn_offset(word, size), HEAL, ARRAY_LEN(HEAL))) continue;
        int count = 0;
        while (count < 0x300 && at + (size_t) count * 4 + 4 <= size) {
            uint32_t entry = 0;
            if (!word_at(rom, size, at + (size_t) count * 4, &entry) || !thumb_rom(entry, size)) break;
            count++;
        }
        if (count >= 200) {
            table = at;
            tableCount = count;
            tables++;
        }
    }
    if (tables != 1) return false;
    int forget = only(rom, size, table, tableCount, FORGET, ARRAY_LEN(FORGET));
    if (forget < 0) return false;
    uint32_t word;
    if (!word_at(rom, size, table + (size_t) forget * 4, &word)) return false;
    long forgetFn = fn_offset(word, size);
    uint32_t var8004 = literal(rom, size, forgetFn + 0x06);
    if (literal(rom, size, forgetFn + 0x14) != var8004 + 2) return false;

    int nickname = only(rom, size, table, tableCount, NICKNAME, ARRAY_LEN(NICKNAME));
    if (nickname >= 0 && word_at(rom, size, table + (size_t) nickname * 4, &word) &&
        literal(rom, size, fn_offset(word, size) + 0x0C) == var8004)
        out->nickname = nickname;
    int otIndex = only(rom, size, table, tableCount, OT_ID, ARRAY_LEN(OT_ID));
    long otFn = -1;
    if (otIndex >= 0 && word_at(rom, size, table + (size_t) otIndex * 4, &word)) {
        otFn = fn_offset(word, size);
        if (literal(rom, size, otFn + 0x08) == var8004)
            out->otIdNotPlayers = ROM_BASE + (uint32_t) otFn;
        else
            otFn = -1;
    }
    out->varResult = otFn >= 0 ? literal(rom, size, otFn + 0x20) : var8004 + 0x10;
    out->var8004 = var8004;
    out->forget = forget;

    struct pd_center_relearn relearn = {0};
    bool hasRelearn = false;
    int relearnIndex = forget + 3;
    if (relearnIndex < tableCount && word_at(rom, size, table + (size_t) relearnIndex * 4, &word)) {
        long relearnFn = fn_offset(word, size);
        if (matches(rom, size, relearnFn, RELEARN, ARRAY_LEN(RELEARN))) {
            uint32_t task = literal(rom, size, relearnFn + 0x08);
            long taskFn = fn_offset(task, size);
            if (matches(rom, size, taskFn, RELEARN_TASK, ARRAY_LEN(RELEARN_TASK))) {
                relearn.init = literal(rom, size, taskFn + 0x12);
                relearn.fieldCallback = literal(rom, size, taskFn + 0x18);
                relearn.continueScript = literal(rom, size, taskFn + 0x1A);
                relearn.index = relearnIndex;
                hasRelearn = thumb_rom(relearn.init, size) && thumb_rom(relearn.continueScript, size) &&
                             relearn.fieldCallback >= 0x03000000u && relearn.fieldCallback <= 0x03007FFFu;
            }
        }
    }
    const char* code = (const char*) rom + 0xAC;
    bool fireRed = !strncmp(code, "BPR", 3) || !strncmp(code, "BPG", 3) || !strncmp(code, "PUV", 3);
    bool rubySapphire = !strncmp(code, "AXV", 3) || !strncmp(code, "AXP", 3);
    if (fireRed) {
        long window = scan_one(rom, size, WINDOW_FRAME, ARRAY_LEN(WINDOW_FRAME));
        if (window >= 0) out->windowFrame = ROM_BASE + (uint32_t) window;
        hasRelearn &= out->windowFrame != 0;
    }
    if (rubySapphire)
        out->relearnSpecial = hasRelearn ? relearnIndex : only(rom, size, table, tableCount, RELEARN, ARRAY_LEN(RELEARN));
    out->relearn = relearn;
    out->hasRelearn = hasRelearn;
    out->specials = ROM_BASE + (uint32_t) table;
    out->heal = 0;
    out->healSong = fireRed ? 256 : 368;
    out->forgetSong = fireRed ? 270 : 378;
    return true;
}

static bool call(const struct pd_center_ops* ops, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                 uint32_t* r0) {
    const uint32_t args[4] = {a0, a1, a2, a3};
    return ops->call(ops->ctx, fn, args, r0);
}

static enum pd_center_result gate(const struct pd_center_ops* ops, const struct pd_config* cfg, bool busy, bool battle) {
    if (!ops || !ops->call || !ops->read || !ops->write || !ops->fieldSupported || !ops->pcSupported ||
        !cfg || !ops->fieldSupported(ops->ctx) || !ops->pcSupported(ops->ctx))
        return PD_CENTER_FAILED;
    if (battle) return PD_CENTER_IN_BATTLE;
    if (busy) return PD_CENTER_NOT_NOW;
    uint32_t locked = 1;
    if (!call(ops, cfg->fieldLocked, 0, 0, 0, 0, &locked)) return PD_CENTER_FAILED;
    return locked ? PD_CENTER_NOT_NOW : PD_CENTER_OK;
}

static enum pd_center_result center_mon(const struct pd_center_ops* ops, const struct pd_config* cfg, int slot,
                                        uint32_t personality, uint32_t* mon) {
    const struct pd_party_fns* p = cfg->partyFns;
    if (!personality) return PD_CENTER_PARTY_CHANGED;
    if (!p || !p->getMonData || !p->party || !p->partyCount) return PD_CENTER_FAILED;
    uint8_t count = 0;
    if (!ops->read(ops->ctx, p->partyCount, &count, 1)) return PD_CENTER_FAILED;
    if (count > PD_PARTY_SIZE) count = PD_PARTY_SIZE;
    if (slot < 0 || slot >= count) return PD_CENTER_PARTY_CHANGED;
    *mon = p->party + (uint32_t) (slot * 100);
    uint32_t got = 0, egg = 1;
    if (!call(ops, p->getMonData, *mon, MON_PERSONALITY, 0, 0, &got)) return PD_CENTER_FAILED;
    if (got != personality) return PD_CENTER_PARTY_CHANGED;
    if (!call(ops, p->getMonData, *mon, MON_IS_EGG + p->monFieldShift, 0, 0, &egg)) return PD_CENTER_FAILED;
    return egg ? PD_CENTER_EGG : PD_CENTER_OK;
}

static enum pd_center_result run_script(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                        const uint8_t* script, size_t len) {
    if (!ops->write(ops->ctx, CENTER_SCRIPT, script, len)) return PD_CENTER_FAILED;
    return call(ops, cfg->pcSetupScript, CENTER_SCRIPT, 0, 0, 0, NULL) ? PD_CENTER_OK : PD_CENTER_FAILED;
}

static void put16(uint8_t* out, int value) {
    out[0] = (uint8_t) value;
    out[1] = (uint8_t) (value >> 8);
}

static void put32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; i++) out[i] = (uint8_t) (value >> (8 * i));
}

enum pd_center_result pd_center_heal(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                     const struct pd_center_found* found, bool busy, bool battle) {
    enum pd_center_result r = gate(ops, cfg, busy, battle);
    if (r != PD_CENTER_OK) return r;
    uint8_t script[11] = {0x69, 0x25, 0, 0, 0x31, 0, 0, 0x32, 0x6B, 0x02, 0};
    put16(script + 2, found->heal);
    put16(script + 5, found->healSong);
    return run_script(ops, cfg, script, 10);
}

enum pd_center_result pd_center_nickname(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                         const struct pd_center_found* found, int slot, uint32_t personality,
                                         bool busy, bool battle) {
    if (found->nickname < 0) return PD_CENTER_FAILED;
    enum pd_center_result r = gate(ops, cfg, busy, battle);
    if (r != PD_CENTER_OK) return r;
    uint32_t mon;
    r = center_mon(ops, cfg, slot, personality, &mon);
    if (r != PD_CENTER_OK) return r;
    if (found->otIdNotPlayers) {
        uint8_t value[2];
        put16(value, slot);
        if (!ops->write(ops->ctx, found->var8004, value, 2) || !call(ops, found->otIdNotPlayers, 0, 0, 0, 0, NULL))
            return PD_CENTER_FAILED;
        uint8_t traded = 0;
        if (!ops->read(ops->ctx, found->varResult, &traded, 1)) return PD_CENTER_FAILED;
        if (traded) return PD_CENTER_TRADED;
    }
    uint8_t script[18] = {0x69, 0x16, 0x04, 0x80, 0, 0, 0x97, 0x01, 0x25, 0, 0, 0x27, 0x6B, 0x02, 0, 0, 0, 0};
    put16(script + 4, slot);
    put16(script + 9, found->nickname);
    return run_script(ops, cfg, script, 14);
}

enum pd_center_result pd_center_forget(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                       const struct pd_center_found* found, int slot, uint32_t personality,
                                       int moveIndex, int move, bool hmFree, bool busy, bool battle) {
    if (found->forget < 0 || !cfg || !cfg->partyFns) return PD_CENTER_FAILED;
    enum pd_center_result r = gate(ops, cfg, busy, battle);
    if (r != PD_CENTER_OK) return r;
    uint32_t mon;
    r = center_mon(ops, cfg, slot, personality, &mon);
    if (r != PD_CENTER_OK) return r;
    const struct pd_party_fns* p = cfg->partyFns;
    uint32_t moves[PD_NUM_MOVES] = {0};
    int known = 0;
    for (int i = 0; i < PD_NUM_MOVES; i++) {
        if (!call(ops, p->getMonData, mon, MON_MOVE1 + p->monFieldShift + i, 0, 0, &moves[i])) return PD_CENTER_FAILED;
        known += moves[i] != 0;
    }
    if (moveIndex < 0 || moveIndex >= PD_NUM_MOVES || !move || moves[moveIndex] != (uint32_t) move)
        return PD_CENTER_PARTY_CHANGED;
    if (known <= 1) return PD_CENTER_LAST_MOVE;
    if (move == MOVE_SURF && !hmFree) {
        uint8_t count = 0;
        if (!ops->read(ops->ctx, p->partyCount, &count, 1)) return PD_CENTER_FAILED;
        if (count > PD_PARTY_SIZE) count = PD_PARTY_SIZE;
        bool another = false;
        for (int other = 0; other < count && !another; other++) {
            if (other == slot) continue;
            uint32_t at = p->party + (uint32_t) (other * 100), egg = 1;
            if (!call(ops, p->getMonData, at, MON_IS_EGG + p->monFieldShift, 0, 0, &egg)) return PD_CENTER_FAILED;
            if (egg) continue;
            for (int i = 0; i < PD_NUM_MOVES && !another; i++) {
                uint32_t knownMove = 0;
                if (!call(ops, p->getMonData, at, MON_MOVE1 + p->monFieldShift + i, 0, 0, &knownMove))
                    return PD_CENTER_FAILED;
                another = knownMove == MOVE_SURF;
            }
        }
        if (!another) return PD_CENTER_LAST_SURFER;
    }
    uint8_t script[21] = {0x69, 0x16, 0x04, 0x80, 0, 0, 0x16, 0x05, 0x80, 0, 0, 0x25, 0, 0, 0x31, 0, 0, 0x32, 0x6B, 0x02, 0};
    put16(script + 4, slot);
    put16(script + 9, moveIndex);
    put16(script + 12, found->forget);
    put16(script + 15, found->forgetSong);
    return run_script(ops, cfg, script, 20);
}

enum pd_center_result pd_center_relearn_script(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                               const struct pd_center_found* found, int slot, uint32_t personality,
                                               bool busy, bool battle) {
    if (found->relearnSpecial < 0) return PD_CENTER_FAILED;
    enum pd_center_result r = gate(ops, cfg, busy, battle);
    if (r != PD_CENTER_OK) return r;
    uint32_t mon;
    r = center_mon(ops, cfg, slot, personality, &mon);
    if (r != PD_CENTER_OK) return r;
    uint8_t script[15] = {0x69, 0x16, 0x04, 0x80, 0, 0, 0x25, 0, 0, 0x27, 0x6B, 0x02, 0, 0, 0};
    put16(script + 4, slot);
    put16(script + 7, found->relearnSpecial);
    return run_script(ops, cfg, script, 12);
}

enum pd_center_result pd_center_relearn_start(const struct pd_center_ops* ops, const struct pd_config* cfg,
                                              const struct pd_center_found* found, int slot, uint32_t personality,
                                              bool busy, bool battle) {
    if (!found->hasRelearn || !ops || !ops->tmSupported || !ops->tmSupported(ops->ctx)) return PD_CENTER_FAILED;
    enum pd_center_result r = gate(ops, cfg, busy, battle);
    if (r != PD_CENTER_OK) return r;
    uint32_t mon;
    r = center_mon(ops, cfg, slot, personality, &mon);
    if (r != PD_CENTER_OK) return r;
    if (!call(ops, cfg->tmLock, 0, 0, 0, 0, NULL) || !call(ops, cfg->tmFade, 1, 0, 0, 0, NULL))
        return PD_CENTER_FAILED;
    return PD_CENTER_OK;
}

bool pd_center_relearn_finish(const struct pd_center_ops* ops, const struct pd_config* cfg,
                              const struct pd_center_found* found, int slot, uint32_t personality, bool leftField) {
    if (leftField || !found->hasRelearn) return false;
    uint32_t mon;
    bool ok = center_mon(ops, cfg, slot, personality, &mon) == PD_CENTER_OK;
    uint8_t value[4];
    if (ok) {
        put16(value, slot);
        ok = ops->write(ops->ctx, found->var8004, value, 2);
    }
    if (ok) {
        static const uint8_t release[] = {0x6B, 0x02};
        ok = run_script(ops, cfg, release, sizeof release) == PD_CENTER_OK;
    }
    if (ok && found->windowFrame) ok = call(ops, found->windowFrame, 0, 0, 0, 0, NULL);
    if (ok) ok = call(ops, cfg->tmCleanup, 0, 0, 0, 0, NULL);
    if (ok) {
        put32(value, found->relearn.continueScript);
        ok = ops->write(ops->ctx, found->relearn.fieldCallback, value, 4) &&
             call(ops, cfg->tmSetMainCallback2, found->relearn.init, 0, 0, 0, NULL);
    }
    if (!ok) {
        call(ops, cfg->tmFade, 0, 0, 0, 0, NULL);
        call(ops, cfg->tmUnlock, 0, 0, 0, 0, NULL);
    }
    return ok;
}

const char* pd_center_result_text(enum pd_center_result result) {
    switch (result) {
    case PD_CENTER_OK: return NULL;
    case PD_CENTER_NOT_NOW: return "Not now: close the game's menus and battles first.";
    case PD_CENTER_IN_BATTLE: return "You can't do that during a battle.";
    case PD_CENTER_EGG: return "Not for an EGG.";
    case PD_CENTER_TRADED: return "Only its original trainer can rename it.";
    case PD_CENTER_LAST_MOVE: return "It knows only one move.";
    case PD_CENTER_LAST_SURFER: return "It's the only party member that knows SURF.";
    case PD_CENTER_PARTY_CHANGED: return "The party has changed. Try again.";
    default: return "It can't be used here.";
    }
}
