// Switch 2 SDK calls that reach services this host does not serve (GAME_NS2, Rekarina). Each answers what an offline PC
// has: SDR output at a fixed rate with no VRR, one stereo output device, no Joy-Con 2 mouse sensor, a fixed graphics
// scale tier, no Nintendo Account, the PC's clock as the network clock, and no add-on content.
//
// Behaviour ported from MacNativeGames games/totk/runtime ns2_host.c, offline_host.c, account.c and time_host.c (MIT),
// which took it from seedds/nxnative (MIT; dm/host/hle_misc.c, hle_hid.c, hle_oe.c, hle_audren.c, dm/gfx/hle_vi.c).
// Written again for this host's calling convention: every function takes the guest Cpu and reads its arguments.
#ifdef GAME_NS2
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

#define R_NO_NINTENDO_ACCOUNT 0x7c67u

static void h_ok(Cpu *c) { ret_x(c, 0); }
static void h_nothing(Cpu *c) { (void)c; }
static void h_zero(Cpu *c) { ret_x(c, 0); }

static uint64_t bound_event(Cpu *c, int signalled) {   // a SystemEvent the host owns, bound once
    uint64_t ev = (uint64_t)(uintptr_t)mem_kernel_alloc(0x80, 16);
    kern_bind_system_event(c, ev, 0);
    if (signalled) kern_signal_system_event(ev);
    return ev;
}

// ---- vi: HDR, VRR, refresh rate. The host presents SDR at the display's rate.
// Result QueryDisplayHdrStatus(bool *on, HdrOutputFormat *fmt, u16 *peak, const Display *)
static void vi_hdr_status(Cpu *c) {
    uint8_t *on = arg_p(c, 0);
    uint32_t *fmt = arg_p(c, 1);
    uint16_t *peak = arg_p(c, 2);
    if (on) *on = 0;
    if (fmt) *fmt = 0;
    if (peak) *peak = 0;
    ret_x(c, 0);
}
static void vi_paper_white(Cpu *c) {   // Result QueryDisplayHdrPaperWhite(u16 *nits, const Display *): BT.2408's 203
    uint16_t *nits = arg_p(c, 0);
    if (nits) *nits = 203;
    ret_x(c, 0);
}
static void vi_vrr_supported(Cpu *c) {   // Result QueryDisplayVrrSupported(bool *, const Display *)
    uint8_t *on = arg_p(c, 0);
    if (on) *on = 0;
    ret_x(c, 0);
}
// Result QueryDisplayCurrentRefreshRate(RefreshRate *rate, bool *, const Display *): 60 Hz. RefreshRate is a u32 of
// Hz in the SDK's enum (60); the flag says whether the rate is variable.
static void vi_refresh_rate(Cpu *c) {
    uint32_t *rate = arg_p(c, 0);
    uint8_t *var = arg_p(c, 1);
    if (rate) *rate = 60;
    if (var) *var = 0;
    ret_x(c, 0);
}
static void vi_ext_swap(Cpu *c) {   // Result SetLayerExtendedSwapInterval(Layer *, int, int): without VRR the NVN one stays
    static int once;
    if (!once++) rt_log("ns2: vi::SetLayerExtendedSwapInterval %d %d: no VRR, the NVN swap interval stays",
                        (int)arg_x(c, 1), (int)arg_x(c, 2));
    ret_x(c, 0);
}

// ---- audio: one built-in stereo output device
enum { DEVNAME = 0x100 };
static void devname(char *out) {
    memset(out, 0, DEVNAME);
    strcpy(out, "AudioBuiltInSpeakerOutput");
}
static void audio_list(Cpu *c) {   // int ListAudioOutputDeviceName(AudioDeviceName *, int)
    char *names = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    if (n > 0 && names) devname(names);
    ret_x(c, n > 0 && names ? 1 : 0);
}
static void audio_active(Cpu *c) {   // void GetActiveAudioOutputDeviceName(AudioDeviceName *)
    char *out = arg_p(c, 0);
    if (out) devname(out);
}
static void audio_channels(Cpu *c) { ret_x(c, 2); }
static uint32_t device_volume = 0x3f800000u;   // 1.0f, kept and read back
static void audio_set_volume(Cpu *c) { __atomic_store_n(&device_volume, (uint32_t)c->v[0], __ATOMIC_RELAXED); }
static void audio_volume(Cpu *c) { c->v[0] = __atomic_load_n(&device_volume, __ATOMIC_RELAXED); }
static uint8_t autotune;
static void audio_set_autotune(Cpu *c) { __atomic_store_n(&autotune, (uint8_t)(arg_x(c, 0) & 1), __ATOMIC_RELAXED); }
static void audio_autotune(Cpu *c) { ret_x(c, __atomic_load_n(&autotune, __ATOMIC_RELAXED)); }
static void audio_notification(Cpu *c) {   // Result AcquireAudioDeviceNotificationForOutput(os::SystemEvent *)
    static int acquired;
    uint64_t ev = arg_x(c, 0);
    if (acquired++) {
        ret_x(c, 0x499);
        return;
    }
    if (ev) kern_bind_system_event(c, ev, 0);
    ret_x(c, 0);
}

