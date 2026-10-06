// TEST ONLY: explicitly preloaded by the reviewed synthetic capture adapter.
// Freeze wall time; leave monotonic clocks and actual financial code untouched.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace {
time_t fixture_epoch = 0;
bool loaded = false;
void load_epoch() noexcept {
    if (loaded) return;
    const char* value = std::getenv("QT_MODEL_CAPTURE_EPOCH");
    if (!value || !*value || std::strlen(value) != 10) _exit(87);
    for (const char* p = value; *p; ++p) if (*p < '0' || *p > '9') _exit(87);
    char* end = nullptr;
    errno = 0;
    const long long epoch = std::strtoll(value, &end, 10);
    if (errno || !end || *end || epoch < 946684800LL || epoch > 4102444800LL) _exit(87);
    fixture_epoch = static_cast<time_t>(epoch);
    if (static_cast<long long>(fixture_epoch) != epoch) _exit(87);
    loaded = true;
}
__attribute__((constructor)) void cache_control_before_environment_clear() noexcept { load_epoch(); }
}

extern "C" int clock_gettime(clockid_t id, struct timespec* result) noexcept {
    load_epoch();
    if (id == CLOCK_REALTIME) {
        if (!result) { errno = EFAULT; return -1; }
        result->tv_sec = fixture_epoch;
        result->tv_nsec = 0;
        return 0;
    }
    // Adapter/process deadlines keep progressing; no monotonic result is changed.
    using Function = int (*)(clockid_t, struct timespec*);
    static const auto real_clock = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "clock_gettime"));
    if (!real_clock) _exit(87);
    return real_clock(id, result);
}

extern "C" time_t time(time_t* result) noexcept {
    load_epoch();
    if (result) *result = fixture_epoch;
    return fixture_epoch;
}

extern "C" int gettimeofday(struct timeval* result, void*) noexcept {
    load_epoch();
    if (!result) { errno = EFAULT; return -1; }
    result->tv_sec = fixture_epoch;
    result->tv_usec = 0;
    return 0;
}
