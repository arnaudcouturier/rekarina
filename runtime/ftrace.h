// Frame trace (TOTK_FTRACE=<first frame>:<frames>[:<path>], a diagnostic): one timeline of what the game's threads, the
// replay, the completion thread and the GPU do during a few frames, on the clock plat_now_ns reads. Each event is a tag,
// the thread and three words. ftrace_frame (at each present) opens and closes the window; a few frames after it closes,
// the events are written as text, sorted by time: <ms since the window opened> <thread> <tag> <a> <b> <c>.
// Costs one predictable branch per call site while off.
#pragma once
#include <stdint.h>

enum {
    FT_FRAME,      // a present was counted: a frame number
    FT_ACQ0, FT_ACQ1,   // nvnWindowAcquireTexture enters (a lr) / returns (a index, b fence waited for, c 1 when it waited)
    FT_PRS0, FT_PRSW, FT_PRS1,   // nvnQueuePresentTexture enters (a lr) / starts its wait for the GPU (a fence) / returns
    FT_SUB0, FT_SUB1,   // nvnQueueSubmitCommands enters (a queue, b handles, c lr) / returns (a fence)
    FT_SW0, FT_SW1,     // nvnSyncWait enters (a lr, b timeout ns, c fence) / returns (a result)
    FT_FSYNC,      // nvnQueueFenceSync (a queue, b fence, c lr)
    FT_FIN0, FT_FIN1,   // nvnQueueFinish enters (a lr) / returns
    FT_FLUSH,      // nvnQueueFlush (a queue, c lr)
    FT_EVSIG,      // nvnEventSignal (a word, b value, c lr)
    FT_WE0, FT_WE1,     // the replay reaches a command buffer's WaitEvent (a word, b value, c fence) / goes on (a result)
    FT_HOLD, FT_RESUME0, FT_RESUME1,   // a submission is held at a wait (a fence) / its rest is recorded (a its fence, b into fence)
    FT_PART,       // a part of a submission goes to the GPU (a fence, b 1 for the submission's last part, c prologue)
    FT_GPART,      // the GPU ran a part: stamped with its start (a ns it took, b ns from vkQueueSubmit2 to its start)
    FT_DONE,       // the completion thread finished a timeline value (a value)
    FT_FWAIT0, FT_FWAIT1,   // a thread waits for a timeline value (a value) / is released
    FT_NOTE,       // anything else (a, b, c)
    FT_N
};

extern int ftrace_on;
void ftrace_ev(int tag, uint64_t a, uint64_t b, uint64_t c);
void ftrace_ev_at(int tag, uint64_t t_ns, uint64_t a, uint64_t b, uint64_t c);
void ftrace_frame(uint64_t frame);
#define FT(tag, a, b, c) do { if (__builtin_expect(ftrace_on, 0)) ftrace_ev(tag, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c)); } while (0)
