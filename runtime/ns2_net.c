// Networking of a Switch 2 game on an offline host (GAME_NS2, Rekarina). The online services report the network
// unavailable, so the game takes its offline paths; eventfds are served for real, because they are a local counter and
// OoT's network library (gRPC, started at boot) aborts without its wake-up eventfd.
//
// Behaviour ported from seedds/nxnative (MIT): dm/host/hle_net.c of its OoT bring-up (branch oot-wip,
// 10e5336: "eventfds served offline", the socket C interface, Poll / Fcntl / Close on eventfds, the guest errno a failed
// eventfd call sets) and MacNativeGames games/totk/runtime/offline_host.c (MIT; nifm, ssl, nsd, crypto). Only the calls
// that reach a service are replaced: the play report builders (Add, SetBuffer, GetBuffer ...) and the SHA-1 helpers are
// pure computation and run as lifted code.
#ifdef GAME_NS2
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define R_NIFM_UNAVAILABLE (110u | (300u << 9))
#define R_SSL_UNAVAILABLE (123u | (204u << 9))
#define R_NSD_UNAVAILABLE (141u | (2u << 9))
#define R_PREPO_UNAVAILABLE (100u | (129u << 9))

// Horizon's bsd service numbers errors as FreeBSD; the guest C library (musl) as Linux.
enum { SK_EBADF = 9, SK_EINVAL = 22, SK_EAGAIN = 35, SK_ENETDOWN = 50, SK_EMFILE = 24 };
enum { LIBC_EBADF = 9, LIBC_EAGAIN = 11, LIBC_EINVAL = 22, LIBC_EMFILE = 24 };
enum { EFD_BASE = 512, EFD_MAX = 64, EFD_SEMAPHORE = 1, EFD_NONBLOCK = 4 };
enum { SK_O_NONBLOCK = 4, SK_F_GETFD = 1, SK_F_SETFD = 2, SK_F_GETFL = 3, SK_F_SETFL = 4 };
enum { SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLRDNORM = 0x40, SK_POLLWRNORM = 4 };

static _Thread_local int sk_errno = SK_ENETDOWN;   // nn::socket::GetLastErrno of the calling thread

static struct { int used, nonblock, sem; uint64_t count; } efd[EFD_MAX];
static PlatMutex efd_mu = PLAT_MUTEX_INIT;
static PlatCond efd_cv;                             // broadcast on every change of a counter
static PlatOnce efd_once = PLAT_ONCE_INIT;
static void efd_init(void *p) { (void)p; plat_cond_init(&efd_cv); }

static int efd_slot(int fd) {   // caller holds efd_mu
    int i = fd - EFD_BASE;
    return i >= 0 && i < EFD_MAX && efd[i].used ? i : -1;
}

// -1 with the socket error code and the guest's errno (gRPC reads errno after a failed eventfd read or write).
static int sk_err(Cpu *c, int sk, int libc) {
    sk_errno = sk;
    static uint64_t loc;
    if (!loc) loc = ldr_symbol("__errno_location");
    if (loc && c) {
        int32_t *e = (int32_t *)(uintptr_t)rt_call(c, loc, 0);
        if (e) *e = libc;
    }
    return -1;
}
static int sk_down(void) {
    sk_errno = SK_ENETDOWN;
    return -1;
}

// Waits for a change of any counter, at most ms (< 0 or > 100: 100 ms slices, as an endless wait); 0 when the time is
// up. Caller holds efd_mu; the guest thread gives its virtual core up while it waits.
static int efd_wait(int64_t ms) {
    int away = kern_core_away();
    int r = plat_cond_wait_ns(&efd_cv, &efd_mu, (uint64_t)(ms < 0 || ms > 100 ? 100 : ms) * 1000000ull);
    kern_core_back(&away);
    return r;
}

static void idle_ms(int64_t ms) {   // a wait on nothing: the timeout, or 100 ms for an endless one
    int away = kern_core_away();
    plat_sleep_ms((uint32_t)(ms < 0 || ms > 100 ? 100 : ms));
    kern_core_back(&away);
}

