#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LINUX_CLONE_VM 0x00000100u
#define LINUX_CLONE_FS 0x00000200u
#define LINUX_CLONE_FILES 0x00000400u
#define LINUX_CLONE_SIGHAND 0x00000800u
#define LINUX_CLONE_THREAD 0x00010000u
#define LINUX_CLONE_SYSVSEM 0x00040000u
#define LINUX_CLONE_SETTLS 0x00080000u
#define LINUX_CLONE_PARENT_SETTID 0x00100000u
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000u
#define LINUX_CLONE_DETACHED 0x00400000u
#define LINUX_CLONE_CHILD_SETTID 0x01000000u
#define LINUX_THREAD_REQUIRED (LINUX_CLONE_VM | LINUX_CLONE_FS | \
    LINUX_CLONE_FILES | LINUX_CLONE_SIGHAND | LINUX_CLONE_THREAD)
#define LINUX_THREAD_ALLOWED (LINUX_THREAD_REQUIRED | LINUX_CLONE_SYSVSEM | \
    LINUX_CLONE_SETTLS | LINUX_CLONE_PARENT_SETTID | \
    LINUX_CLONE_CHILD_CLEARTID | LINUX_CLONE_DETACHED | \
    LINUX_CLONE_CHILD_SETTID)
#define LINUX_FIRST_REALTIME_SIGNAL 32
#define LINUX_LAST_REALTIME_SIGNAL 47
#define REALTIME_SIGNALS (LINUX_LAST_REALTIME_SIGNAL - \
    LINUX_FIRST_REALTIME_SIGNAL + 1)
#define REALTIME_QUEUE_LENGTH 64

struct guest_thread_state {
    uint32_t registers[16];
    uint32_t guest_tls;
    uint32_t *clear_child_tid;
    uint32_t *child_tid;
    int32_t tid;
    volatile int start;
    sigjmp_buf exit_context;
    pthread_t host_thread;
    volatile uint32_t pending_signals[2];
    siginfo_t pending_information[31];
    volatile sig_atomic_t realtime_head[REALTIME_SIGNALS];
    volatile sig_atomic_t realtime_tail[REALTIME_SIGNALS];
    siginfo_t realtime_information[REALTIME_SIGNALS][REALTIME_QUEUE_LENGTH];
    uint32_t signal_mask[2];
    uint32_t suspend_restore_mask[2];
    int suspend_restore_valid;
    sem_t *futex_semaphore;
    volatile sig_atomic_t futex_interrupted;
    /* The thread's signal-wait channel (guest_thread_signal_wait_begin):
       a nonblocking pipe made on first use and kept until the thread ends.
       Posters are this thread's own signal handlers; they write only
       while signal_wait_active.  Never a socket: a handler must not send
       io-pkt a message (an io-pkt crash, docs/FINDINGS.md 2026-11-19). */
    volatile sig_atomic_t signal_wait_write_fd;
    int signal_wait_read_fd;
    volatile sig_atomic_t signal_wait_active;
    volatile sig_atomic_t syscall_active;
    uint32_t altstack_pointer;
    uint32_t altstack_size;
    uintptr_t deferred_unmap_start;
    size_t deferred_unmap_length;
    char name[16]; /* Linux comm: NUL-padded, at most 15 characters */
    struct guest_thread_state *next;
};

static pthread_key_t thread_state_key;
static struct guest_thread_state main_thread;
static volatile uint32_t next_tid;
static pthread_mutex_t thread_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_thread_state *thread_registry;

/*
 * Linux reserves the top two bits of a TID stored in a robust futex word.
 * QNX process IDs can use those bits, so they cannot be exposed directly as
 * guest TIDs.  Keep synthetic guest thread IDs in the Linux 30-bit range.
 */
static int32_t initial_guest_tid(void)
{
    uint32_t tid = (uint32_t)getpid() & 0x3fffffffu;
    return (int32_t)(tid == 0 ? 1 : tid);
}

static int32_t allocate_guest_tid(void)
{
    uint32_t tid;
    do {
        tid = __sync_fetch_and_add(&next_tid, 1u) & 0x3fffffffu;
    } while (tid == 0);
    return (int32_t)tid;
}

__asm__(
    ".text\n"
    ".align 2\n"
    ".arm\n"
    ".global linuxemu_resume_guest\n"
    ".type linuxemu_resume_guest, %function\n"
    "linuxemu_resume_guest:\n"
    "mov lr, r0\n"
    "ldmia lr, {r0-r12}\n"
    "ldr sp, [lr, #52]\n"
    "ldr ip, [lr, #60]\n"
    "ldr lr, [lr, #56]\n"
    "bx ip\n"
    ".size linuxemu_resume_guest, .-linuxemu_resume_guest\n");

extern void linuxemu_resume_guest(uint32_t registers[16]);

static void process_queue_reset(void);

static struct guest_thread_state *current_thread(void)
{
    return pthread_getspecific(thread_state_key);
}

/* Closes the thread's signal-wait channel.  The numbers are withdrawn
   before the close: a handler on this thread that runs from then on sees
   -1 and never writes to a number the close has freed for reuse. */
