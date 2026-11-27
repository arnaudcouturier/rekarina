// The voice language apart from the text language (OoT): REKARINA_VOICE_LANG (game, en, ja, de, fr, it, es, es-419,
// pt-BR, fr-CA; live).
//
// Swapping .bars.zs archives at the RomFS layer breaks playback because each .bars file indexes its audio assets by
// language-prefixed internal names (USen_..., EUfr_..., NV_USen_..., NV_EUfr_...) and allocates buffers from
// ResourceSizeTable under the requested language's name. Instead, the game already supports every voice language
// natively through GameDataMgr's OptionData.VoiceLanguage enum: runtime/oot_fps.c (voice_option, patch set voiceopt)
// writes the chosen enum hash below into GameDataMgr and triggers the game's own update path so .bars, .bwav, internal
// asset names and ResourceSizeTable sizes all stay in sync.
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"

// MurmurHash3_x86_32 (seed 0) values of OptionData.VoiceLanguage's enum entries (verified against main+0x9b47c0 and
// main+0x3f0fd28): Japanese, English, German, French_France, Italian, Spanish_Spain, Spanish_LatinAmerica,
// Portuguese_Brazil, French_Canada.
static const struct { const char *setting, *code; uint32_t hash; } VOICES[] = {
    {"en",     "USen", 0x82591fa9u},
    {"ja",     "JPja", 0x69b014ceu},
    {"de",     "EUde", 0x515b9eebu},
    {"fr",     "EUfr", 0x2498c397u},
    {"it",     "EUit", 0x8d62c251u},
    {"es",     "EUes", 0x33c26987u},
    {"es-419", "USes", 0xa683b1fdu},
    {"pt-BR",  "USpt", 0x1c3498ebu},
    {"fr-CA",  "USfr", 0x7d30fc45u},
};

static uint32_t lookup_hash(const char *v, const char **code_out) {
    for (size_t i = 0; v && i < sizeof VOICES / sizeof *VOICES; i++)
        if (!strcmp(v, VOICES[i].setting)) {
            if (code_out) *code_out = VOICES[i].code;
            return VOICES[i].hash;
        }
    return 0;
}

void voice_lang_init(void) {
    const char *v = cfg_value("REKARINA_VOICE_LANG");
    const char *code = NULL;
    uint32_t h = lookup_hash(v, &code);
    if (h) rt_log("voice: the game speaks %s (%s, option %#x) whatever its text language (voice_lang)", v, code, h);
    else if (v && *v && strcmp(v, "game")) rt_log("voice: voice_lang = %s is not one of the voices; the game's language speaks", v);
}

// The OptionData.VoiceLanguage enum hash for REKARINA_VOICE_LANG (re-read live when cfg_version changes), or 0 when
// set to "game" (follow the game's own language / option.sav).
uint32_t voice_lang_enum_hash(void) {
    static unsigned seen;
    static int init;
    static uint32_t hash;
    unsigned ver = cfg_version();
    if (!init || ver != seen) {
        const char *v = cfg_value("REKARINA_VOICE_LANG");
        hash = lookup_hash(v, NULL);
        seen = ver;
        init = 1;
    }
    return hash;
}
