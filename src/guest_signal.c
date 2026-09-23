#include "linuxemu.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
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

static void fill_siginfo(unsigned char destination[128], int linux_signal,
    const siginfo_t *source)
{
    int32_t *words = (int32_t *)destination;
    memset(destination, 0, 128);
    words[0] = linux_signal;
    if (source == 0) return;
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
    case 283: case 285: case 291: case 292: case 297: case 366:
        return 1;
    default:
        return 0;
    }
}

int guest_signal_deliver_pending(ucontext_t *context, uint32_t syscall_number,
    uint32_t original_r0)
{
    siginfo_t information;
    int has_information;
    int linux_signal = guest_thread_take_pending(&information,
        &has_information);
    if (linux_signal == 0) return 0;
    if (context->uc_mcontext.cpu.gpr[0] == (uint32_t)-EINTR &&
        (guest_actions[linux_signal].flags & LINUX_SA_RESTART) != 0 &&
        restartable_syscall(syscall_number)) {
        context->uc_mcontext.cpu.gpr[0] = original_r0;
        context->uc_mcontext.cpu.gpr[15] -= 4u;
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
        guest_signal_deliver(context, linux_signal, information) == 0) return;
    guest_thread_signal_pending(linux_signal, information);
}

int32_t guest_signal_send(pid_t process, int linux_signal)
{
    int host_signal = linux_signal == 0 ? 0 : linux_signal_to_host(linux_signal);
    if (linux_signal != 0 && host_signal == 0) return -EINVAL;
    return kill(process, host_signal) == 0 ? 0 :
        -(int32_t)linux_errno_number(errno);
}

int32_t guest_signal_send_thread(int32_t process, int32_t thread,
    int linux_signal)
{
    int host_signal;
    if (process != 0 && process != (int32_t)getpid()) return -ESRCH;
    host_signal = linux_signal == 0 ? 0 : linux_signal_to_host(linux_signal);
    if (linux_signal != 0 && host_signal == 0) return -EINVAL;
    return guest_thread_kill(thread, host_signal);
}

int32_t guest_signal_action(int linux_signal, const void *guest_action,
    void *guest_old_action, size_t signal_set_size)
{
    struct sigaction host_action;
    struct linux_sigaction action;
    int host_signal;
    if (signal_set_size != 8 || linux_signal <= 0 ||
        linux_signal > LINUX_SIGNAL_COUNT ||
        (host_signal = linux_signal_to_host(linux_signal)) == 0) return -EINVAL;
    if (guest_old_action != 0)
        memcpy(guest_old_action, &guest_actions[linux_signal], sizeof(action));
    if (guest_action == 0) return 0;
    memcpy(&action, guest_action, sizeof(action));
    if (linux_signal == 9 || linux_signal == 19) return -EINVAL;
    guest_actions[linux_signal] = action;
    if (linux_signal == 4 || linux_signal == 11) return 0;
    memset(&host_action, 0, sizeof(host_action));
    if (action.handler == 1) host_action.sa_handler = SIG_IGN;
    else if (action.handler == 0) host_action.sa_handler = SIG_DFL;
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

int32_t guest_signal_suspend(const void *guest_set, size_t signal_set_size)
{
    sigset_t host_set;
    uint32_t words[2];
    uint32_t old_words[2];
    int saved_errno;
    if (guest_set == 0 || signal_set_size != 8) return -EINVAL;
    memcpy(words, guest_set, sizeof(words));
    words[0] &= ~((1u << (9 - 1)) | (1u << (19 - 1)));
    linux_set_to_host(words, &host_set);
    guest_thread_signal_mask_get(old_words);
    guest_thread_signal_mask_set(words);
    if (sigsuspend(&host_set) != 0) {
        saved_errno = errno;
        if (saved_errno == EINTR)
            guest_thread_signal_suspend_restore(old_words);
        else guest_thread_signal_mask_set(old_words);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    guest_thread_signal_mask_set(old_words);
    return 0;
}
