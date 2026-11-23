// RenHDrum glue (rumble_rh.h). RenHDrum 0.5.x (https://github.com/trzz1214/RenHDrum, MIT OR Apache-2.0) is loaded at
// run time by its full path beside the game; the part of its C ABI used here (its include/renhdrum.h,
// RENHDRUM_VERSION 0x000500, which every 0.5.x keeps) is declared below. What each backend does:
//  - a DualSense on a cable: its USB sound card has four channels, the second its speaker and the third and fourth its
//    voice coils. RenHDrum's DualSense DSP turns the game's bands into that audio (48 kHz float); an SDL audio stream on
//    the pad's sound card pulls it (usb_pull). The DSP is made at full volume and the strength scales its samples.
//  - a DualSense over Bluetooth: the DSP builds the pad's haptics report for each 10.67 ms, 0x32 (142 bytes, the voice
//    coils) or with the extras 0x36 (398 bytes: the same haptics, both triggers' effects and the speaker as Opus); a
//    thread writes one per period through SDL's hidapi to the pad's HID path, beside SDL's own reports (bt_main). The
//    DSP takes the strength as its volume (10..100 %), and only when it is made, so a new strength makes a new DSP.
//  - a DualSense's extras (rh_extras): RenHDrum's trigger model turns each trigger's travel and the vibration into a
//    trigger effect (kickback), and the DSP's speaker channel plays small taps (at RenHDrum's documented 60 %). On the
//    cable the effects go to the pad as DS5 effects blocks through SDL_SendGamepadEffect (with the block that opens or
//    gives back the speaker); over Bluetooth they ride in each 0x36. Off: the speaker at 0, both triggers released
//    (one last 0x36 over Bluetooth), then the plain voice-coil path.
//  - an Xbox One or Series pad: RenHDrum's Xbox mapper gives its four motors (grips and triggers) from the bands; they
//    go out through SDL_RumbleGamepad and SDL_RumbleGamepadTriggers when they change, renewed every second while they
//    run (SDL writes nothing to the pad for a renewal of the same values), the strength on each motor.
//  - a Switch Pro Controller or a Joy-Con pair (Nintendo's own, as input.c pad_adopt's HD rumble rule): RenHDrum's HD
//    rumble engine, in its Switch 2 mode, builds the ten-byte rumble packet SDL_SendGamepadEffect takes (the layout of
//    input.c hd_send's); the strength scales the amplitudes fed to it.
// A side the game stops feeding goes silent after 250 ms: RenHDrum's backends hold the last value they were given.
// Locking: lk guards the handles, the last values and the Bluetooth trigger state; the sound card's callback and the
// Bluetooth thread take it for one fill or one report. Closing a stream or joining a thread happens with lk released
// (both wait for code that takes it).
// REKARINA_RH_TEST=usb|bt (test runs): a DualSense-type pad takes that path with a sink in place of the device (a test
// gamepad has neither a sound card nor a Bluetooth link); the sink logs every 2 s what the pad would have been sent.
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "cfg.h"
#include "host.h"
#include "rumble_rh.h"
#include "platform/platform.h"

#ifdef _WIN32
#define RH_FILE "renhdrum.dll"
#else
#define RH_FILE "renhdrum.so"
#endif
#define RH_ABI 0x000500u
#define RH_STALE_NS 250000000ull
#define RH_FREQ_LOW 160.0f    // the game's own frequencies when it sends none
#define RH_FREQ_HIGH 320.0f
#define RH_BT32 142           // report 0x32's size, and 0x36's
#define RH_BT36 398
#define RH_BT_PERIOD_NS 10666666ull
#define RH_BT_TAIL_NS 2000000000ull
#define RH_RENEW_NS 1000000000ull
#define RH_MOTOR_MS 2000
#define RH_FX 47              // a DS5 effects block
#define RH_SPEAKER 60         // RenHDrum's RENHDRUM_HD_HAPTICS_SPEAKER_PERCENT

typedef struct RhDsp RhDsp;
typedef struct RhXbox RhXbox;
typedef struct RhEngine RhEngine;
typedef struct RhTrigger RhTrigger;

