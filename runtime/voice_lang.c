// The voice language apart from the text language (OoT): REKARINA_VOICE_LANG (game, en, ja, de, fr, it, es, es-419,
// pt-BR, fr-CA; read at start).
//
// The game speaks in the language of its text: its voice files carry a language code in their names, Voice/Resource/
// <code>_EventFlowMsg-*.bars.zs with Voice/Resource/Stream/<code>_EventFlowMsg-*.bwav, and the companion's lines
// Sound/Resource/NV_<code>_*.bars.zs (code USen, JPja, EUde, EUfr, EUit, EUes, USes, USpt; USfr holds a few). With the
// setting, romfs.c (romfs_open, romfs_entry_type) serves the chosen language's file in place of a voice file the game
// asks for, when that language has one of the same name; text, menus and everything else stay in the game's language.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"

static const struct { const char *setting, *code; } VOICES[] = {
    {"en", "USen"}, {"ja", "JPja"}, {"de", "EUde"}, {"fr", "EUfr"}, {"it", "EUit"}, {"es", "EUes"},
    {"es-419", "USes"}, {"pt-BR", "USpt"}, {"fr-CA", "USfr"},
};
// Every language code a file name may carry (the game's region and language codes).
static const char *const CODES[] = {"USen", "EUen", "JPja", "EUde", "EUfr", "EUit", "EUes", "USes", "USpt", "USfr",
                                    "EUnl", "EUru", "EUpt", "KRko", "CNzh", "TWzh", "EUpl"};

static const char *voice;   // the chosen language's code, NULL to follow the game's (voice_lang_init)

void voice_lang_init(void) {
    const char *v = cfg_value("REKARINA_VOICE_LANG");
    for (size_t i = 0; v && i < sizeof VOICES / sizeof *VOICES; i++)
        if (!strcmp(v, VOICES[i].setting)) voice = VOICES[i].code;
    if (voice) rt_log("voice: the game speaks %s (%s files) whatever its text language (voice_lang)", v, voice);
    else if (v && *v && strcmp(v, "game")) rt_log("voice: voice_lang = %s is not one of the voices; the game's language speaks", v);
}

// p with its language code changed to the voice language's when p is a voice file (under Voice/ or Sound/, the code
// starting its leaf name or following "NV_", then '_'), into buf; NULL when the setting is off or p is not one. The
// caller serves the result only when the RomFS has it.
const char *voice_lang_path(const char *p, char *buf, size_t n) {
    const char *want = voice;
    if (!want || (strncmp(p, "Voice/", 6) && strncmp(p, "Sound/", 6))) return NULL;
    const char *leaf = strrchr(p, '/');
    leaf = leaf ? leaf + 1 : p;
    const char *at = !strncmp(leaf, "NV_", 3) ? leaf + 3 : leaf;
    if (strlen(at) < 5 || at[4] != '_' || !strncmp(at, want, 4)) return NULL;
    size_t k = 0;
    while (k < sizeof CODES / sizeof *CODES && strncmp(at, CODES[k], 4)) k++;
    if (k == sizeof CODES / sizeof *CODES || strlen(p) >= n) return NULL;
    memcpy(buf, p, strlen(p) + 1);
    memcpy(buf + (at - p), want, 4);
    return buf;
}