static void signal_wait_close(struct guest_thread_state *state)
{
    int read_fd = state->signal_wait_read_fd;
    int write_fd = state->signal_wait_write_fd;
    state->signal_wait_active = 0;
    state->signal_wait_write_fd = -1;
    __sync_synchronize();
    state->signal_wait_read_fd = -1;
    if (read_fd >= 0) close(read_fd);
    if (write_fd >= 0 && write_fd != read_fd) close(write_fd);
}

static void finish_guest_thread(struct guest_thread_state *state)
{
    struct guest_thread_state **cursor;
    uint32_t *clear_child_tid = state->clear_child_tid;

    /*
     * A detached musl thread ends through __unmapself: it unmaps its own
     * guest stack and immediately invokes exit without using that stack
     * again.  QNX delivers Linuxemu's syscall trap on the guest stack, so
     * the physical unmap has to wait until siglongjmp has returned us to the
     * native pthread stack.  Complete it before publishing clear_child_tid,
     * preserving Linux's observable thread-retirement order.
     */
    if (state->deferred_unmap_length != 0) {
        munmap((void *)state->deferred_unmap_start,
            state->deferred_unmap_length);
        arm_patch_forget_range(state->deferred_unmap_start,
            state->deferred_unmap_length);
        guest_memory_runtime_unmap(state->deferred_unmap_start,
            state->deferred_unmap_length);
    }
    pthread_mutex_lock(&thread_registry_lock);
    for (cursor = &thread_registry; *cursor != 0; cursor = &(*cursor)->next) {
        if (*cursor == state) {
            *cursor = state->next;
            break;
        }
    }
    pthread_mutex_unlock(&thread_registry_lock);
    if (clear_child_tid != 0) {
        __sync_lock_test_and_set(clear_child_tid, 0);
        linux_futex_wake(clear_child_tid, INT_MAX, 0xffffffffu);
    }
    signal_wait_close(state);
    pthread_setspecific(thread_state_key, 0);
    free(state);
}

static void *guest_thread_start(void *argument)
{
    struct guest_thread_state *state = argument;
    sigset_t traps;

    pthread_setspecific(thread_state_key, state);
    sigemptyset(&traps);
    sigaddset(&traps, SIGILL);
    sigaddset(&traps, SIGSEGV);
    pthread_sigmask(SIG_UNBLOCK, &traps, 0);
    if (sigsetjmp(state->exit_context, 1) != 0) {
        finish_guest_thread(state);
        return 0;
    }
    while (!state->start) sched_yield();
    __sync_synchronize();
    if (state->child_tid != 0)
        __sync_lock_test_and_set(state->child_tid, (uint32_t)state->tid);
    linuxemu_resume_guest(state->registers);
    _exit(126);
    return 0;
}

/* The guest's mask starts as the host mask Linuxemu was started with.
   Since the trap writes the guest's mask into the host (rt_sigprocmask),
   an execve (run inside the trap handler) passes the old image's guest
   mask on as the host mask, as Linux keeps the blocked set across exec;
   the new image's guest view must start from it. */
static void seed_signal_mask(struct guest_thread_state *state)
{
    sigset_t host;
    int host_signal;
    if (pthread_sigmask(SIG_SETMASK, 0, &host) != 0) return;
    for (host_signal = 1; host_signal <= SIGRTMAX; ++host_signal) {
        int linux_signal;
        if (host_signal == SIGILL || host_signal == SIGSEGV ||
                sigismember(&host, host_signal) != 1) continue;
        linux_signal = guest_signal_from_host(host_signal);
        if (linux_signal > 0 && linux_signal <= 64 &&
                linux_signal != 9 && linux_signal != 19)
            state->signal_mask[(linux_signal - 1) / 32] |=
                1u << ((linux_signal - 1) % 32);
    }
}

int guest_thread_initialize(void)
{
    int error = pthread_key_create(&thread_state_key, 0);
    if (error != 0) { errno = error; return -1; }
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.signal_wait_read_fd = -1;
    main_thread.signal_wait_write_fd = -1;
    main_thread.tid = initial_guest_tid();
    main_thread.host_thread = pthread_self();
    seed_signal_mask(&main_thread);
    thread_registry = &main_thread;
    next_tid = (uint32_t)main_thread.tid + 1u;
    error = pthread_setspecific(thread_state_key, &main_thread);
    if (error != 0) { errno = error; return -1; }
    return 0;
}