// int EventFd(uint64_t initval, EventFdFlags flags)
static void h_eventfd(Cpu *c) {
    plat_once(&efd_once, efd_init, NULL);
    uint64_t v = arg_x(c, 0);
    int f = (int)arg_x(c, 1), i = 0;
    plat_mutex_lock(&efd_mu);
    while (i < EFD_MAX && efd[i].used) i++;
    if (i < EFD_MAX) {
        efd[i].used = 1, efd[i].count = v;
        efd[i].nonblock = (f & EFD_NONBLOCK) != 0, efd[i].sem = (f & EFD_SEMAPHORE) != 0;
    }
    plat_mutex_unlock(&efd_mu);
    if (i == EFD_MAX) {
        rt_log("ns2: more than %d eventfds", EFD_MAX);
        ret_x(c, (uint32_t)sk_err(c, SK_EMFILE, LIBC_EMFILE));
        return;
    }
    static int once;
    if (!once++) rt_log("ns2: eventfd served by the host (first: descriptor %d, flags %#x)", EFD_BASE + i, f);
    ret_x(c, (uint32_t)(EFD_BASE + i));
}

// int EventFdRead(int fd, uint64_t *value): takes the counter, or 1 with the semaphore flag
static void h_eventfd_read(Cpu *c) {
    plat_once(&efd_once, efd_init, NULL);
    int fd = (int)arg_x(c, 0), i;
    uint64_t *out = arg_p(c, 1), take = 0;
    plat_mutex_lock(&efd_mu);
    while ((i = efd_slot(fd)) >= 0 && !efd[i].count && !efd[i].nonblock) efd_wait(-1);
    int r = i < 0 ? -2 : !efd[i].count ? -1 : 0;
    if (!r) {
        take = efd[i].sem ? 1 : efd[i].count;
        efd[i].count -= take;
        plat_cond_broadcast(&efd_cv);
    }
    plat_mutex_unlock(&efd_mu);
    if (!r && out) *out = take;
    ret_x(c, (uint32_t)(r == -2 ? sk_err(c, SK_EBADF, LIBC_EBADF) : r ? sk_err(c, SK_EAGAIN, LIBC_EAGAIN) : 0));
}

// int EventFdWrite(int fd, uint64_t value): adds to the counter
static void h_eventfd_write(Cpu *c) {
    plat_once(&efd_once, efd_init, NULL);
    int fd = (int)arg_x(c, 0), i;
    uint64_t v = arg_x(c, 1);
    if (v == UINT64_MAX) {
        ret_x(c, (uint32_t)sk_err(c, SK_EINVAL, LIBC_EINVAL));
        return;
    }
    plat_mutex_lock(&efd_mu);
    while ((i = efd_slot(fd)) >= 0 && efd[i].count > UINT64_MAX - 1 - v && !efd[i].nonblock) efd_wait(-1);
    int r = i < 0 ? -2 : efd[i].count > UINT64_MAX - 1 - v ? -1 : 0;
    if (!r) {
        efd[i].count += v;
        plat_cond_broadcast(&efd_cv);
    }
    plat_mutex_unlock(&efd_mu);
    ret_x(c, (uint32_t)(r == -2 ? sk_err(c, SK_EBADF, LIBC_EBADF) : r ? sk_err(c, SK_EAGAIN, LIBC_EAGAIN) : 0));
}

typedef struct { int32_t fd; int16_t events, revents; } SkPollFd;

static int efd_ready(int i, int ev) {   // caller holds efd_mu
    return (ev & (SK_POLLIN | SK_POLLRDNORM) & (efd[i].count ? ~0 : 0)) |
           (ev & (SK_POLLOUT | SK_POLLWRNORM) & (efd[i].count < UINT64_MAX - 1 ? ~0 : 0));
}

