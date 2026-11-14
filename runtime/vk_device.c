// Vulkan instance, physical device choice, logical device, VMA, timeline semaphore and the validation messenger.
// Ported from LAS Recomp, runtime/nvn/vulkan/device.c:33-330
// (https://github.com/peiikai/LASRecomp @ da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai),
// without SDL. The VMA implementation is runtime/vk_vma.cpp (VMA is C++ only).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "platform/platform.h"
#include "vk_dlaa.h"
#include "vk_fg.h"
#include "vk_dlssg.h"

static uint32_t g_gfx_queues = 1;   // queues the graphics family offers (pick_device)

static VkCtx g_vk;
static int g_ready;
static PlatMutex g_lock = PLAT_MUTEX_INIT;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    int err = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (err) plat_inc32(&g_vk.validation_errors);
    static volatile int32_t logged;
    if (plat_inc32(&logged) <= 500) rt_log("vk validation %s: %s", err ? "error" : "warning", d->pMessage);
    return VK_FALSE;
}

// ---- address bindings (VK_EXT_device_address_binding_report; TOTK_VK_ADDR_REPORT=0 turns it off)
// A device fault gives the GPU address of the bad access and nothing else. Pool buffers are known by their device
// address (vk_memory.c vk_mem_fault_report); images and the runtime's other buffers are not, and the faults of Castle
// Town Market all read "an address in no pool buffer" (2026-11-12). With this extension the driver reports every image
// and buffer's address range when it binds and unbinds memory: the last AB_N of those events are kept, and a fault
// lists the ones around its address with their age and what the runtime called the object (vk_addr_label).
#define AB_N (1u << 17)
#define AL_N (1u << 16)
typedef struct { uint64_t base, size, handle, t_ns; uint32_t type; uint8_t bind; } AddrEv;
typedef struct { uint64_t handle; char text[56]; } AddrLabel;
static AddrEv *ab_ring;
static AddrLabel *ab_labels;
static uint32_t ab_next;
static int ab_want = -1;
static PlatMutex ab_lock = PLAT_MUTEX_INIT;
static VkDebugUtilsMessengerEXT ab_messenger;

static int addr_report_wanted(void) {
    if (ab_want < 0) {
        const char *e = cfg_get("TOTK_VK_ADDR_REPORT");
        ab_want = !(e && *e == '0');
    }
    return ab_want;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL addr_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)sev, (void)user;
    if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !ab_ring) return VK_FALSE;
    for (const VkBaseInStructure *p = d->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) continue;
        const VkDeviceAddressBindingCallbackDataEXT *b = (const VkDeviceAddressBindingCallbackDataEXT *)p;
        plat_mutex_lock(&ab_lock);
        ab_ring[ab_next++ % AB_N] = (AddrEv){b->baseAddress, b->size, d->objectCount ? d->pObjects[0].objectHandle : 0, plat_now_ns(),
                                             d->objectCount ? (uint32_t)d->pObjects[0].objectType : 0,
                                             b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT};
        plat_mutex_unlock(&ab_lock);
    }
    return VK_FALSE;
}

void vk_addr_label(uint64_t handle, const char *fmt, ...) {
    if (!ab_labels || !handle) return;
    AddrLabel l = {.handle = handle};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l.text, sizeof l.text, fmt, ap);
    va_end(ap);
    plat_mutex_lock(&ab_lock);
    ab_labels[(handle * 0x9E3779B97F4A7C15ull) >> 48] = l;   // one label per bucket: a later object may take it
    plat_mutex_unlock(&ab_lock);
}

// The binding events whose range holds the fault's address range, newest first (at most 12 lines).
static void addr_fault_report(uint64_t addr, uint64_t precision) {
    if (!ab_ring) return;
    uint64_t lo = precision ? addr & ~(precision - 1) : addr, hi = lo + (precision ? precision : 1), now = plat_now_ns();
    {   // the report comes when a call fails, which can be seconds after the GPU stopped: the times below count from here
        uint64_t vk_mem_lost_seen_ns(void);
        uint64_t seen = vk_mem_lost_seen_ns();
        if (seen) rt_log("vk: fault address %#llx: the device was first seen lost %.1f ms before the report",
                         (unsigned long long)addr, (now - seen) / 1e6);
    }
    int shown = 0, nwho = 0;
    uint64_t who[3];
    plat_mutex_lock(&ab_lock);
    uint32_t n = ab_next < AB_N ? ab_next : AB_N;
    for (uint32_t k = 0; k < n && shown < 12; k++) {
        const AddrEv *e = &ab_ring[(ab_next - 1 - k) % AB_N];
        if (e->base >= hi || e->base + e->size <= lo) continue;
        const AddrLabel *l = &ab_labels[(e->handle * 0x9E3779B97F4A7C15ull) >> 48];
        rt_log("vk: fault address %#llx: %s %#llx (%s) %s %#llx+%#llx %.1f ms before the report",
               (unsigned long long)addr, e->type == VK_OBJECT_TYPE_IMAGE ? "image" : e->type == VK_OBJECT_TYPE_BUFFER ? "buffer" : "object",
               (unsigned long long)e->handle, l->handle == e->handle ? l->text : "no label", e->bind ? "bound at" : "UNBOUND from",
               (unsigned long long)e->base, (unsigned long long)e->size, (now - e->t_ns) / 1e6);
        shown++;
        if (e->type == VK_OBJECT_TYPE_IMAGE && nwho < 3 && (!nwho || who[nwho - 1] != e->handle)) who[nwho++] = e->handle;
    }
    plat_mutex_unlock(&ab_lock);
    // DEBUG-WHONAMES: TOTK_FAULT_WHO=1: what still names an image that lay at the address (the tables take their
    // locks, which a thread stopped by the lost device may hold: off by default so the report always ends)
    const char *fw = getenv("TOTK_FAULT_WHO");
    for (int i = 0; fw && *fw == '1' && i < nwho; i++) {
        void vk_texture_who_names(uint64_t image);
        void vk_tpool_who_names(uint64_t image);
        void vk_exec_draw_who_names(uint64_t image);
        void vk_exec_who_names(uint64_t image);
        rt_log("vk: fault address: who still names image %#llx:", (unsigned long long)who[i]);
        vk_tpool_who_names(who[i]);
        vk_exec_draw_who_names(who[i]);
        vk_exec_who_names(who[i]);
        vk_texture_who_names(who[i]);
    }
    if (!shown)
        rt_log("vk: fault address %#llx: no image or buffer was bound there in the last %u binding events", (unsigned long long)addr, n);
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int layer_present(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *l = calloc(n ? n : 1, sizeof *l);
    vkEnumerateInstanceLayerProperties(&n, l);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(l[i].layerName, name)) found = 1;
    free(l);
    return found;
}

