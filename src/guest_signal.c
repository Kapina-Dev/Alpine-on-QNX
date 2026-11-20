#include "linuxemu.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LINUX_SIGNAL_COUNT 64
#define LINUX_SA_NOCLDSTOP 0x00000001u
#define LINUX_SA_NOCLDWAIT 0x00000002u
#define LINUX_SA_SIGINFO 0x00000004u
#define LINUX_SA_RESTORER 0x04000000u
#define LINUX_SA_ONSTACK 0x08000000u
#define LINUX_SA_RESTART 0x10000000u
#define LINUX_SA_NODEFER 0x40000000u
#define LINUX_SA_RESETHAND 0x80000000u
#define LINUX_VFP_MAGIC 0x56465001u
#define LINUX_VFP_FRAME_SIZE 288u
#define HOST_SIGNAL_STACK_RESERVE 8192u

struct linux_sigaction {
    uint32_t handler, flags, restorer, mask[2];
};

struct linux_sigcontext {
    uint32_t trap_no, error_code, oldmask;
    uint32_t registers[16];
    uint32_t cpsr, fault_address;
};

struct linux_signal_stack { uint32_t pointer; int32_t flags; uint32_t size; };

struct linux_ucontext {
    uint32_t flags, link;
    struct linux_signal_stack stack;
    struct linux_sigcontext machine;
    uint32_t signal_mask[2];
    uint32_t unused[30];
    uint32_t register_space[128] __attribute__((aligned(8)));
};

struct linux_sigframe {
    struct linux_ucontext context;
    uint32_t return_code[4];
};

struct linux_rt_sigframe {
    unsigned char information[128];
    struct linux_sigframe signal;
};

typedef char verify_ucontext_size[sizeof(struct linux_ucontext) == 744 ? 1 : -1];
typedef char verify_sigframe_size[sizeof(struct linux_sigframe) == 760 ? 1 : -1];
typedef char verify_rt_sigframe_size[
    sizeof(struct linux_rt_sigframe) == 888 ? 1 : -1];

static struct linux_sigaction guest_actions[LINUX_SIGNAL_COUNT + 1];

__asm__(
    ".text\n.align 2\n.arm\n"
    ".global linuxemu_signal_return_trampoline\n"
    ".type linuxemu_signal_return_trampoline, %function\n"
    "linuxemu_signal_return_trampoline:\n.word 0xe7f000f0\n"
    ".size linuxemu_signal_return_trampoline, .-linuxemu_signal_return_trampoline\n"
    ".global linuxemu_rt_signal_return_trampoline\n"
    ".type linuxemu_rt_signal_return_trampoline, %function\n"
    "linuxemu_rt_signal_return_trampoline:\n.word 0xe7f000f1\n"
    ".size linuxemu_rt_signal_return_trampoline, .-linuxemu_rt_signal_return_trampoline\n");

static int linux_signal_to_host(int number)
{
    if (number >= 32 && number <= 47) return SIGRTMIN + number - 32;
    switch (number) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 8: case 9:
    case 11: case 13: case 14: case 15: return number;
    case 7: return 10; case 10: return 16; case 12: return 17;
    case 17: return 18; case 18: return 25; case 19: return 23;
    case 20: return 24; case 21: return 26; case 22: return 27;
    case 23: return 21; case 24: return 30; case 25: return 31;
    case 26: return 28; case 27: return 29; case 28: return 20;
    case 29: return 22; case 30: return 19; case 31: return 12;
    default: return 0;
    }
}

int guest_signal_from_host(int number)
{
    int signal_number;
    if (number >= SIGRTMIN && number <= SIGRTMAX)
        return number - SIGRTMIN + 32;
    for (signal_number = 1; signal_number <= 31; ++signal_number)
        if (linux_signal_to_host(signal_number) == number) return signal_number;
    return 0;
}

static void linux_set_to_host(const uint32_t words[2], sigset_t *host_set)
{
    int number;
    sigemptyset(host_set);
    for (number = 1; number <= LINUX_SIGNAL_COUNT; ++number) {
        int host_signal;
        if ((words[(number - 1) / 32] &
                (1u << ((number - 1) % 32))) == 0) continue;
        host_signal = linux_signal_to_host(number);
        if (host_signal != 0) sigaddset(host_set, host_signal);
    }
    sigdelset(host_set, SIGILL);
    sigdelset(host_set, SIGSEGV);
}

static void merge_host_sets(sigset_t *target, const sigset_t *source)
{
    int number;
    for (number = 1; number <= SIGRTMAX; ++number)
        if (sigismember(source, number) == 1) sigaddset(target, number);
}

static void save_vfp(struct linux_ucontext *guest, const ucontext_t *host)
{
    unsigned char *frame = (unsigned char *)guest->register_space;
    uint32_t value;
    memset(frame, 0, sizeof(guest->register_space));
    value = LINUX_VFP_MAGIC; memcpy(frame, &value, 4);
    value = LINUX_VFP_FRAME_SIZE; memcpy(frame + 4, &value, 4);
    memcpy(frame + 8, host->uc_mcontext.fpu.un.vfp.reg.X, 256);
    memcpy(frame + 264, &host->uc_mcontext.fpu.un.vfp.fpscr, 4);
    memcpy(frame + 272, &host->uc_mcontext.fpu.un.vfp.fpexc, 4);
    memcpy(frame + 276, &host->uc_mcontext.fpu.un.vfp.fpinst, 4);
    memcpy(frame + 280, &host->uc_mcontext.fpu.un.vfp.fpinst2, 4);
}

static int restore_vfp(ucontext_t *host, const struct linux_ucontext *guest)
{
    const unsigned char *frame = (const unsigned char *)guest->register_space;
    uint32_t magic, size;
    memcpy(&magic, frame, 4);
    if (magic == 0) return 0;
    memcpy(&size, frame + 4, 4);
    if (magic != LINUX_VFP_MAGIC || size != LINUX_VFP_FRAME_SIZE) return -1;
    memcpy(host->uc_mcontext.fpu.un.vfp.reg.X, frame + 8, 256);
    memcpy(&host->uc_mcontext.fpu.un.vfp.fpscr, frame + 264, 4);
    memcpy(&host->uc_mcontext.fpu.un.vfp.fpexc, frame + 272, 4);
    memcpy(&host->uc_mcontext.fpu.un.vfp.fpinst, frame + 276, 4);
    memcpy(&host->uc_mcontext.fpu.un.vfp.fpinst2, frame + 280, 4);
    return 0;
}

/* Host si_signo tag for siginfo queued by the guest itself
   (rt_tgsigqueueinfo): the other fields already hold Linux values. */
#define GUEST_NATIVE_SIGINFO (-1)