static struct RhFns {
    uint32_t (*version)(void);
    void (*set_nx2_mode)(int32_t mode);
    RhDsp *(*ds_create)(int32_t volume_percent, bool crisp_notes);
    void (*ds_destroy)(RhDsp *d);
    int32_t (*ds_half)(RhDsp *d, bool right, float al, float fl, float ah, float fh);
    int32_t (*ds_fill)(RhDsp *d, float *out4, size_t frames);
    int32_t (*ds_bt)(RhDsp *d, uint8_t buffer_length, uint8_t *out142);
    int32_t (*ds_speaker)(RhDsp *d, int32_t percent);
    int32_t (*ds_bt36)(RhDsp *d, const uint8_t *state47, uint8_t seq, uint8_t *out398);
    RhTrigger *(*tr_create)(void);
    void (*tr_destroy)(RhTrigger *t);
    int32_t (*tr_update)(RhTrigger *t, uint32_t now_ms, uint8_t depth, float al, float fl, float ah, float fh, bool fresh,
                         uint8_t *out11);
    void (*tr_action)(RhTrigger *t, uint32_t now_ms);
    int32_t (*fx_speaker)(int32_t on, uint8_t *out47);
    int32_t (*fx_trigger)(int32_t right, const uint8_t *effect11, uint8_t *out47);
    RhXbox *(*xb_create)(bool bluetooth);
    void (*xb_destroy)(RhXbox *x);
    int32_t (*xb_half)(RhXbox *x, int64_t now_us, bool right, float al, float fl, float ah, float fh);
    int32_t (*xb_tick)(RhXbox *x, int64_t now_us, uint16_t *out4);
    int32_t (*xb_resend)(RhXbox *x);
    RhEngine *(*sw_create)(bool bluetooth, bool pro);
    void (*sw_destroy)(RhEngine *e);
    int32_t (*sw_half)(RhEngine *e, int64_t now_us, bool right, float al, float fl, float ah, float fh);
    int32_t (*sw_tick)(RhEngine *e, int64_t now_us, uint8_t *out10);
} fn;
#define SYM(field, name) {offsetof(struct RhFns, field), "renhdrum_" name}
static const struct { size_t at; const char *name; } syms[] = {
    SYM(version, "version"), SYM(set_nx2_mode, "set_nx2_mode"),
    SYM(ds_create, "dualsense_dsp_create"), SYM(ds_destroy, "dualsense_dsp_destroy"),
    SYM(ds_half, "dualsense_dsp_set_half_target"), SYM(ds_fill, "dualsense_dsp_fill_f32"),
    SYM(ds_bt, "dualsense_dsp_build_bt_report"), SYM(ds_speaker, "dualsense_dsp_set_speaker"),
    SYM(ds_bt36, "dualsense_dsp_build_bt36_report"),
    SYM(tr_create, "tri_trigger_create"), SYM(tr_destroy, "tri_trigger_destroy"), SYM(tr_update, "tri_trigger_update"),
    SYM(tr_action, "tri_trigger_note_action"), SYM(fx_speaker, "hd_haptics_speaker_block"),
    SYM(fx_trigger, "hd_haptics_trigger_block"),
    SYM(xb_create, "xbox_rumble_create"), SYM(xb_destroy, "xbox_rumble_destroy"),
    SYM(xb_half, "xbox_rumble_push_half_sample"), SYM(xb_tick, "xbox_rumble_tick"), SYM(xb_resend, "xbox_rumble_resend"),
    SYM(sw_create, "engine_create"), SYM(sw_destroy, "engine_destroy"), SYM(sw_half, "engine_push_half_sample"),
    SYM(sw_tick, "engine_tick"),
};

static PlatMutex load_lock = PLAT_MUTEX_INIT;
static volatile int32_t load_state;   // 0 not tried, 1 usable, -1 not

static int load(void) {
    int missing = 0;
    void *lib = plat_lib_open_beside(RH_FILE, &missing);
    if (!lib) {
        rt_log("vibration: RenHDrum: %s %s: every controller keeps its own rumble", RH_FILE,
               missing ? "is not beside the game" : "would not load");
        return 0;
    }
    for (size_t i = 0; i < sizeof syms / sizeof *syms; i++) {
        void *p = plat_lib_sym(lib, syms[i].name);
        if (!p) {
            rt_log("vibration: RenHDrum: %s has no %s: every controller keeps its own rumble", RH_FILE, syms[i].name);
            return 0;
        }
        memcpy((char *)&fn + syms[i].at, &p, sizeof p);
    }
    uint32_t v = fn.version();
    if ((v & 0xffff00u) != (RH_ABI & 0xffff00u)) {
        rt_log("vibration: RenHDrum: %s is version %u.%u.%u, this build takes %u.%u.x: every controller keeps its own "
               "rumble", RH_FILE, v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff, RH_ABI >> 16 & 0xff, RH_ABI >> 8 & 0xff);
        return 0;
    }
    fn.set_nx2_mode(2);   // a Switch 2 game: RenHDrum's Mode 2 curves for every pad it plays
    rt_log("vibration: RenHDrum %u.%u.%u loaded", v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff);
    return 1;
}

