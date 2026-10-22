// scripts/live_mode_gate/hostclock_shim.c
//
// The clock shim of the live mode gate (gate.sh). NEVER part of the product, never linked into it.
// It shifts the wall clock of ONE process by TDATED_HOST_CLOCK_OFFSET seconds, so an unpatched live
// runner sees a chosen date as the host's own date: that is how the gate runs production mode on a
// machine whose data ends before today. Every clock read of the process moves together
// (clock_gettime(CLOCK_REALTIME), gettimeofday, time), so a wall-clock decision added to the runner
// later is exercised too, which a test switch inside the product could not promise.
// macOS: clang -dynamiclib, loaded with DYLD_INSERT_LIBRARIES. The database server's clock is not touched.
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
static long long off(void) { const char* e = getenv("TDATED_HOST_CLOCK_OFFSET"); return e ? atoll(e) : 0; }
int shim_clock_gettime(clockid_t id, struct timespec* ts) {
    int rc = clock_gettime(id, ts);
    if (rc == 0 && id == CLOCK_REALTIME) ts->tv_sec += off();
    return rc;
}
int shim_gettimeofday(struct timeval* tv, void* tz) {
    int rc = gettimeofday(tv, tz);
    if (rc == 0 && tv) tv->tv_sec += off();
    return rc;
}
time_t shim_time(time_t* t) {
    time_t v = time(NULL) + off();
    if (t) *t = v;
    return v;
}
#define INTERPOSE(n, o) __attribute__((used)) static struct { const void* a; const void* b; } _i_##o \
    __attribute__((section("__DATA,__interpose"))) = {(const void*)(unsigned long)&n, (const void*)(unsigned long)&o};
INTERPOSE(shim_clock_gettime, clock_gettime)
INTERPOSE(shim_gettimeofday, gettimeofday)
INTERPOSE(shim_time, time)