static void fill_siginfo(unsigned char destination[128], int linux_signal,
    const siginfo_t *source)
{
    int32_t *words = (int32_t *)destination;
    memset(destination, 0, 128);
    words[0] = linux_signal;
    if (source == 0) return;
    if (source->si_signo == GUEST_NATIVE_SIGINFO) {
        words[1] = source->si_errno;
        words[2] = source->si_code;
        words[3] = source->si_pid;
        words[4] = (int32_t)source->si_uid;
        words[5] = source->si_value.sival_int;
        return;
    }
    words[1] = linux_errno_number(source->si_errno);
    words[2] = source->si_code;
    if (linux_signal == 4 || linux_signal == 7 || linux_signal == 8 ||
        linux_signal == 11)
        words[3] = (int32_t)(uintptr_t)source->si_addr;
    else {
        words[3] = source->si_pid;
        words[4] = (int32_t)source->si_uid;
    }
}

static void prepare_context(struct linux_ucontext *guest,
    const ucontext_t *host, const uint32_t old_mask[2], uintptr_t fault_address)
{
    memset(guest, 0, sizeof(*guest));
    guest->stack.flags = 2;
    guest->machine.oldmask = old_mask[0];
    memcpy(guest->machine.registers, host->uc_mcontext.cpu.gpr,
        sizeof(guest->machine.registers));
    guest->machine.cpsr = host->uc_mcontext.cpu.spsr;
    guest->machine.fault_address = (uint32_t)fault_address;
    memcpy(guest->signal_mask, old_mask, sizeof(guest->signal_mask));
    save_vfp(guest, host);
}

int guest_signal_deliver(ucontext_t *context, int linux_signal,
    const siginfo_t *host_info)
{
    struct linux_sigaction action;
    struct linux_ucontext *guest_context;
    struct linux_sigframe *plain_frame = 0;
    struct linux_rt_sigframe *realtime_frame = 0;
    sigset_t action_mask, desired_mask;
    uint32_t old_mask[2];
    uint32_t new_mask[2];
    uintptr_t stack_pointer, restorer;
    uint32_t altstack_pointer, altstack_size;
    int on_altstack;
    int host_signal, realtime;

    if (linux_signal <= 0 || linux_signal > LINUX_SIGNAL_COUNT) return -1;
    guest_thread_signal_mask_get(old_mask);
    if (linux_signal != 4 && linux_signal != 11 &&
        (old_mask[(linux_signal - 1) / 32] &
            (1u << ((linux_signal - 1) % 32))) != 0) {
        guest_thread_signal_pending(linux_signal, host_info);
        return 1;
    }
    action = guest_actions[linux_signal];
    if (action.handler <= 1u) return -1;
    guest_thread_signal_delivery_mask(old_mask);
    realtime = (action.flags & LINUX_SA_SIGINFO) != 0;
    /* QNX enters this handler on the interrupted guest stack. Keep the Linux
       frame below the live QNX signal frame and emulator call frames. */
    guest_thread_altstack_get(&altstack_pointer, &altstack_size);
    on_altstack = altstack_size != 0 &&
        context->uc_mcontext.cpu.gpr[13] >= altstack_pointer &&
        context->uc_mcontext.cpu.gpr[13] < altstack_pointer + altstack_size;
    if ((action.flags & LINUX_SA_ONSTACK) != 0 && altstack_size != 0 &&
        !on_altstack)
        stack_pointer = altstack_pointer + altstack_size;
    else stack_pointer = context->uc_mcontext.cpu.gpr[13] -
        HOST_SIGNAL_STACK_RESERVE;
    if (realtime) {
        stack_pointer = (stack_pointer - sizeof(*realtime_frame)) & ~7u;
        realtime_frame = (struct linux_rt_sigframe *)stack_pointer;
        memset(realtime_frame, 0, sizeof(*realtime_frame));
        fill_siginfo(realtime_frame->information, linux_signal, host_info);
        guest_context = &realtime_frame->signal.context;
    } else {
        stack_pointer = (stack_pointer - sizeof(*plain_frame)) & ~7u;
        plain_frame = (struct linux_sigframe *)stack_pointer;
        memset(plain_frame, 0, sizeof(*plain_frame));
        guest_context = &plain_frame->context;
    }
    prepare_context(guest_context, context, old_mask,
        host_info == 0 ? 0 : (uintptr_t)host_info->si_addr);
    guest_context->stack.pointer = altstack_pointer;
    guest_context->stack.size = altstack_size;
    guest_context->stack.flags = on_altstack ? 1 :
        (altstack_size == 0 ? 2 : 0);
    desired_mask = context->uc_sigmask;
    linux_set_to_host(action.mask, &action_mask);
    merge_host_sets(&desired_mask, &action_mask);
    host_signal = linux_signal_to_host(linux_signal);
    if ((action.flags & LINUX_SA_NODEFER) == 0 && host_signal != 0)
        sigaddset(&desired_mask, host_signal);
    context->uc_sigmask = desired_mask;
    new_mask[0] = old_mask[0] | action.mask[0];
    new_mask[1] = old_mask[1] | action.mask[1];
    if ((action.flags & LINUX_SA_NODEFER) == 0)
        new_mask[(linux_signal - 1) / 32] |=
            1u << ((linux_signal - 1) % 32);
    guest_thread_signal_mask_set(new_mask);

    restorer = ((action.flags & LINUX_SA_RESTORER) != 0 &&
        action.restorer != 0) ? action.restorer : (realtime ?
        (uintptr_t)linuxemu_rt_signal_return_trampoline :
        (uintptr_t)linuxemu_signal_return_trampoline);
    context->uc_mcontext.cpu.gpr[0] = (uint32_t)linux_signal;
    if (realtime) {
        context->uc_mcontext.cpu.gpr[1] =
            (uint32_t)(uintptr_t)realtime_frame->information;
        context->uc_mcontext.cpu.gpr[2] =
            (uint32_t)(uintptr_t)&realtime_frame->signal.context;
    }
    context->uc_mcontext.cpu.gpr[13] = (uint32_t)stack_pointer;
    context->uc_mcontext.cpu.gpr[14] = (uint32_t)restorer;
    context->uc_mcontext.cpu.gpr[15] = action.handler;
    if ((action.flags & LINUX_SA_RESETHAND) != 0)
        guest_actions[linux_signal].handler = 0;
    return 0;
}

static int restartable_syscall(uint32_t number)
{
    switch (number) {
    case 3: case 4: case 54: case 114: case 145: case 146: case 240:
    case 239: case 283: case 285: case 291: case 292: case 297: case 361:
    case 362: case 365: case 366: case 374: case 392: case 393:
        return 1;
    default:
        return 0;
    }
}

/* Signals with a guest handler. */
void guest_signal_handled_mask(uint32_t words[2])
{
    int linux_signal;
    words[0] = words[1] = 0;
    for (linux_signal = 1; linux_signal <= LINUX_SIGNAL_COUNT; ++linux_signal)
        if (guest_actions[linux_signal].handler > 1u)
            words[(linux_signal - 1) / 32] |=
                1u << ((linux_signal - 1) % 32);
}

