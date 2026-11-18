// `Rekarina --memory-test` (Linux): can this GPU and driver share the game's memory without shadow storage?
//
// Shadow storage (vk_memory.c) exists because the Linux build cannot import the game's memory for the GPU: a page
// imported as a userptr (VK_EXT_external_memory_host) is watched by the kernel's MMU notifier, so every protection
// change of the write watch (userfaultfd write-protect, mprotect) invalidates the import (about 1.3 ms per 16 MiB at
// the next submission on amdgpu), and the import's pinning clears the watch's bits. The way out is the other
// direction: memory the GPU driver allocates itself, mapped at the address the game uses, so the CPU's view and the
// GPU's are the same pages and the CPU-side protection does not touch the GPU's view (docs/oot-notes.md, "Guest
// memory from the GPU driver on Linux"). This test runs that sequence on the player's machine in one command and says
// which ways work, before the game relies on any of them:
//   placed   VK_EXT_map_memory_placed: vkMapMemory2KHR maps driver memory at a chosen address (Mesa RADV/ANV/NVK
//            24.1+, NVIDIA 560+)
//   fd       the memory exported as a dma-buf or opaque fd and mmapped there by us (RADV, ANV, lavapipe)
//   import   anonymous memory imported as a host pointer: today's userptr, the baseline the protection cost is
//            compared with
// For each: the GPU reads what the CPU wrote and the CPU reads what the GPU wrote through the fixed address; a
// read-only page faults on a CPU write (the write watch's fallback); and the cost of protection changes on the next
// submission. The result goes to stdout and to memory-test.txt beside the game (the problem report takes it).
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MEMTEST_STANDALONE   // cc -O1 -DMEMTEST_STANDALONE runtime/memtest.c -lvulkan -o memtest (any Linux with a compiler)
#include <unistd.h>
#define RT_PORT_NAME "Rekarina"
#define PLAT_PATH_MAX 4096
static int plat_exe_path(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}
int memtest_main(void);
int main(void) { return memtest_main(); }
#else
#include "host.h"
#include "platform/platform.h"
#endif