int32_t guest_thread_clone(ucontext_t *context)
{
    uint32_t flags = context->uc_mcontext.cpu.gpr[0];
    uint32_t child_stack = context->uc_mcontext.cpu.gpr[1];
    uint32_t *parent_tid = (uint32_t *)context->uc_mcontext.cpu.gpr[2];
    uint32_t tls = context->uc_mcontext.cpu.gpr[3];
    uint32_t *child_tid = (uint32_t *)context->uc_mcontext.cpu.gpr[4];
    struct guest_thread_state *parent = current_thread();
    struct guest_thread_state *state;
    pthread_attr_t attributes;
    pthread_t thread;
    int32_t tid;
    int attributes_initialized = 0;
    int error;

    if ((flags & LINUX_THREAD_REQUIRED) != LINUX_THREAD_REQUIRED ||
        (flags & ~LINUX_THREAD_ALLOWED) != 0 || (flags & 0xffu) != 0 ||
        child_stack == 0) return -EINVAL;
    state = calloc(1, sizeof(*state));
    if (state == 0) return -ENOMEM;
    state->signal_wait_read_fd = -1;
    state->signal_wait_write_fd = -1;
    memcpy(state->registers, context->uc_mcontext.cpu.gpr,
        sizeof(state->registers));
    state->registers[0] = 0;
    state->registers[13] = child_stack;
    state->registers[15] += 4;
    state->guest_tls = (flags & LINUX_CLONE_SETTLS) != 0 ? tls :
        parent->guest_tls;
    memcpy(state->signal_mask, parent->signal_mask,
        sizeof(state->signal_mask));
    memcpy(state->name, parent->name, sizeof(state->name));
    state->tid = allocate_guest_tid();
    tid = state->tid;
    if ((flags & LINUX_CLONE_CHILD_CLEARTID) != 0)
        state->clear_child_tid = child_tid;
    if ((flags & LINUX_CLONE_CHILD_SETTID) != 0)
        state->child_tid = child_tid;
    error = pthread_attr_init(&attributes);
    if (error == 0) {
        attributes_initialized = 1;
        error = pthread_attr_setdetachstate(&attributes,
            PTHREAD_CREATE_DETACHED);
    }
    if (error == 0)
        error = pthread_create(&thread, &attributes, guest_thread_start, state);
    if (attributes_initialized) pthread_attr_destroy(&attributes);
    if (error != 0) {
        free(state);
        return -(int32_t)linux_errno_number(error);
    }
    state->host_thread = thread;
    pthread_mutex_lock(&thread_registry_lock);
    state->next = thread_registry;
    thread_registry = state;
    pthread_mutex_unlock(&thread_registry_lock);
    if ((flags & LINUX_CLONE_PARENT_SETTID) != 0)
        __sync_lock_test_and_set(parent_tid, (uint32_t)state->tid);
    __sync_synchronize();
    state->start = 1;
    return tid;
}

int32_t guest_thread_tid(void)
{
    struct guest_thread_state *state = current_thread();
    return state == 0 ? initial_guest_tid() : state->tid;
}

int32_t guest_thread_set_tid_address(uint32_t *address)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) state->clear_child_tid = address;
    return guest_thread_tid();
}

int guest_thread_defer_active_stack_unmap(uintptr_t address, size_t length)
{
    volatile unsigned char stack_marker;
    uintptr_t active_stack = (uintptr_t)&stack_marker;
    struct guest_thread_state *state = current_thread();
    uintptr_t start;
    uintptr_t end;

    if (state == 0 || active_stack < address ||
        active_stack - address >= length) return 0;
    start = address;
    end = address + length;
    if (state->deferred_unmap_length != 0) {
        uintptr_t old_start = state->deferred_unmap_start;
        uintptr_t old_end = old_start + state->deferred_unmap_length;
        if (old_start < start) start = old_start;
        if (old_end > end) end = old_end;
    }
    state->deferred_unmap_start = start;
    state->deferred_unmap_length = end - start;
    return 1;
}

void guest_thread_after_fork(void)
{
    pthread_mutex_t fresh_lock = PTHREAD_MUTEX_INITIALIZER;
    struct guest_thread_state *state = current_thread();
    struct guest_thread_state *cursor;
    uint32_t guest_tls = state == 0 ? 0 : state->guest_tls;
    uint32_t signal_mask[2] = { 0, 0 };
    uint32_t altstack_pointer = state == 0 ? 0 : state->altstack_pointer;
    uint32_t altstack_size = state == 0 ? 0 : state->altstack_size;
    char name[16] = { 0 };
    if (state != 0) {
        memcpy(signal_mask, state->signal_mask, sizeof(signal_mask));
        memcpy(name, state->name, sizeof(name));
    }
    /* The channels are the parent's threads' (the child's copies of the
       same pipes): the child makes its own on first use. */
    for (cursor = thread_registry; cursor != 0; cursor = cursor->next)
        signal_wait_close(cursor);
    if (state != 0 && state != &main_thread) free(state);
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.signal_wait_read_fd = -1;
    main_thread.signal_wait_write_fd = -1;
    main_thread.guest_tls = guest_tls;
    memcpy(main_thread.signal_mask, signal_mask,
        sizeof(main_thread.signal_mask));
    main_thread.altstack_pointer = altstack_pointer;
    main_thread.altstack_size = altstack_size;
    memcpy(main_thread.name, name, sizeof(main_thread.name));
    main_thread.tid = initial_guest_tid();
    main_thread.host_thread = pthread_self();
    next_tid = (uint32_t)main_thread.tid + 1u;
    thread_registry_lock = fresh_lock;
    thread_registry = &main_thread;
    pthread_setspecific(thread_state_key, &main_thread);
    linux_futex_after_fork();
    guest_memory_after_fork();
    guest_fd_after_fork();
    linux_epoll_after_fork();
    linux_timerfd_after_fork();
    /* Linux doesn't pass pending signals to a child; the lock may have
       been held by a parent thread that doesn't exist here. */
    process_queue_reset();
    linux_signalfd_after_fork();
    linux_resource_after_fork();
}