static int default_action_terminates(int linux_signal);
static void default_host_action(int linux_signal, struct sigaction *action);

/* SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU: the default action stops. */
static int default_action_stops(int linux_signal)
{
    return linux_signal >= 19 && linux_signal <= 22;
}

/* Signals whose delivery does something now: a guest handler, or a
   default action that ends or stops the process.  The rest (ignored, or
   a default that ignores: SIGCHLD, SIGCONT, SIGURG, SIGWINCH) are only
   discarded, and a wait isn't interrupted for them. */
void guest_signal_acting_mask(uint32_t words[2])
{
    int linux_signal;
    words[0] = words[1] = 0;
    for (linux_signal = 1; linux_signal <= LINUX_SIGNAL_COUNT; ++linux_signal) {
        uint32_t handler = guest_actions[linux_signal].handler;
        if (handler > 1u || (handler == 0 &&
                (default_action_terminates(linux_signal) ||
                    default_action_stops(linux_signal))))
            words[(linux_signal - 1) / 32] |=
                1u << ((linux_signal - 1) % 32);
    }
}

/*
 * The default action of a signal Linuxemu took itself (signalfd's
 * collector, or a per-thread queue whose handler was reset since), now
 * that the guest lets it in.  An ignoring default discards it.  A default
 * that ends or stops the process is carried out the way the self-signal
 * path does it: write the diagnostics report, put the host default back
 * and raise the signal at this thread with it unblocked, so the kernel
 * applies exactly that action (the collector may be waiting for the same
 * signal, so it is thread-directed).  The sender's details are lost,
 * which only matters to a process that is ending.  After a stop the
 * thread's host mask and the host action are put back.  from_handler: a
 * host signal handler that interrupted guest code (only the
 * async-signal-safe report is written).
 */
static void apply_default_action(int linux_signal, int from_handler)
{
    struct sigaction default_action, restore;
    sigset_t only, old;
    int host_signal = linux_signal_to_host(linux_signal);
    if (host_signal == 0 || (!default_action_terminates(linux_signal) &&
            !default_action_stops(linux_signal))) return;
    if (default_action_terminates(linux_signal)) {
        if (from_handler) linux_diag_flush_final();
        else linux_diag_flush();
    }
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(host_signal, &default_action, 0);
    sigemptyset(&only);
    sigaddset(&only, host_signal);
    pthread_sigmask(SIG_UNBLOCK, &only, &old);
    pthread_kill(pthread_self(), host_signal);
    /* Still here: the process was stopped and continued. */
    pthread_sigmask(SIG_SETMASK, &old, 0);
    memset(&restore, 0, sizeof(restore));
    sigemptyset(&restore.sa_mask);
    if (guest_actions[linux_signal].handler == 0) {
        default_host_action(linux_signal, &restore);
        sigaction(host_signal, &restore, 0);
    }
}

/* The next signal this thread takes now: its own queue first, then the
   process queue (signals the collector took), as Linux takes private
   pending signals before shared ones, among the signals the thread
   doesn't block.  A taken signal without a guest handler gets its
   default or ignore action here (apply_default_action) and the search
   goes on; only a signal with a handler is returned. */
static int take_deliverable(siginfo_t *information, int *has_information,
    int from_handler)
{
    for (;;) {
        uint32_t pending[2], mask[2], allowed[2];
        int linux_signal = guest_thread_take_pending(information,
            has_information);
        if (linux_signal == 0) {
            guest_process_pending_mask(pending, 1);
            if ((pending[0] | pending[1]) == 0) return 0;
            guest_thread_signal_mask_get(mask);
            allowed[0] = ~mask[0] & pending[0];
            allowed[1] = ~mask[1] & pending[1];
            if ((allowed[0] | allowed[1]) == 0) return 0;
            linux_signal = guest_process_take_pending(allowed, information);
            *has_information = linux_signal != 0;
            if (linux_signal == 0) return 0;
        }
        /* SIGILL and SIGSEGV queued per thread keep their old path. */
        if (guest_actions[linux_signal].handler > 1u || linux_signal == 4 ||
                linux_signal == 11)
            return linux_signal;
        if (guest_actions[linux_signal].handler == 0)
            apply_default_action(linux_signal, from_handler);
    }
}

/* Process-queue entries in unblocked whose action ignores them: taken
   and dropped (Linux discards an ignored signal once it is unblocked).
   Run by a masked wait before it sleeps, so such a signal neither wakes
   it nor stays queued. */
void guest_signal_drop_ignored(const uint32_t unblocked[2])
{
    uint32_t acting[2], pending[2], drop[2];
    siginfo_t information;
    guest_process_pending_mask(pending, 1);
    if (((pending[0] & unblocked[0]) | (pending[1] & unblocked[1])) == 0)
        return;
    guest_signal_acting_mask(acting);
    drop[0] = pending[0] & unblocked[0] & ~acting[0];
    drop[1] = pending[1] & unblocked[1] & ~acting[1];
    if ((drop[0] | drop[1]) == 0) return;
    while (guest_process_take_pending(drop, &information) != 0) {}
}

int guest_signal_deliver_pending(ucontext_t *context, uint32_t syscall_number,
    uint32_t original_r0, uint32_t instruction_width)
{
    siginfo_t information;
    int has_information;
    int linux_signal = take_deliverable(&information, &has_information, 0);
    if (linux_signal == 0) {
        /* A masked wait that ended with nothing to deliver goes back to
           the guest's own mask. */
        guest_thread_signal_suspend_cancel();
        return 0;
    }
    if (context->uc_mcontext.cpu.gpr[0] == (uint32_t)-EINTR &&
        (guest_actions[linux_signal].flags & LINUX_SA_RESTART) != 0 &&
        restartable_syscall(syscall_number)) {
        context->uc_mcontext.cpu.gpr[0] = original_r0;
        context->uc_mcontext.cpu.gpr[15] -= instruction_width;
    }
    return guest_signal_deliver(context, linux_signal,
        has_information ? &information : 0) == 0 ? 1 : -1;
}

int guest_signal_return(ucontext_t *context, int realtime)
{
    uintptr_t stack_pointer = context->uc_mcontext.cpu.gpr[13];
    struct linux_ucontext *guest;
    sigset_t host_mask;
    if ((stack_pointer & 7u) != 0) return -1;
    guest = realtime ? &((struct linux_rt_sigframe *)stack_pointer)->signal.context :
        &((struct linux_sigframe *)stack_pointer)->context;
    if (restore_vfp(context, guest) != 0) return -1;
    linux_set_to_host(guest->signal_mask, &host_mask);
    context->uc_sigmask = host_mask;
    guest_thread_signal_mask_set(guest->signal_mask);
    memcpy(context->uc_mcontext.cpu.gpr, guest->machine.registers,
        sizeof(guest->machine.registers));
    context->uc_mcontext.cpu.spsr = guest->machine.cpsr;
    if (realtime) {
        if (guest->stack.flags == 2)
            guest_thread_altstack_set(0, 0);
        else if (guest->stack.flags == 0)
            guest_thread_altstack_set(guest->stack.pointer,
                guest->stack.size);
    }
    return 0;
}