// ---- hid: the Joy-Con 2 mouse sensor (JCMS) and the mouse (Nmouse): none on the host
static void jcms_handle(Cpu *c) {   // JcmsHandle GetJcmsHandle(const u32 &npad, const JcmsType &)
    const uint32_t *id = arg_p(c, 0), *type = arg_p(c, 1);
    ret_x(c, (id ? *id & 0xff : 0) | (type ? (*type & 0xff) << 8 : 0));
}
static void nmouse_handles(Cpu *c) { ret_x(c, 0); }   // int GetNmouseHandles(NmouseHandle *, int, u32): none
static void nmouse_types(Cpu *c) {   // void GetNmouseTypeSet(BitFlagSet<32, NmouseType> *, NmouseHandle)
    uint32_t *out = arg_p(c, 0);
    if (out) *out = 0;
}

// ---- oe
// The graphics scale tier is the factor by which the Switch 2's compositor enlarges the game's picture: OoT
// draws its frame at 4/(4 + tier) of the window texture (1, 4/5, 2/3; its 3D scene at 3/4 of that) and leaves
// the rest to the system. Tier 0 (default) draws at the window's full size. With tier 1 or 2 the present takes the
// top-left part the game drew and enlarges it to the window as the compositor would (nvn_api_obj.c), with the
// upscaler (REKARINA_UPSCALER): 64 % or 44 % of the pixels to shade. On the forest porch (RTX 5070) the GPU time drops
// only from 19.9 to 18.4 and 17.9 ms: the frame is not bound by its pixels. REKARINA_GRAPHICS_SCALE=0|1|2
// (OOT_GRAPHICS_SCALE, the old name, still read).
int host_graphics_tier(void) {
    static int t = -1;
    if (t < 0) {
        const char *e = cfg_get("REKARINA_GRAPHICS_SCALE");
        if (!e || !*e) e = getenv("OOT_GRAPHICS_SCALE");
        t = e && *e >= '0' && *e <= '2' ? *e - '0' : 0;
        if (t) rt_log("oe: graphics scale tier %d: the game draws %s of the window, the present enlarges it", t, t == 1 ? "4/5" : "2/3");
    }
    return t;
}
static void oe_graphics_scale(Cpu *c) { ret_x(c, (uint64_t)host_graphics_tier()); }
static void oe_display_version(Cpu *c) {   // void GetDisplayVersion(DisplayVersion *): 16 bytes of text
    char *out = arg_p(c, 0);
    if (out) memset(out, 0, 16), strcpy(out, "1.1.1");
}

// ---- audio input: the host has no microphone
// OoT's option that plays its instrument by voice (or by a real instrument) listens through two input streams of the
// SDK's stream interface (SDK 22: nn::audio::OpenInputStream and the calls below, 16-bit mono at 48 kHz). It lists the
// virtual input devices first and opens the streams only when there is one; the host answers none
// (ListVirtualInputDevices below), so nothing is opened. With the option on, the game still purges and starts both
// streams when it begins to listen (main+0x336ee10 checks the option, not whether the streams were opened: the
// console always has a microphone), and the SDK's own PurgeInputStream / StartInputStream read through the stream's
// implementation pointer, which is null in a stream that was never opened. The game stopped there (player report
// 2026-11-19: "the game crashes if voice recognition is enabled"). Every input stream call is answered here for a
// stream that does not exist: it cannot be opened or started, it is closed, and it holds no frames, so the option does
// nothing and the buttons play as before.
#define R_AUDIO_NO_INPUT (153u | 1u << 9)   // a failure of the audio module; the game tests these results for zero only
static void audin_log(const char *what) {
    static int told;
    if (!told++) rt_log("audio input: the game asks for a microphone (%s): the host has none, its voice input stays silent", what);
}
// Result OpenInputStream(InputStreamType *, [os::SystemEvent *,] void *buffer, size_t, const AudioDeviceName &,
// const StreamFormat &, int frames[, int]): the first word of the stream is its implementation pointer
static void audin_open(Cpu *c) {
    uint64_t *stream = arg_p(c, 0);
    if (stream) *stream = 0;
    audin_log("OpenInputStream");
    ret_x(c, R_AUDIO_NO_INPUT);
}
static void audin_start(Cpu *c) {   // Result StartInputStream(InputStreamType *)
    audin_log("StartInputStream");
    ret_x(c, R_AUDIO_NO_INPUT);
}
// InputStreamStatistics GetInputStreamStatistics(InputStreamType *, bool reset): three 64-bit counts, returned in
// memory (x8)
static void audin_statistics(Cpu *c) {
    void *out = (void *)(uintptr_t)c->x[8];
    if (out) memset(out, 0, 24);
}

