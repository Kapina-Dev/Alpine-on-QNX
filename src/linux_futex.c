#include "linuxemu.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define LINUX_FUTEX_WAIT 0u
#define LINUX_FUTEX_WAKE 1u
#define LINUX_FUTEX_REQUEUE 3u
#define LINUX_FUTEX_CMP_REQUEUE 4u
#define LINUX_FUTEX_WAIT_BITSET 9u
#define LINUX_FUTEX_WAKE_BITSET 10u
#define LINUX_FUTEX_PRIVATE_FLAG 128u
#define LINUX_FUTEX_CLOCK_REALTIME 256u
#define LINUX_FUTEX_CMD_MASK 127u
#define LINUX_FUTEX_BITSET_MATCH_ANY 0xffffffffu

struct futex_waiter {
    uint32_t *address;
    uint32_t bitset;
    int woken;
    pthread_cond_t condition;
    struct futex_waiter *next;
};

static pthread_mutex_t futex_lock = PTHREAD_MUTEX_INITIALIZER;
static struct futex_waiter *futex_waiters;

void linux_futex_after_fork(void)
{
    pthread_mutex_t fresh_lock = PTHREAD_MUTEX_INITIALIZER;
    futex_lock = fresh_lock;
    futex_waiters = 0;
}

static int load_linux_timespec(const void *guest_timeout,
    struct timespec *timeout)
{
    int32_t values[2];
    memcpy(values, guest_timeout, sizeof(values));
    if (values[0] < 0 || values[1] < 0 || values[1] >= 1000000000)
        return -1;
    timeout->tv_sec = values[0];
    timeout->tv_nsec = values[1];
    return 0;
}

static int relative_deadline(const void *guest_timeout,
    struct timespec *deadline)
{
    struct timespec relative;
    if (load_linux_timespec(guest_timeout, &relative) != 0) return -1;
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) return -1;
    deadline->tv_sec += relative.tv_sec;
    deadline->tv_nsec += relative.tv_nsec;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int absolute_deadline(const void *guest_timeout, int realtime,
    struct timespec *deadline)
{
    struct timespec guest;
    struct timespec monotonic_now;
    struct timespec realtime_now;
    time_t seconds;
    long nanoseconds;

    if (load_linux_timespec(guest_timeout, &guest) != 0) return -1;
    if (realtime) {
        *deadline = guest;
        return 0;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic_now) != 0 ||
        clock_gettime(CLOCK_REALTIME, &realtime_now) != 0) return -1;
    if (guest.tv_sec < monotonic_now.tv_sec ||
        (guest.tv_sec == monotonic_now.tv_sec &&
        guest.tv_nsec <= monotonic_now.tv_nsec)) {
        *deadline = realtime_now;
        return 0;
    }
    seconds = guest.tv_sec - monotonic_now.tv_sec;
    nanoseconds = guest.tv_nsec - monotonic_now.tv_nsec;
    if (nanoseconds < 0) { seconds--; nanoseconds += 1000000000L; }
    deadline->tv_sec = realtime_now.tv_sec + seconds;
    deadline->tv_nsec = realtime_now.tv_nsec + nanoseconds;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static void remove_waiter(struct futex_waiter *waiter)
{
    struct futex_waiter **link = &futex_waiters;
    while (*link != 0 && *link != waiter) link = &(*link)->next;
    if (*link == waiter) *link = waiter->next;
}

static int32_t futex_wait(uint32_t *address, uint32_t expected,
    const void *guest_timeout, uint32_t bitset, int absolute_clock)
{
    struct futex_waiter waiter;
    struct timespec deadline;
    const struct timespec *deadline_pointer = 0;
    int error = 0;

    if (bitset == 0) return -EINVAL;
    if (guest_timeout != 0) {
        if (absolute_clock != 0) {
            if (absolute_deadline(guest_timeout, absolute_clock == 2,
                    &deadline) != 0)
                return -EINVAL;
        } else if (relative_deadline(guest_timeout, &deadline) != 0)
            return -EINVAL;
        deadline_pointer = &deadline;
    }
    memset(&waiter, 0, sizeof(waiter));
    waiter.address = address;
    waiter.bitset = bitset;
    error = pthread_cond_init(&waiter.condition, 0);
    if (error != 0) return -(int32_t)linux_errno_number(error);
    error = pthread_mutex_lock(&futex_lock);
    if (error != 0) {
        pthread_cond_destroy(&waiter.condition);
        return -(int32_t)linux_errno_number(error);
    }
    __sync_synchronize();
    if (*(volatile uint32_t *)address != expected) {
        pthread_mutex_unlock(&futex_lock);
        pthread_cond_destroy(&waiter.condition);
        return -EAGAIN;
    }
    waiter.next = futex_waiters;
    futex_waiters = &waiter;
    while (!waiter.woken && error == 0) {
        error = deadline_pointer == 0 ?
            pthread_cond_wait(&waiter.condition, &futex_lock) :
            pthread_cond_timedwait(&waiter.condition, &futex_lock,
                deadline_pointer);
    }
    remove_waiter(&waiter);
    pthread_mutex_unlock(&futex_lock);
    pthread_cond_destroy(&waiter.condition);
    if (waiter.woken) return 0;
    if (error == ETIMEDOUT) return -110;
    return error == 0 ? 0 : -(int32_t)linux_errno_number(error);
}