int32_t guest_signal_altstack(ucontext_t *context, const void *guest_stack,
    void *guest_old_stack)
{
    uint32_t pointer, size;
    uint32_t current_sp = context->uc_mcontext.cpu.gpr[13];
    int on_stack;
    guest_thread_altstack_get(&pointer, &size);
    on_stack = size != 0 && current_sp >= pointer && current_sp < pointer + size;
    if (guest_old_stack != 0) {
        uint32_t old[3];
        old[0] = pointer;
        old[1] = on_stack ? 1u : (size == 0 ? 2u : 0u);
        old[2] = size;
        memcpy(guest_old_stack, old, sizeof(old));
    }
    if (guest_stack != 0) {
        uint32_t requested[3];
        memcpy(requested, guest_stack, sizeof(requested));
        if (on_stack) return -EPERM;
        if ((requested[1] & ~2u) != 0) return -EINVAL;
        if ((requested[1] & 2u) != 0)
            guest_thread_altstack_set(0, 0);
        else {
            if (requested[2] < 2048u) return -ENOMEM;
            guest_thread_altstack_set(requested[0], requested[2]);
        }
    }
    return 0;
}

static void host_signal_handler(int host_signal, siginfo_t *information,
    void *argument)
{
    ucontext_t *context = argument;
    int linux_signal = guest_signal_from_host(host_signal);
    uintptr_t pc = context->uc_mcontext.cpu.gpr[15];
    if (linux_signal == 0) return;
    if (!guest_thread_syscall_active() && guest_memory_is_executable(pc, 4) &&
        guest_signal_deliver(context, linux_signal, information) == 0) {
        /* The guest handler's mask, on the host until its sigreturn. */
        guest_signal_apply_frame_mask(information);
        return;
    }
    guest_thread_signal_pending(linux_signal, information);
}

/*
 * The poke: the host signal signalfd's collector sends a guest thread
 * after routing it a signal it took but the thread has unblocked
 * (guest_thread_route_signal).  QNX's SIGEMT (7): no Linux signal maps
 * to it (Linux 7, SIGBUS, is QNX 10), so a guest can neither send nor
 * block it, and Linuxemu uses it for nothing else.  In guest code the
 * handler delivers the routed signal at once, as host_signal_handler does
 * for a direct one; inside a syscall it only wakes the thread's wait,
 * and the syscall's return delivers it (guest_signal_deliver_pending).
 */
#define GUEST_SIGNAL_POKE SIGEMT

static void poke_handler(int host_signal, siginfo_t *information,
    void *argument)
{
    ucontext_t *context = argument;
    uintptr_t pc = context->uc_mcontext.cpu.gpr[15];
    siginfo_t pending;
    int has_information, linux_signal;
    (void)host_signal;
    if (guest_thread_syscall_active() || !guest_memory_is_executable(pc, 4)) {
        guest_thread_signal_wake();
        return;
    }
    linux_signal = take_deliverable(&pending, &has_information, 1);
    if (linux_signal == 0 ||
            guest_signal_deliver(context, linux_signal,
                has_information ? &pending : 0) != 0) return;
    guest_signal_apply_frame_mask(information);
    /* More routed signals: the next poke arrives once this frame is set
       up (the poke is blocked until this handler returns), so they nest
       as Linux delivers several pending signals. */
    {
        uint32_t queued[2];
        guest_process_pending_mask(queued, 1);
        if ((queued[0] | queued[1]) != 0)
            (void)pthread_kill(pthread_self(), GUEST_SIGNAL_POKE);
    }
}

int guest_signal_poke_install(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = poke_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    return sigaction(GUEST_SIGNAL_POKE, &action, 0);
}

/* The collector took host_signal.  A guest thread that has it unblocked
   gets it through normal delivery (routed); otherwise it goes into the
   process queue.  Returns the Linux signal queued for signalfd, or 0. */
int guest_signal_collected(int host_signal, const siginfo_t *information)
{
    int linux_signal = guest_signal_from_host(host_signal);
    if (linux_signal == 0 || linux_signal == 9 || linux_signal == 19)
        return 0;
    if (guest_thread_route_signal(linux_signal, information,
            guest_actions[linux_signal].handler > 1u ? 0 : host_signal,
            GUEST_SIGNAL_POKE))
        return 0;
    guest_process_signal_queue(linux_signal, information);
    /* A thread that unblocked the signal (a masked wait starting, a
       sigprocmask) after the routing check above but before its own
       look at the process queue would miss the entry: poke every thread
       that lets it in now.  Delivery then takes it like a routed one.
       An ignored one isn't worth interrupting anybody for. */
    {
        uint32_t acting[2];
        guest_signal_acting_mask(acting);
        if ((acting[(linux_signal - 1) / 32] &
                (1u << ((linux_signal - 1) % 32))) != 0)
            guest_thread_poke_unblocked(linux_signal, GUEST_SIGNAL_POKE);
    }
    return linux_signal;
}

/* QNX si_code to Linux: the user codes differ (QNX SI_QUEUE -2, SI_TIMER
   -3, SI_MESGQ -5; Linux -1, -2, -3); the kernel codes (CLD_*, SEGV_*,
   ...) have the same values.  QNX SI_NOINFO (127) says only si_signo is
   valid. */
static int32_t linux_si_code(int host_code)
{
    switch (host_code) {
    case SI_QUEUE: return -1;
    case SI_TIMER: return -2;
    case SI_MESGQ: return -3;
    case SI_ASYNCIO: return -4;
    case SI_NOINFO: return 0;
    default: return host_code;
    }
}

static void put32(unsigned char *record, size_t offset, uint32_t value)
{
    memcpy(record + offset, &value, sizeof(value));
}

static void put64(unsigned char *record, size_t offset, uint64_t value)
{
    memcpy(record + offset, &value, sizeof(value));
}

/*
 * One 128-byte Linux struct signalfd_siginfo.  QNX's sigwaitinfo reports
 * si_uid 0 whoever sent the signal (FINDINGS 2026-11-19, Phase 3 part 2
 * probes); every BB10 app process runs as the same user, so a sender uid
 * of 0 with a sender pid is reported as this process's real uid
 * (documented: a root sender shows as the user's uid).
 */
