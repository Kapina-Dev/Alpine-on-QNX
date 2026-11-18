#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syspage.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>

#define LINUX_NR_SETRLIMIT 75
#define LINUX_NR_GETRUSAGE 77
#define LINUX_NR_SETITIMER 104
#define LINUX_NR_GETITIMER 105
#define LINUX_NR_SYSINFO 116
#define LINUX_NR_UGETRLIMIT 191
#define LINUX_NR_PRLIMIT64 369

#define LINUX_EPERM 1
#define LINUX_EINVAL 22
#define LINUX_ENOSYS 38

struct linux_rlimit64 {
    uint64_t current;
    uint64_t maximum;
};

static int linux_resource_number(uint32_t resource)
{
    switch (resource) {
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
#ifdef RLIMIT_RSS
    case 5: return RLIMIT_RSS;
#endif
#ifdef RLIMIT_NPROC
    case 6: return RLIMIT_NPROC;
#endif
    case 7: return RLIMIT_NOFILE;
#ifdef RLIMIT_AS
    case 9: return RLIMIT_AS;
#endif
    default: return -1;
    }
}

/* Linux resources 8 (MEMLOCK), 10 (LOCKS), 11 (SIGPENDING), 12 (MSGQUEUE),
   13 (NICE), 14 (RTPRIO) and 15 (RTTIME) are valid there but have no QNX
   counterpart; 16 and above are plain EINVAL. */
static int rlimit_unmapped(uint32_t resource)
{
    return resource == 8 || (resource >= 10 && resource <= 15);
}

static uint64_t linux_rlimit_value(rlim_t value)
{
    return value == RLIM_INFINITY ? UINT64_MAX : (uint64_t)value;
}

static rlim_t host_rlimit_value(uint64_t value)
{
    return value == UINT64_MAX ? RLIM_INFINITY : (rlim_t)value;
}

static int32_t linux_prlimit64(int32_t process, uint32_t resource,
    const struct linux_rlimit64 *guest_new_limit,
    struct linux_rlimit64 *guest_old_limit)
{
    struct rlimit host_limit;
    struct rlimit host_new_limit;
    int host_resource = linux_resource_number(resource);
    if (process != 0 && process != (int32_t)getpid()) return -ESRCH;
    if (host_resource < 0) return -EINVAL;
    if (host_resource == RLIMIT_NOFILE) {
        uint64_t soft, hard;
        guest_fd_nofile_get(&soft, &hard);
        if (guest_new_limit != 0 &&
                guest_fd_nofile_set(guest_new_limit->current,
                    guest_new_limit->maximum) != 0)
            return -(int32_t)linux_errno_number(errno);
        if (guest_old_limit != 0) {
            guest_old_limit->current = soft;
            guest_old_limit->maximum = hard;
        }
        return 0;
    }
    if (getrlimit(host_resource, &host_limit) != 0)
        return -(int32_t)linux_errno_number(errno);
    if (guest_old_limit != 0) {
        guest_old_limit->current = linux_rlimit_value(host_limit.rlim_cur);
        guest_old_limit->maximum = linux_rlimit_value(host_limit.rlim_max);
    }
    if (guest_new_limit != 0) {
        /* Linux order: soft above hard is EINVAL, raising the hard limit
           without privilege is EPERM.  Decided here so it holds whatever
           the host's own setrlimit does. */
        if (guest_new_limit->current > guest_new_limit->maximum)
            return -LINUX_EINVAL;
        if (geteuid() != 0 && guest_new_limit->maximum >
                linux_rlimit_value(host_limit.rlim_max))
            return -LINUX_EPERM;
        host_new_limit.rlim_cur = host_rlimit_value(guest_new_limit->current);
        host_new_limit.rlim_max = host_rlimit_value(guest_new_limit->maximum);
        if (setrlimit(host_resource, &host_new_limit) != 0)
            return -(int32_t)linux_errno_number(errno);
    }
    return 0;
}

static uint32_t limit32(uint64_t value)
{
    return value >= 0xffffffffull ? 0xffffffffu : (uint32_t)value;
}

static uint64_t limit64(uint32_t value)
{
    return value == 0xffffffffu ? UINT64_MAX : value;
}

static int32_t linux_ugetrlimit(uint32_t resource, uint32_t *guest)
{
    struct linux_rlimit64 old;
    int32_t result;
    if (guest == 0) return -EFAULT;
    result = linux_prlimit64(0, resource, 0, &old);
    if (result != 0) return result;
    guest[0] = limit32(old.current);
    guest[1] = limit32(old.maximum);
    return 0;
}