int rh_ready(void) {
    int32_t s = __atomic_load_n(&load_state, __ATOMIC_ACQUIRE);
    if (s) return s > 0;
    plat_mutex_lock(&load_lock);
    if (!load_state) __atomic_store_n(&load_state, load() ? 1 : -1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&load_lock);
    return __atomic_load_n(&load_state, __ATOMIC_ACQUIRE) > 0;
}

// ---- the game's values and the backends that play them (lk) ----
static PlatMutex lk = PLAT_MUTEX_INIT;
static volatile int32_t kind;        // RH_*: the backend open
static RhDsp *dsp;                   // a DualSense's, on either path
static RhXbox *xbox;
static RhEngine *engine;
static RhTrigger *trig[2];           // a DualSense's two trigger models while its extras are on (left, right)
static float last[2][4] = {{0, RH_FREQ_LOW, 0, RH_FREQ_HIGH}, {0, RH_FREQ_LOW, 0, RH_FREQ_HIGH}};
static uint64_t fed_at[2];           // plat_now_ns of each side's last value, 0 none
static uint32_t fed_n[2], trig_seen[2];   // values fed per side, and the count each trigger model saw
static int silent[2] = {1, 1};
static float sw_gain = 1.0f;         // the strength on the Switch engine's amplitudes
static volatile int32_t strength = 100;   // the Vibration strength in percent (rh_tick)
static uint8_t bt_state[RH_FX];      // both triggers' effects as report 0x36 carries them
static volatile int32_t bt36;        // the Bluetooth thread builds 0x36 (the extras on), else 0x32
static volatile int32_t bt36_last;   // ... one more 0x36 (both triggers released) before 0x32 again
static char test_path[8];            // REKARINA_RH_TEST: "usb", "bt" or ""

static void push(int s, uint64_t now) {   // lk held: side s's last value into whatever plays
    const float *v = last[s];
    const bool right = s == 1;
    const int64_t us = (int64_t)(now / 1000);
    if (dsp) fn.ds_half(dsp, right, v[0], v[1], v[2], v[3]);
    if (xbox) fn.xb_half(xbox, us, right, v[0], v[1], v[2], v[3]);
    if (engine) {
        float al = v[0] * sw_gain, ah = v[2] * sw_gain;
        fn.sw_half(engine, us, right, al < 1 ? al : 1, v[1], ah < 1 ? ah : 1, v[3]);
    }
}

void rh_feed(int side, float al, float fl, float ah, float fh) {
    if (side < 0 || side > 1) return;
    al = al > 0 ? (al < 1 ? al : 1) : 0;   // a NaN too: 0
    ah = ah > 0 ? (ah < 1 ? ah : 1) : 0;
    if (!(fl > 0) || !isfinite(fl)) fl = RH_FREQ_LOW;
    if (!(fh > 0) || !isfinite(fh)) fh = RH_FREQ_HIGH;
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&lk);
    last[side][0] = al, last[side][1] = fl, last[side][2] = ah, last[side][3] = fh;
    fed_at[side] = now, silent[side] = 0, fed_n[side]++;
    push(side, now);
    plat_mutex_unlock(&lk);
}

int rh_kind(void) { return __atomic_load_n(&kind, __ATOMIC_ACQUIRE); }

const char *rh_kind_name(int k) {
    static const char *const names[] = {"none", "a DualSense's voice coils on its cable",
                                        "a DualSense's voice coils over Bluetooth", "an Xbox pad's four motors",
                                        "a Switch controller's HD rumble"};
    return k >= 0 && k <= RH_SWITCH ? names[k] : "?";
}

// ---- a DualSense on a cable: its sound card ----
static SDL_AudioStream *usb_stream;   // gamepad thread
static PlatThread *usb_test;          // REKARINA_RH_TEST=usb: the sink that pulls in its place
static volatile int32_t usb_test_run;

// frames of the four channels into buf (frames <= 512): the DSP's, the strength on them; silence without a DSP
static void usb_fill(float *buf, int frames) {
    int ok = 0;
    plat_mutex_lock(&lk);
    if (dsp && kind == RH_DUALSENSE_USB) ok = fn.ds_fill(dsp, buf, (size_t)frames) >= 0;
    plat_mutex_unlock(&lk);
    float g = (float)__atomic_load_n(&strength, __ATOMIC_RELAXED) / 100.0f;
    if (!ok) {
        memset(buf, 0, (size_t)frames * 16);   // nothing to play: silence, never a held waveform
    } else if (g != 1.0f) {
        for (int i = 0; i < frames * 4; i++) {
            float v = buf[i] * g;
            buf[i] = v < -1 ? -1 : v > 1 ? 1 : v;
        }
    }
}

