/* Test helper for tests/sim/screens/shots_compare.py, loaded with LD_PRELOAD into `zero_bot --shots`: the wall
 * clock starts at ZB_FIXED_TIME (seconds since 1970) and runs on from there, so screens that show
 * "today", a date a few days away or a countdown come out the same on every run. The monotonic
 * clock (LVGL's tick, animations) is left alone. Without ZB_FIXED_TIME it changes nothing.
 *
 *   cc -shared -fPIC -O2 -o fixclock.so tests/sim/screens/fixclock.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

static long long offset_ns;
static int ready;
static int (*real_clock_gettime)(clockid_t, struct timespec *);

static void init(void) {
    if (ready) return;
    ready = 1;
    real_clock_gettime = (int (*)(clockid_t, struct timespec *))dlsym(RTLD_NEXT, "clock_gettime");
    const char *fixed = getenv("ZB_FIXED_TIME");
    if (!fixed || !*fixed || !real_clock_gettime) return;
    struct timespec now;
    real_clock_gettime(CLOCK_REALTIME, &now);
    offset_ns = (atoll(fixed) - (long long)now.tv_sec) * 1000000000LL - now.tv_nsec;
}

int clock_gettime(clockid_t id, struct timespec *ts) {
    init();
    if (!real_clock_gettime) return -1;
    const int r = real_clock_gettime(id, ts);
    if (r == 0 && offset_ns && (id == CLOCK_REALTIME || id == CLOCK_REALTIME_COARSE)) {
        const long long ns = (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec + offset_ns;
        ts->tv_sec = (time_t)(ns / 1000000000LL);
        ts->tv_nsec = (long)(ns % 1000000000LL);
    }
    return r;
}

time_t time(time_t *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return (time_t)-1;
    if (out) *out = ts.tv_sec;
    return ts.tv_sec;
}

int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
    (void)tz;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    if (tv) { tv->tv_sec = ts.tv_sec; tv->tv_usec = ts.tv_nsec / 1000; }
    return 0;
}