static int instance_ext_present(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateInstanceExtensionProperties(NULL, &n, e);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) found = 1;
    free(e);
    return found;
}

// Mesa's drivers (RADV on Linux and the Steam Deck) keep a shader cache of their own beside our pipeline cache, by
// default in ~/.cache and up to 1 GB: about 35 MB in a minute of the title screen. A Deck's /home is small and the game
// usually sits on the SD card, so unless MESA_SHADER_CACHE_DIR names a folder, it goes to mesa-cache beside the exe.
static void mesa_cache_beside_exe(void) {
    char dir[PLAT_PATH_MAX * 2];
    if (plat_getenv("MESA_SHADER_CACHE_DIR", dir, sizeof dir)) return;
    int len = plat_exe_path(dir, sizeof dir - 16);
    char *cut = len ? plat_last_sep(dir) : NULL;
    if (!cut) return;
    strcpy(cut + 1, "mesa-cache");
    plat_setenv("MESA_SHADER_CACHE_DIR", dir);
}

// NVIDIA's driver keeps its compiled pipelines in one per-user folder, capped at about 1 GB and shared by every Vulkan and
// OpenGL program. Once that is full nothing more is stored: the 42,177 shaders tools/oot_prepare_shaders.py builds never
// reached it, and pipelines met in play compiled from nothing on every run, for seconds at a time (a 4.9 s frame after
// picking up a rock). Each of these is left alone when the user has set it. TOTK_NV_CACHE=0 keeps the driver's own folder.
// main calls this first: the window system loads the driver before the instance exists, and the driver reads these once.
void vk_nvidia_cache_beside_exe(void) {
    char dir[PLAT_PATH_MAX * 2], tmp[32];
    if (plat_getenv("TOTK_NV_CACHE", tmp, sizeof tmp) && tmp[0] == '0') return;
    if (!plat_getenv("__GL_SHADER_DISK_CACHE_PATH", dir, sizeof dir)) {
        int len = plat_exe_path(dir, sizeof dir - 16);
        char *cut = len ? plat_last_sep(dir) : NULL;
        if (!cut) return;
        strcpy(cut + 1, "nv-cache");
        plat_file_mkdir_p(dir);   // the driver ignores a path that does not exist
        plat_setenv("__GL_SHADER_DISK_CACHE_PATH", dir);
    }
    if (!plat_getenv("__GL_SHADER_DISK_CACHE", tmp, sizeof tmp)) plat_setenv("__GL_SHADER_DISK_CACHE", "1");
    if (!plat_getenv("__GL_SHADER_DISK_CACHE_SIZE", tmp, sizeof tmp)) plat_setenv("__GL_SHADER_DISK_CACHE_SIZE", "21474836480");
    if (!plat_getenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", tmp, sizeof tmp)) plat_setenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1");
}

static int create_instance(void) {
    vk_dlssg_early();   // DLSS Frame Generation: Streamline before any Vulkan call of the process (its slInit asks so)
    mesa_cache_beside_exe();
    const char *ext[12];
    uint32_t n_ext = (uint32_t)gfx_window_vk_extensions(ext, 7);
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    int validate = cfg_bool("TOTK_VK_VALIDATION");
    if (validate && !layer_present(layers[0])) {
        rt_log("vk: TOTK_VK_VALIDATION set but %s is not installed; continuing without", layers[0]);
        validate = 0;
    }
    int utils = instance_ext_present(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validate || ((vk_dlssg_wanted() || addr_report_wanted()) && utils)) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (vk_hdr_wanted() && instance_ext_present(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))   // HDR10 surface formats
        ext[n_ext++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
    vk_dlaa_instance_exts(ext, &n_ext, 12);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = RT_PORT_NAME,
                             .pEngineName = RT_PORT_NAME, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    rdoc_init();
    VkResult r = vk_dlssg_create_instance(&ci, &g_vk.instance);
    if (r != VK_SUCCESS) { rt_log("vk: vkCreateInstance failed (%d); is a Vulkan 1.3 driver installed?", (int)r); return 0; }
    g_vk.validation = validate;
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.instance, &mi, NULL, &g_vk.messenger);
    }
    if (addr_report_wanted() && utils) {   // the binding reports come as messages of their own type
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
            .pfnUserCallback = addr_cb};
        if (!f || f(g_vk.instance, &mi, NULL, &ab_messenger) != VK_SUCCESS) ab_want = 0;
    } else {
        ab_want = 0;
    }
    return 1;
}