static void SDLCALL usb_pull(void *user, SDL_AudioStream *s, int additional, int total) {
    (void)user, (void)total;
    static float buf[4 * 512];
    while (additional > 0) {
        int frames = (additional + 15) / 16;
        if (frames > 512) frames = 512;
        usb_fill(buf, frames);
        SDL_PutAudioStreamData(s, buf, frames * 16);
        additional -= frames * 16;
    }
}

// REKARINA_RH_TEST=usb: 480 frames every 10 ms, as the sound card would take them; every 2 s the level of each channel
static void usb_test_main(void *arg) {
    (void)arg;
    static float buf[4 * 480];
    double sum[4] = {0}, peak[4] = {0};
    uint64_t frames = 0, next = plat_now_ns(), report = next + 2000000000ull;
    while (__atomic_load_n(&usb_test_run, __ATOMIC_ACQUIRE)) {
        usb_fill(buf, 480);
        for (int i = 0; i < 480 * 4; i++) {
            double v = buf[i];
            sum[i & 3] += v * v;
            if (fabs(v) > peak[i & 3]) peak[i & 3] = fabs(v);
        }
        frames += 480;
        uint64_t now = plat_now_ns();
        if (now >= report) {
            if (peak[1] > 0 || peak[2] > 0 || peak[3] > 0)
                rt_log("vibration test: the cable's sound card got %llu frames; rms (peak) of channels 1-4: %.4f (%.3f) "
                       "%.4f (%.3f) %.4f (%.3f) %.4f (%.3f)", (unsigned long long)frames, sqrt(sum[0] / frames), peak[0],
                       sqrt(sum[1] / frames), peak[1], sqrt(sum[2] / frames), peak[2], sqrt(sum[3] / frames), peak[3]);
            memset(sum, 0, sizeof sum), memset(peak, 0, sizeof peak), frames = 0, report = now + 2000000000ull;
        }
        next += 10000000ull;
        if (next > now) plat_wait_ns(next - now);
        else next = now;
    }
}

// The pad's own sound card: a playback device named for a DualSense with four channels; 0 when there is none.
static SDL_AudioDeviceID usb_card(void) {
    static int audio_init;
    if (!audio_init) audio_init = SDL_InitSubSystem(SDL_INIT_AUDIO) ? 1 : -1;
    if (audio_init < 0) return 0;
    int n = 0, named = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n), found = 0;
    for (int i = 0; ids && i < n && !found; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (!name || (!strstr(name, "DualSense") && !strstr(name, "Wireless Controller"))) continue;
        named++;
        SDL_AudioSpec spec;
        int frames = 0;
        if (SDL_GetAudioDeviceFormat(ids[i], &spec, &frames) && spec.channels >= 4) found = ids[i];
    }
    SDL_free(ids);
    if (!found) rt_log("vibration: %d playback devices, %d named for a DualSense, none with four channels", n, named);
    return found;
}

static int usb_open(void) {
    int test = !strcmp(test_path, "usb");
    SDL_AudioDeviceID card = test ? 0 : usb_card();
    if (!card && !test) return RH_NONE;
    RhDsp *d = fn.ds_create(100, false);   // full volume: the strength scales its samples (usb_fill)
    if (!d) return RH_NONE;
    plat_mutex_lock(&lk);
    dsp = d;
    plat_mutex_unlock(&lk);
    if (test) {
        __atomic_store_n(&usb_test_run, 1, __ATOMIC_RELEASE);
        usb_test = plat_thread_new(usb_test_main, NULL, "vibration test (cable)");
        if (usb_test) {
            rt_log("vibration test: the cable's sound card is a sink (REKARINA_RH_TEST=usb)");
            return RH_DUALSENSE_USB;
        }
    } else {
        const SDL_AudioSpec spec = {SDL_AUDIO_F32, 4, 48000};
        usb_stream = SDL_OpenAudioDeviceStream(card, &spec, usb_pull, NULL);
        if (usb_stream && SDL_ResumeAudioStreamDevice(usb_stream)) {
            // The system's volume for the pad's sound card scales this stream as any other (shared mode): at 0 or muted
            // the voice coils get silence. Read only; the player sets it (Windows: Sound settings, the controller).
            static const char *const names[] = {"DualSense", "Wireless Controller"};
            float vol = 0;
            int muted = 0;
            char name[160];
            if (plat_audio_out_volume(names, 2, &vol, &muted, name, sizeof name))
                rt_log("vibration: the DualSense's volume in the system is %.0f %%%s (%s)%s", vol * 100.0f,
                       muted ? ", muted" : "", name, muted || vol < 0.05f ? ": its vibration is silent until it is "
                       "turned up in the sound settings" : vol < 0.5f ? ": its vibration is weaker by as much" : "");
            return RH_DUALSENSE_USB;
        }
        rt_log("vibration: the DualSense's sound card would not open (%s)", SDL_GetError());
        if (usb_stream) SDL_DestroyAudioStream(usb_stream);
        usb_stream = NULL;
    }
    plat_mutex_lock(&lk);
    dsp = NULL;
    plat_mutex_unlock(&lk);
    fn.ds_destroy(d);
    return RH_NONE;
}