// ---- account: no Nintendo Account, so the network service account calls fail at once
static void acc_no_account(Cpu *c) { ret_x(c, R_NO_NINTENDO_ACCOUNT); }
static void acc_async_event(Cpu *c) {   // Result AsyncContext::GetSystemEvent(os::SystemEvent *): signalled
    uint64_t ev = arg_x(c, 1);
    if (!ev) {
        ret_x(c, R_NO_NINTENDO_ACCOUNT);
        return;
    }
    kern_bind_system_event(c, ev, 0);
    kern_signal_system_event(ev);
    ret_x(c, 0);
}

// ---- time: the network clock is the PC's
static void net_clock_now(Cpu *c) { ret_x(c, (uint64_t)(int64_t)time(NULL)); }   // time_point now(): POSIX seconds
static void clock_task_event(Cpu *c) {   // os::SystemEvent *GetFinishEvent(): signalled, the clock is ready
    static uint64_t ev;
    if (!ev) ev = bound_event(c, 1);
    ret_x(c, ev);
}

// Result GetStandardSteadyClockCurrentTimespec(timespec *): the game installs it as musl's CLOCK_MONOTONIC source
// (sdk+0xaf0f8, mode 3), so std::chrono::steady_clock::now reads it; through the time service it failed and libc++
// threw system_error. The host's monotonic clock: the steady clock only promises monotonic time from an arbitrary
// epoch.
static void steady_timespec(Cpu *c) {
    int64_t *ts = arg_p(c, 0);
    uint64_t ns = plat_now_ns();
    if (ts) ts[0] = (int64_t)(ns / 1000000000ull), ts[1] = (int64_t)(ns % 1000000000ull);
    ret_x(c, 0);
}

// Result Get{Standard,Adjustable}UserSystemClockCurrentTimespec(timespec *): musl's CLOCK_REALTIME sources
// (sdk+0xaf040 installs one), behind std::chrono::system_clock::now. The PC's UTC time.
static void user_timespec(Cpu *c) {
    int64_t *ts = arg_p(c, 0);
    struct timespec now;
    if (!timespec_get(&now, TIME_UTC)) now.tv_sec = time(NULL), now.tv_nsec = 0;
    if (ts) ts[0] = (int64_t)now.tv_sec, ts[1] = (int64_t)now.tv_nsec;
    ret_x(c, 0);
}

// ---- fs: the host's file system takes no allocator and keeps no per-thread context
static uint8_t fs_context[0x10];
static void fs_get_context(Cpu *c) {   // const FsContext *GetCurrentThreadFsContext()
    static uint64_t ctx;
    if (!ctx) ctx = (uint64_t)(uintptr_t)mem_kernel_alloc(sizeof fs_context, 16);
    ret_x(c, ctx);
}

HOST_OVERRIDE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", vi_hdr_status);
HOST_OVERRIDE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", vi_paper_white);
HOST_OVERRIDE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", vi_vrr_supported);
HOST_OVERRIDE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", h_ok);
HOST_OVERRIDE("_ZN2nn2vi30QueryDisplayCurrentRefreshRateEPNS0_11RefreshRateEPbPKNS0_7DisplayE", vi_refresh_rate);
HOST_OVERRIDE("_ZN2nn2vi36SetDisplay120HzRefreshRateCapabilityEPNS0_7DisplayEb", h_ok);
HOST_OVERRIDE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", vi_ext_swap);
HOST_OVERRIDE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", h_ok);
HOST_OVERRIDE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", h_ok);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", h_ok);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", h_ok);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", h_ok);
HOST_OVERRIDE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", audio_list);
HOST_OVERRIDE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", audio_active);
HOST_OVERRIDE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", audio_channels);
HOST_OVERRIDE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", audio_set_volume);
HOST_OVERRIDE("_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE", audio_volume);
HOST_OVERRIDE("_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", audio_set_autotune);
HOST_OVERRIDE("_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv", audio_autotune);
HOST_OVERRIDE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", audio_notification);
// SDK 22 lists the virtual input devices (the Switch 2's microphone routing) through auddmg:u at boot: the host has
// no microphone (nxnative audio.md "No virtual audio input devices (SDK 22)", found on OoT)
HOST_OVERRIDE("_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", h_zero);
// the input streams the game would open on such a device (audin_open): none can be opened, and a stream never opened
// is closed (state 0), empty, and takes Stop, Purge and Close as nothing
HOST_OVERRIDE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", audin_open);
HOST_OVERRIDE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEii",
              audin_open);
