#include "pacer_clock.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define TEST_FPS 70U
#define TEST_INTERVAL_NS (UINT64_C(1000000000) / TEST_FPS)

struct fake_time {
    uint64_t now_ns;
    uint64_t last_deadline_ns;
    int interruptions;
};

static uint64_t fake_now(void *opaque)
{
    return ((struct fake_time *)opaque)->now_ns;
}

static int fake_sleep(void *opaque, uint64_t deadline_ns)
{
    struct fake_time *time = opaque;

    time->last_deadline_ns = deadline_ns;
    if (time->interruptions-- > 0)
        return EINTR;
    time->now_ns = deadline_ns;
    return 0;
}

static void sequential_waits(void)
{
    struct frame_pacer_clock clock;
    struct frame_pacer_decision decision;
    struct fake_time time = {.now_ns = 100};
    uint64_t before;

    frame_pacer_clock_init(&clock);
    frame_pacer_clock_wait(&clock, TEST_FPS, fake_now, fake_sleep, &time,
                           &decision);
    assert(decision.first);

    frame_pacer_clock_wait(&clock, TEST_FPS, fake_now, fake_sleep, &time,
                           &decision);
    assert(time.last_deadline_ns == 100 + TEST_INTERVAL_NS);

    time.now_ns += TEST_INTERVAL_NS * 3;
    frame_pacer_clock_wait(&clock, TEST_FPS, fake_now, fake_sleep, &time,
                           &decision);
    assert(decision.missed);
    assert(time.last_deadline_ns > time.now_ns - TEST_INTERVAL_NS);

    time.interruptions = 2;
    frame_pacer_clock_wait(&clock, TEST_FPS, fake_now, fake_sleep, &time,
                           &decision);
    assert(decision.interruptions == 2);

    frame_pacer_clock_wait(&clock, 30, fake_now, fake_sleep, &time, &decision);
    assert(decision.first);
    before = time.now_ns;
    frame_pacer_clock_wait(&clock, 30, fake_now, fake_sleep, &time, &decision);
    assert(time.last_deadline_ns == before + UINT64_C(1000000000) / 30);

    before = time.now_ns;
    frame_pacer_clock_wait(&clock, FRAME_PACER_FPS_LIMIT_OFF, fake_now,
                           fake_sleep, &time, &decision);
    assert(!decision.first && !decision.missed && !decision.deadline_ns);
    assert(!clock.started && clock.fps == FRAME_PACER_FPS_LIMIT_OFF &&
           !clock.next_deadline_ns);
    assert(time.now_ns == before);
    frame_pacer_clock_wait(&clock, 30, fake_now, fake_sleep, &time, &decision);
    assert(decision.first && clock.started && clock.fps == 30);
    frame_pacer_clock_wait(&clock, 1000, fake_now, fake_sleep, &time,
                           &decision);
    assert(!decision.first && !clock.started &&
           clock.fps == FRAME_PACER_FPS_LIMIT_OFF);
    frame_pacer_clock_destroy(&clock);
    frame_pacer_clock_destroy(&clock);
    frame_pacer_clock_init(0);
    frame_pacer_clock_wait(0, 70, fake_now, fake_sleep, &time, &decision);
}

static void deadlines_saturate_at_uint64_max(void)
{
    struct frame_pacer_clock clock;
    struct frame_pacer_decision decision;
    struct fake_time time = {.now_ns = UINT64_MAX - 1};

    frame_pacer_clock_init(&clock);
    frame_pacer_clock_wait(&clock, TEST_FPS, fake_now, fake_sleep, &time,
                           &decision);
    assert(decision.first);
    assert(clock.next_deadline_ns == UINT64_MAX);
    time.now_ns = UINT64_MAX;
    frame_pacer_clock_wait(&clock, TEST_FPS, fake_now, fake_sleep, &time,
                           &decision);
    assert(decision.missed);
    assert(decision.deadline_ns == UINT64_MAX);
    assert(clock.next_deadline_ns == UINT64_MAX);
    frame_pacer_clock_destroy(&clock);
}

struct worker_context {
    struct frame_pacer_clock *clock;
    struct fake_time *time;
    uint64_t deadline_ns;
};

static void *wait_in_worker(void *opaque)
{
    struct worker_context *context = opaque;
    struct frame_pacer_decision decision;

    frame_pacer_clock_wait(context->clock, TEST_FPS, fake_now, fake_sleep,
                           context->time, &decision);
    context->deadline_ns = decision.deadline_ns;
    return 0;
}

