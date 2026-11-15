// Map changes on request, for the performance runs and their videos (OoT 1.1.1, main build e0f3ccfa).
//
// REKARINA_WARP_AT=<seconds>:<place>[;<seconds>:<place>...] sends the hero to each place when the game has run that many
// seconds (counted from the first pad read). Places: forest, field, market, castle, village, or one spelled out as
// <map>/<start position>/<arrival event> with the game's own names (<map>_Child/<a start of that map>/
// Dm_SY_BancChangeArrive; the arrival event may be left out). A request the game is not ready for (an event playing,
// the event manager not there yet) is asked again each second.
//
// The game changes maps through an event: an entrance asks the event manager for "Dm_SY_BancChangeGo" with the next
// map (a "banc"), the start position in it and the event that plays on arrival, and the sequence loads the map when
// that event ends. This file makes the same request: it builds the game's event request argument on the calling
// thread's stack with the game's own constructor, fills in the event's name and its four arguments, hands it to the
// event manager, and releases the argument's two actor links as the game's callers do.
//
// How the game does this (the event, its arguments, the layout of the request argument, the singletons and the
// functions involved, and the names of the maps and their start positions) was worked out by the Hookshot project
// (github.com/NotRexed/Hookshot, host/hos_main.c "Cross-map teleport", GPL-3.0). Nothing of its code is here: it was
// read for what the game does, and the addresses below were placed in our layout by hand (Hookshot loads main
// 0x20000 higher: its f_80d99f10 is main+0xd79f10 here, checked on the callers that reserve the argument's 0x1b20
// bytes of stack).
//
// Called from the pad read (ov_input.c npad_states), which the game makes on its main thread once or more a frame.
#ifdef GAME_OOT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cpu.h"
#include "host.h"
#include "platform/platform.h"

void *rt_patch_page(void);

enum {
    EVENT_MGR_SLOT = 0x4c7e090,   // main+: -> holder -> the event manager
    GAME_DATA_SLOT = 0x4c7e180,   // main+: -> holder -> the game data manager
    FN_GAME_DATA_BOOL = 0x20d9c0, // (manager, bool *out, name hash) -> found
    FN_ARG_MAKE = 0xd79f10,       // (arg): the request argument's constructor
    FN_EVENT_REQUEST = 0xd7a820,  // (event manager, arg) -> nonzero when taken
    FN_LINK_RELEASE = 0x20b6f0,   // (link): an actor link's release
    LINK_VTABLE = 0x4974b20,      // main+: what the game's callers store in a link before releasing it
    ARG_BYTES = 0x1b20,           // the request argument
    ARG_EVENT_NAME = 6800,        // arg+: -> the name's buffer; +8 its capacity
    ARG_LINK_A = 0x1968, ARG_LINK_B = 0x1950,
    ENTRY_BYTES = 200,            // one argument: +0 type (1 string pointer, 4 bool), +8 -> name buffer, +16 its
                                  // capacity, +152 the value; the array at arg+8, its capacity at arg+0, count arg+16
    EVENT_PLAYING = 104,          // event manager+: nonzero while an event plays
    STRINGS_AT = 0x800,           // the patch page from here: the three strings the event reads after the request
};
#define HASH_IS_CHILD 0xcea220d3u     // "Sequence_IsChildhood"
#define HASH_IS_LOADING 0xef2b60c2u   // "Sequence_IsLoadingBanc"

