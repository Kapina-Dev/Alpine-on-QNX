#include "linuxemu.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
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
    uint32_t signal_mask[2];
    uint32_t suspend_restore_mask[2];
    int suspend_restore_valid;
    sem_t *futex_semaphore;
    volatile sig_atomic_t futex_interrupted;
    uint32_t altstack_pointer;
    uint32_t altstack_size;
    struct guest_thread_state *next;
};

static pthread_key_t thread_state_key;
static struct guest_thread_state main_thread;
static volatile uint32_t next_tid;
static pthread_mutex_t thread_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_thread_state *thread_registry;

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

static struct guest_thread_state *current_thread(void)
{
    return pthread_getspecific(thread_state_key);
}

static void finish_guest_thread(struct guest_thread_state *state)
{
    struct guest_thread_state **cursor;
    uint32_t *clear_child_tid = state->clear_child_tid;
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

int guest_thread_initialize(void)
{
    int error = pthread_key_create(&thread_state_key, 0);
    if (error != 0) { errno = error; return -1; }
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.tid = (int32_t)getpid();
    main_thread.host_thread = pthread_self();
    thread_registry = &main_thread;
    next_tid = (uint32_t)getpid() + 1u;
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
    memcpy(state->registers, context->uc_mcontext.cpu.gpr,
        sizeof(state->registers));
    state->registers[0] = 0;
    state->registers[13] = child_stack;
    state->registers[15] += 4;
    state->guest_tls = (flags & LINUX_CLONE_SETTLS) != 0 ? tls :
        parent->guest_tls;
    memcpy(state->signal_mask, parent->signal_mask,
        sizeof(state->signal_mask));
    state->tid = (int32_t)__sync_fetch_and_add(&next_tid, 1u);
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
    return state == 0 ? (int32_t)getpid() : state->tid;
}

int32_t guest_thread_set_tid_address(uint32_t *address)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0) state->clear_child_tid = address;
    return guest_thread_tid();
}

void guest_thread_after_fork(void)
{
    pthread_mutex_t fresh_lock = PTHREAD_MUTEX_INITIALIZER;
    struct guest_thread_state *state = current_thread();
    uint32_t guest_tls = state == 0 ? 0 : state->guest_tls;
    uint32_t signal_mask[2] = { 0, 0 };
    uint32_t altstack_pointer = state == 0 ? 0 : state->altstack_pointer;
    uint32_t altstack_size = state == 0 ? 0 : state->altstack_size;
    if (state != 0)
        memcpy(signal_mask, state->signal_mask, sizeof(signal_mask));
    if (state != 0 && state != &main_thread) free(state);
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.guest_tls = guest_tls;
    memcpy(main_thread.signal_mask, signal_mask,
        sizeof(main_thread.signal_mask));
    main_thread.altstack_pointer = altstack_pointer;
    main_thread.altstack_size = altstack_size;
    main_thread.tid = (int32_t)getpid();
    main_thread.host_thread = pthread_self();
    next_tid = (uint32_t)getpid() + 1u;
    thread_registry_lock = fresh_lock;
    thread_registry = &main_thread;
    pthread_setspecific(thread_state_key, &main_thread);
    linux_futex_after_fork();
}

void guest_thread_signal_pending(int linux_signal)
{
    struct guest_thread_state *state = current_thread();
    if (state != 0 && linux_signal > 0 && linux_signal <= 64) {
        __sync_or_and_fetch(&state->pending_signals[(linux_signal - 1) / 32],
            (uint32_t)1u << ((linux_signal - 1) % 32));
        if (state->futex_semaphore != 0) {
            state->futex_interrupted = 1;
            sem_post(state->futex_semaphore);
        }
    }
}

int guest_thread_take_pending(void)
{
    struct guest_thread_state *state = current_thread();
    uint32_t old_value;
    uint32_t candidates;
    uint32_t new_value;
    int word;
    int signal_number;
    if (state == 0) return 0;
    for (word = 0; word != 2; ++word) {
        do {
            old_value = state->pending_signals[word];
            candidates = old_value & ~state->signal_mask[word];
            if (candidates == 0) break;
            signal_number = __builtin_ctz(candidates) + 1 + word * 32;
            new_value = old_value & ~(1u << ((signal_number - 1) % 32));
        } while (!__sync_bool_compare_and_swap(&state->pending_signals[word],
            old_value, new_value));
        if (candidates != 0) return signal_number;
    }
    return 0;
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

void guest_thread_signal_delivery_mask(uint32_t words[2])
{
    struct guest_thread_state *state = current_thread();
    if (state != 0 && state->suspend_restore_valid) {
        memcpy(words, state->suspend_restore_mask,
            sizeof(state->suspend_restore_mask));
        state->suspend_restore_valid = 0;
    } else guest_thread_signal_mask_get(words);
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
    if (state == 0 || state == &main_thread) _exit(status & 0xff);
    siglongjmp(state->exit_context, 1);
    _exit(126);
}