static const char *const k_required_ext[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};

// Name of the first missing required extension, or NULL.
static const char *missing_required(VkPhysicalDevice pd) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    const char *miss = NULL;
    for (size_t i = 0; i < sizeof k_required_ext / sizeof *k_required_ext && !miss; i++)
        if (!has_ext(e, n, k_required_ext[i])) miss = k_required_ext[i];
    free(e);
    return miss;
}

// Graphics+compute family that can present to the window.
static uint32_t find_gfx_family(VkPhysicalDevice pd, const VkQueueFamilyProperties *qp, uint32_t nq) {
    for (uint32_t i = 0; i < nq; i++)
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            gfx_window_vk_presentable(g_vk.instance, pd, i))
            return i;
    return UINT32_MAX;
}

static int name_has(const char *s, const char *sub) {
    for (size_t n = strlen(sub); *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return 0;
}

// First discrete GPU with Vulkan 1.3, the required extensions and a graphics+present queue; TOTK_GPU_INDEX (index) or
// TOTK_GPU_NAME (case-insensitive part of the name) overrides.
static int pick_device(void) {
    uint32_t n = 0;
    vk_dlssg_enumerate(g_vk.instance, &n, NULL);
    if (!n) { rt_log("vk: no Vulkan device"); return 0; }
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vk_dlssg_enumerate(g_vk.instance, &n, pd);
    const char *e = cfg_get("TOTK_GPU_INDEX");   // no fixed default: the best device when unset
    int want = e && *e ? atoi(e) : -1;
    const char *name = cfg_get("TOTK_GPU_NAME");
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        rt_log("vk: gpu %u: %s (type %d, api %u.%u.%u)", i, p.deviceName, (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (want >= 0 && (int)i != want) continue;
        if (name && *name && !name_has(p.deviceName, name)) continue;
        if (p.apiVersion < VK_API_VERSION_1_3) { rt_log("vk: gpu %u skipped: Vulkan below 1.3", i); continue; }
        const char *miss = missing_required(pd[i]);
        if (miss) { rt_log("vk: gpu %u skipped: missing extension %s", i, miss); continue; }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, NULL);
        VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, qp);
        uint32_t fam = find_gfx_family(pd[i], qp, nq);
        free(qp);
        if (fam == UINT32_MAX) { rt_log("vk: gpu %u skipped: no graphics+compute+present queue family", i); continue; }
        int score = (int)i == want ? 100 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) {
        if (want >= 0) rt_log("vk: TOTK_GPU_INDEX=%d is not usable (see above)", want);
        else if (name && *name) rt_log("vk: no usable gpu matches TOTK_GPU_NAME=%s (see above)", name);
        else rt_log("vk: no device with Vulkan 1.3 and the required extensions (see above)");
        free(pd);
        return 0;
    }
    g_vk.phys = pd[best];
    g_vk.device_index = best;
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.mem_props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.gfx_family = find_gfx_family(g_vk.phys, qp, nq);
    g_gfx_queues = qp[g_vk.gfx_family].queueCount;
    g_vk.xfer_family = g_vk.gfx_family;
    g_vk.awb_family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)   // a compute-only family (async compute; the async write-back runs there)
        if ((qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            g_vk.awb_family = i;
            break;
        }
    for (uint32_t i = 0; i < nq; i++)   // a family that does transfer only (async DMA engine)
        if ((qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            g_vk.xfer_family = i;
            break;
        }
    free(qp);
    rt_log("vk: using gpu %d: %s", best, g_vk.props.deviceName);
    vk_mem_class_log(&g_vk.props, &g_vk.mem_props);
    return 1;
}

typedef struct {
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids;
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb;
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceRobustness2FeaturesEXT rob2;
    VkPhysicalDeviceFaultFeaturesEXT fault;
    VkPhysicalDeviceAddressBindingReportFeaturesEXT abr;
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pep;   // DEBUG-PIPESTATS
    VkPhysicalDeviceCooperativeMatrixFeaturesNV cm;
#ifdef GAME_NS2
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs;
#endif
} FeatureChain;

// The result guard's wait (memory.c): a guest thread gives its virtual core up while its access waits for the GPU.
static int guard_wait(uint64_t fence) {
    if (fence > vk_sync_fence_issued()) return 0;   // not submitted yet (the replay touching its own pages): no wait
    int away = kern_core_away();
    int ok = vk_sync_fence_wait(fence, 500000000ull);   // half a second at most, then the page opens as before
    kern_core_back(&away);
    return ok;
}

static uint64_t result_frame(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

// TOTK_SECTIONS: VMA's vkAllocateMemory goes through this (thread, size, time); off, VMA calls the driver directly.
static VKAPI_ATTR VkResult VKAPI_CALL timed_alloc(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a,
                                                  VkDeviceMemory *m) {
    uint64_t t0 = sec_clock();
    VkResult r = vkAllocateMemory(d, i, a, m);
    sec_alloc_note(i->allocationSize, i->memoryTypeIndex, sec_clock() - t0);
    return r;
}

static int create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
#ifdef GAME_NS2
    const char *ext[53];   // one more: VK_EXT_mesh_shader (and one for DEBUG-PIPESTATS, one for cooperative matrices)
#else
    const char *ext[52];
#endif
    uint32_t n_ext = 0;
    for (size_t i = 0; i < sizeof k_required_ext / sizeof *k_required_ext; i++) ext[n_ext++] = k_required_ext[i];

    FeatureChain *c = calloc(1, sizeof *c);
    void **tail = &c->f2.pNext;
#define CHAIN(member, stype) do { c->member.sType = stype; *tail = &c->member; tail = &c->member.pNext; } while (0)
    c->f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    CHAIN(f11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    CHAIN(f12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    CHAIN(f13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    CHAIN(clip, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT);
#define OPTIONAL_EXT(flag, name, member, stype) do { \
        if (has_ext(ep, ne, name)) { g_vk.flag = 1; ext[n_ext++] = name; CHAIN(member, stype); } } while (0)
    OPTIONAL_EXT(has_custom_border_color, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, cbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
    OPTIONAL_EXT(has_eds, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, eds, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
    OPTIONAL_EXT(has_eds2, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, eds2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT);
    OPTIONAL_EXT(has_eds3, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    OPTIONAL_EXT(has_vertex_input_dynamic_state, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, vids, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT);
    OPTIONAL_EXT(has_conditional_rendering, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, cond, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT);
    OPTIONAL_EXT(has_transform_feedback, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, xfb, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT);
    OPTIONAL_EXT(has_provoking_vertex, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, pv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT);
    OPTIONAL_EXT(has_robustness2, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, rob2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
    OPTIONAL_EXT(has_device_fault, VK_EXT_DEVICE_FAULT_EXTENSION_NAME, fault, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT);
    if (ab_want > 0 && has_ext(ep, ne, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME)) {
        ext[n_ext++] = VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME;
        CHAIN(abr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT);
        ab_ring = calloc(AB_N, sizeof *ab_ring), ab_labels = calloc(AL_N, sizeof *ab_labels);
        if (!ab_ring || !ab_labels) free(ab_ring), free(ab_labels), ab_ring = NULL, ab_labels = NULL;
    }
    // Ryujinx's SPIR-V declares fragment shader interlock whenever the host reports it (vk_pipeline.c spirv_fixup).
    OPTIONAL_EXT(has_fragment_shader_interlock, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, fsi,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT);
    // Feedback loops (vk.h VkCtx.feedback): the layout, and barriers inside dynamic rendering
    int has_fbl = 0, has_lr = 0;
    if (has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
        has_ext(ep, ne, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME)) {
        has_fbl = has_lr = 1;
        ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
        CHAIN(fbl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT);
        CHAIN(lr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR);
    }
#ifdef GAME_NS2
    // NVN2 task and mesh shaders (vk_pipeline.c vk_pipeline_stages): VK_EXT_mesh_shader when the GPU has it
    OPTIONAL_EXT(has_mesh_shader, VK_EXT_MESH_SHADER_EXTENSION_NAME, mesh, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT);
    // The foliage's far shadow cascades shade one fragment per 2x2 pixels (vk_exec_draw.c mesh_shading_rate)
    OPTIONAL_EXT(has_vrs, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, vrs, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR);
#endif
    vk_dlaa_device_exts(ep, ne, ext, &n_ext, 40);   // NGX's; the eight slots after it stay for the eight below
    // FSR 3.1 (vk_ffx.cpp): its Vulkan back end looks vkGetBufferMemoryRequirements2KHR up by that name, which resolves
    // only with the extension enabled (the core function has no suffix); without it the back end called address 0
    if ((vk_dlaa_fsr3() || vk_fg_wanted() || vk_dlssg_wanted()) && has_ext(ep, ne, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
        ext[n_ext++] = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME;
    // DLSS Frame Generation (vk_dlssg.c): Streamline's plugins look functions of extensions core in Vulkan 1.3 up by
    // their suffixed names too, which resolve only with the extension itself enabled
    if (vk_dlssg_wanted()) {
        static const char *const promoted[] = {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                               VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
                                               VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                               VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
                                               VK_KHR_MAINTENANCE_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_3_EXTENSION_NAME};
        for (size_t i = 0; i < sizeof promoted / sizeof *promoted && n_ext < 56; i++)
            if (has_ext(ep, ne, promoted[i])) ext[n_ext++] = promoted[i];
    }
    // Float atomic add (gfx_shader_float_atomics): the translator's RED / ATOMG .ADD.F32 become one atomic, not a compare-and-swap loop.
    OPTIONAL_EXT(has_atomic_float, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, af, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT);
    if (has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) { g_vk.has_memory_budget = 1; ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME; }
    // GPU timestamps on the CPU clock (vk_sync.c): the perf line splits GPU idle time into waiting for a submission or not.
    if (has_ext(ep, ne, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) { g_vk.has_calibrated = 2; ext[n_ext++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME; }
    else if (has_ext(ep, ne, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) { g_vk.has_calibrated = 1; ext[n_ext++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME; }
    if (vk_hdr_wanted() && has_ext(ep, ne, VK_EXT_HDR_METADATA_EXTENSION_NAME)) {   // the display's peak (vk_hdr_metadata)
        g_vk.has_hdr_metadata = 1;
        ext[n_ext++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    }
    // GPU breadcrumbs for a lost device (vk_crumbs.c): AMD's drivers and RADV report it.
    if (has_ext(ep, ne, VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) { g_vk.has_buffer_marker = 1; ext[n_ext++] = VK_AMD_BUFFER_MARKER_EXTENSION_NAME; }
    // Pass marks on NVIDIA (vk_crumbs.c): checkpoints only with TOTK_PASS_CRUMBS=1, since NVIDIA documents a cost for
    // the extension. NVIDIA's drivers report it, AMD's do not.
    if (vk_pass_marks_want_nv() && has_ext(ep, ne, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) {
        g_vk.has_nv_checkpoints = 1;
        ext[n_ext++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    }
#ifdef GAME_NS2
    // Cooperative matrices (gfx_shader_coop_matrix): HMMA.16816 with an f16 accumulator, the matrix multiply of the game's
    // upscaler network, runs on the tensor cores (translator patch 0033) when the device has 16 x 8 x 16 f16 matrices at
    // subgroup scope and 16-bit floats in shaders. TOTK_COOP_MATRIX=0: the shared-memory emulation as before.
    int coop_ok = 0;
    if (has_ext(ep, ne, VK_NV_COOPERATIVE_MATRIX_EXTENSION_NAME) && rt_knob("COOP_MATRIX", "1")) {
        PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV q = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV)
            vkGetInstanceProcAddr(g_vk.instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesNV");
        uint32_t np = 0;
        VkCooperativeMatrixPropertiesNV cp[64];
        if (q && q(g_vk.phys, &np, NULL) == VK_SUCCESS && np) {
            if (np > 64) np = 64;
            for (uint32_t i = 0; i < np; i++) cp[i] = (VkCooperativeMatrixPropertiesNV){.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_NV};
            if (q(g_vk.phys, &np, cp) != VK_SUCCESS) np = 0;
        }
        for (uint32_t i = 0; i < np && !coop_ok; i++)
            coop_ok = cp[i].MSize == 16 && cp[i].NSize == 8 && cp[i].KSize == 16 && cp[i].AType == VK_COMPONENT_TYPE_FLOAT16_NV &&
                      cp[i].BType == VK_COMPONENT_TYPE_FLOAT16_NV && cp[i].CType == VK_COMPONENT_TYPE_FLOAT16_NV &&
                      cp[i].DType == VK_COMPONENT_TYPE_FLOAT16_NV && cp[i].scope == VK_SCOPE_SUBGROUP_NV;
        if (coop_ok) {
            ext[n_ext++] = VK_NV_COOPERATIVE_MATRIX_EXTENSION_NAME;
            CHAIN(cm, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_NV);
        }
    }
#endif
    // DEBUG-PIPESTATS: TOTK_PIPESTATS=1: the driver's statistics of each pipeline (vk_pipeline.c pipestats_log)
    if (cfg_get("TOTK_PIPESTATS") && has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)) {
        ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
        CHAIN(pep, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR);
    }
    // Graphics pipeline library: chained last so it can be dropped again when vk_caps_gpl declines it.
    void **gpl_link = NULL;
    if (has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        gpl_link = tail;
        CHAIN(gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
    }
    free(ep);

    // The driver fills the chain with what it supports; the same chain then enables all of it (robustBufferAccess
    // stays on: guest shaders index constant buffers freely and out-of-range reads must stay harmless).
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &c->f2);
    vk_caps_driver(g_vk.phys);
    vk_caps_features(&c->f2.features);
    if (gpl_link) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        if (!vk_caps_gpl(&c->gpl, &gp)) { *gpl_link = NULL; n_ext -= 2; }
    }
    const VkPhysicalDeviceFeatures *h = &c->f2.features;
    const char *miss = NULL;
    if (!c->f12.timelineSemaphore) miss = "timelineSemaphore";
    else if (!c->f13.dynamicRendering) miss = "dynamicRendering";
    else if (!c->f13.synchronization2) miss = "synchronization2";
    else if (!h->shaderStorageImageWriteWithoutFormat) miss = "shaderStorageImageWriteWithoutFormat";
    else if (!c->clip.depthClipControl) miss = "depthClipControl";
    else if (!h->shaderInt64) miss = "shaderInt64";
    else if (!h->samplerAnisotropy) miss = "samplerAnisotropy";
    else if (!h->independentBlend) miss = "independentBlend";
    else if (!h->geometryShader) miss = "geometryShader";
    else if (!h->textureCompressionBC) miss = "textureCompressionBC";
    if (miss) { rt_log("vk: required feature missing on %s: %s", g_vk.props.deviceName, miss); free(c); return 0; }
    if (!c->fault.deviceFault) g_vk.has_device_fault = 0;   // vkGetDeviceFaultInfoEXT needs the feature, not just the extension
    if (!c->af.shaderBufferFloat32AtomicAdd) g_vk.has_atomic_float = 0;   // storage buffers and device addresses both need it
#ifdef GAME_NS2
    if (g_vk.has_vrs) {   // the rate is set per draw: no rate attachment and no per-primitive rates
        c->vrs.attachmentFragmentShadingRate = c->vrs.primitiveFragmentShadingRate = VK_FALSE;
        g_vk.has_vrs = c->vrs.pipelineFragmentShadingRate;
        rt_log("vk: fragment shading rate: %s", g_vk.has_vrs ? "per draw" : "not supported");
    }
    if (g_vk.has_mesh_shader) {
        // only what the replay uses: the multiview, shading-rate and query parts need features this chain does not enable
        c->mesh.multiviewMeshShader = c->mesh.primitiveFragmentShadingRateMeshShader = c->mesh.meshShaderQueries = VK_FALSE;
        g_vk.has_mesh_shader = c->mesh.meshShader && c->mesh.taskShader;
        rt_log("vk: VK_EXT_mesh_shader: task %d, mesh %d", (int)c->mesh.taskShader, (int)c->mesh.meshShader);
    }
#endif
    g_vk.has_buffer_device_address = c->f12.bufferDeviceAddress;
    g_vk.has_astc_ldr = h->textureCompressionASTC_LDR;
    {
        const char *e = cfg_get("TOTK_FEEDBACK");
        int layout_ok = has_fbl && has_lr && c->fbl.attachmentFeedbackLoopLayout && c->lr.dynamicRenderingLocalRead;
        int want_layout = !(e && !strcmp(e, "restart"));
#ifdef GAME_OOT
        // OoT on a card that is not AMD's: the restart, unless TOTK_FEEDBACK=layout asks for the layout.
        // The layout mode gives every attachable image the feedback usage at its creation, and with that usage an
        // RTX 5070 took 12.2 ms of GPU time for a frame of forest village at 2560x1440 against 9.0 without (75 against
        // 90 fps, standing camera); the game has two feedback draws a frame, so the restarts cost nothing to speak of.
        // The layout mode came for AMD cards, where the read without it showed dark blocks (vk.h VkCtx.feedback).
        if (g_vk.props.vendorID != 0x1002 && !(e && !strcmp(e, "layout"))) want_layout = 0;
#endif
        g_vk.feedback = e && !strcmp(e, "0") ? VK_FEEDBACK_OFF : layout_ok && want_layout ? VK_FEEDBACK_LAYOUT : VK_FEEDBACK_RESTART;
        rt_log("vk: feedback loops: %s (attachmentFeedbackLoopLayout %d, dynamicRenderingLocalRead %d)",
               g_vk.feedback == VK_FEEDBACK_LAYOUT ? "feedback layout + by-region barrier"
               : g_vk.feedback == VK_FEEDBACK_RESTART ? "pass restart + full barrier" : "off (TOTK_FEEDBACK=0)",
               has_fbl && c->fbl.attachmentFeedbackLoopLayout, has_lr && c->lr.dynamicRenderingLocalRead);
    }

    // Frame generation (vk_fg.cpp) presents on a second queue of the graphics family and acquires on a third
    static const float prio_all[3] = {1.0f, 1.0f, 1.0f};
    uint32_t nq_gfx = vk_fg_queue_count() < g_gfx_queues ? vk_fg_queue_count() : g_gfx_queues;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi[3] = {
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.gfx_family, .queueCount = nq_gfx,
         .pQueuePriorities = prio_all}};
    uint32_t nqi = 1;
    if (g_vk.xfer_family != g_vk.gfx_family) {
        qi[1] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = g_vk.xfer_family, .queueCount = 1, .pQueuePriorities = &prio};
        nqi = 2;
    }
    if (g_vk.awb_family != UINT32_MAX && g_vk.awb_family != g_vk.gfx_family && g_vk.awb_family != g_vk.xfer_family)
        qi[nqi++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = g_vk.awb_family, .queueCount = 1, .pQueuePriorities = &prio};
    else g_vk.awb_family = UINT32_MAX;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &c->f2, .queueCreateInfoCount = nqi,
                             .pQueueCreateInfos = qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VkResult r = vk_dlssg_create_device(g_vk.instance, g_vk.phys, &di, &g_vk.device);
    free(c);
    if (r != VK_SUCCESS) { rt_log("vk: vkCreateDevice failed (%d)", (int)r); return 0; }
    vk_dlssg_device_ready(g_vk.device);
    vkGetDeviceQueue(g_vk.device, g_vk.gfx_family, 0, &g_vk.gfx_queue);
    if (nq_gfx >= 2) {
        VkQueue present, acquire;
        vkGetDeviceQueue(g_vk.device, g_vk.gfx_family, 1, &present);
        vkGetDeviceQueue(g_vk.device, g_vk.gfx_family, nq_gfx - 1, &acquire);
        vk_fg_queues(present, acquire);
    }
    if (g_vk.xfer_family != g_vk.gfx_family) vkGetDeviceQueue(g_vk.device, g_vk.xfer_family, 0, &g_vk.xfer_queue);
    else { g_vk.xfer_queue = g_vk.gfx_queue; g_vk.xfer_shared = 1; }
    if (g_vk.awb_family != UINT32_MAX) vkGetDeviceQueue(g_vk.device, g_vk.awb_family, 0, &g_vk.awb_queue);

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &emh};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    g_vk.min_imported_host_alignment = (uint32_t)emh.minImportedHostPointerAlignment;
#ifdef GAME_NS2
    // NVN2 shaders reach global memory at loaded addresses through device addresses of the pool buffers (vk_memory.c
    // vk_mem_gmem_table); shaderInt64 is required above. Read by gfx_shader_set_host, which vk_caps_init calls.
    gfx_shader_gmem = g_vk.has_buffer_device_address;
    gfx_shader_float_atomics = g_vk.has_atomic_float && rt_knob("FLOAT_ATOMICS", "1");
    gfx_shader_coop_matrix = coop_ok && c->cm.cooperativeMatrix && c->f12.shaderFloat16;
    rt_log("vk: HMMA.16816 %s", gfx_shader_coop_matrix ? "on cooperative matrices (VK_NV_cooperative_matrix)" : "emulated in shared memory");
    {   // TOTK_MESH_INDICES=auto|whole|split: how a mesh shader's entry point stores a primitive's indices (translator
        // patch 0041). Split (a thread per index byte) is what NVIDIA has run since the first mesh draw and stays there;
        // AMD's Windows driver for RDNA 1 and 2 loses split stores (no mesh draw showed anything), so every other
        // device gets whole vectors, which no driver has a reason to lose.
        const char *mi = cfg_get("TOTK_MESH_INDICES");
        gfx_shader_mesh_whole = mi && !strcmp(mi, "whole") ? 1 : mi && !strcmp(mi, "split") ? 0 : g_vk.props.vendorID != 0x10de;
        rt_log("vk: mesh shaders store a primitive's indices %s (TOTK_MESH_INDICES)",
               gfx_shader_mesh_whole ? "whole, one thread a primitive" : "split, one thread an index");
    }
    rt_log("vk: float atomic add %s", gfx_shader_float_atomics ? "native (VK_EXT_shader_atomic_float)" : "as compare-and-swap loops");
    rt_log("vk: NVN2 global memory pool table %s", gfx_shader_gmem ? "on (bufferDeviceAddress)" : "off: no bufferDeviceAddress");
#endif
    vk_caps_init(&g_vk);
    mem_result_set_wait(guard_wait, kern_guest_thread);   // TOTK_RESULT_GUARD (memory.c)
    mem_result_setup(vk_sync_fence_done, result_frame);   // TOTK_RESULT_WATCH (memory.c)

    sec_init();
    static VmaVulkanFunctions vma_fn;
    vma_fn.vkAllocateMemory = sec_on ? timed_alloc : NULL;   // TOTK_SECTIONS: VMA's vkAllocateMemory is timed
    // Blocks of 64 MiB (TOTK_VMA_BLOCK_MB; the allocator's own default is 256): a block goes back to the system only
    // when nothing is left in it, and the textures of a map the player left (vk_texture.c evict_cold) were made
    // together, so they empty small blocks whole where they left a quarter-gigabyte block a tenth full: after four
    // map changes the blocks held 1.9 GiB with no allocation in it, counted against the video memory budget.
    const char *bm = cfg_get("TOTK_VMA_BLOCK_MB");
    int block_mb = bm && atoi(bm) >= 16 ? atoi(bm) : 64;
    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.device, .instance = g_vk.instance,
                                 .vulkanApiVersion = VK_API_VERSION_1_3,
                                 .preferredLargeHeapBlockSize = (VkDeviceSize)block_mb << 20,
                                 .flags = (g_vk.has_buffer_device_address ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0) |
                                          (g_vk.has_memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0),
                                 .pDeviceMemoryCallbacks = vk_headroom_callbacks(),
                                 .pVulkanFunctions = sec_on ? &vma_fn : NULL};
    if ((r = vmaCreateAllocator(&ai, &g_vk.vma)) != VK_SUCCESS) { rt_log("vk: vmaCreateAllocator failed (%d)", (int)r); return 0; }
    {   // DEBUG-VRAMHOG: TOTK_VRAM_HOG_MB=<n> takes that much device memory away for the life of the process, to see
        // the port on a card with less VRAM (dedicated 256 MiB buffers, never used)
        const char *e = cfg_get("TOTK_VRAM_HOG_MB");
        int mb = e ? atoi(e) : 0, got = 0;
        for (int i = 0; i < mb / 256; i++) {
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256ull << 20,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
            VmaAllocationCreateInfo ci = {.usage = VMA_MEMORY_USAGE_GPU_ONLY, .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT};
            VkBuffer hb;
            VmaAllocation ha;
            if (vmaCreateBuffer(g_vk.vma, &bi, &ci, &hb, &ha, NULL) == VK_SUCCESS) got += 256;
        }
        if (mb) rt_log("vk: DEBUG-VRAMHOG holds %d MiB of device memory", got);
    }

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    if ((r = vkCreateSemaphore(g_vk.device, &si, NULL, &g_vk.timeline)) != VK_SUCCESS) {
        rt_log("vk: timeline semaphore creation failed (%d)", (int)r);
        return 0;
    }
    vk_crumbs_init(&g_vk);
    return 1;
}

static long g_errors_total;     // outlives the device so the probe can print it after vk_shutdown

static void destroy_all(void) {
    gfx_window_release_vk();
    if (g_vk.device) vkDeviceWaitIdle(g_vk.device);
    if (g_vk.device) vk_astc_release();
    if (g_vk.device) vk_crumbs_release();
    if (g_vk.timeline) vkDestroySemaphore(g_vk.device, g_vk.timeline, NULL);
    vk_headroom_stop();
    if (g_vk.vma) vmaDestroyAllocator(g_vk.vma);
    if (g_vk.device) vkDestroyDevice(g_vk.device, NULL);
    if (g_vk.messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (f) f(g_vk.instance, g_vk.messenger, NULL);
    }
    if (g_vk.instance) vkDestroyInstance(g_vk.instance, NULL);
    g_errors_total += g_vk.validation_errors;
    memset(&g_vk, 0, sizeof g_vk);
}

long vk_validation_errors_total(void) { return g_errors_total + (g_ready ? g_vk.validation_errors : 0); }

int vk_init(void) {
    plat_mutex_lock(&g_lock);
    if (!g_ready) {
        memset(&g_vk, 0, sizeof g_vk);
        if (create_instance() && pick_device() && create_device()) g_ready = 1, vk_headroom_start(), vk_dlaa_init();
        else destroy_all();
    }
    int ok = g_ready;
    plat_mutex_unlock(&g_lock);
    return ok;
}

void vk_shutdown(void) {
    plat_mutex_lock(&g_lock);
    if (g_ready) { destroy_all(); g_ready = 0; }
    plat_mutex_unlock(&g_lock);
}

VkCtx *vk_ctx(void) { return g_ready ? &g_vk : NULL; }

static const char *fault_address_type(VkDeviceFaultAddressTypeEXT t) {
    switch (t) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "instruction pointer (fault)";
    default: return "unknown type";
    }
}

// The query runs once per process. Threads that hit the loss at the same time wait on the lock until it is logged, so
// none of them reaches its rt_fatal and ends the process before the report is written.
void vk_device_fault_report(VkResult r) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static int done;
    if (r != VK_ERROR_DEVICE_LOST) return;
    plat_mutex_lock(&lock);
    if (done) { plat_mutex_unlock(&lock); return; }
    done = 1;
    vk_crumbs_report();
    vk_pass_marks_report();
    VkCtx *c = vk_ctx();
    PFN_vkGetDeviceFaultInfoEXT get =
        c && c->has_device_fault ? (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(c->device, "vkGetDeviceFaultInfoEXT")
                                 : NULL;
    if (!get) {
        rt_log("vk: device lost; no fault report (VK_EXT_device_fault %s)", c && c->has_device_fault ? "has no entry point"
                                                                                                    : "not enabled");
        plat_mutex_unlock(&lock);
        return;
    }
    VkDeviceFaultCountsEXT n = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult q = get(c->device, &n, NULL);
    VkDeviceFaultAddressInfoEXT *ai = calloc(n.addressInfoCount + 1, sizeof *ai);
    VkDeviceFaultVendorInfoEXT *vi = calloc(n.vendorInfoCount + 1, sizeof *vi);
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = ai, .pVendorInfos = vi};
    uint64_t binary = n.vendorBinarySize;
    n.vendorBinarySize = 0;   // the vendor binary is not fetched; its size alone is logged
    if (q == VK_SUCCESS && ai && vi) q = get(c->device, &n, &info);
    if (q != VK_SUCCESS && q != VK_INCOMPLETE) {
        rt_log("vk: device lost; vkGetDeviceFaultInfoEXT failed (%d)", (int)q);
    } else {
        rt_log("vk: device fault: \"%s\" (%u address infos, %u vendor infos, vendor binary %llu bytes)", info.description,
               n.addressInfoCount, n.vendorInfoCount, (unsigned long long)binary);
        // The faulting access lies in the precision-aligned range around the reported address.
        for (uint32_t i = 0; i < n.addressInfoCount; i++)
            rt_log("vk: device fault address %u: %s at %#llx, precision %#llx", i, fault_address_type(ai[i].addressType),
                   (unsigned long long)ai[i].reportedAddress, (unsigned long long)ai[i].addressPrecision);
        for (uint32_t i = 0; i < n.addressInfoCount; i++)
            if (ai[i].addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT &&
                ai[i].addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT &&
                ai[i].addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT)
                vk_mem_fault_report(ai[i].reportedAddress, ai[i].addressPrecision), addr_fault_report(ai[i].reportedAddress, ai[i].addressPrecision);
        for (uint32_t i = 0; i < n.vendorInfoCount; i++)
            rt_log("vk: device fault vendor info %u: \"%s\" code %#llx data %#llx", i, vi[i].description,
                   (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
    }
    free(ai);
    free(vi);
    plat_mutex_unlock(&lock);
}

int vk_selftest(void) {
    if (!vk_init()) return 1;
    VkCtx *c = &g_vk;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 << 10,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    VkBuffer buf;
    VmaAllocation alloc;
    VkResult r = vmaCreateBuffer(c->vma, &bi, &aci, &buf, &alloc, NULL);
    if (r != VK_SUCCESS) { rt_log("vk selftest: vmaCreateBuffer failed (%d)", (int)r); return 2; }
    vmaDestroyBuffer(c->vma, buf, alloc);

    VkSemaphoreSignalInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline, .value = 1};
    if ((r = vkSignalSemaphore(c->device, &sig)) != VK_SUCCESS) { rt_log("vk selftest: vkSignalSemaphore failed (%d)", (int)r); return 3; }
    uint64_t value = 1;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &c->timeline,
                              .pValues = &value};
    if ((r = vkWaitSemaphores(c->device, &wi, 1000000000ull)) != VK_SUCCESS) { rt_log("vk selftest: vkWaitSemaphores failed (%d)", (int)r); return 4; }
    uint64_t now = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
    if (now != 1) { rt_log("vk selftest: timeline value %llu, expected 1", (unsigned long long)now); return 5; }
    return 0;
}

// --probe-vkdevice (host.h)
int probe_vkdevice(void) {
    int rc = vk_selftest();
    VkCtx *c = vk_ctx();
    if (rc || !c) { printf("probe-vkdevice: FAILED (selftest %d)\n", rc ? rc : 1); vk_shutdown(); return 1; }
    uint32_t v = c->props.apiVersion;
    printf("probe-vkdevice: device %d \"%s\" api %u.%u.%u, gfx queue family %u, transfer family %u%s, host import alignment %#x\n",
           c->device_index, c->props.deviceName, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v),
           c->gfx_family, c->xfer_family, c->xfer_shared ? " (shared queue)" : "", c->min_imported_host_alignment);
    printf("probe-vkdevice: optional: custom_border=%d eds=%d eds2=%d eds3=%d vertex_input=%d cond_render=%d xfb=%d provoking=%d "
           "robustness2=%d fault=%d budget=%d bda=%d interlock=%d buffer_marker=%d nv_checkpoints=%d validation=%d\n",
           c->has_custom_border_color, c->has_eds, c->has_eds2, c->has_eds3, c->has_vertex_input_dynamic_state,
           c->has_conditional_rendering, c->has_transform_feedback, c->has_provoking_vertex, c->has_robustness2,
           c->has_device_fault, c->has_memory_budget, c->has_buffer_device_address, c->has_fragment_shader_interlock,
           c->has_buffer_marker, c->has_nv_checkpoints, c->validation);
    vk_shutdown();
    printf("probe-vkdevice: %s (validation errors %ld)\n", g_errors_total ? "FAILED" : "passed", g_errors_total);
    return g_errors_total ? 1 : 0;
}
