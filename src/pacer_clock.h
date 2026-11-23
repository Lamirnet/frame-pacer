#ifndef FRAME_PACER_CLOCK_H
#define FRAME_PACER_CLOCK_H

#include "pacer_limit.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint64_t (*frame_pacer_now_fn)(void *);
typedef int (*frame_pacer_sleep_fn)(void *, uint64_t);

/* Bounded percentile sample, including for long configured windows. */
#define FRAME_PACER_WINDOW_CAPACITY 320U

struct frame_pacer_clock {
    pthread_mutex_t mutex;
    bool initialized;
    bool started;
    _Atomic uint32_t fps;
    struct frame_pacer_dynamic_config dynamic;
    uint64_t last_release_ns, window_start_ns, retry_after_ns;
    uint32_t work_samples[FRAME_PACER_WINDOW_CAPACITY];
    uint64_t samples;
    unsigned next_rise, previous_cap, late_streak;
    bool headroom_confirmed;
    unsigned recent_late;
    uint64_t next_drop_ns;
    struct frame_pacer_spike_trace *trace;
    uint64_t next_deadline_ns;
};

struct frame_pacer_decision {
    uint64_t observed_ns;
    uint64_t deadline_ns;
    unsigned int interruptions;
    bool first;
    bool missed;
};

void frame_pacer_clock_init(struct frame_pacer_clock *);
/* Enable during backend initialization. Drain at normal shutdown. */
FRAME_PACER_LIMIT_INTERNAL void
frame_pacer_clock_enable_trace(struct frame_pacer_clock *);
FRAME_PACER_LIMIT_INTERNAL void
frame_pacer_clock_flush_trace(struct frame_pacer_clock *,
                              void (*log)(const char *, ...));
void frame_pacer_clock_destroy(struct frame_pacer_clock *);
void frame_pacer_clock_wait(struct frame_pacer_clock *, uint32_t fps,
                            frame_pacer_now_fn, frame_pacer_sleep_fn, void *,
                            struct frame_pacer_decision *);
FRAME_PACER_LIMIT_INTERNAL void
frame_pacer_clock_wait_config(struct frame_pacer_clock *, uint32_t fps,
                              const struct frame_pacer_dynamic_config *,
                              frame_pacer_now_fn, frame_pacer_sleep_fn, void *,
                              struct frame_pacer_decision *);

#endif