// ---- a DualSense over Bluetooth: report 0x32 or 0x36 beside SDL's own ----
static SDL_hid_device *bt_hid;
static PlatThread *bt_thread;
static volatile int32_t bt_run, bt_failed;
static int bt_volume;   // the volume the current DSP was made with (gamepad thread)

static int volume_of(int percent) { return percent < 10 ? 10 : percent > 100 ? 100 : percent; }

// REKARINA_RH_TEST=bt: what would have been written, counted (the Bluetooth thread's own)
static struct { uint32_t r32, r36, loud; uint8_t trig_bits; uint64_t at; } bt_tally;
static int bt_write(const uint8_t *report, int size) {
    if (bt_hid) return SDL_hid_write(bt_hid, report, (size_t)size);
    bt_tally.r32 += report[0] == 0x32, bt_tally.r36 += report[0] == 0x36;
    uint64_t now = plat_now_ns();
    if (!bt_tally.at) bt_tally.at = now;
    if (now - bt_tally.at >= 2000000000ull) {
        if (bt_tally.loud)
            rt_log("vibration test: Bluetooth would have got %u reports 0x32 and %u reports 0x36, %u of them carrying "
                   "vibration; trigger bits in the last 2 s %02x", bt_tally.r32, bt_tally.r36, bt_tally.loud,
                   bt_tally.trig_bits);
        memset(&bt_tally, 0, sizeof bt_tally);
        bt_tally.at = now;
    }
    return size;
}

static void bt_state_rest(void) {   // lk held: both triggers released, as 0x36 carries them
    uint8_t one[RH_FX];
    memset(bt_state, 0, sizeof bt_state);
    for (int s = 0; s < 2; s++) {
        fn.fx_trigger(s, NULL, one);
        int at = s ? 10 : 21;   // the right trigger's effect at bytes 10..20, the left's at 21..31
        bt_state[0] |= one[0];
        memcpy(bt_state + at, one + at, 11);
    }
}

static void bt_main(void *arg) {
    (void)arg;
    uint8_t report[RH_BT36];
    uint64_t next = plat_now_ns(), loud = 0;
    int fails = 0, was36 = 0;
    while (__atomic_load_n(&bt_run, __ATOMIC_ACQUIRE)) {
        next += RH_BT_PERIOD_NS;
        int r = -1, size = 0;
        plat_mutex_lock(&lk);
        if (dsp) {
            if (bt36 || bt36_last) {
                r = fn.ds_bt36(dsp, bt_state, 0, report), size = RH_BT36;
                bt36_last = 0;
                if (!bt_hid) bt_tally.trig_bits |= bt_state[0];
            } else {
                r = fn.ds_bt(dsp, 0xff, report), size = RH_BT32;
            }
        }
        plat_mutex_unlock(&lk);
        uint64_t now = plat_now_ns();
        int send;
        if (size == RH_BT36) {
            send = r == 1;   // 1: send it (RenHDrum stops by itself two seconds after the last that played)
            was36 = 1;
        } else {
            // (silent reports only just after the last that carried vibration: the radio stays free for the input)
            if (r == 1) loud = now;
            send = r >= 0 && loud && now - loud < RH_BT_TAIL_NS;
            was36 = 0;
        }
        if (!bt_hid && r == 1) bt_tally.loud++;
        if (send && __atomic_load_n(&strength, __ATOMIC_RELAXED) > 0) {
            if (bt_write(report, size) < 0) {
                if (++fails >= 50) {
                    rt_log("vibration: the DualSense's Bluetooth haptics reports keep failing (%s): its motors instead",
                           SDL_GetError());
                    __atomic_store_n(&bt_failed, 1, __ATOMIC_RELEASE);
                    return;
                }
            } else {
                fails = 0;
            }
        }
        if (now > next + 4 * RH_BT_PERIOD_NS) next = now;   // far behind: start again from now
        if (next > now) plat_wait_ns(next - now);
    }
    if (was36) {   // closing with the extras on: both triggers released, so neither stays stiff
        int r = -1;
        plat_mutex_lock(&lk);
        bt_state_rest();
        if (dsp) r = fn.ds_bt36(dsp, bt_state, 0, report);
        plat_mutex_unlock(&lk);
        if (r == 1) bt_write(report, RH_BT36);
    }
}