HOST_OVERRIDE("_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", audin_start);
HOST_OVERRIDE("_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", h_ok);
HOST_OVERRIDE("_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE", h_ok);
HOST_OVERRIDE("_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", h_nothing);
HOST_OVERRIDE("_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE", h_zero);
HOST_OVERRIDE("_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", h_zero);
HOST_OVERRIDE("_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", h_zero);
HOST_OVERRIDE("_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb", audin_statistics);
HOST_OVERRIDE("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", jcms_handle);
HOST_OVERRIDE("_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", h_nothing);
HOST_OVERRIDE("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", h_nothing);
HOST_OVERRIDE("_ZN2nn3hid12FinalizeJcmsEv", h_nothing);
HOST_OVERRIDE("_ZN2nn3hid14FinalizeNmouseEv", h_nothing);
// Switch 2 hid calls whose SDK path opens the hid service itself instead of the nn::hid::detail calls ov_input.c
// serves: the gyroscope drift mode is the host's (motion_fusion.c), there is no Joy-Con 2 ("Ounce") rumble device,
// and no sensor-fusion handles (MacNativeGames input.c GAME_NS2 / GAME_BOTW, MIT)
HOST_OVERRIDE("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",
              h_nothing);
HOST_OVERRIDE("_ZN2nn3hid29IsOunceVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE", h_zero);
HOST_OVERRIDE("_ZN2nn3hid22GetSensorFusionHandlesEPNS0_18SensorFusionHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
              h_zero);
HOST_OVERRIDE("_ZN2nn3hid20FinalizeSensorFusionEv", h_nothing);
HOST_OVERRIDE("_ZN2nn3hid16GetNmouseHandlesEPNS0_12NmouseHandleEij", nmouse_handles);
HOST_OVERRIDE("_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE", nmouse_types);
HOST_OVERRIDE("_ZN2nn2oe23GetCurrentGraphicsScaleEv", oe_graphics_scale);
HOST_OVERRIDE("_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", h_nothing);
HOST_OVERRIDE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", oe_display_version);
HOST_OVERRIDE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", acc_no_account);
HOST_OVERRIDE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", acc_no_account);
HOST_OVERRIDE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", acc_no_account);
HOST_OVERRIDE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", acc_no_account);
HOST_OVERRIDE("_ZN2nn7account12AsyncContextC1Ev", h_nothing);
HOST_OVERRIDE("_ZN2nn7account12AsyncContextD1Ev", h_nothing);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", acc_async_event);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext9GetResultEv", acc_no_account);
HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock3nowEv", net_clock_now);
HOST_OVERRIDE("_ZN2nn4time37GetStandardSteadyClockCurrentTimespecEP8timespec", steady_timespec);
HOST_OVERRIDE("_ZN2nn4time41GetStandardUserSystemClockCurrentTimespecEP8timespec", user_timespec);
HOST_OVERRIDE("_ZN2nn4time43GetAdjustableUserSystemClockCurrentTimespecEP8timespec", user_timespec);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", h_nothing);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", h_nothing);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", h_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", h_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", h_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", clock_task_event);
HOST_OVERRIDE("_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE", h_nothing);
HOST_OVERRIDE("_ZN2nn2fs25GetCurrentThreadFsContextEv", fs_get_context);
HOST_OVERRIDE("_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE", h_nothing);
HOST_OVERRIDE("_ZN2nn3aoc17CountAddOnContentEv", h_zero);   // add-on content is not served yet
// Result RequestLoadNxFont(void *buffer, size_t size) (SDK 22): asks the system to load the Switch 1 shared fonts into
// the game's buffer. The host serves every shared font from its own memory (ov_system.c GetSharedFontAddress), so
// there is nothing to load.
HOST_OVERRIDE("_ZN2nn2pl17RequestLoadNxFontEPvm", h_ok);
#endif