/* Wakes the thread's interruptible waits (futex semaphore, signal-wait
   channel).  Async-signal-safe. */
static void signal_wake(struct guest_thread_state *state)
{
    unsigned char byte = 1;
    if (state->futex_semaphore != 0) {
        state->futex_interrupted = 1;
        sem_post(state->futex_semaphore);
    }
    if (state->signal_wait_active && state->signal_wait_write_fd >= 0)
        write(state->signal_wait_write_fd, &byte, sizeof(byte));
}

void guest_thread_signal_wake(void)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) signal_wake(state);
}

void guest_thread_signal_pending(int linux_signal, const siginfo_t *information)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0 && linux_signal > 0 && linux_signal <= 64) {
        if (linux_signal >= LINUX_FIRST_REALTIME_SIGNAL &&
            linux_signal <= LINUX_LAST_REALTIME_SIGNAL) {
            int index = linux_signal - LINUX_FIRST_REALTIME_SIGNAL;
            sig_atomic_t tail = state->realtime_tail[index];
            if (tail - state->realtime_head[index] < REALTIME_QUEUE_LENGTH) {
                if (information != 0)
                    state->realtime_information[index]
                        [tail % REALTIME_QUEUE_LENGTH] = *information;
                else memset(&state->realtime_information[index]
                    [tail % REALTIME_QUEUE_LENGTH], 0, sizeof(siginfo_t));
                __sync_synchronize();
                state->realtime_tail[index] = tail + 1;
            }
        } else {
            uint32_t bit = (uint32_t)1u << ((linux_signal - 1) % 32);
            uint32_t old = state->pending_signals[(linux_signal - 1) / 32];
            if ((old & bit) == 0 && linux_signal <= 31) {
                if (information != 0)
                    state->pending_information[linux_signal - 1] =
                        *information;
                else memset(&state->pending_information[linux_signal - 1],
                    0, sizeof(siginfo_t));
            }
            __sync_or_and_fetch(
                &state->pending_signals[(linux_signal - 1) / 32], bit);
        }
        signal_wake(state);
    }
}

static int mask_has(const uint32_t mask[2], int signal_number)
{
    return (mask[(signal_number - 1) / 32] &
        (1u << ((signal_number - 1) % 32))) != 0;
}

static void mask_add(uint32_t mask[2], int signal_number)
{
    mask[(signal_number - 1) / 32] |= 1u << ((signal_number - 1) % 32);
}

/* Takes the lowest standard signal in allowed, else the first real-time
   one in allowed, from the thread's queue (Linux dequeue order). */
static int take_thread(struct guest_thread_state *state,
    const uint32_t allowed[2], siginfo_t *information, int *has_information)
{
    uint32_t old_value;
    uint32_t candidates;
    uint32_t new_value;
    siginfo_t selected_information;
    int signal_number;
    if (has_information != 0) *has_information = 0;
    if (information != 0) memset(information, 0, sizeof(*information));
    if (state == 0) return 0;
    do {
        old_value = state->pending_signals[0];
        /* Bit 31 would be signal 32, which lives in its ring. */
        candidates = old_value & allowed[0] & 0x7fffffffu;
        if (candidates == 0) break;
        signal_number = __builtin_ctz(candidates) + 1;
        selected_information = state->pending_information[signal_number - 1];
        new_value = old_value & ~(1u << (signal_number - 1));
    } while (!__sync_bool_compare_and_swap(&state->pending_signals[0],
        old_value, new_value));
    if (candidates != 0) {
        if (information != 0) *information = selected_information;
        if (has_information != 0) *has_information = 1;
        return signal_number;
    }
    for (signal_number = LINUX_FIRST_REALTIME_SIGNAL;
        signal_number <= LINUX_LAST_REALTIME_SIGNAL; ++signal_number) {
        int index = signal_number - LINUX_FIRST_REALTIME_SIGNAL;
        sig_atomic_t head;
        if (!mask_has(allowed, signal_number)) continue;
        head = state->realtime_head[index];
        if (head == state->realtime_tail[index]) continue;
        if (information != 0)
            *information = state->realtime_information[index]
                [head % REALTIME_QUEUE_LENGTH];
        __sync_synchronize();
        state->realtime_head[index] = head + 1;
        if (has_information != 0) *has_information = 1;
        return signal_number;
    }
    return 0;
}

int guest_thread_take_pending(siginfo_t *information, int *has_information)
{
    struct guest_thread_state *state = current_thread();
    uint32_t allowed[2];
    if (state == 0) return take_thread(0, 0, information, has_information);
    allowed[0] = ~state->signal_mask[0];
    allowed[1] = ~state->signal_mask[1];
    return take_thread(state, allowed, information, has_information);
}