// Poll: eventfds are waited on; a descriptor the host never made reports nothing and the timeout is waited out, as a
// console's socket with nothing to say does (nxnative dm_sk_poll).
static void h_poll(Cpu *c) {
    plat_once(&efd_once, efd_init, NULL);
    SkPollFd *p = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int ms = (int)arg_x(c, 2), any = 0, ready = 0;
    plat_mutex_lock(&efd_mu);
    for (uint64_t i = 0; i < n; i++)
        if (p[i].fd >= 0 && efd_slot(p[i].fd) >= 0) any = 1;
    if (!any) {
        plat_mutex_unlock(&efd_mu);
        for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
        idle_ms(ms);
        ret_x(c, 0);
        return;
    }
    for (int64_t left = ms;;) {
        ready = 0;
        for (uint64_t i = 0; i < n; i++) {
            int k = p[i].fd >= 0 ? efd_slot(p[i].fd) : -1;
            p[i].revents = (int16_t)(k >= 0 ? efd_ready(k, p[i].events) : 0);
            ready += p[i].revents != 0;
        }
        if (ready || !left || !efd_wait(left)) break;
        if (left > 0) left = left > 100 ? left - 100 : 0;
    }
    plat_mutex_unlock(&efd_mu);
    ret_x(c, (uint32_t)ready);
}

// int Fcntl(int fd, int cmd, ...): O_NONBLOCK of an eventfd; close-on-exec is accepted
static void h_fcntl(Cpu *c) {
    int fd = (int)arg_x(c, 0), cmd = (int)arg_x(c, 1), r = 0;
    long a = (long)arg_x(c, 2);
    plat_mutex_lock(&efd_mu);
    int i = efd_slot(fd);
    if (i >= 0) {
        if (cmd == SK_F_GETFL) r = efd[i].nonblock ? SK_O_NONBLOCK : 0;
        else if (cmd == SK_F_SETFL) efd[i].nonblock = (a & SK_O_NONBLOCK) != 0;
        else if (cmd != SK_F_GETFD && cmd != SK_F_SETFD) r = -2;
    }
    plat_mutex_unlock(&efd_mu);
    ret_x(c, (uint32_t)(i < 0 ? sk_down() : r == -2 ? sk_err(c, SK_EINVAL, LIBC_EINVAL) : r));
}

static void h_close(Cpu *c) {
    int fd = (int)arg_x(c, 0);
    plat_mutex_lock(&efd_mu);
    int i = efd_slot(fd);
    if (i >= 0) efd[i].used = 0;
    plat_mutex_unlock(&efd_mu);
    ret_x(c, (uint32_t)(i >= 0 ? 0 : sk_down()));
}

static void h_sk_fail(Cpu *c) { ret_x(c, (uint64_t)(int64_t)sk_down()); }   // every socket call: -1, ENETDOWN
static void h_ok(Cpu *c) { ret_x(c, 0); }
static void h_nothing(Cpu *c) { (void)c; }
static void h_true(Cpu *c) { ret_x(c, 1); }
static void h_false(Cpu *c) { ret_x(c, 0); }
static void h_last_errno(Cpu *c) { ret_x(c, (uint32_t)sk_errno); }
static void h_set_errno(Cpu *c) { sk_errno = (int)arg_x(c, 0); }
static void h_gai_fail(Cpu *c) {   // GetAddrInfo(node, service, hints, res): EAI_FAIL, *res = NULL
    void **res = arg_p(c, 3);
    if (res) *res = NULL;
    ret_x(c, 4);
}
static void h_gai_opt_fail(Cpu *c) {   // GetAddrInfoWithOptions(node, service, hints, res, options)
    void **res = arg_p(c, 3);
    if (res) *res = NULL;
    ret_x(c, 4);
}
static void h_gai_strerror(Cpu *c) {
    static uint64_t text;
    if (!text) {
        static const char msg[] = "Non-recoverable failure in name resolution";
        char *p = mem_kernel_alloc(sizeof msg, 16);
        memcpy(p, msg, sizeof msg);
        text = (uint64_t)(uintptr_t)p;
    }
    ret_x(c, text);
}
static void h_htonl(Cpu *c) { ret_x(c, __builtin_bswap32((uint32_t)arg_x(c, 0))); }
static void h_htons(Cpu *c) { ret_x(c, __builtin_bswap16((uint16_t)arg_x(c, 0))); }

