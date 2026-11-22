#include "pacer_clock.h"

#include <errno.h>
#include <stdlib.h>
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

#define DYNAMIC_WINDOW_NS UINT64_C(300000000)
#define DYNAMIC_RETRY_NS UINT64_C(600000000)

static int compare_work(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static void reset_rise(struct frame_pacer_clock *clock)
{
    clock->next_rise = clock->dynamic.step_up;
    clock->headroom_confirmed = false;
    clock->previous_cap = 0;
    clock->late_streak = 0;
}

static uint32_t adjust_cap(struct frame_pacer_clock *clock, uint32_t fps,
                           uint64_t now, uint32_t work)
{
    const struct frame_pacer_dynamic_config *cfg = &clock->dynamic;
    if (clock->previous_cap) {
        /* Only roll back an unconfirmed rise. One late frame is tolerated. */
        if ((uint64_t)work * fps > UINT64_C(1000000000))
            ++clock->late_streak;
        else
            clock->late_streak = 0;
        if (clock->late_streak >= 2) {
            fps = clock->previous_cap;
            reset_rise(clock);
            clock->retry_after_ns = add_intervals(now, DYNAMIC_RETRY_NS, 1);
            clock->window_start_ns = now;
            clock->samples = 0;
            return fps;
        }
    }

    clock->work_samples[clock->samples++] = work;
    if (clock->samples < FRAME_PACER_WINDOW_CAPACITY &&
        (now - clock->window_start_ns < DYNAMIC_WINDOW_NS ||
         clock->samples < 5))
        return fps;

    qsort(clock->work_samples, clock->samples, sizeof(clock->work_samples[0]),
          compare_work);
    /* Nearest-rank P80. Five or more samples exclude a single worst frame. */
    uint64_t p80 = clock->work_samples[clock->samples - clock->samples / 5 - 1];
    clock->samples = 0;
    clock->window_start_ns = now;
    uint64_t capacity =
        p80 ? cfg->target_load * UINT64_C(10000000) / p80 : UINT64_MAX;
    uint64_t supported = capacity;
    if (supported < cfg->min_fps)
        supported = cfg->min_fps;
    if (supported > cfg->max_fps)
        supported = cfg->max_fps;

    unsigned high = cfg->target_load > 95 ? 100 : cfg->target_load + 5;
    if (supported < fps && p80 * fps >= high * UINT64_C(10000000)) {
        uint32_t drop = fps - (uint32_t)supported;
        uint32_t next = fps - (drop < cfg->step_down ? drop : cfg->step_down);
        if (clock->previous_cap && clock->previous_cap < next)
            next = clock->previous_cap;
        if (clock->previous_cap)
            clock->retry_after_ns = add_intervals(now, DYNAMIC_RETRY_NS, 1);
        reset_rise(clock);
        return next;
    }

    bool climbing = clock->previous_cap != 0;
    clock->previous_cap = 0;
    clock->late_streak = 0;
    /* Allow a running climb to finish its last small step. */
    bool room = supported > fps &&
                (climbing ||
                 (capacity - fps >= 3 &&
                  p80 * fps * 105 <= cfg->target_load * UINT64_C(1000000000)));
    if (!room) {
        reset_rise(clock);
        return fps;
    }
    if (!clock->headroom_confirmed) {
        clock->headroom_confirmed = true;
        return fps;
    }

    if (now < clock->retry_after_ns)
        return fps;

    uint32_t rise = (uint32_t)supported - fps;
    if (rise > clock->next_rise)
        rise = clock->next_rise;
    clock->previous_cap = fps;
    /* Preserve an explicitly configured base step above eight. */
    unsigned maximum_step = cfg->step_up > 8 ? cfg->step_up : 8;
    clock->next_rise *= 2;
    if (clock->next_rise > maximum_step)
        clock->next_rise = maximum_step;
    return fps + rise;
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
        clock->samples = 0;
        clock->retry_after_ns = 0;
        reset_rise(clock);
        clock->started = true;
        decision->first = true;
        return;
    }
    uint32_t adjusted =
        adjust_cap(clock, fps, now, (uint32_t)(now - clock->last_release_ns));
    if (fps != adjusted)
        clock->fps = adjusted;
    fps = adjusted;
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