int guest_thread_take_pending_mask(const uint32_t mask[2],
    siginfo_t *information)
{
    return take_thread(current_thread(), mask, information, 0);
}

void guest_thread_pending_mask(uint32_t words[2])
{
    struct guest_thread_state *state = current_thread();
    int index;
    words[0] = words[1] = 0;
    if (state == 0) return;
    words[0] = state->pending_signals[0] & 0x7fffffffu;
    for (index = 0; index < REALTIME_SIGNALS; ++index)
        if (state->realtime_head[index] != state->realtime_tail[index])
            mask_add(words, LINUX_FIRST_REALTIME_SIGNAL + index);
}

/*
 * The process-wide pending queue, fed by signalfd's collector
 * (linux_signalfd.c).
 *
 * A process-directed signal that every guest thread blocks stays pending
 * in the QNX kernel; the collector takes it with sigwaitinfo and queues it
 * here with its siginfo.  Standard signals coalesce (one pending instance,
 * the first siginfo kept); real-time signals queue in order in a ring per
 * signal as deep as the per-thread ones (REALTIME_QUEUE_LENGTH).  A full
 * ring never overflows: the collector stops waiting for that signal while
 * it is full (guest_process_full_mask), so later instances stay queued in
 * the QNX kernel, in order, and are taken once a reader makes room (the
 * kernel's own limit then applies: sigqueue fails EAGAIN, as on Linux).
 *
 * An entry is "routed" when the collector took a signal that some guest
 * thread had unblocked: normal delivery must get it, so delivery takes
 * routed entries and signalfd reads and readiness don't see them.
 *
 * Consumers are any thread (delivery after a syscall or in the poke
 * handler, signalfd reads); producers are the collector and routing.
 * process_queue.lock is a spin lock and a leaf: nothing is taken under
 * it.  It is taken only by emulator code, and by the poke handler only
 * when the handler interrupted guest code, so no thread ever spins on a
 * lock it holds itself.  The summary words are written under it and read
 * without it.
 */
static struct {
    volatile int lock;
    uint32_t standard;                  /* signals 1..31: bit n-1 */
    uint32_t standard_routed;
    siginfo_t standard_information[31];
    int realtime_head[REALTIME_SIGNALS];
    int realtime_tail[REALTIME_SIGNALS];
    unsigned char realtime_routed[REALTIME_SIGNALS][REALTIME_QUEUE_LENGTH];
    siginfo_t realtime_information[REALTIME_SIGNALS][REALTIME_QUEUE_LENGTH];
    volatile uint32_t any[2];           /* signals with an entry */
    volatile uint32_t visible[2];       /* ... whose next one is unrouted */
    volatile uint32_t full[2];          /* real-time rings that are full */
} process_queue;

static void process_queue_reset(void)
{
    memset(&process_queue, 0, sizeof(process_queue));
}

static void process_lock(void)
{
    while (__sync_lock_test_and_set(&process_queue.lock, 1) != 0)
        sched_yield();
}

static void process_unlock(void)
{
    __sync_lock_release(&process_queue.lock);
}

/* Lock held. */
static void process_summarize(void)
{
    uint32_t any[2], visible[2], full[2];
    int index;
    any[0] = process_queue.standard;
    visible[0] = process_queue.standard & ~process_queue.standard_routed;
    any[1] = visible[1] = full[0] = full[1] = 0;
    for (index = 0; index < REALTIME_SIGNALS; ++index) {
        int head = process_queue.realtime_head[index];
        int count = process_queue.realtime_tail[index] - head;
        int signal_number = LINUX_FIRST_REALTIME_SIGNAL + index;
        if (count == 0) continue;
        mask_add(any, signal_number);
        if (!process_queue.realtime_routed[index]
                [head % REALTIME_QUEUE_LENGTH])
            mask_add(visible, signal_number);
        if (count >= REALTIME_QUEUE_LENGTH) mask_add(full, signal_number);
    }
    __sync_synchronize();
    process_queue.any[0] = any[0];
    process_queue.any[1] = any[1];
    process_queue.visible[0] = visible[0];
    process_queue.visible[1] = visible[1];
    process_queue.full[0] = full[0];
    process_queue.full[1] = full[1];
    __sync_synchronize();
}

/* 0 when the signal is out of range or its ring is full (lost: only a
   routed signal or a race with a ring filling can meet a full ring, since
   the collector doesn't wait for a signal whose ring is full). */
