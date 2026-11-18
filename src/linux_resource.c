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
#include <sys/neutrino.h>
#include <signal.h>

#define LINUX_NR_SETRLIMIT 75
#define LINUX_NR_GETRUSAGE 77
#define LINUX_NR_SETITIMER 104
#define LINUX_NR_GETITIMER 105
#define LINUX_NR_SYSINFO 116
#define LINUX_NR_UGETRLIMIT 191
#define LINUX_NR_PRLIMIT64 369

#define LINUX_NR_GETPRIORITY 96
#define LINUX_NR_SETPRIORITY 97
#define LINUX_NR_SCHED_SETPARAM 154
#define LINUX_NR_SCHED_GETPARAM 155
#define LINUX_NR_SCHED_SETSCHEDULER 156
#define LINUX_NR_SCHED_GETSCHEDULER 157
#define LINUX_NR_PRCTL 172
#define LINUX_NR_CAPGET 184
#define LINUX_NR_CAPSET 185
#define LINUX_NR_SCHED_SETAFFINITY 241
#define LINUX_NR_SCHED_GETAFFINITY 242
#define LINUX_NR_MEMBARRIER 389

#define LINUX_EPERM 1
#define LINUX_ESRCH 3
#define LINUX_EACCES 13
#define LINUX_EINVAL 22
#define LINUX_ENOSYS 38

#define LINUX_PRIO_PROCESS 0
#define LINUX_PRIO_PGRP 1
#define LINUX_PRIO_USER 2
#define LINUX_NICE_MIN (-20)
#define LINUX_NICE_MAX 19
#define LINUX_NICE_BIAS 20

#define LINUX_SCHED_OTHER 0
#define LINUX_SCHED_FIFO 1
#define LINUX_SCHED_RR 2
#define LINUX_SCHED_BATCH 3
#define LINUX_SCHED_UNUSED 4
#define LINUX_SCHED_IDLE 5
#define LINUX_SCHED_DEADLINE 6
#define LINUX_SCHED_RESET_ON_FORK 0x40000000u
#define LINUX_SCHED_RT_MIN 1
#define LINUX_SCHED_RT_MAX 99

#define LINUX_CAP_VERSION_1 0x19980330u
#define LINUX_CAP_VERSION_2 0x20071026u
#define LINUX_CAP_VERSION_3 0x20080522u

#define LINUX_PR_SET_PDEATHSIG 1
#define LINUX_PR_GET_PDEATHSIG 2
#define LINUX_PR_GET_DUMPABLE 3
#define LINUX_PR_SET_DUMPABLE 4
#define LINUX_PR_SET_NAME 15
#define LINUX_PR_GET_NAME 16
#define LINUX_PR_SET_NO_NEW_PRIVS 38
#define LINUX_PR_GET_NO_NEW_PRIVS 39
#define LINUX_PR_SET_VMA 0x53564d41u
#define LINUX_NSIG 64

#define LINUX_MEMBARRIER_CMD_QUERY 0

#define AFFINITY_WORDS_MAX 4
#define AFFINITY_BITS_PER_WORD 32
#define QNX_TCTL_RUNMASK_GET_AND_SET_INHERIT 10

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

/* ---- priority, scheduler, affinity ---- */