static int bt_open(SDL_Gamepad *pad, int percent) {
    int test = !strcmp(test_path, "bt");
    const char *path = test ? NULL : SDL_GetGamepadPath(pad);
    if (!test && (!path || !*path)) {
        rt_log("vibration: the DualSense has no HID path SDL opened it by: its motors");
        return RH_NONE;
    }
    SDL_hid_device *h = test ? NULL : SDL_hid_open_path(path);
    RhDsp *d = h || test ? fn.ds_create(volume_of(percent), false) : NULL;
    if (!d) {
        rt_log("vibration: the DualSense's HID device would not open for its Bluetooth haptics (%s): its motors",
               h ? "no DSP" : SDL_GetError());
        if (h) SDL_hid_close(h);
        return RH_NONE;
    }
    plat_mutex_lock(&lk);
    dsp = d, bt36 = 0, bt36_last = 0;
    plat_mutex_unlock(&lk);
    bt_hid = h, bt_volume = volume_of(percent);
    memset(&bt_tally, 0, sizeof bt_tally);
    __atomic_store_n(&bt_run, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&bt_failed, 0, __ATOMIC_RELEASE);
    bt_thread = plat_thread_new(bt_main, NULL, "vibration (Bluetooth)");
    if (bt_thread) {
        if (test) rt_log("vibration test: the DualSense's Bluetooth link is a sink (REKARINA_RH_TEST=bt)");
        return RH_DUALSENSE_BT;
    }
    plat_mutex_lock(&lk);
    dsp = NULL;
    plat_mutex_unlock(&lk);
    fn.ds_destroy(d);
    if (h) SDL_hid_close(h);
    bt_hid = NULL;
    return RH_NONE;
}

// ---- a DualSense's extras ----
static int extras;   // gamepad thread: the extras as applied to the DualSense played now

static void send_block(SDL_Gamepad *pad, const uint8_t block[RH_FX]) {
    if (pad) SDL_SendGamepadEffect(pad, block, RH_FX);   // a pad that refuses it returns false: nothing more to do
}

// On or off on the DualSense played now (k: its path). Gamepad thread. pad NULL: it went away, nothing is sent to it.
static void extras_set(SDL_Gamepad *pad, int k, int on) {
    uint8_t block[RH_FX];
    plat_mutex_lock(&lk);
    if (dsp) fn.ds_speaker(dsp, on ? RH_SPEAKER : 0);
    for (int s = 0; s < 2; s++) {
        if (on && !trig[s]) trig[s] = fn.tr_create();
        if (!on && trig[s]) fn.tr_destroy(trig[s]), trig[s] = NULL;
        trig_seen[s] = fed_n[s];
    }
    if (k == RH_DUALSENSE_BT) {
        bt_state_rest();
        bt36_last = bt36 && !on;   // going off: one last 0x36 with both triggers released, then 0x32
        bt36 = on;
    }
    plat_mutex_unlock(&lk);
    if (k == RH_DUALSENSE_USB) {
        fn.fx_speaker(on, block);
        send_block(pad, block);
        if (!on)
            for (int s = 0; s < 2; s++) fn.fx_trigger(s, NULL, block), send_block(pad, block);
    }
    extras = on;
    rt_log("vibration: the DualSense's extras %s (trigger kickback, the speaker's taps)", on ? "on" : "off");
}