static int process_enqueue(int linux_signal, const siginfo_t *information,
    int routed)
{
    int queued = 1;
    if (linux_signal <= 0 || linux_signal > LINUX_LAST_REALTIME_SIGNAL)
        return 0;
    process_lock();
    if (linux_signal < LINUX_FIRST_REALTIME_SIGNAL) {
        uint32_t bit = 1u << (linux_signal - 1);
        if ((process_queue.standard & bit) == 0) {
            if (information != 0)
                process_queue.standard_information[linux_signal - 1] =
                    *information;
            else memset(&process_queue.standard_information[linux_signal - 1],
                0, sizeof(siginfo_t));
            process_queue.standard |= bit;
            process_queue.standard_routed &= ~bit;
        }
        /* Coalesced onto a pending instance: a routed arrival keeps it
           for normal delivery. */
        if (routed) process_queue.standard_routed |= bit;
    } else {
        int index = linux_signal - LINUX_FIRST_REALTIME_SIGNAL;
        int tail = process_queue.realtime_tail[index];
        if (tail - process_queue.realtime_head[index] >=
                REALTIME_QUEUE_LENGTH) queued = 0;
        else {
            int slot = tail % REALTIME_QUEUE_LENGTH;
            if (information != 0)
                process_queue.realtime_information[index][slot] = *information;
            else memset(&process_queue.realtime_information[index][slot], 0,
                sizeof(siginfo_t));
            process_queue.realtime_routed[index][slot] = (unsigned char)routed;
            process_queue.realtime_tail[index] = tail + 1;
        }
    }
    process_summarize();
    process_unlock();
    return queued;
}

void guest_process_signal_queue(int linux_signal, const siginfo_t *information)
{
    (void)process_enqueue(linux_signal, information, 0);
}

/* Lowest standard signal, else the first real-time one, in mask; routed
   entries only when include_routed.  *freed is set when a full ring got
   room (the collector must re-arm to take that signal again). */
static int process_take(const uint32_t mask[2], int include_routed,
    siginfo_t *information, int *freed)
{
    uint32_t candidates;
    int signal_number = 0, index;
    const volatile uint32_t *summary = include_routed ? process_queue.any :
        process_queue.visible;
    *freed = 0;
    if (information != 0) memset(information, 0, sizeof(*information));
    if ((summary[0] & mask[0]) == 0 && (summary[1] & mask[1]) == 0) return 0;
    process_lock();
    candidates = process_queue.standard & mask[0] & 0x7fffffffu;
    if (!include_routed) candidates &= ~process_queue.standard_routed;
    if (candidates != 0) {
        signal_number = __builtin_ctz(candidates) + 1;
        if (information != 0)
            *information =
                process_queue.standard_information[signal_number - 1];
        process_queue.standard &= ~(1u << (signal_number - 1));
        process_queue.standard_routed &= ~(1u << (signal_number - 1));
    } else {
        for (index = 0; index < REALTIME_SIGNALS; ++index) {
            int head = process_queue.realtime_head[index];
            int count = process_queue.realtime_tail[index] - head;
            int slot = head % REALTIME_QUEUE_LENGTH;
            if (count == 0 || !mask_has(mask,
                    LINUX_FIRST_REALTIME_SIGNAL + index)) continue;
            if (!include_routed && process_queue.realtime_routed[index][slot])
                continue;
            signal_number = LINUX_FIRST_REALTIME_SIGNAL + index;
            if (information != 0)
                *information = process_queue.realtime_information[index][slot];
            process_queue.realtime_head[index] = head + 1;
            if (count >= REALTIME_QUEUE_LENGTH) *freed = 1;
            break;
        }
    }
    if (signal_number != 0) process_summarize();
    process_unlock();
    return signal_number;
}

int guest_process_take_pending(const uint32_t mask[2], siginfo_t *information)
{
    int freed;
    int signal_number = process_take(mask, 1, information, &freed);
    if (freed) linux_signalfd_collector_update();
    return signal_number;
}

int guest_process_take_visible(const uint32_t mask[2], siginfo_t *information,
    int *freed)
{
    return process_take(mask, 0, information, freed);
}

void guest_process_pending_mask(uint32_t words[2], int include_routed)
{
    const volatile uint32_t *summary = include_routed ? process_queue.any :
        process_queue.visible;
    words[0] = summary[0];
    words[1] = summary[1];
}

void guest_process_full_mask(uint32_t words[2])
{
    words[0] = process_queue.full[0];
    words[1] = process_queue.full[1];
}

/* The collector took linux_signal, but a guest thread has it unblocked,
   so normal delivery must get it (the collector never steals such a
   signal).  With resend_host_signal (the guest's action is the default or
   ignore) the signal is sent again to that thread and the kernel applies
   the action; otherwise it is queued routed, with the sender's siginfo,
   and the thread is poked with poke_host_signal, whose handler delivers
   it at once (in guest code) or wakes the thread's wait (in a syscall).
   Lock order: thread_registry_lock, then the process queue's leaf lock.
   Returns 1, or 0 when every guest thread blocks the signal. */
int guest_thread_route_signal(int linux_signal, const siginfo_t *information,
    int resend_host_signal, int poke_host_signal)
{
    struct guest_thread_state *state;
    int routed = 0;
    if (linux_signal <= 0 || linux_signal > 64) return 0;
    pthread_mutex_lock(&thread_registry_lock);
    for (state = thread_registry; state != 0; state = state->next) {
        if (mask_has(state->signal_mask, linux_signal)) continue;
        if (resend_host_signal != 0)
            (void)pthread_kill(state->host_thread, resend_host_signal);
        else {
            (void)process_enqueue(linux_signal, information, 1);
            (void)pthread_kill(state->host_thread, poke_host_signal);
        }
        routed = 1;
        break;
    }
    pthread_mutex_unlock(&thread_registry_lock);
    return routed;
}