// The game's own names of the maps, start positions and arrival events are kept here with every letter moved 13 places
// in the alphabet, so the sources name no place of the game; places_init turns them back once.
typedef struct { const char *name, *banc[2], *start, *boot; } Place;   // banc: adult, child
static Place places[] = {
    {"forest", {"XbxvevSberfg_Nqhyg", "XbxvevSberfg_Puvyq"}, "YbfgJbbqOevqtr_SebagBsUlehyrSvryq", "Qz_FL_OnapPunatrNeevir"},
    // outside the Market's drawbridge, the open field ahead (the start in front of the woods bridge is in a gully)
    {"field", {"UlehyrSvryq_Nqhyg", "UlehyrSvryq_Puvyq"}, "UlehyrSvryq_SebagBsZnexrg", "Qz_FL_OnapPunatrNeevir"},
    {"market", {"Znexrg_Nqhyg", "Znexrg_Puvyq"}, "Znexrg_Ragenapr", "Qz_FL_OnapPunatrNeevir_Znexrg_02"},
    {"castle", {"UlehyrPnfgyr", "UlehyrPnfgyr"}, "UlehyrPnfgyr_Ragenapr", "Qz_FL_OnapPunatrNeevir_UlehyrPnfgyr_01"},
    {"village", {"XnxnevxbIvyyntr_Nqhyg", "XnxnevxbIvyyntr_Puvyq"}, "XnxnevxbIvyyntr_Ragenapr",
     "Qz_FL_OnapPunatrNeevir_XnxnevxbIvyyntr_01"},
};

static const char *letters_back(const char *t) {
    char *d = strdup(t);
    if (!d) rt_fatal("warp: out of memory");
    for (char *q = d; *q; q++) {
        if ((*q >= 'a' && *q <= 'm') || (*q >= 'A' && *q <= 'M')) *q += 13;
        else if ((*q >= 'n' && *q <= 'z') || (*q >= 'N' && *q <= 'Z')) *q -= 13;
    }
    return d;
}

static void places_init(void) {
    static int done;
    if (done) return;
    done = 1;
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++) {
        Place *pl = &places[i];
        pl->banc[0] = letters_back(pl->banc[0]), pl->banc[1] = letters_back(pl->banc[1]);
        pl->start = letters_back(pl->start), pl->boot = letters_back(pl->boot);
    }
}

static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static void wr64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static void wr32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }

static uint64_t singleton(uint64_t base, uint64_t slot) {
    uint64_t holder = rd64(base + slot);
    return holder ? rd64(holder) : 0;
}

// A game data flag by its name's hash: 1 found (and *value), 0 not there. Uses 0x40 bytes under the stack pointer.
static int game_flag(Cpu *c, uint64_t base, uint32_t hash, int *value) {
    uint64_t mgr = singleton(base, GAME_DATA_SLOT);
    if (!mgr) return 0;
    uint64_t sp = c->sp, out = (sp - 0x40) & ~0xfull;
    wr64(out, 0);
    c->sp = out;
    int found = (int)(rt_call(c, base + FN_GAME_DATA_BOOL, 3, mgr, out, (uint64_t)hash) & 1);
    *value = *(uint8_t *)(uintptr_t)out & 1;
    c->sp = sp;
    return found;
}

static int add_argument(uint64_t arg, const char *name, uint32_t type, uint64_t value) {
    uint32_t capacity = rd32(arg), count = rd32(arg + 16);
    if (count >= capacity) return 0;
    uint64_t entry = rd64(arg + 8) + (uint64_t)count * ENTRY_BYTES, buf = rd64(entry + 8);
    size_t n = strlen(name) + 1;
    if (!buf || n > rd32(entry + 16)) return 0;
    memcpy((void *)(uintptr_t)buf, name, n);
    if (type == 4) *(uint8_t *)(uintptr_t)(entry + 152) = (uint8_t)(value & 1);
    else wr64(entry + 152, value);
    wr32(entry, type);
    wr32(arg + 16, count + 1);
    return 1;
}

