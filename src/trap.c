#include "linuxemu.h"

#include <string.h>
#include <unistd.h>

#define ARM_LINUX_SVC_0 0xef000000u
#define ARM_TPIDRURO_MASK 0xffff0fffu
#define ARM_TPIDRURO_READ 0xee1d0f70u
#define LINUX_KUSER_MEMORY_BARRIER 0xffff0fa0u
#define LINUX_KUSER_CMPXCHG 0xffff0fc0u
#define LINUX_KUSER_GET_TLS 0xffff0fe0u
#define LINUX_KUSER_VERSION_ADDRESS 0xffff0ffcu
#define LINUX_KUSER_VERSION 5u
#define ARM_KUSER_VERSION_LOAD 0xe51cc003u

static size_t append_hex32(char *buffer, size_t position, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    for (shift = 28; shift >= 0; shift -= 4)
        buffer[position++] = digits[(value >> shift) & 15u];
    return position;
}

static void fatal_signal(int sig, siginfo_t *info, uintptr_t pc)
{
    char message[96];
    const char prefix[] = "linuxemu: fatal signal=0x";
    const char middle[] = " pc=0x";
    const char suffix[] = " address=0x";
    size_t length = 0;
    size_t i;

    for (i = 0; i < sizeof(prefix) - 1; ++i) message[length++] = prefix[i];
    length = append_hex32(message, length, (uint32_t)sig);
    for (i = 0; i < sizeof(middle) - 1; ++i) message[length++] = middle[i];
    length = append_hex32(message, length, (uint32_t)pc);
    for (i = 0; i < sizeof(suffix) - 1; ++i) message[length++] = suffix[i];
    length = append_hex32(message, length, (uint32_t)(uintptr_t)info->si_addr);
    message[length++] = '\n';
    write(STDERR_FILENO, message, length);
    _exit(125);
}

static void trap_handler(int sig, siginfo_t *info, void *argument)
{
    ucontext_t *context = argument;
    uintptr_t pc = context->uc_mcontext.cpu.gpr[15];
    const uint32_t *original = arm_patch_original(pc);

    if (sig == SIGILL && pc == (uintptr_t)linuxemu_signal_return_trampoline) {
        if (guest_signal_return(context, 0) == 0) return;
        fatal_signal(sig, info, pc);
    }
    if (sig == SIGILL &&
        pc == (uintptr_t)linuxemu_rt_signal_return_trampoline) {
        if (guest_signal_return(context, 1) == 0) return;
        fatal_signal(sig, info, pc);
    }

    if (sig == SIGSEGV) {
        if (pc == LINUX_KUSER_GET_TLS) {
            context->uc_mcontext.cpu.gpr[0] = runtime_guest_tls();
            context->uc_mcontext.cpu.gpr[15] = context->uc_mcontext.cpu.gpr[14];
            return;
        }
        if (pc == LINUX_KUSER_MEMORY_BARRIER) {
            __sync_synchronize();
            context->uc_mcontext.cpu.gpr[15] = context->uc_mcontext.cpu.gpr[14];
            return;
        }
        if (pc == LINUX_KUSER_CMPXCHG) {
            volatile uint32_t *address =
                (volatile uint32_t *)context->uc_mcontext.cpu.gpr[2];
            uint32_t old_value = context->uc_mcontext.cpu.gpr[0];
            context->uc_mcontext.cpu.gpr[0] =
                __sync_val_compare_and_swap(address, old_value,
                    context->uc_mcontext.cpu.gpr[1]) == old_value ? 0 :
                    (uint32_t)-1;
            context->uc_mcontext.cpu.gpr[15] = context->uc_mcontext.cpu.gpr[14];
            return;
        }
        if ((uintptr_t)info->si_addr == LINUX_KUSER_VERSION_ADDRESS &&
            guest_memory_is_executable(pc, 4) &&
            *(const uint32_t *)pc == ARM_KUSER_VERSION_LOAD) {
            context->uc_mcontext.cpu.gpr[12] = LINUX_KUSER_VERSION;
            context->uc_mcontext.cpu.gpr[15] = (uint32_t)(pc + 4u);
            return;
        }
        if (guest_memory_is_executable(pc, 4) &&
            guest_signal_deliver(context, 11, info) == 0) return;
        fatal_signal(sig, info, pc);
    }
    if (sig != SIGILL) fatal_signal(sig, info, pc);
    if (original == 0) {
        if (guest_memory_is_executable(pc, 4) &&
            guest_signal_deliver(context, 4, info) == 0) return;
        fatal_signal(sig, info, pc);
    }
    if ((*original & ARM_TPIDRURO_MASK) == ARM_TPIDRURO_READ) {
        unsigned destination = (*original >> 12) & 15u;
        context->uc_mcontext.cpu.gpr[destination] = runtime_guest_tls();
    } else if (*original == ARM_LINUX_SVC_0) {
        uint32_t syscall_number = context->uc_mcontext.cpu.gpr[7];
        uint32_t original_r0 = context->uc_mcontext.cpu.gpr[0];
        if (context->uc_mcontext.cpu.gpr[7] == 119u) {
            if (guest_signal_return(context, 0) == 0) return;
            fatal_signal(sig, info, pc);
        }
        if (context->uc_mcontext.cpu.gpr[7] == 173u) {
            if (guest_signal_return(context, 1) == 0) return;
            fatal_signal(sig, info, pc);
        }
        linux_syscall_dispatch(context);
        context->uc_mcontext.cpu.gpr[15] = (uint32_t)(pc + 4u);
        guest_signal_deliver_pending(context, syscall_number, original_r0);
        return;
    } else {
        fatal_signal(sig, info, pc);
    }
    context->uc_mcontext.cpu.gpr[15] = (uint32_t)(pc + 4u);
}

int install_guest_traps(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = trap_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, 0) != 0) return -1;
    return sigaction(SIGSEGV, &action, 0);
}