static void concurrent_waits_are_serialized(void)
{
    struct frame_pacer_clock clock;
    struct fake_time time = {0};
    struct worker_context first = {&clock, &time, 0};
    struct worker_context second = {&clock, &time, 0};
    struct frame_pacer_decision decision;
    pthread_t first_thread;
    pthread_t second_thread;

    frame_pacer_clock_init(&clock);
    frame_pacer_clock_wait(&clock, TEST_FPS, fake_now, fake_sleep, &time,
                           &decision);
    assert(!pthread_create(&first_thread, 0, wait_in_worker, &first));
    assert(!pthread_create(&second_thread, 0, wait_in_worker, &second));
    assert(!pthread_join(first_thread, 0));
    assert(!pthread_join(second_thread, 0));
    assert(first.deadline_ns != second.deadline_ns);
    assert(first.deadline_ns + TEST_INTERVAL_NS == second.deadline_ns ||
           second.deadline_ns + TEST_INTERVAL_NS == first.deadline_ns);
    frame_pacer_clock_destroy(&clock);
}

static void dynamic_load_changes(void)
{
    struct frame_pacer_clock clock;
    struct frame_pacer_decision decision;
    struct fake_time time = {0};
    struct frame_pacer_dynamic_config cfg = {1, 90, 100, 85, 7, 3};

    frame_pacer_clock_init(&clock);
    frame_pacer_clock_wait_config(&clock, 100, &cfg, fake_now, fake_sleep,
                                  &time, &decision);
    assert(decision.first && clock.fps == 100);
    for (unsigned i = 0; i < 30; ++i) {
        time.now_ns += 10000000;
        frame_pacer_clock_wait_config(&clock, 100, &cfg, fake_now, fake_sleep,
                                      &time, &decision);
        assert(clock.fps == (i < 29 ? 100U : 93U));
    }
    for (unsigned i = 0; i < 50; ++i) {
        time.now_ns += 12000000;
        frame_pacer_clock_wait_config(&clock, 100, &cfg, fake_now, fake_sleep,
                                      &time, &decision);
    }
    assert(clock.fps == 90);
    for (unsigned i = 0; i < 60; ++i) {
        time.now_ns += 9000000;
        frame_pacer_clock_wait_config(&clock, 100, &cfg, fake_now, fake_sleep,
                                      &time, &decision);
        assert(clock.fps == 90);
    }
    for (unsigned i = 0; i < 200; ++i) {
        time.now_ns += 1000000;
        frame_pacer_clock_wait_config(&clock, 100, &cfg, fake_now, fake_sleep,
                                      &time, &decision);
    }
    assert(clock.fps == 100);
    time.now_ns += UINT64_C(3000000000);
    frame_pacer_clock_wait_config(&clock, 100, &cfg, fake_now, fake_sleep,
                                  &time, &decision);
    assert(decision.first && !clock.samples);
    frame_pacer_clock_destroy(&clock);
}

static void advance_dynamic(struct frame_pacer_clock *clock,
                            struct fake_time *time,
                            const struct frame_pacer_dynamic_config *cfg,
                            uint64_t work_ns)
{
    struct frame_pacer_decision decision;

    time->now_ns += work_ns;
    frame_pacer_clock_wait_config(clock, cfg->max_fps, cfg, fake_now,
                                  fake_sleep, time, &decision);
}