#ifdef _WIN32
int memtest_main(void) {
    printf("memory test: Linux only (the Windows build imports the game's memory directly)\n");
    return 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#ifndef UFFD_FEATURE_WP_ASYNC   // older kernel headers (the release sysroot): as platform/linux.c
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

#define MT_SIZE (16ull << 20)   // the test's memory object
#define MT_RESERVE (64ull << 20)

static FILE *mt_out;
static int mt_nolist;  // a child of memtest_main: the devices and extensions were listed by the parent
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    char line[1024];
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    fputs(line, stdout), fputc('\n', stdout), fflush(stdout);
    if (mt_out) fputs(line, mt_out), fputc('\n', mt_out);
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// ---- the write watch's fallback: a CPU write into a read-only page faults, is counted and opens the page
static volatile uint64_t wf_lo, wf_hi;
static volatile int wf_count;
static struct sigaction wf_old;
static void wf_handler(int sig, siginfo_t *si, void *uc) {
    uint64_t a = (uint64_t)(uintptr_t)si->si_addr;
    if (a >= wf_lo && a < wf_hi) {
        wf_count++;
        mprotect((void *)(uintptr_t)(a & ~0xfffull), 4096, PROT_READ | PROT_WRITE);
        return;
    }
    sigaction(SIGSEGV, &wf_old, NULL);   // not ours: the default action (or the previous handler) on the retry
    (void)sig, (void)uc;
}

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qf, mtype;
    VkCommandPool pool;
    int has_placed, has_fd, has_dmabuf, has_host;
    VkDeviceSize placed_align, host_align;
    PFN_vkMapMemory2KHR map2;
    PFN_vkUnmapMemory2KHR unmap2;
    PFN_vkGetMemoryFdKHR get_fd;
    PFN_vkGetMemoryHostPointerPropertiesEXT host_props;
} Mt;

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int mt_device(Mt *m) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Rekarina memory test",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (vkCreateInstance(&ii, NULL, &m->inst) != VK_SUCCESS) return say("no Vulkan instance"), 0;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(m->inst, &n, NULL);
    VkPhysicalDevice pds[16];
    if (n > 16) n = 16;
    if (!n || vkEnumeratePhysicalDevices(m->inst, &n, pds) < 0) return say("no Vulkan device"), 0;
    const char *gi = getenv("REKARINA_GPU_INDEX");
    uint32_t pick = gi && *gi ? (uint32_t)atoi(gi) : 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (!mt_nolist) say("gpu %u: %s (vendor 0x%04x device 0x%04x, driver 0x%x, api %u.%u.%u)%s", i, p.deviceName, p.vendorID,
            p.deviceID, p.driverVersion, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion), i == (pick < n ? pick : 0) ? "  <- tested" : "");
    }
    m->pd = pds[pick < n ? pick : 0];
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(m->pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex);
    vkEnumerateDeviceExtensionProperties(m->pd, NULL, &ne, ex);
    m->has_placed = has_ext(ex, ne, "VK_EXT_map_memory_placed") && has_ext(ex, ne, "VK_KHR_map_memory2");
    m->has_fd = has_ext(ex, ne, "VK_KHR_external_memory_fd");
    m->has_dmabuf = m->has_fd && has_ext(ex, ne, "VK_EXT_external_memory_dma_buf");
    m->has_host = has_ext(ex, ne, "VK_EXT_external_memory_host");
    free(ex);
    if (!mt_nolist) say("extensions: map_memory_placed %s, external_memory_fd %s, external_memory_dma_buf %s, external_memory_host %s",
        m->has_placed ? "yes" : "no", m->has_fd ? "yes" : "no", m->has_dmabuf ? "yes" : "no", m->has_host ? "yes" : "no");
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT pf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = m->has_placed ? &pf : NULL};
    vkGetPhysicalDeviceFeatures2(m->pd, &f2);
    if (m->has_placed && !pf.memoryMapPlaced) m->has_placed = 0;
    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
                                                          .pNext = m->has_placed ? &pp : NULL};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                      .pNext = m->has_host ? (void *)&hp : m->has_placed ? (void *)&pp : NULL};
    vkGetPhysicalDeviceProperties2(m->pd, &p2);
    m->placed_align = pp.minPlacedMemoryMapAlignment ? pp.minPlacedMemoryMapAlignment : 4096;
    m->host_align = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;
    if (m->has_placed)
        say("placed maps: memoryMapPlaced %u, memoryMapRangePlaced %u, memoryUnmapReserve %u, alignment %llu",
            pf.memoryMapPlaced, pf.memoryMapRangePlaced, pf.memoryUnmapReserve, (unsigned long long)m->placed_align);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m->pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(m->pd, &nq, qp);
    m->qf = UINT32_MAX;
    for (uint32_t i = 0; i < nq && m->qf == UINT32_MAX; i++)
        if (qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) m->qf = i;
    if (m->qf == UINT32_MAX) return say("no graphics or compute queue"), 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(m->pd, &mp);
    int best = -1, score = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags fl = mp.memoryTypes[i].propertyFlags, need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((fl & need) != need) continue;
        int s = (fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 2 : 1;   // the CPU reads it too: cached where there is one
        if (s > score) score = s, best = (int)i;
    }
    if (best < 0) return say("no host-visible coherent memory type"), 0;
    m->mtype = (uint32_t)best;
    say("memory type %d: flags 0x%x, heap %u (%llu MiB)", best, mp.memoryTypes[best].propertyFlags,
        mp.memoryTypes[best].heapIndex, (unsigned long long)(mp.memoryHeaps[mp.memoryTypes[best].heapIndex].size >> 20));
    const char *names[6];
    int nn = 0;
    if (m->has_placed) names[nn++] = "VK_KHR_map_memory2", names[nn++] = "VK_EXT_map_memory_placed";
    if (m->has_fd) names[nn++] = "VK_KHR_external_memory_fd";
    if (m->has_dmabuf) names[nn++] = "VK_EXT_external_memory_dma_buf";
    if (m->has_host) names[nn++] = "VK_EXT_external_memory_host";
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = m->qf,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT pon = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
                                                      .memoryMapPlaced = 1, .memoryUnmapReserve = pf.memoryUnmapReserve};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = m->has_placed ? &pon : NULL,
                             .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = (uint32_t)nn,
                             .ppEnabledExtensionNames = names};
    if (vkCreateDevice(m->pd, &di, NULL, &m->dev) != VK_SUCCESS) return say("vkCreateDevice failed"), 0;
    vkGetDeviceQueue(m->dev, m->qf, 0, &m->q);
    VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = m->qf,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    vkCreateCommandPool(m->dev, &ci, NULL, &m->pool);
    m->map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(m->dev, "vkMapMemory2KHR");
    m->unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(m->dev, "vkUnmapMemory2KHR");
    m->get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(m->dev, "vkGetMemoryFdKHR");
    m->host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(m->dev, "vkGetMemoryHostPointerPropertiesEXT");
    return 1;
}