void guest_signal_signalfd_record(unsigned char record[128], int linux_signal,
    const siginfo_t *source)
{
    int32_t code;
    memset(record, 0, 128);
    put32(record, 0, (uint32_t)linux_signal);
    if (source == 0) return;
    if (source->si_signo == GUEST_NATIVE_SIGINFO) {
        put32(record, 4, (uint32_t)source->si_errno);
        put32(record, 8, (uint32_t)source->si_code);
        put32(record, 12, (uint32_t)source->si_pid);
        put32(record, 16, (uint32_t)source->si_uid);
        put32(record, 44, (uint32_t)source->si_value.sival_int);
        put64(record, 48, (uint32_t)source->si_value.sival_int);
        return;
    }
    if (source->si_code == SI_NOINFO) return;
    code = linux_si_code(source->si_code);
    if (source->si_errno != 0)
        put32(record, 4, (uint32_t)linux_errno_number(source->si_errno));
    put32(record, 8, (uint32_t)code);
    if (linux_signal == 4 || linux_signal == 7 || linux_signal == 8 ||
            linux_signal == 11) {
        if (source->si_code > 0) {
            put64(record, 72, (uint32_t)(uintptr_t)source->si_addr);
            return;
        }
    }
    put32(record, 12, (uint32_t)source->si_pid);
    if (linux_signal == 17 && source->si_code > 0) {
        /* SIGCHLD from the kernel: status and the child's times; the
           child runs as this user. */
        put32(record, 16, (uint32_t)getuid());
        put32(record, 40, (uint32_t)source->si_status);
        put64(record, 56, (uint64_t)source->si_utime);
        put64(record, 64, (uint64_t)source->si_stime);
        return;
    }
    put32(record, 16, source->si_uid == 0 && source->si_pid != 0 ?
        (uint32_t)getuid() : (uint32_t)source->si_uid);
    if (source->si_code <= 0) {
        put32(record, 44, (uint32_t)source->si_value.sival_int);
        put64(record, 48, (uint32_t)(uintptr_t)source->si_value.sival_ptr);
    }
}

/* Linux signals whose default action does not end the process: SIGCHLD,
   SIGCONT, SIGURG and SIGWINCH are ignored; SIGSTOP/TSTP/TTIN/TTOU stop. */
static int default_action_terminates(int linux_signal)
{
    switch (linux_signal) {
    case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 28:
        return 0;
    default:
        return linux_signal > 0 && linux_signal <= LINUX_SIGNAL_COUNT;
    }
}

/*
 * A signal this process sends itself with the default action may be the
 * last thing it does (abort() does exactly this), and the host kernel ends
 * it without returning here, so write the diagnostics report first. A
 * signal sent by another process cannot be intercepted this way.
 */
static void flush_before_self_signal(int linux_signal)
{
    if (default_action_terminates(linux_signal) &&
        guest_actions[linux_signal].handler == 0)
        linux_diag_flush();
}

/*
 * With LINUXEMU_DIAGNOSTICS set, a signal whose action is the default and
 * whose Linux default ends the process (SIGPIPE from a closed pipe, SIGALRM,
 * SIGXCPU, a kill from another process, ...) gets this host handler instead
 * of SIG_DFL: it writes the report, then restores SIG_DFL and re-raises, so
 * the process still ends by the same signal. With diagnostics off nothing
 * changes: the host action stays SIG_DFL.
 */
static int diagnostics_handlers;

static void diagnostics_terminate_handler(int host_signal)
{
    struct sigaction default_action;
    sigset_t unblock;

    /* Only async-signal-safe calls here: the signal may interrupt libc
       holding a lock (a fork child setting up its exec, for example). */
    linux_diag_flush_final();
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(host_signal, &default_action, 0);
    sigemptyset(&unblock);
    sigaddset(&unblock, host_signal);
    sigprocmask(SIG_UNBLOCK, &unblock, 0);
    kill(getpid(), host_signal);
}

/* SIGILL and SIGSEGV belong to the trap handler; KILL and STOP can't be
   caught. */
static int diagnostics_catchable(int linux_signal)
{
    return default_action_terminates(linux_signal) && linux_signal != 4 &&
        linux_signal != 9 && linux_signal != 11 && linux_signal != 19;
}

static void diagnostics_terminate_action(int host_signal,
    siginfo_t *information, void *argument)
{
    (void)information;
    (void)argument;
    diagnostics_terminate_handler(host_signal);
}

/*
 * The host action for a guest's default action (into a zeroed sigaction).
 * QNX queues a real-time signal only when its action has SA_SIGINFO
 * (SIG_DFL without it: one of three sigqueues stays pending, FINDINGS
 * 2026-11-19, signalfd task), and Linux always queues them, so a
 * real-time default action carries the flag.
 */
static void default_host_action(int linux_signal, struct sigaction *action)
{
    int queued = linux_signal >= 32;
    if (diagnostics_handlers && diagnostics_catchable(linux_signal)) {
        if (queued) action->sa_sigaction = diagnostics_terminate_action;
        else action->sa_handler = diagnostics_terminate_handler;
    } else action->sa_handler = SIG_DFL;
    if (queued) action->sa_flags |= SA_SIGINFO;
}

/* At start-up: the real-time signals still at SIG_DFL get the queueing
   default (an inherited SIG_IGN stays). */
void guest_signal_queue_defaults(void)
{
    int linux_signal;
    for (linux_signal = 32; linux_signal <= LINUX_SIGNAL_COUNT; ++linux_signal) {
        struct sigaction current, action;
        int host_signal = linux_signal_to_host(linux_signal);
        if (host_signal == 0 || sigaction(host_signal, 0, &current) != 0 ||
                current.sa_handler != SIG_DFL ||
                (current.sa_flags & SA_SIGINFO) != 0) continue;
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        default_host_action(linux_signal, &action);
        (void)sigaction(host_signal, &action, 0);
    }
}

void guest_signal_diagnostics_initialize(void)
{
    int linux_signal;

    diagnostics_handlers = 1;
    for (linux_signal = 1; linux_signal <= LINUX_SIGNAL_COUNT; ++linux_signal) {
        struct sigaction action;
        int host_signal = linux_signal_to_host(linux_signal);
        if (host_signal == 0 || !diagnostics_catchable(linux_signal) ||
            guest_actions[linux_signal].handler != 0) continue;
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        default_host_action(linux_signal, &action);
        sigaction(host_signal, &action, 0);
    }
}

int32_t guest_signal_send(pid_t process, int linux_signal, uint32_t number,
    const uint32_t *args)
{
    int host_signal = linux_signal == 0 ? 0 : linux_signal_to_host(linux_signal);
    if (linux_signal < 0 || linux_signal > LINUX_SIGNAL_COUNT) return -EINVAL;
    if (linux_signal != 0 && host_signal == 0)
        return linux_report_unsupported(number, EINVAL, "signal-unmapped",
            args);
    if (process == 0 || process == -1 || process == getpid() ||
        process == -getpgrp())
        flush_before_self_signal(linux_signal);
    return kill(process, host_signal) == 0 ? 0 :
        -(int32_t)linux_errno_number(errno);
}