/* Whether pid names a task: 0 (the caller), a guest thread, or a process. */
static int task_exists(int32_t pid)
{
    if (pid == 0 || guest_thread_kill(pid, 0) == 0) return 1;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

static int task_is_caller(int32_t pid)
{
    return pid == 0 || pid == (int32_t)getpid() ||
        pid == guest_thread_tid();
}

/* Resolve a getpriority/setpriority target.  0 means the caller's own nice
   value applies; anything else is the error to return. */
static int32_t priority_target(uint32_t number, uint32_t which, int32_t who,
    const uint32_t *args)
{
    switch (which) {
    case LINUX_PRIO_PROCESS:
        if (task_is_caller(who)) return 0;
        if (!task_exists(who)) return -LINUX_ESRCH;
        break;
    case LINUX_PRIO_PGRP:
        if (who == 0 || who == (int32_t)getpgrp()) return 0;
        break;
    case LINUX_PRIO_USER:
        if (who == 0 || who == (int32_t)geteuid()) return 0;
        break;
    default:
        return -LINUX_EINVAL;
    }
    return linux_report_unsupported(number, LINUX_EPERM,
        "priority-other", args);
}

/* QNX's getpriority/setpriority are stubs (always 0, no state), so the nice
   value is kept here and carried across exec.  It is recorded only: nothing
   maps it onto a QNX scheduling priority. */
static int32_t linux_getpriority(uint32_t number, uint32_t which,
    int32_t who, const uint32_t *args)
{
    int32_t target = priority_target(number, which, who, args);
    if (target != 0) return target;
    return LINUX_NICE_BIAS - guest_process_nice();
}

static int32_t linux_setpriority(uint32_t number, uint32_t which,
    int32_t who, int32_t value, const uint32_t *args)
{
    int32_t target = priority_target(number, which, who, args);
    if (target != 0) return target;
    if (value < LINUX_NICE_MIN) value = LINUX_NICE_MIN;
    if (value > LINUX_NICE_MAX) value = LINUX_NICE_MAX;
    /* Linux: without privilege a nice value cannot be lowered. */
    if (value < guest_process_nice() && geteuid() != 0)
        return -LINUX_EACCES;
    guest_process_set_nice(value);
    if (value != 0)
        linux_report_unsupported(number, 0, "priority-no-effect", args);
    return 0;
}

/* Every guest task runs SCHED_OTHER at priority 0 on QNX's default
   scheduling; Linux's other policies cannot be honoured. */
static int32_t linux_sched_getscheduler(int32_t pid)
{
    if (pid < 0) return -LINUX_EINVAL;
    if (!task_exists(pid)) return -LINUX_ESRCH;
    return LINUX_SCHED_OTHER;
}

static int32_t linux_sched_getparam(int32_t pid, int32_t *guest)
{
    if (guest == 0 || pid < 0) return -LINUX_EINVAL;
    if (!task_exists(pid)) return -LINUX_ESRCH;
    guest[0] = 0;
    return 0;
}

static int32_t linux_sched_setparam(int32_t pid, const int32_t *guest)
{
    if (guest == 0 || pid < 0) return -LINUX_EINVAL;
    if (!task_exists(pid)) return -LINUX_ESRCH;
    /* The policy is always SCHED_OTHER, whose only priority is 0. */
    return guest[0] == 0 ? 0 : -LINUX_EINVAL;
}

static int32_t linux_sched_setscheduler(uint32_t number, int32_t pid,
    uint32_t policy, const int32_t *guest, const uint32_t *args)
{
    uint32_t base = policy & ~LINUX_SCHED_RESET_ON_FORK;
    if (guest == 0 || pid < 0) return -LINUX_EINVAL;
    if (base > LINUX_SCHED_DEADLINE || base == LINUX_SCHED_UNUSED)
        return -LINUX_EINVAL;
    if (!task_exists(pid)) return -LINUX_ESRCH;
    if (base == LINUX_SCHED_FIFO || base == LINUX_SCHED_RR) {
        if (guest[0] < LINUX_SCHED_RT_MIN || guest[0] > LINUX_SCHED_RT_MAX)
            return -LINUX_EINVAL;
        return linux_report_unsupported(number, LINUX_EPERM,
            "sched-realtime", args);
    }
    if (guest[0] != 0) return -LINUX_EINVAL;
    if (base != LINUX_SCHED_OTHER)
        return linux_report_unsupported(number, LINUX_EPERM,
            "sched-policy", args);
    if ((policy & LINUX_SCHED_RESET_ON_FORK) != 0)
        return linux_report_unsupported(number, LINUX_EINVAL,
            "sched-reset-on-fork", args);
    return 0;
}

static unsigned affinity_words(void)
{
    unsigned cpus = _syspage_ptr->num_cpu;
    unsigned words = (cpus + AFFINITY_BITS_PER_WORD - 1) /
        AFFINITY_BITS_PER_WORD;
    if (words == 0) words = 1;
    return words > AFFINITY_WORDS_MAX ? AFFINITY_WORDS_MAX : words;
}

/* ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT) sets the calling
   thread's run mask and the mask its children inherit, and returns the old
   ones in place.  (The plain _NTO_TCTL_RUNMASK_GET_AND_SET does not write
   the old values back on this system.)  An all-zero run mask means no
   restriction. */
static int affinity_swap(unsigned words, const uint32_t *run,
    const uint32_t *inherit, uint32_t *old_run, uint32_t *old_inherit)
{
    uint32_t buffer[1 + 2 * AFFINITY_WORDS_MAX];
    unsigned i;
    buffer[0] = words;
    for (i = 0; i != words; ++i) {
        buffer[1 + i] = run[i];
        buffer[1 + words + i] = inherit[i];
    }
    if (ThreadCtl(QNX_TCTL_RUNMASK_GET_AND_SET_INHERIT, buffer) != EOK)
        return -1;
    for (i = 0; i != words; ++i) {
        old_run[i] = buffer[1 + i];
        old_inherit[i] = buffer[1 + words + i];
    }
    return 0;
}

/* Bits for the CPUs that exist, in the words QNX uses. */
static void affinity_valid(unsigned words, uint32_t *valid)
{
    unsigned cpus = _syspage_ptr->num_cpu;
    unsigned i;
    for (i = 0; i != words; ++i) {
        unsigned low = i * AFFINITY_BITS_PER_WORD;
        if (cpus >= low + AFFINITY_BITS_PER_WORD) valid[i] = 0xffffffffu;
        else if (cpus > low) valid[i] = (1u << (cpus - low)) - 1u;
        else valid[i] = 0;
    }
}

static int32_t linux_sched_getaffinity(int32_t pid, uint32_t size,
    uint32_t *guest, const uint32_t *args)
{
    unsigned words = affinity_words(), i, any = 0;
    uint32_t run[AFFINITY_WORDS_MAX], inherit[AFFINITY_WORDS_MAX];
    uint32_t zero[AFFINITY_WORDS_MAX] = { 0 }, valid[AFFINITY_WORDS_MAX];
    if (pid < 0) return -LINUX_EINVAL;
    if (size < words * 4 || (size & 3) != 0) return -LINUX_EINVAL;
    if (guest == 0) return -EFAULT;
    if (!task_exists(pid)) return -LINUX_ESRCH;
    if (!task_is_caller(pid))
        return linux_report_unsupported(LINUX_NR_SCHED_GETAFFINITY,
            LINUX_EPERM, "affinity-other", args);
    if (affinity_swap(words, zero, zero, run, inherit) != 0 ||
            affinity_swap(words, run, inherit, zero, zero) != 0)
        return -LINUX_EINVAL;
    affinity_valid(words, valid);
    for (i = 0; i != words; ++i) any |= run[i] & valid[i];
    for (i = 0; i != words; ++i)
        guest[i] = any == 0 ? valid[i] : run[i] & valid[i];
    return (int32_t)(words * 4);
}

static int32_t linux_sched_setaffinity(int32_t pid, uint32_t size,
    const uint32_t *guest, const uint32_t *args)
{
    unsigned words = affinity_words(), i, any = 0;
    uint32_t mask[AFFINITY_WORDS_MAX] = { 0 }, valid[AFFINITY_WORDS_MAX];
    uint32_t old_run[AFFINITY_WORDS_MAX], old_inherit[AFFINITY_WORDS_MAX];
    if (pid < 0) return -LINUX_EINVAL;
    if (guest == 0) return -EFAULT;
    if (!task_exists(pid)) return -LINUX_ESRCH;
    if (!task_is_caller(pid))
        return linux_report_unsupported(LINUX_NR_SCHED_SETAFFINITY,
            LINUX_EPERM, "affinity-other", args);
    /* Linux reads as much of the mask as it has CPUs for, and zero-fills a
       short one. */
    if (size > words * 4) size = words * 4;
    memcpy(mask, guest, size & ~3u);
    affinity_valid(words, valid);
    for (i = 0; i != words; ++i) {
        mask[i] &= valid[i];
        any |= mask[i];
    }
    if (any == 0) return -LINUX_EINVAL;
    if (affinity_swap(words, mask, mask, old_run, old_inherit) != 0)
        return -LINUX_EINVAL;
    return 0;
}

/* ---- capabilities ---- */

struct linux_cap_header {
    uint32_t version;
    int32_t pid;
};

/* Number of 32-bit capability words per set, or 0 for an unknown version. */
static unsigned cap_elements(uint32_t version)
{
    switch (version) {
    case LINUX_CAP_VERSION_1: return 1;
    case LINUX_CAP_VERSION_2:
    case LINUX_CAP_VERSION_3: return 2;
    default: return 0;
    }
}

/* QNX processes have no Linux capability sets: every set reads as empty and
   only an empty set can be installed.  An unknown version gets the preferred
   one written back; capget with NULL data then succeeds (the probe libcap
   uses), anything else is EINVAL. */
static int32_t cap_header_check(struct linux_cap_header *header,
    unsigned *elements, int probe_ok)
{
    if (header == 0) return -EFAULT;
    *elements = cap_elements(header->version);
    if (*elements == 0) {
        header->version = LINUX_CAP_VERSION_3;
        return probe_ok ? 0 : -LINUX_EINVAL;
    }
    if (header->pid < 0) return -LINUX_EINVAL;
    return 0;
}

static int32_t linux_capget(struct linux_cap_header *header, uint32_t *data)
{
    unsigned elements;
    int32_t result = cap_header_check(header, &elements, data == 0);
    if (result != 0 || elements == 0) return result;
    if (data == 0) return 0;
    if (!task_exists(header->pid)) return -LINUX_ESRCH;
    /* Each element is {effective, permitted, inheritable}. */
    memset(data, 0, elements * 3 * sizeof(uint32_t));
    return 0;
}

static int32_t linux_capset(struct linux_cap_header *header,
    const uint32_t *data, const uint32_t *args)
{
    unsigned elements, i;
    int32_t result = cap_header_check(header, &elements, 0);
    if (result != 0) return result;
    if (data == 0) return -EFAULT;
    if (header->pid != 0 && header->pid != (int32_t)getpid())
        return -LINUX_EPERM;
    for (i = 0; i != elements * 3; ++i)
        if (data[i] != 0)
            return linux_report_unsupported(LINUX_NR_CAPSET, LINUX_EPERM,
                "capset-nonzero", args);
    return 0;
}

/* ---- prctl ---- */

static int linux_dumpable = 1;
static int linux_pdeathsig;

/* Linux clears the parent-death signal in a fork child. */
void linux_resource_after_fork(void)
{
    linux_pdeathsig = 0;
}

/* prctl options bionic and glibc/musl use.  PR_SET_VMA (anonymous mapping
   names) is not available here: EINVAL, as on a Linux kernel without
   CONFIG_ANON_VMA_NAME.  Unknown options are EINVAL, as on Linux.
   PR_SET_PDEATHSIG is stored but QNX has no parent-death notification
   (procmgr events cover daemon death only), so the signal is never sent. */
static int32_t linux_prctl(const uint32_t *args)
{
    char name[16];
    switch (args[0]) {
    case LINUX_PR_SET_NAME:
        if (args[1] == 0) return -EFAULT;
        memset(name, 0, sizeof(name));
        strncpy(name, (const char *)(uintptr_t)args[1], sizeof(name) - 1);
        guest_thread_name_set(name);
        return 0;
    case LINUX_PR_GET_NAME:
        if (args[1] == 0) return -EFAULT;
        guest_thread_name_get(name);
        memcpy((void *)(uintptr_t)args[1], name, sizeof(name));
        return 0;
    case LINUX_PR_SET_PDEATHSIG:
        if (args[1] > LINUX_NSIG) return -LINUX_EINVAL;
        linux_pdeathsig = (int)args[1];
        if (args[1] != 0)
            linux_report_unsupported(LINUX_NR_PRCTL, 0,
                "pdeathsig-delivery", args);
        return 0;
    case LINUX_PR_GET_PDEATHSIG:
        if (args[1] == 0) return -EFAULT;
        *(int32_t *)(uintptr_t)args[1] = linux_pdeathsig;
        return 0;
    case LINUX_PR_GET_DUMPABLE:
        return linux_dumpable;
    case LINUX_PR_SET_DUMPABLE:
        if (args[1] > 1) return -LINUX_EINVAL;
        linux_dumpable = (int)args[1];
        return 0;
    case LINUX_PR_SET_NO_NEW_PRIVS:
        /* Only ever set, never cleared; the other arguments must be 0. */
        if (args[1] != 1 || args[2] != 0 || args[3] != 0 || args[4] != 0)
            return -LINUX_EINVAL;
        guest_process_set_no_new_privs();
        return 0;
    case LINUX_PR_GET_NO_NEW_PRIVS:
        if (args[1] != 0 || args[2] != 0 || args[3] != 0 || args[4] != 0)
            return -LINUX_EINVAL;
        return guest_process_no_new_privs();
    case LINUX_PR_SET_VMA:
        return linux_report_unsupported(LINUX_NR_PRCTL, LINUX_EINVAL,
            "prctl-set-vma", args);
    default:
        return linux_report_unsupported(LINUX_NR_PRCTL, LINUX_EINVAL,
            "prctl-option", args);
    }
}

/* No memory barrier is faked: QUERY reports that no command is supported. */
static int32_t linux_membarrier(uint32_t number, uint32_t command,
    const uint32_t *args)
{
    if (command == LINUX_MEMBARRIER_CMD_QUERY) return 0;
    return linux_report_unsupported(number, LINUX_EINVAL,
        "membarrier-cmd", args);
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
    case LINUX_NR_GETPRIORITY:
        return linux_getpriority(number, args[0], (int32_t)args[1], args);
    case LINUX_NR_SETPRIORITY:
        return linux_setpriority(number, args[0], (int32_t)args[1],
            (int32_t)args[2], args);
    case LINUX_NR_SCHED_SETPARAM:
        return linux_sched_setparam((int32_t)args[0],
            (const int32_t *)args[1]);
    case LINUX_NR_SCHED_GETPARAM:
        return linux_sched_getparam((int32_t)args[0], (int32_t *)args[1]);
    case LINUX_NR_SCHED_SETSCHEDULER:
        return linux_sched_setscheduler(number, (int32_t)args[0], args[1],
            (const int32_t *)args[2], args);
    case LINUX_NR_SCHED_GETSCHEDULER:
        return linux_sched_getscheduler((int32_t)args[0]);
    case LINUX_NR_SCHED_SETAFFINITY:
        return linux_sched_setaffinity((int32_t)args[0], args[1],
            (const uint32_t *)args[2], args);
    case LINUX_NR_SCHED_GETAFFINITY:
        return linux_sched_getaffinity((int32_t)args[0], args[1],
            (uint32_t *)args[2], args);
    case LINUX_NR_CAPGET:
        return linux_capget((struct linux_cap_header *)args[0],
            (uint32_t *)args[1]);
    case LINUX_NR_CAPSET:
        return linux_capset((struct linux_cap_header *)args[0],
            (const uint32_t *)args[1], args);
    case LINUX_NR_PRCTL:
        return linux_prctl(args);
    case LINUX_NR_MEMBARRIER:
        return linux_membarrier(number, args[0], args);
    default:
        return linux_report_unsupported(number, LINUX_ENOSYS,
            "resource-syscall", args);
    }
}
