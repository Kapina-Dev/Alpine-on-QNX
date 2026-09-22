#include "linuxemu.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define LINUX_NR_TIME 13
#define LINUX_NR_GETTIMEOFDAY 78
#define LINUX_NR_NANOSLEEP 162
#define LINUX_NR_CLOCK_GETTIME 263
#define LINUX_NR_CLOCK_GETRES 264
#define LINUX_NR_CLOCK_NANOSLEEP 265
#define LINUX_NR_CLOCK_GETTIME64 403
#define LINUX_NR_CLOCK_GETRES_TIME64 406
#define LINUX_NR_CLOCK_NANOSLEEP_TIME64 407
#define LINUX_TIMER_ABSTIME 1u

static int map_clock(int linux_clock, clockid_t *host_clock)
{
    switch (linux_clock) {
    case 0: *host_clock = CLOCK_REALTIME; return 0;
    case 1: *host_clock = CLOCK_MONOTONIC; return 0;
    case 2: *host_clock = CLOCK_PROCESS_CPUTIME_ID; return 0;
    case 3: *host_clock = CLOCK_THREAD_CPUTIME_ID; return 0;
    default: return -1;
    }
}

static int load_time32(const void *address, struct timespec *value)
{
    int32_t fields[2];
    if (address == 0) return -EFAULT;
    memcpy(fields, address, sizeof(fields));
    if (fields[0] < 0 || fields[1] < 0 || fields[1] >= 1000000000)
        return -EINVAL;
    value->tv_sec = fields[0];
    value->tv_nsec = fields[1];
    return 0;
}

static int load_time64(const void *address, struct timespec *value)
{
    int64_t fields[2];
    if (address == 0) return -EFAULT;
    memcpy(fields, address, sizeof(fields));
    if (fields[0] < 0 || fields[0] > LONG_MAX || fields[1] < 0 ||
        fields[1] >= 1000000000) return -EINVAL;
    value->tv_sec = (time_t)fields[0];
    value->tv_nsec = (long)fields[1];
    return 0;
}

static void store_time32(void *address, const struct timespec *value)
{
    int32_t fields[2];
    fields[0] = (int32_t)value->tv_sec;
    fields[1] = (int32_t)value->tv_nsec;
    memcpy(address, fields, sizeof(fields));
}

static void store_time64(void *address, const struct timespec *value)
{
    int64_t fields[2];
    fields[0] = value->tv_sec;
    fields[1] = value->tv_nsec;
    memcpy(address, fields, sizeof(fields));
}

static int32_t get_clock(uint32_t *arguments, int time64, int resolution)
{
    clockid_t host_clock;
    struct timespec value;
    void *result = (void *)arguments[1];
    if (map_clock((int)arguments[0], &host_clock) != 0) return -EINVAL;
    if (result == 0) {
        if (!resolution) return -EFAULT;
        return clock_getres(host_clock, 0) == 0 ? 0 :
            -(int32_t)linux_errno_number(errno);
    }
    if ((resolution ? clock_getres(host_clock, &value) :
            clock_gettime(host_clock, &value)) != 0)
        return -(int32_t)linux_errno_number(errno);
    if (!time64 && value.tv_sec > INT32_MAX) return -EOVERFLOW;
    if (time64) store_time64(result, &value);
    else store_time32(result, &value);
    return 0;
}

static int32_t sleep_clock(uint32_t *arguments, int time64, int clock_call)
{
    clockid_t host_clock = CLOCK_REALTIME;
    uint32_t flags = 0;
    const void *request;
    void *remainder;
    struct timespec wanted;
    struct timespec left;
    int result;

    if (clock_call) {
        if (map_clock((int)arguments[0], &host_clock) != 0) return -EINVAL;
        flags = arguments[1];
        request = (const void *)arguments[2];
        remainder = (void *)arguments[3];
        if ((flags & ~LINUX_TIMER_ABSTIME) != 0) return -EINVAL;
    } else {
        request = (const void *)arguments[0];
        remainder = (void *)arguments[1];
    }
    result = time64 ? load_time64(request, &wanted) :
        load_time32(request, &wanted);
    if (result != 0) return result;
    if (clock_call)
        result = clock_nanosleep(host_clock,
            (flags & LINUX_TIMER_ABSTIME) != 0 ? TIMER_ABSTIME : 0,
            &wanted, &left);
    else result = nanosleep(&wanted, &left);
    if (result != 0) {
        int saved_errno = clock_call && result > 0 ? result : errno;
        if (remainder != 0 && (flags & LINUX_TIMER_ABSTIME) == 0) {
            if (time64) store_time64(remainder, &left);
            else store_time32(remainder, &left);
        }
        return -(int32_t)linux_errno_number(saved_errno);
    }
    return 0;
}

int32_t linux_time_syscall(uint32_t number, uint32_t arguments[6])
{
    if (number == LINUX_NR_TIME) {
        time_t now = time(0);
        if (now == (time_t)-1) return -(int32_t)linux_errno_number(errno);
        if (now > INT32_MAX) return -EOVERFLOW;
        if (arguments[0] != 0) {
            int32_t stored = (int32_t)now;
            memcpy((void *)arguments[0], &stored, sizeof(stored));
        }
        return (int32_t)now;
    }
    if (number == LINUX_NR_GETTIMEOFDAY) {
        struct timeval now;
        int32_t fields[2];
        if (arguments[0] == 0 && arguments[1] == 0) return 0;
        if (gettimeofday(&now, 0) != 0)
            return -(int32_t)linux_errno_number(errno);
        if (now.tv_sec > INT32_MAX) return -EOVERFLOW;
        fields[0] = (int32_t)now.tv_sec;
        fields[1] = (int32_t)now.tv_usec;
        if (arguments[0] != 0)
            memcpy((void *)arguments[0], fields, sizeof(fields));
        if (arguments[1] != 0) memset((void *)arguments[1], 0, 8);
        return 0;
    }
    if (number == LINUX_NR_NANOSLEEP)
        return sleep_clock(arguments, 0, 0);
    if (number == LINUX_NR_CLOCK_GETTIME)
        return get_clock(arguments, 0, 0);
    if (number == LINUX_NR_CLOCK_GETRES)
        return get_clock(arguments, 0, 1);
    if (number == LINUX_NR_CLOCK_NANOSLEEP)
        return sleep_clock(arguments, 0, 1);
    if (number == LINUX_NR_CLOCK_GETTIME64)
        return get_clock(arguments, 1, 0);
    if (number == LINUX_NR_CLOCK_GETRES_TIME64)
        return get_clock(arguments, 1, 1);
    if (number == LINUX_NR_CLOCK_NANOSLEEP_TIME64)
        return sleep_clock(arguments, 1, 1);
    return -ENOSYS;
}
