// The just-in-time frame start (frame_start.c, REKARINA_FRAME_START).
#pragma once
#include <stdint.h>
void frame_start_acquire(uint64_t waited_ns, uint64_t frame);   // nvnWindowAcquireTexture, after its waits
void frame_start_host_wait(uint64_t ns);                        // nvnSyncWait: a host wait for the GPU
void frame_start_present(void);                                 // nvnQueuePresentTexture: who presents
void frame_start_present_wait(uint64_t ns);                     // nvnQueuePresentTexture: its present sync waited ns for the GPU
void frame_start_kwait(int svc, uint64_t addr, uint64_t arg);   // kernel.c: after SVC 0x34 (arg: ns blocked) / 0x35 (arg: woken tid)
void frame_start_pad(void);                                     // nn::hid::GetNpadStates answered live, any thread
void frame_start_pad_census(uint32_t want, int live, uint32_t id);   // DEBUG-PADCENSUS: every GetNpadStates call