// Copies [0, 4 MiB) to [8, 12 MiB) and fills [12, 16 MiB) with 0xa5a5a5a5, `reps` times; the time of one submission.
static double mt_gpu(Mt *m, VkBuffer buf, int reps) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = m->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(m->dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    for (int r = 0; r < reps; r++) {
        VkBufferCopy c = {0, 8ull << 20, 4ull << 20};
        vkCmdCopyBuffer(cb, buf, buf, 1, &c);
        vkCmdFillBuffer(cb, buf, 12ull << 20, 4ull << 20, 0xa5a5a5a5u);
    }
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkFence f;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(m->dev, &fi, NULL, &f);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    double t0 = now_ms();
    VkResult r = vkQueueSubmit(m->q, 1, &si, f);
    if (r == VK_SUCCESS) r = vkWaitForFences(m->dev, 1, &f, VK_TRUE, 10000000000ull);
    double t = now_ms() - t0;
    vkDestroyFence(m->dev, f, NULL);
    vkFreeCommandBuffers(m->dev, m->pool, 1, &cb);
    return r == VK_SUCCESS ? t : -1;
}

static uint32_t pat(uint64_t i) { return (uint32_t)(i * 2654435761u) ^ 0x5bd1e995u; }

static VkExternalMemoryHandleTypeFlagBits mt_fd_type;   // the "fd" way: a dma-buf, else an opaque fd

