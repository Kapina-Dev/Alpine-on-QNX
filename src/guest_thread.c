#include "linuxemu.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
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
};

static pthread_key_t thread_state_key;
static struct guest_thread_state main_thread;
static volatile uint32_t next_tid;

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
    uint32_t *clear_child_tid = state->clear_child_tid;
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
    struct guest_thread_state *state = current_thread();
    uint32_t guest_tls = state == 0 ? 0 : state->guest_tls;
    if (state != 0 && state != &main_thread) free(state);
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.guest_tls = guest_tls;
    main_thread.tid = (int32_t)getpid();
    next_tid = (uint32_t)getpid() + 1u;
    pthread_setspecific(thread_state_key, &main_thread);
    linux_futex_after_fork();
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
