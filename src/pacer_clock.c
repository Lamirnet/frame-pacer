#include "pacer_clock.h"

#include <errno.h>
#include <string.h>

static uint64_t add_intervals(uint64_t value, uint64_t interval, uint64_t count)
{
    return count > (UINT64_MAX - value) / interval ? UINT64_MAX
                                                   : value + count * interval;
}

void frame_pacer_clock_init(struct frame_pacer_clock *clock)
{
    if (!clock)
        return;
    memset(clock, 0, sizeof(*clock));
    atomic_init(&clock->fps, 0);
    clock->initialized = pthread_mutex_init(&clock->mutex, 0) == 0;
}

void frame_pacer_clock_destroy(struct frame_pacer_clock *clock)
{
    if (!clock || !clock->initialized)
        return;
    clock->initialized = false;
    (void)pthread_mutex_destroy(&clock->mutex);
}

/* Called under the pacing mutex. Time outside our wait includes any
 * downstream present, driver or compositor blocking. */
static void dynamic_wait(struct frame_pacer_clock *clock,
                         const struct frame_pacer_dynamic_config *cfg,
                         frame_pacer_now_fn now_fn,
                         frame_pacer_sleep_fn sleep_fn, void *opaque,
                         struct frame_pacer_decision *decision)
{
    uint64_t now = now_fn(opaque);
    uint32_t fps = clock->fps;
    decision->observed_ns = now;

    if (!clock->dynamic.enabled || memcmp(cfg, &clock->dynamic, sizeof(*cfg)) ||
        now < clock->last_release_ns ||
        now - clock->last_release_ns > UINT64_C(2000000000)) {
        clock->dynamic = *cfg;
        clock->fps = cfg->max_fps;
        clock->window_start_ns = clock->last_release_ns = now;
        clock->work_ns = clock->samples = 0;
        clock->started = true;
        decision->first = true;
        return;
    }
    clock->work_ns += now - clock->last_release_ns;
    ++clock->samples;
    if (now - clock->window_start_ns >= UINT64_C(300000000)) {
        uint64_t mean = clock->work_ns / clock->samples;
        uint64_t load = mean * fps * 100 / UINT64_C(1000000000);
        unsigned high = cfg->target_load > 95 ? 100 : cfg->target_load + 5;
        unsigned low = cfg->target_load > 10 ? cfg->target_load - 10 : 0;

        if (load >= high)
            fps -= fps - cfg->min_fps < cfg->step_down ? fps - cfg->min_fps
                                                       : cfg->step_down;
        else if (load <= low)
            fps += cfg->max_fps - fps < cfg->step_up ? cfg->max_fps - fps
                                                     : cfg->step_up;
        clock->fps = fps;
        clock->window_start_ns = now;
        clock->work_ns = clock->samples = 0;
    }
    decision->deadline_ns =
        add_intervals(clock->last_release_ns, UINT64_C(1000000000) / fps, 1);
    decision->missed = now >= decision->deadline_ns;
    if (!decision->missed)
        while (sleep_fn(opaque, decision->deadline_ns) == EINTR)
            ++decision->interruptions;
    clock->last_release_ns = now_fn(opaque);
}

void frame_pacer_clock_wait_config(struct frame_pacer_clock *clock,
                                   uint32_t fps,
                                   const struct frame_pacer_dynamic_config *cfg,
                                   frame_pacer_now_fn now_fn,
                                   frame_pacer_sleep_fn sleep_fn, void *opaque,
                                   struct frame_pacer_decision *decision)
{
    uint64_t interval_ns;
    uint64_t now_ns;

    if (decision)
        *decision = (struct frame_pacer_decision){0};
    if (!clock || !clock->initialized || !now_fn || !sleep_fn || !decision)
        return;
    (void)pthread_mutex_lock(&clock->mutex);
    if (cfg && cfg->enabled) {
        dynamic_wait(clock, cfg, now_fn, sleep_fn, opaque, decision);
        (void)pthread_mutex_unlock(&clock->mutex);
        return;
    }
    if (clock->dynamic.enabled) {
        clock->dynamic.enabled = false;
        clock->started = false;
    }
    if (fps < FRAME_PACER_MIN_FPS || fps > FRAME_PACER_MAX_FPS) {
        clock->started = false;
        clock->fps = FRAME_PACER_FPS_LIMIT_OFF;
        clock->next_deadline_ns = 0;
        (void)pthread_mutex_unlock(&clock->mutex);
        return;
    }
    interval_ns = UINT64_C(1000000000) / fps;

    now_ns = now_fn(opaque);
    decision->observed_ns = now_ns;

    if (!clock->started || clock->fps != fps) {
        clock->started = true;
        clock->fps = fps;
        clock->next_deadline_ns = add_intervals(now_ns, interval_ns, 1);
        decision->first = true;
        (void)pthread_mutex_unlock(&clock->mutex);
        return;
    }

    if (now_ns >= clock->next_deadline_ns) {
        uint64_t missed_intervals =
            (now_ns - clock->next_deadline_ns) / interval_ns + 1;

        clock->next_deadline_ns = add_intervals(clock->next_deadline_ns,
                                                interval_ns, missed_intervals);
        decision->missed = true;
    }

    decision->deadline_ns = clock->next_deadline_ns;
    clock->next_deadline_ns =
        add_intervals(clock->next_deadline_ns, interval_ns, 1);
    while (sleep_fn(opaque, decision->deadline_ns) == EINTR)
        ++decision->interruptions;

    (void)pthread_mutex_unlock(&clock->mutex);
}

void frame_pacer_clock_wait(struct frame_pacer_clock *clock, uint32_t fps,
                            frame_pacer_now_fn now_fn,
                            frame_pacer_sleep_fn sleep_fn, void *opaque,
                            struct frame_pacer_decision *decision)
{
    frame_pacer_clock_wait_config(clock, fps, NULL, now_fn, sleep_fn, opaque,
                                  decision);
}