int32_t guest_signal_send_thread(int32_t process, int32_t thread,
    int linux_signal, uint32_t number, const uint32_t *args)
{
    int host_signal;
    if (process != 0 && process != (int32_t)getpid()) return -ESRCH;
    host_signal = linux_signal == 0 ? 0 : linux_signal_to_host(linux_signal);
    if (linux_signal < 0 || linux_signal > LINUX_SIGNAL_COUNT) return -EINVAL;
    if (linux_signal != 0 && host_signal == 0)
        return linux_report_unsupported(number, EINVAL, "signal-unmapped",
            args);
    flush_before_self_signal(linux_signal);
    return guest_thread_kill(thread, host_signal);
}

/*
 * rt_tgsigqueueinfo. Linux hands the caller's siginfo to the handler as
 * written. That is exact here for a signal to the calling thread with a
 * handler installed: it is queued on this thread and delivered on return
 * from the syscall, as on Linux. Default and ignored actions, and other
 * threads, take the tgkill path, so their handler sees host-made siginfo.
 * Only this process can be targeted, as with tgkill.
 */
int32_t guest_signal_queue_thread(int32_t process, int32_t thread,
    int linux_signal, const void *guest_information, uint32_t number,
    const uint32_t *args)
{
    int32_t words[6];
    siginfo_t information;

    if (process <= 0 || thread <= 0) return -EINVAL;
    if (linux_signal < 0 || linux_signal > LINUX_SIGNAL_COUNT) return -EINVAL;
    if (linux_signal != 0 && linux_signal_to_host(linux_signal) == 0)
        return linux_report_unsupported(number, EINVAL, "signal-unmapped",
            args);
    if (process != (int32_t)getpid()) return -ESRCH;
    if (linux_signal == 0 || thread != guest_thread_tid() ||
        guest_actions[linux_signal].handler <= 1u)
        return guest_signal_send_thread(process, thread, linux_signal, number,
            args);
    memcpy(words, guest_information, sizeof(words));
    memset(&information, 0, sizeof(information));
    information.si_signo = GUEST_NATIVE_SIGINFO;
    information.si_errno = words[1];
    information.si_code = words[2];
    information.si_pid = words[3];
    information.si_uid = (uid_t)words[4];
    information.si_value.sival_int = words[5];
    guest_thread_signal_pending(linux_signal, &information);
    return 0;
}

/*
 * rt_sigqueueinfo (178), the call under musl's sigqueue.  QNX's sigqueue
 * carries the signal and si_value; the receiver sees this process as the
 * sender and SI_QUEUE as the code, whatever si_code the caller wrote (a
 * Linux process may write any negative code to itself; QNX can't send
 * one).  As on Linux, another process may only be sent a negative code
 * other than SI_TKILL (-6): EPERM otherwise.
 */
int32_t guest_signal_queue_process(pid_t process, int linux_signal,
    const void *guest_information, uint32_t number, const uint32_t *args)
{
    int32_t words[6];
    union sigval value;
    int host_signal;
    if (linux_signal < 0 || linux_signal > LINUX_SIGNAL_COUNT) return -EINVAL;
    host_signal = linux_signal == 0 ? 0 : linux_signal_to_host(linux_signal);
    if (linux_signal != 0 && host_signal == 0)
        return linux_report_unsupported(number, EINVAL, "signal-unmapped",
            args);
    if (guest_information == 0) return -EFAULT;
    memcpy(words, guest_information, sizeof(words));
    if (process != getpid() && (words[2] >= 0 || words[2] == -6))
        return -EPERM;
    if (process == getpid()) flush_before_self_signal(linux_signal);
    if (linux_signal == 0)
        return kill(process, 0) == 0 ? 0 :
            -(int32_t)linux_errno_number(errno);
    memset(&value, 0, sizeof(value));
    value.sival_int = words[5];
    return sigqueue(process, host_signal, value) == 0 ? 0 :
        -(int32_t)linux_errno_number(errno);
}

int32_t guest_signal_action(int linux_signal, const void *guest_action,
    void *guest_old_action, size_t signal_set_size)
{
    struct sigaction host_action;
    struct linux_sigaction action;
    int host_signal;
    if (signal_set_size != 8 || linux_signal <= 0 ||
        linux_signal > LINUX_SIGNAL_COUNT) return -EINVAL;
    host_signal = linux_signal_to_host(linux_signal);
    if (guest_old_action != 0)
        memcpy(guest_old_action, &guest_actions[linux_signal], sizeof(action));
    if (guest_action == 0) return 0;
    memcpy(&action, guest_action, sizeof(action));
    if (linux_signal == 9 || linux_signal == 19) return -EINVAL;
    guest_actions[linux_signal] = action;
    /* Signals QNX lacks (16, 48-64): the action is stored only. */
    if (host_signal == 0 || linux_signal == 4 || linux_signal == 11) return 0;
    memset(&host_action, 0, sizeof(host_action));
    if (action.handler == 1) host_action.sa_handler = SIG_IGN;
    else if (action.handler == 0)
        default_host_action(linux_signal, &host_action);
    else {
        host_action.sa_sigaction = host_signal_handler;
        host_action.sa_flags = SA_SIGINFO;
    }
    if ((action.flags & LINUX_SA_NOCLDSTOP) != 0)
        host_action.sa_flags |= SA_NOCLDSTOP;
    if ((action.flags & LINUX_SA_NOCLDWAIT) != 0)
        host_action.sa_flags |= SA_NOCLDWAIT;
    if ((action.flags & LINUX_SA_NODEFER) != 0)
        host_action.sa_flags |= SA_NODEFER;
    if ((action.flags & LINUX_SA_RESETHAND) != 0)
        host_action.sa_flags |= SA_RESETHAND;
    linux_set_to_host(action.mask, &host_action.sa_mask);
    if (sigaction(host_signal, &host_action, 0) != 0)
        return -(int32_t)linux_errno_number(errno);
    return 0;
}

int guest_signal_host_mask(const void *guest_set, size_t signal_set_size,
    sigset_t *host_set)
{
    uint32_t words[2];
    if (guest_set == 0 || signal_set_size != sizeof(words)) return -EINVAL;
    memcpy(words, guest_set, sizeof(words));
    linux_set_to_host(words, host_set);
    return 0;
}