/* After the collector queued linux_signal in the process queue: pokes
   every guest thread whose (maybe temporary) mask lets it in, so one that
   unblocked it after the routing check still takes it (its poke handler
   delivers it in guest code, or wakes its wait in a syscall).  Under the
   registry lock, so every poked thread exists. */
void guest_thread_poke_unblocked(int linux_signal, int poke_host_signal)
{
    struct guest_thread_state *state;
    if (linux_signal <= 0 || linux_signal > 64) return;
    pthread_mutex_lock(&thread_registry_lock);
    for (state = thread_registry; state != 0; state = state->next)
        if (!mask_has(state->signal_mask, linux_signal))
            (void)pthread_kill(state->host_thread, poke_host_signal);
    pthread_mutex_unlock(&thread_registry_lock);
}

void guest_thread_signal_mask_get(uint32_t words[2])
{
    struct guest_thread_state *state = current_thread();
    if (state == 0) words[0] = words[1] = 0;
    else memcpy(words, state->signal_mask, sizeof(state->signal_mask));
}

void guest_thread_signal_mask_set(const uint32_t words[2])
{
    struct guest_thread_state *state = current_thread();
    if (state != 0)
        memcpy(state->signal_mask, words, sizeof(state->signal_mask));
}

void guest_thread_signal_suspend_restore(const uint32_t words[2])
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) {
        memcpy(state->suspend_restore_mask, words,
            sizeof(state->suspend_restore_mask));
        state->suspend_restore_valid = 1;
    }
}

/* A masked wait returned EINTR but nothing was delivered: the thread goes
   back to the mask it had before the wait. */
void guest_thread_signal_suspend_cancel(void)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0 && state->suspend_restore_valid) {
        memcpy(state->signal_mask, state->suspend_restore_mask,
            sizeof(state->signal_mask));
        state->suspend_restore_valid = 0;
    }
}

void guest_thread_signal_delivery_mask(uint32_t words[2])
{
    struct guest_thread_state *state = current_thread();
    if (state != 0 && state->suspend_restore_valid) {
        memcpy(words, state->suspend_restore_mask,
            sizeof(state->suspend_restore_mask));
        state->suspend_restore_valid = 0;
    } else guest_thread_signal_mask_get(words);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ? -1 : 0;
}

/* Two fds: a nonblocking pipe, both ends hidden (close-on-exec). */
static int make_pipe_channel(int *read_fd, int *write_fd)
{
    int descriptors[2], i;
    if (pipe(descriptors) != 0) return -1;
    for (i = 0; i != 2; ++i) {
        int hidden = guest_fd_hide(descriptors[i]);
        if (hidden < 0) {
            int saved_errno = errno == EINVAL ? EMFILE : errno;
            close(descriptors[0]); close(descriptors[1]);
            errno = saved_errno;
            return -1;
        }
        descriptors[i] = hidden;
    }
    if (set_nonblocking(descriptors[0]) != 0 ||
            set_nonblocking(descriptors[1]) != 0) {
        int saved_errno = errno;
        close(descriptors[0]); close(descriptors[1]);
        errno = saved_errno;
        return -1;
    }
    *read_fd = descriptors[0];
    *write_fd = descriptors[1];
    return 0;
}

/* The thread's signal-wait channel is made once and kept: a fresh pipe per
   wait cost two hidden fds for every blocked thread (the hidden reserve
   ran out near 127 of them, and the read failed EMFILE).  A byte left by
   an earlier wait (a signal posted after its poll returned) is drained
   here; a signal already pending is re-posted by
   guest_thread_signal_wait_arm, which every caller runs next. */
int guest_thread_signal_wait_begin(void)
{
    struct guest_thread_state *state = current_thread();
    char sink[64];
    if (state == 0) { errno = EINVAL; return -1; }
    if (state->signal_wait_read_fd < 0) {
        int read_fd, write_fd;
        if (make_pipe_channel(&read_fd, &write_fd) != 0) return -1;
        state->signal_wait_read_fd = read_fd;
        __sync_synchronize();
        state->signal_wait_write_fd = write_fd;
    }
    state->signal_wait_active = 1;
    __sync_synchronize();
    while (read(state->signal_wait_read_fd, sink, sizeof(sink)) > 0) {}
    return state->signal_wait_read_fd;
}

/* A signal this thread would act on now under its current (maybe
   temporary) mask: one in its own queue, or one in the process queue (the
   collector routed it here, or the mask now lets it in) whose action does
   something (guest_signal_acting_mask: a handler, or a default that ends
   or stops the process; ignored ones only get discarded). */