// One way of placing GPU-visible memory at `at`; 1 when every step held.
static int mt_way(Mt *m, const char *way, uint8_t *at) {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    int fd = -1, ok = 0;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MT_SIZE,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    uint32_t mtype = m->mtype;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = MT_SIZE};
    VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    if (!strcmp(way, "fd")) {
        exp.handleTypes = eb.handleTypes = mt_fd_type;
        bci.pNext = &eb, mai.pNext = &exp;
    } else if (!strcmp(way, "import")) {
        if (mmap(at, MT_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != at)
            return say("%s: anonymous memory at %p failed (%s)", way, (void *)at, strerror(errno)), 0;
        VkMemoryHostPointerPropertiesEXT hpp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (m->host_props(m->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, at, &hpp) != VK_SUCCESS ||
            !hpp.memoryTypeBits)
            return say("%s: no memory type takes this host pointer", way), 0;
        mtype = (uint32_t)__builtin_ctz(hpp.memoryTypeBits);
        eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        imp.pHostPointer = at;
        bci.pNext = &eb, mai.pNext = &imp;
    }
    mai.memoryTypeIndex = mtype;
    if (vkCreateBuffer(m->dev, &bci, NULL, &buf) != VK_SUCCESS) { say("%s: vkCreateBuffer failed", way); goto out; }
    VkResult r = vkAllocateMemory(m->dev, &mai, NULL, &mem);
    if (r != VK_SUCCESS) { say("%s: vkAllocateMemory failed (%d)", way, (int)r); goto out; }
    if (vkBindBufferMemory(m->dev, buf, mem, 0) != VK_SUCCESS) { say("%s: vkBindBufferMemory failed", way); goto out; }
    if (!strcmp(way, "placed")) {
        VkMemoryMapPlacedInfoEXT pl = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = at};
        VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pl,
                                 .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = mem, .size = VK_WHOLE_SIZE};
        void *p = NULL;
        r = m->map2(m->dev, &mi, &p);
        if (r != VK_SUCCESS || p != at) { say("%s: vkMapMemory2KHR at %p gave %d, %p", way, (void *)at, (int)r, p); goto out; }
    } else if (!strcmp(way, "fd")) {
        VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem, .handleType = exp.handleTypes};
        if (m->get_fd(m->dev, &gi, &fd) != VK_SUCCESS || fd < 0) { say("%s: vkGetMemoryFdKHR failed", way); goto out; }
        void *p = mmap(at, MT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        if (p != at) { say("%s: mmap of the %s fd at %p failed (%s)", way,
                           mt_fd_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ? "dma-buf" : "opaque", (void *)at,
                           strerror(errno)); goto out; }
    }
    // 1. the CPU writes, the GPU copies and fills, the CPU reads the GPU's result: all through `at`
    uint32_t *w = (uint32_t *)at;
    for (uint64_t i = 0; i < (8ull << 20) / 4; i++) w[i] = pat(i);
    double t1 = mt_gpu(m, buf, 1);
    int bad_copy = 0, bad_fill = 0;
    int64_t first_copy = -1, first_fill = -1;
    for (uint64_t i = 0; i < (4ull << 20) / 4; i++)
        if (w[(8ull << 20) / 4 + i] != pat(i) && !bad_copy++) first_copy = (int64_t)i * 4;
    for (uint64_t i = 0; i < (4ull << 20) / 4; i++)
        if (w[(12ull << 20) / 4 + i] != 0xa5a5a5a5u && !bad_fill++) first_fill = (int64_t)i * 4;
    if (bad_copy || bad_fill)
        say("%s: first wrong word at +%lld of the copy (holds 0x%08x), +%lld of the fill (holds 0x%08x)", way,
            (long long)first_copy, first_copy >= 0 ? w[(8ull << 20) / 4 + first_copy / 4] : 0, (long long)first_fill,
            first_fill >= 0 ? w[(12ull << 20) / 4 + first_fill / 4] : 0);
    say("%s: GPU read the CPU's 4 MiB and the CPU read the GPU's 8 MiB at %p: %s (%d copied and %d filled words wrong, %.2f ms)",
        way, (void *)at, !bad_copy && !bad_fill && t1 >= 0 ? "ok" : "WRONG", bad_copy, bad_fill, t1);
    // 2. the write watch's fallback: read-only pages fault on a CPU write and are counted
    wf_lo = (uint64_t)(uintptr_t)at, wf_hi = wf_lo + MT_SIZE, wf_count = 0;
    struct sigaction sa = {.sa_sigaction = wf_handler, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &wf_old);
    int prot_ok = !mprotect(at, MT_SIZE, PROT_READ);
    for (int i = 0; prot_ok && i < 100; i++) ((volatile uint32_t *)at)[(uint64_t)i * 1024 * 37 % (MT_SIZE / 4)] = 1;
    sigaction(SIGSEGV, &wf_old, NULL);
    mprotect(at, MT_SIZE, PROT_READ | PROT_WRITE);
    say("%s: write faults on read-only pages: %s (%d of 100 writes faulted)", way,
        prot_ok && wf_count == 100 ? "ok" : prot_ok ? "WRONG" : "mprotect failed", wf_count);
    // 3. userfaultfd write-protect (today's watch) over the same pages
    int uf = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    struct uffdio_register reg = {.range = {(uint64_t)(uintptr_t)at, MT_SIZE}, .mode = UFFDIO_REGISTER_MODE_WP};
    int ufok = uf >= 0 && !ioctl(uf, UFFDIO_API, &api) && !ioctl(uf, UFFDIO_REGISTER, &reg);
    say("%s: userfaultfd write-protect over the pages: %s", way, ufok ? "yes" : strerror(errno));
    if (uf >= 0) close(uf);
    // 4. what a protection change costs the next submission (the userptr problem): 20 rounds with and without
    double base = 0, prot = 0;
    for (int i = 0; i < 20; i++) base += mt_gpu(m, buf, 1);
    for (int i = 0; i < 20; i++) {
        mprotect(at, MT_SIZE, PROT_READ);
        mprotect(at, MT_SIZE, PROT_READ | PROT_WRITE);
        prot += mt_gpu(m, buf, 1);
    }
    double extra = (prot - base) / 20;
    say("%s: a submission takes %.3f ms, %.3f ms after a protection change of the 16 MiB (%+.3f ms)", way, base / 20,
        prot / 20, extra);
    ok = !bad_copy && !bad_fill && t1 >= 0 && prot_ok && wf_count == 100;
    say("%s: %s", way, ok ? (extra < 0.25 ? "USABLE" : "works, but protection changes cost the GPU") : "NOT USABLE");
    if (!strcmp(way, "import")) ok = 0;   // the baseline, never the answer