// ---- nifm, ssl, nsd, crypto, play reports
static void h_nifm_unavailable(Cpu *c) { ret_x(c, R_NIFM_UNAVAILABLE); }
static void h_nifm_submit_wait(Cpu *c) {
    (void)c;
    static int once;
    if (!once++) rt_log("ns2: network request refused (offline host)");
}
static void h_ssl_unavailable(Cpu *c) { ret_x(c, R_SSL_UNAVAILABLE); }
static void h_nsd_unavailable(Cpu *c) { ret_x(c, R_NSD_UNAVAILABLE); }
static void h_nsd_environment(Cpu *c) {   // Result GetEnvironmentIdentifier(EnvironmentIdentifier *): "lp1", production
    char *out = arg_p(c, 0);
    if (out) memset(out, 0, 8), memcpy(out, "lp1", 3);
    ret_x(c, 0);
}
static void h_random_bytes(Cpu *c) {
    void *out = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    if (out && n && !plat_random(out, n)) rt_fatal("ns2: no random bytes from the system");
}
static void h_prepo_event(Cpu *c) {   // os::SystemEvent *GetOverSaveFrequencyLimitEvent(): bound, never signalled
    static uint64_t ev;
    if (!ev) {
        ev = (uint64_t)(uintptr_t)mem_kernel_alloc(0x80, 16);
        memset((void *)(uintptr_t)ev, 0, 0x80);
        kern_bind_system_event(c, ev, 0);
    }
    ret_x(c, ev);
}
static void h_prepo_completion(Cpu *c) {   // AsyncContext::GetCompletionEvent(SystemEvent *): bound and signalled
    uint64_t ev = arg_x(c, 1);
    if (ev) {
        kern_bind_system_event(c, ev, 0);
        kern_signal_system_event(ev);
    }
}
static void h_prepo_unavailable(Cpu *c) { ret_x(c, R_PREPO_UNAVAILABLE); }

