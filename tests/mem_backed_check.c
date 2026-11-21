// Driver-backed memory of the platform layer (plat_mem_back / plat_mem_unback, runtime/platform/linux.c), checked with
// a memfd mapping standing in for a GPU driver's: the kernel's write protection does not apply to it either way
// (a shared file mapping is covered by userfaultfd, so the dirty bits are what answers here as in a driver's case).
// No game files, no GPU:
//   clang -O1 tests/mem_backed_check.c runtime/platform/linux.c runtime/platform/common.c -Iruntime/platform \
//       $(pkg-config --cflags --libs sdl3) -lpthread -ldl -o build/mem_backed_check && build/mem_backed_check
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "platform.h"

#define BASE 0x2000000000ull
#define N (64ull << 20)
#define PG 4096ull

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int mfd = -1;
static volatile int placed_calls, released_calls;

static int place(void *ctx) {
    (void)ctx;
    placed_calls++;
    mfd = memfd_create("backed", 0);
    if (mfd < 0 || ftruncate(mfd, N)) return 0;
    void *t = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (t == MAP_FAILED) return 0;
    memcpy(t, (void *)(uintptr_t)BASE, N);
    munmap(t, N);
    return mmap((void *)(uintptr_t)BASE, N, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, mfd, 0) != MAP_FAILED;
}

static int release(void *ctx) {
    (void)ctx;
    released_calls++;
    // the driver's unmap with the reserve bit: the range stays, inaccessible
    int ok = mmap((void *)(uintptr_t)BASE, N, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != MAP_FAILED;
    close(mfd);
    return ok;
}

static uint64_t written(uint64_t *pages, int reset) { return plat_mem_written(BASE, N, pages, 16384, reset); }

static volatile int stop;
static volatile uint64_t wcount[8];
static void *writer(void *arg) {
    int id = (int)(intptr_t)arg;
    volatile uint64_t *w = (volatile uint64_t *)(uintptr_t)(BASE + (32ull << 20) + id * PG);   // one page each
    uint64_t v = 0;
    while (!stop) *w = ++v, wcount[id] = v;
    return NULL;
}

int main(void) {
    if (!plat_mem_reserve(BASE, N, 1) || !plat_mem_commit(BASE, N)) { printf("cannot reserve\n"); return 2; }
    uint64_t *pages = malloc(16384 * sizeof *pages);
    if (plat_mem_written(BASE, PG, pages, 1, 0) == UINT64_MAX) {   // no asynchronous write protection (a container, an old kernel)
        printf("mem_backed_check: skipped, this kernel or sandbox has no userfaultfd write protection\n");
        return 0;
    }
    uint8_t *m = (uint8_t *)(uintptr_t)BASE;
    for (uint64_t i = 0; i < N; i += PG) m[i] = (uint8_t)(i / PG * 7 + 1), m[i + PG - 1] = 0x5a;
    uint64_t n = written(pages, 1);   // consume what the fills wrote
    CHECK(n != UINT64_MAX && n == N / PG, "before: %llu pages written, expected %llu", (unsigned long long)n, (unsigned long long)(N / PG));
    m[9 * PG] = 0x11;   // written, and still unreported when the range is backed
    CHECK(plat_mem_back(BASE, N, place, NULL) == 1, "plat_mem_back failed");
    CHECK(placed_calls == 1, "place ran %d times", placed_calls);
    int bad = 0;
    for (uint64_t i = 0; i < N; i += PG) bad += i == 9 * PG ? m[i] != 0x11 : m[i] != (uint8_t)(i / PG * 7 + 1), bad += m[i + PG - 1] != 0x5a;
    CHECK(!bad, "%d bytes changed by the swap", bad);
    n = written(pages, 0);
    CHECK(n == 1 && pages[0] == BASE + 9 * PG, "after backing: %llu dirty pages (first %#llx), expected page 9 only", (unsigned long long)n, n ? (unsigned long long)pages[0] : 0ull);
    n = written(pages, 1);
    n = written(pages, 1);
    CHECK(n == 0, "after a reset: %llu pages", (unsigned long long)n);
    m[5 * PG] = 1;
    m[5 * PG + 1] = 2;
    m[700 * PG] = 3;
    n = written(pages, 1);
    CHECK(n == 2 && pages[0] == BASE + 5 * PG && pages[1] == BASE + 700 * PG, "two writes: %llu pages", (unsigned long long)n);
    m[5 * PG] = 4;   // closed again by the reset: faults, is marked, opens
    n = written(pages, 1);
    CHECK(n == 1 && pages[0] == BASE + 5 * PG, "write after reset: %llu pages", (unsigned long long)n);
    // memory.c's protections compose: read-only, then writable again
    CHECK(plat_mem_protect(BASE + 20 * PG, 4 * PG, PLAT_PROT_READ), "protect read");
    CHECK(plat_mem_state(BASE + 20 * PG, BASE + 24 * PG, &(uint64_t){0}) == PLAT_PROT_READ, "state read");
    CHECK(plat_mem_protect(BASE + 20 * PG, 4 * PG, PLAT_PROT_RW), "protect rw");
    m[21 * PG] = 9;
    n = written(pages, 1);
    CHECK(n == 1 && pages[0] == BASE + 21 * PG, "composed protection: %llu pages", (unsigned long long)n);
    // no write is lost across a swap: four threads count into their own pages while the range is unbacked and backed
    PlatThread *t[4];
    for (int i = 0; i < 4; i++) t[i] = NULL;
    extern int pthread_create(void *, const void *, void *(*)(void *), void *);
    unsigned long th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, writer, (void *)(intptr_t)i);
    for (int round = 0; round < 20; round++) {
        CHECK(plat_mem_unback(BASE, N, release, NULL) == 1, "unback %d", round);
        usleep(2000);
        CHECK(plat_mem_back(BASE, N, place, NULL) == 1, "back %d", round);
        usleep(2000);
    }
    stop = 1;
    extern int pthread_join(unsigned long, void **);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < 4; i++) {
        uint64_t seen = *(volatile uint64_t *)(uintptr_t)(BASE + (32ull << 20) + i * PG);
        CHECK(seen == wcount[i], "writer %d: memory holds %llu, it wrote %llu", i, (unsigned long long)seen, (unsigned long long)wcount[i]);
    }
    n = written(pages, 1);
    CHECK(n >= 1 && n <= 5 + 4, "after the swaps: %llu dirty pages", (unsigned long long)n);
    // give it back: bytes kept, only the pages written since are reported
    CHECK(plat_mem_unback(BASE, N, release, NULL) == 1, "final unback");
    bad = 0;
    for (uint64_t i = 64 * PG; i < 200 * PG; i += PG) bad += m[i] != (uint8_t)(i / PG * 7 + 1);
    CHECK(!bad, "%d bytes changed on the way back", bad);
    n = written(pages, 1);
    CHECK(n != UINT64_MAX && n <= 16, "after unback: %llu pages written", (unsigned long long)n);
    m[40 * PG] = 1;
    n = written(pages, 1);
    CHECK(n == 1 && pages[0] == BASE + 40 * PG, "kernel watch again: %llu pages", (unsigned long long)n);
    CHECK(plat_mem_unback(BASE, N, release, NULL) == 0, "unback of an unbacked range succeeded");
    printf(fails ? "mem_backed_check: %d FAILED\n" : "mem_backed_check: ok\n", fails);
    return fails != 0;
}