// 1 sent and taken, 0 the game is not ready (ask again), -1 it cannot be done.
static int warp(Cpu *c, const Place *p) {
    places_init();
    const RtLoaded *m = ldr_module(0);
    uint8_t *page = rt_patch_page();
    if (!m || !page) return -1;
    uint64_t base = m->base, events = singleton(base, EVENT_MGR_SLOT);
    if (!events || rd64(events + EVENT_PLAYING)) return 0;
    int loading = 0, child = 1;
    if (game_flag(c, base, HASH_IS_LOADING, &loading) && loading) return 0;
    game_flag(c, base, HASH_IS_CHILD, &child);

    char *strings = (char *)page + STRINGS_AT;   // read by the event after this returns: memory that stays
    memset(strings, 0, 384);
    snprintf(strings, 128, "%s", p->banc[child]);
    snprintf(strings + 128, 128, "%s", p->start);
    snprintf(strings + 256, 128, "%s", p->boot);

    Cpu saved = *c;
    uint64_t arg = (c->sp - ARG_BYTES - 0x100) & ~0xfull;
    c->sp = arg;
    rt_call(c, base + FN_ARG_MAKE, 1, arg);
    static const char event[] = "Dm_SY_BancChangeGo";
    uint64_t name = rd64(arg + ARG_EVENT_NAME);
    int ok = name && sizeof event <= rd32(arg + ARG_EVENT_NAME + 8);
    if (ok) memcpy((void *)(uintptr_t)name, event, sizeof event);
    uint64_t at = (uint64_t)(uintptr_t)strings;
    ok = ok && add_argument(arg, "NextBanc", 1, at) && add_argument(arg, "NextStartPos", 1, at + 128) &&
         add_argument(arg, "BootEventName", 1, at + 256) && add_argument(arg, "IsCheckAir", 4, 0);
    uint64_t taken = 0;
    if (ok) {
        c->sp = arg;
        taken = rt_call(c, base + FN_EVENT_REQUEST, 2, events, arg);
    }
    static const uint32_t links[] = {ARG_LINK_A, ARG_LINK_B};
    for (int i = 0; i < 2; i++) {
        wr64(arg + links[i], base + LINK_VTABLE);
        c->sp = arg;
        rt_call(c, base + FN_LINK_RELEASE, 1, arg + links[i]);
    }
    *c = saved;
    rt_log("warp: %s (%s at %s, arrival %s): %s, the event manager answered %#llx", p->name, p->banc[child], p->start,
           p->boot, ok ? "asked" : "the request could not be built", (unsigned long long)taken);
    return ok ? (taken ? 1 : 0) : -1;
}

void oot_warp_tick(Cpu *c) {
    enum { MAX = 8 };
    static struct { double at; const Place *place; } plan[MAX];
    static Place spelled[MAX];        // places given as <map>/<start position>/<arrival event>
    static char spelled_text[256];    // their names: the request's text, cut at the slashes
    static int n = -1, next;
    static uint64_t t0, retry_at, tries;
    if (n < 0) {   // the plan, once
        n = 0;
        const char *e = cfg_get("REKARINA_WARP_AT");
        char *buf = spelled_text;
        snprintf(buf, sizeof spelled_text, "%s", e ? e : "");
        for (char *save = NULL, *tok = strtok_r(buf, ";", &save); tok && n < MAX; tok = strtok_r(NULL, ";", &save)) {
            char *colon = strchr(tok, ':');
            if (!colon) continue;
            *colon = 0;
            char *start = strchr(colon + 1, '/');
            if (start) {
                *start++ = 0;
                char *boot = strchr(start, '/');
                if (boot) *boot++ = 0;
                if (!boot || !*boot) boot = "Dm_SY_BancChangeArrive";
                spelled[n] = (Place){colon + 1, {colon + 1, colon + 1}, start, boot};
                plan[n].at = atof(tok), plan[n].place = &spelled[n], n++;
                continue;
            }
            for (size_t i = 0; i < sizeof places / sizeof *places; i++)
                if (!strcmp(colon + 1, places[i].name)) plan[n].at = atof(tok), plan[n++].place = &places[i];
        }
        if (e && *e) rt_log("warp: %d map changes planned (REKARINA_WARP_AT=%s)", n, e);
    }
    if (next >= n) return;
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now;
    if ((double)(now - t0) / 1e9 < plan[next].at || now < retry_at) return;
    int r = warp(c, plan[next].place);
    if (r == 0 && ++tries < 120) {   // not ready: again in a second, for two minutes
        retry_at = now + 1000000000ull;
        return;
    }
    if (r <= 0) rt_log("warp: %s given up", plan[next].place->name);
    next++, tries = 0, retry_at = now + 8000000000ull;   // the game needs a few seconds between two map changes
}
#endif