void rh_extras(SDL_Gamepad *pad, uint64_t now, int on, const int depth[2], int other) {
    int k = rh_kind();
    if (k != RH_DUALSENSE_USB && k != RH_DUALSENSE_BT) return;
    if (!!on != extras) extras_set(pad, k, !!on);
    if (!extras) return;
    const uint32_t ms = (uint32_t)(now / 1000000);
    uint8_t blocks[2][RH_FX], effect[11];
    int n = 0;
    plat_mutex_lock(&lk);
    for (int s = 0; s < 2; s++) {
        if (!trig[s]) continue;
        if (other) fn.tr_action(trig[s], ms);   // another button held: a posture, or the kick is muted
        bool fresh = fed_n[s] != trig_seen[s];
        trig_seen[s] = fed_n[s];
        int d = depth[s] < 0 ? 0 : depth[s] > 255 ? 255 : depth[s];
        if (fn.tr_update(trig[s], ms, (uint8_t)d, last[s][0], last[s][1], last[s][2], last[s][3], fresh, effect) != 1)
            continue;
        if (k == RH_DUALSENSE_USB) {
            fn.fx_trigger(s, effect, blocks[n++]);
        } else {
            bt_state[0] |= s ? 0x04 : 0x08;   // the right trigger's enable bit, or the left's
            memcpy(bt_state + (s ? 10 : 21), effect, 11);
        }
    }
    plat_mutex_unlock(&lk);
    for (int i = 0; i < n; i++) send_block(pad, blocks[i]);
}

// ---- open, close ----
static uint16_t xb_sent[4];      // the motors last written, and when (gamepad thread)
static uint64_t xb_at;
static int xb_trig;              // the trigger motors were last written non-zero

int rh_attach(SDL_Gamepad *pad, int percent) {
    if (!pad || !rh_ready() || rh_kind() != RH_NONE) return RH_NONE;
    __atomic_store_n(&strength, percent, __ATOMIC_RELAXED);
    const char *t_env = cfg_get("REKARINA_RH_TEST");
    snprintf(test_path, sizeof test_path, "%s", t_env ? t_env : "");
    SDL_GamepadType t = SDL_GetGamepadType(pad);
    int wireless = SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRELESS;
    int k = RH_NONE;
    if (t == SDL_GAMEPAD_TYPE_PS5) {
        k = (wireless && strcmp(test_path, "usb")) || !strcmp(test_path, "bt") ? bt_open(pad, percent) : usb_open();
    } else if (t == SDL_GAMEPAD_TYPE_XBOXONE) {
        RhXbox *x = fn.xb_create(wireless != 0);
        plat_mutex_lock(&lk);
        xbox = x;
        plat_mutex_unlock(&lk);
        if (x) k = RH_XBOX;
    } else if ((t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) &&
               SDL_GetGamepadVendor(pad) == 0x057e) {
        RhEngine *e = fn.sw_create(wireless != 0, t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO);
        plat_mutex_lock(&lk);
        engine = e, sw_gain = percent / 100.0f;
        plat_mutex_unlock(&lk);
        if (e) k = RH_SWITCH;
    }
    const char *type = SDL_GetGamepadStringForType(t);
    if (k == RH_NONE) {
        rt_log("vibration: RenHDrum does not play \"%s\" (%s, %s): its own rumble", SDL_GetGamepadName(pad),
               type && *type ? type : "unknown type", wireless ? "wireless" : "wired or unknown");
        return RH_NONE;
    }
    memset(xb_sent, 0, sizeof xb_sent), xb_at = 0, xb_trig = 0, extras = 0;
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&lk);
    __atomic_store_n(&kind, k, __ATOMIC_RELEASE);
    for (int s = 0; s < 2; s++) push(s, now);   // what the game sent last, at once
    plat_mutex_unlock(&lk);
    rt_log("vibration: RenHDrum plays \"%s\": %s", SDL_GetGamepadName(pad), rh_kind_name(k));
    return k;
}

void rh_detach(SDL_Gamepad *pad) {
    int k = rh_kind();
    if (extras && k == RH_DUALSENSE_USB) extras_set(pad, k, 0);   // the speaker given back, the triggers released
    __atomic_store_n(&kind, RH_NONE, __ATOMIC_RELEASE);
    if (usb_stream) SDL_DestroyAudioStream(usb_stream);   // waits for usb_pull, which takes lk: not held here
    usb_stream = NULL;
    if (usb_test) {
        __atomic_store_n(&usb_test_run, 0, __ATOMIC_RELEASE);
        plat_thread_join(usb_test);
        usb_test = NULL;
    }
    if (bt_thread) {   // (its last report releases the triggers when the extras were on)
        __atomic_store_n(&bt_run, 0, __ATOMIC_RELEASE);
        plat_thread_join(bt_thread);
        bt_thread = NULL;
    }
    if (bt_hid) SDL_hid_close(bt_hid);
    bt_hid = NULL;
    plat_mutex_lock(&lk);
    RhDsp *d = dsp;
    RhXbox *x = xbox;
    RhEngine *e = engine;
    RhTrigger *t0 = trig[0], *t1 = trig[1];
    dsp = NULL, xbox = NULL, engine = NULL, trig[0] = trig[1] = NULL, bt36 = bt36_last = 0;
    plat_mutex_unlock(&lk);
    if (d) fn.ds_destroy(d);
    if (x) fn.xb_destroy(x);
    if (e) fn.sw_destroy(e);
    if (t0) fn.tr_destroy(t0);
    if (t1) fn.tr_destroy(t1);
    extras = 0;
    if (k != RH_NONE) rt_log("vibration: RenHDrum stopped (%s)", rh_kind_name(k));
}