// nn::socket (C++)
HOST_OVERRIDE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", h_ok);
HOST_OVERRIDE("_ZN2nn6socket8FinalizeEv", h_ok);
HOST_OVERRIDE("_ZN2nn6socket13IsInitializedEv", h_true);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_fail);
HOST_OVERRIDE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", h_eventfd);
HOST_OVERRIDE("_ZN2nn6socket11EventFdReadEiPm", h_eventfd_read);
HOST_OVERRIDE("_ZN2nn6socket12EventFdWriteEim", h_eventfd_write);
// the socket C interface (gRPC)
HOST_OVERRIDE("nnsocketPoll", h_poll);
HOST_OVERRIDE("nnsocketFcntl", h_fcntl);
HOST_OVERRIDE("nnsocketClose", h_close);
HOST_OVERRIDE("nnsocketGetLastErrno", h_last_errno);
HOST_OVERRIDE("nnsocketSetLastErrno", h_set_errno);
HOST_OVERRIDE("nnsocketGetAddrInfo", h_gai_fail);
HOST_OVERRIDE("nnsocketGetAddrInfoWithOptions", h_gai_opt_fail);
HOST_OVERRIDE("nnsocketFreeAddrInfo", h_nothing);
HOST_OVERRIDE("nnsocketGAIStrError", h_gai_strerror);
HOST_OVERRIDE("nnsocketInetHtonl", h_htonl);
HOST_OVERRIDE("nnsocketInetNtohl", h_htonl);
HOST_OVERRIDE("nnsocketInetHtons", h_htons);
HOST_OVERRIDE("nnsocketInetNtohs", h_htons);
HOST_OVERRIDE("nnsocketSocket", h_sk_fail);
HOST_OVERRIDE("nnsocketAccept", h_sk_fail);
HOST_OVERRIDE("nnsocketBind", h_sk_fail);
HOST_OVERRIDE("nnsocketConnect", h_sk_fail);
HOST_OVERRIDE("nnsocketGetPeerName", h_sk_fail);
HOST_OVERRIDE("nnsocketGetSockName", h_sk_fail);
HOST_OVERRIDE("nnsocketGetSockOpt", h_sk_fail);
HOST_OVERRIDE("nnsocketSetSockOpt", h_sk_fail);
HOST_OVERRIDE("nnsocketListen", h_sk_fail);
HOST_OVERRIDE("nnsocketRecv", h_sk_fail);
HOST_OVERRIDE("nnsocketRecvMsg", h_sk_fail);
HOST_OVERRIDE("nnsocketSend", h_sk_fail);
HOST_OVERRIDE("nnsocketSendMsg", h_sk_fail);
HOST_OVERRIDE("nnsocketShutdown", h_sk_fail);
HOST_OVERRIDE("nnsocketResolverGetOption", h_sk_fail);
HOST_OVERRIDE("nnsocketResolverSetOption", h_sk_fail);
// nifm: no network
HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", h_ok);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", h_false);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", h_nothing);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", h_nifm_unavailable);
HOST_OVERRIDE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", h_nifm_submit_wait);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", h_false);   // bool: no system screen resolved it
// ssl: the library starts, nothing connects
HOST_OVERRIDE("nnsslInitialize", h_ok);
HOST_OVERRIDE("nnsslFinalize", h_ok);
HOST_OVERRIDE("nnsslContextDestroy", h_ok);
HOST_OVERRIDE("nnsslContextGetContextId", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionCreate", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionDestroy", h_ok);
HOST_OVERRIDE("nnsslConnectionDoHandshakeWithCertBuffer", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionGetConnectionId", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionGetNextAlpnProto", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionGetServerCertDetail", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionGetVerifyCertError", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionPeek", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionPending", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionRead", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionSetHostName", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionSetIoMode", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionSetNextAlpnProto", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionSetOption", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionSetServerCertBuffer", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionSetSessionCacheMode", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionSetSocketDescriptor", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionSetVerifyOption", h_ssl_unavailable);
HOST_OVERRIDE("nnsslConnectionWrite", h_ssl_unavailable);
// ssl, C++ interface (MacNativeGames offline_host.c): the library starts, a context cannot be created
HOST_OVERRIDE("_ZN2nn3ssl10InitializeEv", h_ok);
HOST_OVERRIDE("_ZN2nn3ssl8FinalizeEv", h_ok);
HOST_OVERRIDE("_ZN2nn3ssl7ContextC1Ev", h_nothing);
HOST_OVERRIDE("_ZN2nn3ssl7ContextD1Ev", h_nothing);
HOST_OVERRIDE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", h_ssl_unavailable);
HOST_OVERRIDE("_ZN2nn3ssl7Context7DestroyEv", h_ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", h_ssl_unavailable);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", h_ssl_unavailable);
HOST_OVERRIDE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", h_ssl_unavailable);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_unavailable);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj",
              h_ssl_unavailable);
// nsd, crypto
HOST_OVERRIDE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", h_nsd_environment);
HOST_OVERRIDE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", h_nsd_unavailable);
HOST_OVERRIDE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", h_random_bytes);
// play reports: built by the lifted SDK, never sent (Save, RequestImmediateTransmission and AddInternetConnectionStatus
// are ov_system.c's, as for TotK)
// AsyncContext PlayReport::Send(const Uid &): the context comes back through x8 and the game waits on its completion
// event, then reads its result (OoT at its first gameplay transition, nxnative services.md "An
// asynchronous play report opens prepo:u"). Every method of the context the game calls is the host's below, so the
// storage at x8 is left as it is: a completed send that succeeded, as nxnative answers it.
HOST_OVERRIDE("_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE", h_nothing);
HOST_OVERRIDE("_ZN2nn5prepo30GetOverSaveFrequencyLimitEventEv", h_prepo_event);
HOST_OVERRIDE("_ZN2nn5prepo12AsyncContext18GetCompletionEventEPNS_2os11SystemEventE", h_prepo_completion);
HOST_OVERRIDE("_ZN2nn5prepo12AsyncContext9GetResultEv", h_ok);
HOST_OVERRIDE("_ZN2nn5prepo12AsyncContextD1Ev", h_nothing);
#endif