int32_t linux_futex_wake(uint32_t *address, int count, uint32_t bitset)
{
    struct futex_waiter *waiter;
    int woken = 0;
    int error;

    if (count < 0 || bitset == 0) return -EINVAL;
    error = pthread_mutex_lock(&futex_lock);
    if (error != 0) return -(int32_t)linux_errno_number(error);
    for (waiter = futex_waiters; waiter != 0 && woken < count;
            waiter = waiter->next) {
        if (waiter->address != address ||
            (waiter->bitset & bitset) == 0 || waiter->woken) continue;
        waiter->woken = 1;
        pthread_cond_signal(&waiter->condition);
        woken++;
    }
    pthread_mutex_unlock(&futex_lock);
    return woken;
}

static int32_t futex_requeue(uint32_t *address, int wake_count,
    int requeue_count, uint32_t *second_address, int compare,
    uint32_t expected)
{
    struct futex_waiter *waiter;
    int affected = 0;
    int woken = 0;
    int requeued = 0;
    int error;

    if (wake_count < 0 || requeue_count < 0 || second_address == 0)
        return -EINVAL;
    error = pthread_mutex_lock(&futex_lock);
    if (error != 0) return -(int32_t)linux_errno_number(error);
    __sync_synchronize();
    if (compare && *(volatile uint32_t *)address != expected) {
        pthread_mutex_unlock(&futex_lock);
        return -EAGAIN;
    }
    for (waiter = futex_waiters; waiter != 0; waiter = waiter->next) {
        if (waiter->address != address || waiter->woken) continue;
        if (woken < wake_count) {
            waiter->woken = 1;
            pthread_cond_signal(&waiter->condition);
            woken++;
            affected++;
        } else if (requeued < requeue_count) {
            waiter->address = second_address;
            requeued++;
            affected++;
        }
        if (woken == wake_count && requeued == requeue_count) break;
    }
    pthread_mutex_unlock(&futex_lock);
    return affected;
}

int32_t linux_futex(uint32_t *address, uint32_t operation, uint32_t value,
    const void *guest_timeout, uint32_t *second_address, uint32_t bitset)
{
    uint32_t command = operation & LINUX_FUTEX_CMD_MASK;
    uint32_t flags = operation & ~LINUX_FUTEX_CMD_MASK;

    if ((flags & ~(LINUX_FUTEX_PRIVATE_FLAG |
            LINUX_FUTEX_CLOCK_REALTIME)) != 0) return -EINVAL;
    if ((operation & LINUX_FUTEX_CLOCK_REALTIME) != 0 &&
        command != LINUX_FUTEX_WAIT_BITSET) return -38;
    switch (command) {
    case LINUX_FUTEX_WAIT:
        return futex_wait(address, value, guest_timeout,
            LINUX_FUTEX_BITSET_MATCH_ANY, 0);
    case LINUX_FUTEX_WAKE:
        if (value > INT_MAX) return -EINVAL;
        return linux_futex_wake(address, (int)value,
            LINUX_FUTEX_BITSET_MATCH_ANY);
    case LINUX_FUTEX_REQUEUE:
        if (value > INT_MAX || (uintptr_t)guest_timeout > INT_MAX)
            return -EINVAL;
        return futex_requeue(address, (int)value,
            (int)(uintptr_t)guest_timeout, second_address, 0, 0);
    case LINUX_FUTEX_CMP_REQUEUE:
        if (value > INT_MAX || (uintptr_t)guest_timeout > INT_MAX)
            return -EINVAL;
        return futex_requeue(address, (int)value,
            (int)(uintptr_t)guest_timeout, second_address, 1, bitset);
    case LINUX_FUTEX_WAIT_BITSET:
        return futex_wait(address, value, guest_timeout, bitset,
            (operation & LINUX_FUTEX_CLOCK_REALTIME) != 0 ? 2 : 1);
    case LINUX_FUTEX_WAKE_BITSET:
        if (value > INT_MAX) return -EINVAL;
        return linux_futex_wake(address, (int)value, bitset);
    default:
        return -38;
    }
}