static void accelerated_recovery(void)
{
    struct frame_pacer_dynamic_config cfg = {1, 40, 80, 80, 5, 1};
    struct frame_pacer_clock clock;
    struct fake_time time = {0};
    const unsigned steps[] = {41, 43, 47, 55, 63, 71, 79, 80};
    frame_pacer_clock_init(&clock);
    advance_dynamic(&clock, &time, &cfg, 0);
    for (unsigned i = 0; i < 160; ++i)
        advance_dynamic(&clock, &time, &cfg, 30000000);
    assert(clock.fps == 40);

    uint64_t start = time.now_ns;
    unsigned step = 0;
    while (clock.fps < 80 && time.now_ns - start < UINT64_C(3500000000)) {
        unsigned previous = clock.fps;
        advance_dynamic(&clock, &time, &cfg, 10000000);
        if (clock.fps != previous) {
            assert(step < sizeof(steps) / sizeof(steps[0]));
            assert(clock.fps == steps[step++]);
        }
    }
    assert(step == sizeof(steps) / sizeof(steps[0]));
    for (unsigned i = 0; i < 100; ++i)
        advance_dynamic(&clock, &time, &cfg, 10000000);
    advance_dynamic(&clock, &time, &cfg, 96000000);
    for (unsigned i = 0; i < 40; ++i) {
        advance_dynamic(&clock, &time, &cfg, 10000000);
        assert(clock.fps == 80);
    }

    for (unsigned i = 0; i < 160; ++i)
        advance_dynamic(&clock, &time, &cfg, 30000000);
    for (unsigned i = 0; i < 200 && clock.fps < 43; ++i)
        advance_dynamic(&clock, &time, &cfg, 10000000);
    assert(clock.fps == 43);
    advance_dynamic(&clock, &time, &cfg, 26000000);
    assert(clock.fps == 43);
    advance_dynamic(&clock, &time, &cfg, 26000000);
    assert(clock.fps == 41);
    start = time.now_ns;
    while (time.now_ns - start < UINT64_C(570000000)) {
        advance_dynamic(&clock, &time, &cfg, 14000000);
        assert(clock.fps == 41);
    }
    for (unsigned i = 0; i < 300; ++i) {
        unsigned previous = clock.fps;
        advance_dynamic(&clock, &time, &cfg, 14000000);
        if (previous == 41 && clock.fps > previous)
            assert(clock.fps == 42);
        assert(clock.fps <= 57);
    }
    assert(clock.fps == 57);
    frame_pacer_clock_destroy(&clock);
}

static void steady_overload(void)
{
    struct frame_pacer_dynamic_config cfg = {1, 40, 80, 80, 5, 1};
    struct frame_pacer_clock clock;
    struct fake_time time = {0};
    frame_pacer_clock_init(&clock);
    advance_dynamic(&clock, &time, &cfg, 0);
    advance_dynamic(&clock, &time, &cfg, 20000000);
    advance_dynamic(&clock, &time, &cfg, 9000000);
    assert(clock.fps == 80);
    advance_dynamic(&clock, &time, &cfg, 21000000);
    assert(clock.fps == 75);
    for (unsigned i = 0; i < 14; ++i) {
        advance_dynamic(&clock, &time, &cfg, 20000000);
        assert(clock.fps == 75);
    }
    advance_dynamic(&clock, &time, &cfg, 20000000);
    assert(clock.fps == 70);
    frame_pacer_clock_destroy(&clock);
}

static char trace_line[384];
static char trace_summary[128];
static unsigned trace_events;

static void collect_trace(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (strstr(format, "frame-spike")) {
        vsnprintf(trace_line, sizeof(trace_line), format, args);
        ++trace_events;
    } else
        vsnprintf(trace_summary, sizeof(trace_summary), format, args);
    va_end(args);
}

static int wake_late(void *opaque, uint64_t deadline)
{
    struct fake_time *time = opaque;
    time->now_ns = deadline + UINT64_C(10000000);
    return 0;
}

static void long_frame_trace(void)
{
    struct frame_pacer_dynamic_config cfg = {1, 80, 80, 80, 5, 1};
    struct frame_pacer_clock clock;
    struct frame_pacer_decision decision;
    struct fake_time time = {0};
    frame_pacer_clock_init(&clock);
    frame_pacer_clock_enable_trace(&clock);
    advance_dynamic(&clock, &time, &cfg, 0);
    time.now_ns += 10000000;
    frame_pacer_clock_wait_config(&clock, 80, &cfg, fake_now, wake_late, &time,
                                  &decision);
    frame_pacer_clock_flush_trace(&clock, collect_trace);
    assert(trace_events == 1);
    assert(strstr(trace_line, "total_ns=22500000 outside_ns=10000000 "
                              "requested_ns=2500000 wait_ns=12500000"));
    frame_pacer_clock_enable_trace(&clock);
    trace_events = 0;
    advance_dynamic(&clock, &time, &cfg, 20000000);
    for (unsigned i = 0; i < 1026; ++i)
        advance_dynamic(&clock, &time, &cfg, 22000000);
    frame_pacer_clock_flush_trace(&clock, collect_trace);
    assert(strstr(trace_summary, "count=1026 overwritten=2"));
    assert(trace_events == 1024);
    assert(strstr(trace_line, "outside_ns=22000000 requested_ns=0 wait_ns=0"));
    frame_pacer_clock_destroy(&clock);
}

int main(void)
{
    long_frame_trace();
    steady_overload();
    accelerated_recovery();
    dynamic_load_changes();
    sequential_waits();
    deadlines_saturate_at_uint64_max();
    concurrent_waits_are_serialized();
    return 0;
}