static int signal_ready(struct guest_thread_state *state)
{
    int signal_number;
    uint32_t process[2], acting[2];
    guest_process_pending_mask(process, 1);
    if ((process[0] | process[1]) != 0) {
        guest_signal_acting_mask(acting);
        if ((process[0] & acting[0] & ~state->signal_mask[0]) != 0 ||
                (process[1] & acting[1] & ~state->signal_mask[1]) != 0)
            return 1;
    }
    if ((state->pending_signals[0] & ~state->signal_mask[0] & 0x7fffffffu)
            != 0)
        return 1;
    for (signal_number = LINUX_FIRST_REALTIME_SIGNAL;
        signal_number <= LINUX_LAST_REALTIME_SIGNAL; ++signal_number) {
        int index = signal_number - LINUX_FIRST_REALTIME_SIGNAL;
        if (!mask_has(state->signal_mask, signal_number) &&
                state->realtime_head[index] != state->realtime_tail[index])
            return 1;
    }
    return 0;
}

int guest_thread_signal_ready(void)
{
    struct guest_thread_state *state = current_thread();
    return state != 0 && signal_ready(state);
}

/* Every masked wait (ppoll, pselect6, epoll_pwait, rt_sigsuspend) runs
   this after taking its temporary mask: a signal already queued that the
   mask lets in posts the wait byte, so the wait returns EINTR at once and
   the trap's return delivers it.  Queued signals the mask lets in whose
   action ignores them are dropped first, as Linux discards them. */
void guest_thread_signal_wait_arm(void)
{
    struct guest_thread_state *state = current_thread();
    unsigned char byte = 1;
    uint32_t unblocked[2];
    if (state == 0 || state->signal_wait_write_fd < 0) return;
    unblocked[0] = ~state->signal_mask[0];
    unblocked[1] = ~state->signal_mask[1];
    guest_signal_drop_ignored(unblocked);
    if (signal_ready(state))
        write(state->signal_wait_write_fd, &byte, sizeof(byte));
}

void guest_thread_signal_wait_interrupted(const uint32_t old_words[2])
{
    guest_thread_signal_suspend_restore(old_words);
}

void guest_thread_signal_wait_complete(const uint32_t old_words[2])
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) state->suspend_restore_valid = 0;
    guest_thread_signal_mask_set(old_words);
}

/* The channel stays open for the thread's next wait; posting stops. */
void guest_thread_signal_wait_end(void)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) state->signal_wait_active = 0;
}

void guest_thread_syscall_enter(void)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) state->syscall_active = 1;
}

void guest_thread_syscall_leave(void)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) state->syscall_active = 0;
}

int guest_thread_syscall_active(void)
{
    struct guest_thread_state *state = current_thread();
    return state != 0 && state->syscall_active;
}

void guest_thread_futex_wait_begin(sem_t *semaphore)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) {
        state->futex_interrupted = 0;
        state->futex_semaphore = semaphore;
        __sync_synchronize();
    }
}

int guest_thread_futex_wait_end(void)
{
    struct guest_thread_state *state = current_thread();
    int interrupted;
    if (state == 0) return 0;
    __sync_synchronize();
    state->futex_semaphore = 0;
    interrupted = state->futex_interrupted;
    state->futex_interrupted = 0;
    return interrupted;
}

void guest_thread_altstack_get(uint32_t *pointer, uint32_t *size)
{
    struct guest_thread_state *state = current_thread();
    *pointer = state == 0 ? 0 : state->altstack_pointer;
    *size = state == 0 ? 0 : state->altstack_size;
}

void guest_thread_altstack_set(uint32_t pointer, uint32_t size)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) {
        state->altstack_pointer = pointer;
        state->altstack_size = size;
    }
}

int32_t guest_thread_kill(int32_t tid, int host_signal)
{
    struct guest_thread_state *state;
    int error = ESRCH;
    pthread_mutex_lock(&thread_registry_lock);
    for (state = thread_registry; state != 0; state = state->next) {
        if (state->tid == tid) {
            error = host_signal == 0 ? 0 :
                pthread_kill(state->host_thread, host_signal);
            break;
        }
    }
    pthread_mutex_unlock(&thread_registry_lock);
    return error == 0 ? 0 : -(int32_t)linux_errno_number(error);
}

void guest_thread_name_set(const char *name)
{
    struct guest_thread_state *state = current_thread();
    if (state == 0) return;
    strncpy(state->name, name, sizeof(state->name) - 1);
    state->name[sizeof(state->name) - 1] = '\0';
}

void guest_thread_name_get(char name[16])
{
    struct guest_thread_state *state = current_thread();
    if (state == 0) memset(name, 0, 16);
    else memcpy(name, state->name, 16);
}

uint32_t runtime_guest_tls(void)
{
    struct guest_thread_state *state = current_thread();
    return state == 0 ? 0 : state->guest_tls;
}

void runtime_set_guest_tls(uint32_t value)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) state->guest_tls = value;
}

void guest_thread_exit(int status)
{
    struct guest_thread_state *state = current_thread();
    (void)status;
    if (state == 0 || state == &main_thread) {
        linux_diag_flush();
        _exit(status & 0xff);
    }
    siglongjmp(state->exit_context, 1);
    linux_diag_flush();
    _exit(126);
}