int32_t guest_signal_mask(int how, const void *guest_set, void *guest_old_set,
    size_t signal_set_size)
{
    sigset_t host_set, host_old_set;
    const sigset_t *host_set_pointer = 0;
    uint32_t words[2];
    uint32_t old_words[2];
    uint32_t new_words[2];
    int error;
    if (signal_set_size != 8 || how < 0 || how > 2) return -EINVAL;
    guest_thread_signal_mask_get(old_words);
    if (guest_old_set != 0)
        memcpy(guest_old_set, old_words, sizeof(old_words));
    if (guest_set != 0) {
        memcpy(words, guest_set, sizeof(words));
        words[0] &= ~((1u << (9 - 1)) | (1u << (19 - 1)));
        linux_set_to_host(words, &host_set);
        host_set_pointer = &host_set;
    }
    error = pthread_sigmask(how, host_set_pointer, &host_old_set);
    if (error != 0) return -(int32_t)linux_errno_number(error);
    if (guest_set != 0) {
        if (how == SIG_BLOCK) {
            new_words[0] = old_words[0] | words[0];
            new_words[1] = old_words[1] | words[1];
        } else if (how == SIG_UNBLOCK) {
            new_words[0] = old_words[0] & ~words[0];
            new_words[1] = old_words[1] & ~words[1];
        } else {
            new_words[0] = words[0];
            new_words[1] = words[1];
        }
        guest_thread_signal_mask_set(new_words);
    }
    return 0;
}

/*
 * The guest's mask on the host thread.  Syscalls run inside the trap's
 * SIGILL handler, and when a QNX handler returns, SignalReturn restores
 * the thread's mask from the frame QNX built for it, not from the
 * thread's current mask and not from the ucontext (QNX leaves uc_sigmask
 * zero and ignores it; FINDINGS 2026-11-19, signalfd task).  The frame is
 * struct _sighandler_info { siginfo_t siginfo; handler; context; data[] }
 * (sys/neutrino.h), and the saved sigset_t is the start of data[], right
 * after the fixed fields.  So a pthread_sigmask in an emulated
 * rt_sigprocmask was undone at the syscall's return, and a signal the
 * guest blocked stayed unblocked on QNX: a blocked default-action signal
 * ended the process, a handled one was queued by the host handler.
 *
 * guest_signal_frame_probe checks that layout once at start-up: a handler
 * on a spare signal must find a known mask at that offset, and a signal it
 * adds there must be blocked after the handler returns (so the slot is
 * really what SignalReturn restores).  Only then does
 * guest_signal_apply_frame_mask write the guest's mask into a handler's
 * frame: every host signal a Linux signal maps to is blocked exactly when
 * the guest blocks it (SIGILL and SIGSEGV never); the other host signals
 * keep their saved state.  A failed probe leaves the old behaviour and is
 * reported in LINUXEMU_DIAGNOSTICS.
 */
#define FRAME_MASK_OFFSET (sizeof(siginfo_t) + 2 * sizeof(void *))
/* The signal the probe adds through the slot (the first choice; another
   when the saved mask already holds it). */
#define FRAME_PROBE_ADDED SIGUSR2
static int frame_mask_ok;
static volatile int frame_probe_added;
static volatile int frame_probe_matched;
static sigset_t frame_probe_expected;

/* Same membership for every signal a mask can change (QNX keeps its top
   eight, SIGRTMAX + 1 and up, masked whatever is asked). */
static int same_signals(const sigset_t *first, const sigset_t *second)
{
    int host_signal;
    for (host_signal = 1; host_signal <= SIGRTMAX; ++host_signal)
        if (sigismember(first, host_signal) !=
                sigismember(second, host_signal)) return 0;
    return 1;
}

static void frame_probe_handler(int host_signal, siginfo_t *information,
    void *argument)
{
    sigset_t *slot = (sigset_t *)(void *)((char *)information +
        FRAME_MASK_OFFSET);
    sigset_t saved;
    (void)host_signal;
    (void)argument;
    memcpy(&saved, slot, sizeof(saved));
    frame_probe_matched = same_signals(&saved, &frame_probe_expected);
    /* Only write where the layout matched. */
    if (frame_probe_matched) sigaddset(slot, frame_probe_added);
}

void guest_signal_frame_probe(void)
{
    struct sigaction action, old_action;
    sigset_t old_mask, after, wanted;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = frame_probe_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGEMT, &action, &old_action) != 0) return;
    pthread_sigmask(SIG_SETMASK, 0, &old_mask);
    /* A distinctive saved mask: the mask Linuxemu started with plus
       SIGHUP, SIGQUIT, SIGPIPE and SIGTTOU, without the probe's SIGEMT.
       Nothing blocked at start-up is unblocked even briefly: an execve
       hands the old image's blocked set over, and a signal it holds
       pending (re-raised by guest_signal_restore_pending) must stay
       pending, as on Linux, not meet its default action here. */
    frame_probe_expected = old_mask;
    sigaddset(&frame_probe_expected, SIGHUP);
    sigaddset(&frame_probe_expected, SIGQUIT);
    sigaddset(&frame_probe_expected, SIGPIPE);
    sigaddset(&frame_probe_expected, SIGTTOU);
    sigdelset(&frame_probe_expected, SIGEMT);
    /* The written signal must be one the saved mask doesn't hold, or the
       probe proves nothing. */
    frame_probe_added = 0;
    {
        static const int candidates[] = { FRAME_PROBE_ADDED, SIGUSR1,
            SIGALRM, SIGWINCH, SIGURG, SIGPROF };
        size_t index;
        for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]);
                ++index)
            if (sigismember(&frame_probe_expected, candidates[index]) != 1) {
                frame_probe_added = candidates[index];
                break;
            }
    }
    frame_probe_matched = 0;
    if (frame_probe_added != 0) {
        pthread_sigmask(SIG_SETMASK, &frame_probe_expected, 0);
        pthread_kill(pthread_self(), SIGEMT);
        pthread_sigmask(SIG_SETMASK, 0, &after);
        pthread_sigmask(SIG_SETMASK, &old_mask, 0);
    }
    sigaction(SIGEMT, &old_action, 0);
    /* After return: the saved mask plus the signal written into the
       slot, proving the write is what SignalReturn restores. */
    wanted = frame_probe_expected;
    if (frame_probe_added != 0) sigaddset(&wanted, frame_probe_added);
    frame_mask_ok = frame_probe_added != 0 && frame_probe_matched &&
        same_signals(&after, &wanted);
    if (!frame_mask_ok) {
        uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
        args[0] = (uint32_t)frame_probe_matched;
        args[1] = (uint32_t)FRAME_MASK_OFFSET;
        /* Reported against rt_sigprocmask (175): guest masks then stay
           emulated only, as before. */
        (void)linux_report_unsupported(175, ENOSYS, "signal-frame-mask",
            args);
    }
}

void guest_signal_apply_frame_mask(siginfo_t *trap_information)
{
    sigset_t *saved;
    uint32_t words[2];
    sigset_t guest_set;
    int linux_signal;
    if (!frame_mask_ok || trap_information == 0) return;
    saved = (sigset_t *)(void *)((char *)trap_information + FRAME_MASK_OFFSET);
    guest_thread_signal_mask_get(words);
    linux_set_to_host(words, &guest_set);
    for (linux_signal = 1; linux_signal <= LINUX_SIGNAL_COUNT; ++linux_signal) {
        int host_signal = linux_signal_to_host(linux_signal);
        if (host_signal == 0 || host_signal == SIGILL ||
                host_signal == SIGSEGV) continue;
        if (sigismember(&guest_set, host_signal) == 1)
            sigaddset(saved, host_signal);
        else sigdelset(saved, host_signal);
    }
}