static int32_t linux_setrlimit(uint32_t resource, const uint32_t *guest)
{
    struct linux_rlimit64 limit;
    if (guest == 0) return -EFAULT;
    limit.current = limit64(guest[0]);
    limit.maximum = limit64(guest[1]);
    return linux_prlimit64(0, resource, &limit, 0);
}

static int32_t linux_getrusage(int32_t who, void *guest)
{
    struct rusage usage;
    if (guest == 0) return -EFAULT;
    if (who == 0 || who == -1) {
        if (getrusage(who == 0 ? RUSAGE_SELF : RUSAGE_CHILDREN, &usage) != 0)
            return -(int32_t)linux_errno_number(errno);
    } else if (who == 1) {
        /* RUSAGE_THREAD: QNX cannot split user from system time for a
           thread (only the thread CPU clock exists), so all thread CPU time
           is reported as user time and every other field is 0. */
        struct timespec now;
        memset(&usage, 0, sizeof(usage));
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
            return -(int32_t)linux_errno_number(errno);
        usage.ru_utime.tv_sec = now.tv_sec;
        usage.ru_utime.tv_usec = now.tv_nsec / 1000;
    } else {
        return -LINUX_EINVAL;
    }
    linux_rusage_store(guest, &usage);
    return 0;
}

static void put32(unsigned char *buffer, size_t offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

/* QNX 8 has no usable sysconf(_SC_PHYS_PAGES) (it returns -1) and
   /proc/meminfo is not readable by an ordinary user, so:
   - total RAM is the sum of the system page's "ram" asinfo entries;
   - free RAM is posix_typed_mem_get_info() on the "/sysram" typed memory
     object, which reports the memory still available to allocate.
   QNX has no load average, so loads stay 0, and there is no cheap process
   count that is safe inside the SIGILL handler, so procs is 1. */
static uint64_t qnx_total_ram(void)
{
    const struct asinfo_entry *entry = SYSPAGE_ENTRY(asinfo);
    const char *strings = SYSPAGE_ENTRY(strings)->data;
    unsigned count = SYSPAGE_ENTRY_SIZE(asinfo) / sizeof(*entry);
    uint64_t total = 0;
    unsigned i;
    for (i = 0; i != count; ++i) {
        const char *name = strings + entry[i].name;
        if (strcmp(name, "ram") == 0)
            total += (uint64_t)entry[i].end - (uint64_t)entry[i].start + 1;
    }
    return total;
}

static uint64_t qnx_free_ram(void)
{
    struct posix_typed_mem_info info;
    uint64_t free_bytes = 0;
    int fd = posix_typed_mem_open("/sysram", O_RDWR,
        POSIX_TYPED_MEM_ALLOCATE);
    if (fd < 0) return 0;
    if (posix_typed_mem_get_info(fd, &info) == 0)
        free_bytes = (uint64_t)info.posix_tmi_length;
    close(fd);
    return free_bytes;
}

static int32_t linux_sysinfo(void *guest)
{
    unsigned char info[64];
    struct timespec now;
    uint64_t total, avail, unit = 1;
    if (guest == 0) return -EFAULT;
    total = qnx_total_ram();
    avail = qnx_free_ram();
    if (avail > total) avail = total;
    while (total / unit > 0xffffffffull) unit <<= 1;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -(int32_t)linux_errno_number(errno);
    memset(info, 0, sizeof(info));
    put32(info, 0, (uint32_t)now.tv_sec);
    put32(info, 16, (uint32_t)(total / unit));
    put32(info, 20, (uint32_t)(avail / unit));
    put32(info, 40, 1);
    put32(info, 52, (uint32_t)unit);
    memcpy(guest, info, sizeof(info));
    return 0;
}

static int itimer_which(uint32_t which)
{
    switch (which) {
    case 0: return ITIMER_REAL;
    case 1: return ITIMER_VIRTUAL;
    case 2: return ITIMER_PROF;
    default: return -1;
    }
}

static void itimer_store(uint32_t *guest, const struct itimerval *value)
{
    guest[0] = (uint32_t)value->it_interval.tv_sec;
    guest[1] = (uint32_t)value->it_interval.tv_usec;
    guest[2] = (uint32_t)value->it_value.tv_sec;
    guest[3] = (uint32_t)value->it_value.tv_usec;
}

/* QNX refuses ITIMER_VIRTUAL and ITIMER_PROF: report them for
   LINUXEMU_DIAGNOSTICS and return EINVAL. */
static int32_t itimer_host_failure(uint32_t number, uint32_t which,
    const uint32_t *args)
{
    if (which == 1)
        return linux_report_unsupported(number, LINUX_EINVAL,
            "itimer-virtual", args);
    if (which == 2)
        return linux_report_unsupported(number, LINUX_EINVAL,
            "itimer-prof", args);
    return -(int32_t)linux_errno_number(errno);
}

/* Linux keeps an armed ITIMER_REAL across execve; QNX drops it.  When the
   guest execs with a timer armed, record it as unsupported (detail
   itimer-exec, not an error: errno 0) and let the exec proceed.  Carrying
   the timer over is Phase 6 work. */
void linux_resource_note_exec(const uint32_t args[6])
{
    struct itimerval value;
    if (getitimer(ITIMER_REAL, &value) != 0) return;
    if (value.it_value.tv_sec == 0 && value.it_value.tv_usec == 0) return;
    linux_report_unsupported(11, 0, "itimer-exec", args);
}

static int32_t linux_getitimer(uint32_t number, uint32_t which,
    uint32_t *guest, const uint32_t *args)
{
    struct itimerval value;
    int host = itimer_which(which);
    if (guest == 0) return -EFAULT;
    if (host < 0) return -LINUX_EINVAL;
    if (getitimer(host, &value) != 0) {
        if (host != ITIMER_REAL) return itimer_host_failure(number, which, args);
        /* QNX fails getitimer(ITIMER_REAL) with EINVAL while no timer has
           been created in the process (a fresh process, a fork child, or
           after exec); Linux reports a disarmed timer. */
        if (errno != EINVAL)
            return -(int32_t)linux_errno_number(errno);
        memset(&value, 0, sizeof(value));
    }
    itimer_store(guest, &value);
    return 0;
}

/* Limit: an armed ITIMER_REAL does not survive execve on QNX, unlike Linux
   (see linux_resource_note_exec). */
static int32_t linux_setitimer(uint32_t number, uint32_t which,
    const uint32_t *guest_new, uint32_t *guest_old, const uint32_t *args)
{
    struct itimerval value, old;
    int host = itimer_which(which);
    if (host < 0) return -LINUX_EINVAL;
    memset(&value, 0, sizeof(value));
    if (guest_new != 0) {
        if (guest_new[1] >= 1000000u || guest_new[3] >= 1000000u ||
                (int32_t)guest_new[0] < 0 || (int32_t)guest_new[2] < 0)
            return -LINUX_EINVAL;
        value.it_interval.tv_sec = (int32_t)guest_new[0];
        value.it_interval.tv_usec = (int32_t)guest_new[1];
        value.it_value.tv_sec = (int32_t)guest_new[2];
        value.it_value.tv_usec = (int32_t)guest_new[3];
    }
    if (setitimer(host, &value, &old) != 0)
        return itimer_host_failure(number, which, args);
    if (guest_old != 0) itimer_store(guest_old, &old);
    return 0;
}

int32_t linux_resource_syscall(uint32_t number, uint32_t args[6])
{
    switch (number) {
    case LINUX_NR_SETRLIMIT:
        if (rlimit_unmapped(args[0]))
            return linux_report_unsupported(number, LINUX_EINVAL,
                "rlimit-resource", args);
        return linux_setrlimit(args[0], (const uint32_t *)args[1]);
    case LINUX_NR_UGETRLIMIT:
        if (rlimit_unmapped(args[0]))
            return linux_report_unsupported(number, LINUX_EINVAL,
                "rlimit-resource", args);
        return linux_ugetrlimit(args[0], (uint32_t *)args[1]);
    case LINUX_NR_PRLIMIT64:
        if (rlimit_unmapped(args[1]))
            return linux_report_unsupported(number, LINUX_EINVAL,
                "rlimit-resource", args);
        return linux_prlimit64((int32_t)args[0], args[1],
            (const struct linux_rlimit64 *)args[2],
            (struct linux_rlimit64 *)args[3]);
    case LINUX_NR_GETRUSAGE:
        return linux_getrusage((int32_t)args[0], (void *)args[1]);
    case LINUX_NR_SYSINFO:
        return linux_sysinfo((void *)args[0]);
    case LINUX_NR_SETITIMER:
        return linux_setitimer(number, args[0], (const uint32_t *)args[1],
            (uint32_t *)args[2], args);
    case LINUX_NR_GETITIMER:
        return linux_getitimer(number, args[0], (uint32_t *)args[1], args);
    default:
        return linux_report_unsupported(number, LINUX_ENOSYS,
            "resource-syscall", args);
    }
}