out:
    if (!strcmp(way, "placed") && mem && m->unmap2) {
        VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem};
        m->unmap2(m->dev, &ui);
    }
    if (buf) vkDestroyBuffer(m->dev, buf, NULL);
    if (mem) vkFreeMemory(m->dev, mem, NULL);
    if (fd >= 0) close(fd);
    mmap(at, MT_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);   // reserved again
    return ok;
}

int memtest_main(void) {
    char path[PLAT_PATH_MAX + 32] = "memory-test.txt";   // beside the game (the problem report takes it)
    if (plat_exe_path(path, PLAT_PATH_MAX)) {
        char *cut = strrchr(path, '/');
        if (cut) strcpy(cut + 1, "memory-test.txt");
    }
    mt_out = fopen(path, "w");
    say("Rekarina memory test: can the GPU and the game share memory without shadow storage? (%s)", RT_PORT_NAME);
    Mt m = {0};
    if (!mt_device(&m)) {
        say("RESULT: no test (no usable Vulkan device)");
        if (mt_out) fclose(mt_out);
        return 1;
    }
    int has_placed = m.has_placed, has_fd = m.has_fd, has_dmabuf = m.has_dmabuf, has_host = m.has_host;
    vkDestroyCommandPool(m.dev, m.pool, NULL);
    vkDestroyDevice(m.dev, NULL);
    vkDestroyInstance(m.inst, NULL);
    // Each way in a child process with a device of its own: a driver that crashes on one (lavapipe on an opaque fd
    // mapped by us) is a result for that way, not the end of the test.
    struct { const char *way; VkExternalMemoryHandleTypeFlagBits fd_type; int on; const char *what; } ways[] = {
        {"placed", 0, has_placed, "placed: the driver has no VK_EXT_map_memory_placed"},
        {"fd", VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, has_dmabuf, "fd: the driver exports no dma-buf"},
        {"fd", VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, has_fd, "fd: the driver exports no memory as a file descriptor"},
        {"import", 0, has_host, "import: the driver imports no host memory"},
    };
    int placed = 0, fd = 0;
    for (int w = 0; w < 4; w++) {
        if (!ways[w].on) { say("%s", ways[w].what); continue; }
        if (w == 2 && fd) continue;   // an opaque fd only where no dma-buf worked
        if (w == 1) say("fd: as a dma-buf");
        if (w == 2) say("fd: as an opaque fd");
        if (mt_out) fflush(mt_out);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            mt_nolist = 1;
            Mt c = {0};
            if (!mt_device(&c)) _exit(3);
            uint8_t *res = mmap(NULL, MT_RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (res == MAP_FAILED) _exit(3);
            uint8_t *at = (uint8_t *)(((uintptr_t)res + (16ull << 20) - 1) & ~((uintptr_t)(16ull << 20) - 1));
            mt_fd_type = ways[w].fd_type;
            int ok = mt_way(&c, ways[w].way, at);
            if (mt_out) fflush(mt_out);
            fflush(stdout);
            _exit(ok ? 0 : 1);
        }
        int st = 0;
        if (pid < 0 || waitpid(pid, &st, 0) != pid) { say("%s: no child process for the test", ways[w].way); continue; }
        if (WIFSIGNALED(st)) say("%s: the driver CRASHED in this way (signal %d): NOT USABLE", ways[w].way, WTERMSIG(st));
        int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (w == 0) placed = ok;
        if (w == 1 || w == 2) fd |= ok;
    }
    say("RESULT: %s", placed ? "driver memory usable (placed maps)" : fd ? "driver memory usable (exported fd mmapped)"
                                                                          : "driver memory NOT usable: the game keeps shadow storage");
    say("Please send this text (or %s) with your report.", path);
    if (mt_out) fclose(mt_out);
    return placed || fd ? 0 : 2;
}
#endif