// ---- every poll ----
static uint16_t scaled(uint16_t m, int percent) {
    if (!m || percent == 100) return m;
    uint32_t v = (uint32_t)m * (uint32_t)percent / 100u;
    return v > 65535u ? 65535 : v ? (uint16_t)v : (percent ? 1 : 0);   // RenHDrum's 1s (its reset steps) stay 1
}

int rh_tick(SDL_Gamepad *pad, uint64_t now, int percent, int triggers) {
    int k = rh_kind();
    if (k == RH_NONE) return 0;
    int was = __atomic_exchange_n(&strength, percent, __ATOMIC_RELAXED);
    if (k == RH_DUALSENSE_BT && __atomic_load_n(&bt_failed, __ATOMIC_ACQUIRE)) return 0;
    if (k == RH_DUALSENSE_BT && volume_of(percent) != bt_volume) {   // the DSP's volume is set when it is made
        RhDsp *d = fn.ds_create(volume_of(percent), false), *old = NULL;
        if (d) {
            plat_mutex_lock(&lk);
            old = dsp, dsp = d;
            fn.ds_speaker(dsp, extras ? RH_SPEAKER : 0);
            for (int s = 0; s < 2; s++) fn.ds_half(dsp, s == 1, last[s][0], last[s][1], last[s][2], last[s][3]);
            plat_mutex_unlock(&lk);
            fn.ds_destroy(old);
        }
        bt_volume = volume_of(percent);
    }
    uint16_t m[4];
    uint8_t packet[10];
    int send_m = 0, send_p = 0;
    const int64_t us = (int64_t)(now / 1000);
    plat_mutex_lock(&lk);
    if (percent != was) sw_gain = percent / 100.0f;
    for (int s = 0; s < 2; s++)
        if (fed_at[s] && !silent[s] && now - fed_at[s] > RH_STALE_NS) {
            last[s][0] = 0, last[s][1] = RH_FREQ_LOW, last[s][2] = 0, last[s][3] = RH_FREQ_HIGH;
            silent[s] = 1;
            push(s, now);
        }
    if (xbox) send_m = fn.xb_tick(xbox, us, m) == 1;
    if (engine) send_p = fn.sw_tick(engine, us, packet) == 1;
    plat_mutex_unlock(&lk);
    if (k == RH_XBOX) {
        int renew = !send_m && (xb_sent[0] || xb_sent[1] || xb_sent[2] || xb_sent[3]) && now - xb_at >= RH_RENEW_NS;
        if (send_m)
            for (int i = 0; i < 4; i++) xb_sent[i] = scaled(m[i], percent);
        if (send_m || renew) {
            xb_at = now;
            if (!SDL_RumbleGamepad(pad, xb_sent[0], xb_sent[1], RH_MOTOR_MS)) {
                plat_mutex_lock(&lk);
                if (xbox) fn.xb_resend(xbox);   // refused: the next tick sends again even if nothing changed
                plat_mutex_unlock(&lk);
            }
            if (triggers || xb_trig) {   // trigger motors only while the Trigger vibration row is on
                uint16_t l = triggers ? xb_sent[2] : 0, r = triggers ? xb_sent[3] : 0;
                SDL_RumbleGamepadTriggers(pad, l, r, RH_MOTOR_MS);   // a pad without them returns false: ignored
                xb_trig = l || r;
            }
        }
    }
    if (k == RH_SWITCH && send_p && !SDL_SendGamepadEffect(pad, packet, sizeof packet)) {
        rt_log("vibration: \"%s\" does not take RenHDrum's HD rumble packets (%s): its own rumble",
               SDL_GetGamepadName(pad), SDL_GetError());
        return 0;
    }
    return 1;
}