/*
 * Pending signals across execve.  Linux keeps the pending set (the
 * calling thread's and the process's) across exec; QNX drops it (a
 * native probe: a blocked SIGUSR1, pending before execve, was gone in the
 * new image whether sent with kill or pthread_kill; FINDINGS 2026-11-20).
 * So the exec carries the Linux numbers of every signal pending for the
 * calling thread in LINUXEMU_GUEST_PENDING as two sets, and the new image
 * raises each one again at start-up, before the frame probe (which never
 * unblocks a signal blocked at start-up), while the inherited blocked set
 * still holds it:
 *   - process-directed (kill(getpid())): the host's own pending set and
 *     the process queue (the collector's).  Process-directed so the new
 *     image's collector, and so its signalfds, can take them.  QNX's
 *     sigpending doesn't tell a signal pending for the thread from one
 *     pending for the process, so a host thread-directed one also comes
 *     back process-directed; the new image has one thread then, which
 *     still gets it (a thread made later could get it instead: a
 *     documented difference).
 *   - thread-directed (pthread_kill to the main thread): the calling
 *     thread's own queue, minus the above; on Linux they stay pending for
 *     that thread, which is the new image's main thread.
 * One instance each: sender details, sigqueue values and real-time counts
 * are lost (documented).  Format "P0,P1,T0,T1" (hex Linux set words).
 */
#define PENDING_VARIABLE "LINUXEMU_GUEST_PENDING="

/* Never SIGILL, SIGKILL, SIGSEGV or SIGSTOP. */
#define NOT_CARRIED_WORD0 ((1u << 3) | (1u << 8) | (1u << 10) | (1u << 18))

int guest_signal_exec_pending(char *buffer, size_t size)
{
    sigset_t host;
    uint32_t process[2] = { 0, 0 }, thread[2], queued[2];
    int host_signal;
    if (sigpending(&host) == 0)
        for (host_signal = 1; host_signal <= SIGRTMAX; ++host_signal) {
            int linux_signal;
            if (sigismember(&host, host_signal) != 1) continue;
            linux_signal = guest_signal_from_host(host_signal);
            if (linux_signal > 0 && linux_signal <= LINUX_SIGNAL_COUNT)
                process[(linux_signal - 1) / 32] |=
                    1u << ((linux_signal - 1) % 32);
        }
    guest_process_pending_mask(queued, 1);
    process[0] |= queued[0];
    process[1] |= queued[1];
    guest_thread_pending_mask(thread);
    thread[0] &= ~process[0];
    thread[1] &= ~process[1];
    process[0] &= ~NOT_CARRIED_WORD0;
    thread[0] &= ~NOT_CARRIED_WORD0;
    if ((process[0] | process[1] | thread[0] | thread[1]) == 0) return 0;
    snprintf(buffer, size, PENDING_VARIABLE "%08x,%08x,%08x,%08x",
        (unsigned)process[0], (unsigned)process[1], (unsigned)thread[0],
        (unsigned)thread[1]);
    return 1;
}

static void raise_set(const uint32_t words[2], int process_directed)
{
    pid_t self = getpid();
    int linux_signal;
    for (linux_signal = 1; linux_signal <= LINUX_SIGNAL_COUNT; ++linux_signal) {
        int host_signal;
        if ((words[(linux_signal - 1) / 32] &
                (1u << ((linux_signal - 1) % 32))) == 0) continue;
        host_signal = linux_signal_to_host(linux_signal);
        if (host_signal == 0) continue;
        if (process_directed) (void)kill(self, host_signal);
        else (void)pthread_kill(pthread_self(), host_signal);
    }
}

void guest_signal_restore_pending(const char *value)
{
    uint32_t words[4];
    const char *cursor = value;
    char *end;
    int index;
    if (value == 0 || value[0] == '\0') return;
    for (index = 0; index < 4; ++index) {
        words[index] = (uint32_t)strtoul(cursor, &end, 16);
        if (end == cursor || *end != (index == 3 ? '\0' : ',')) return;
        cursor = end + 1;
    }
    words[0] &= ~NOT_CARRIED_WORD0;
    words[2] &= ~NOT_CARRIED_WORD0;
    raise_set(words, 1);
    raise_set(words + 2, 0);
}

/*
 * rt_sigsuspend on the signal-wait channel, like ppoll's temporary mask:
 * the host thread takes the temporary mask, the guest view too, and the
 * thread sleeps in poll on its channel.  Every way a signal can reach it
 * posts there: a host handler queueing a direct signal, the poke for one
 * the collector routed or queued, and guest_thread_signal_wait_arm for one
 * already in a queue (the process queue included) that the temporary mask
 * lets in.  It returns EINTR only once a signal is there to deliver (the
 * trap's return delivers it with the old mask in the frame, as Linux
 * does); a wake for nothing (a poke whose signal another thread took)
 * sleeps again.  A signal whose default action ends the process ends it
 * in the kernel, as before.
 */
int32_t guest_signal_suspend(const void *guest_set, size_t signal_set_size)
{
    sigset_t host_set, host_old;
    uint32_t words[2];
    uint32_t old_words[2];
    int wait_fd, saved_errno;
    if (guest_set == 0 || signal_set_size != 8) return -EINVAL;
    memcpy(words, guest_set, sizeof(words));
    words[0] &= ~((1u << (9 - 1)) | (1u << (19 - 1)));
    linux_set_to_host(words, &host_set);
    guest_thread_signal_mask_get(old_words);
    wait_fd = guest_thread_signal_wait_begin();
    if (wait_fd < 0) return -(int32_t)linux_errno_number(errno);
    if (pthread_sigmask(SIG_SETMASK, &host_set, &host_old) != 0) {
        saved_errno = errno;
        guest_thread_signal_wait_end();
        return -(int32_t)linux_errno_number(saved_errno);
    }
    guest_thread_signal_mask_set(words);
    for (;;) {
        struct pollfd entry;
        guest_thread_signal_wait_arm();
        entry.fd = wait_fd;
        entry.events = POLLIN;
        entry.revents = 0;
        if (poll(&entry, 1, -1) < 0 && errno != EINTR) {
            saved_errno = errno;
            guest_thread_signal_mask_set(old_words);
            pthread_sigmask(SIG_SETMASK, &host_old, 0);
            guest_thread_signal_wait_end();
            return -(int32_t)linux_errno_number(saved_errno);
        }
        if (guest_thread_signal_ready()) break;
        /* Nothing to deliver: drain and wait again. */
        (void)guest_thread_signal_wait_begin();
    }
    guest_thread_signal_suspend_restore(old_words);
    pthread_sigmask(SIG_SETMASK, &host_old, 0);
    guest_thread_signal_wait_end();
    return -EINTR;
}
