#include "linuxemu.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define LINUX_SIGNAL_COUNT 64
#define LINUX_SIGACTION_SIZE 20

static unsigned char guest_actions[LINUX_SIGNAL_COUNT + 1]
    [LINUX_SIGACTION_SIZE];
static volatile sig_atomic_t pending_linux_signal;

static void host_signal_wakeup(int signal_number)
{
    if (signal_number == SIGCHLD) pending_linux_signal = 17;
}

static void trace_action(int signal_number, uint32_t handler, uint32_t flags)
{
    static const char digits[] = "0123456789abcdef";
    char message[64];
    size_t position = 0;
    int shift;
    const char *text = "linuxemu: sigaction signal=";
    if (!runtime_trace_enabled()) return;
    while (*text != '\0') message[position++] = *text++;
    for (shift = 28; shift >= 0; shift -= 4)
        message[position++] = digits[((uint32_t)signal_number >> shift) & 15u];
    text = " handler=";
    while (*text != '\0') message[position++] = *text++;
    for (shift = 28; shift >= 0; shift -= 4)
        message[position++] = digits[(handler >> shift) & 15u];
    text = " flags=";
    while (*text != '\0') message[position++] = *text++;
    for (shift = 28; shift >= 0; shift -= 4)
        message[position++] = digits[(flags >> shift) & 15u];
    message[position++] = '\n';
    write(STDERR_FILENO, message, position);
}

static int linux_signal_to_host(int number)
{
    switch (number) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 8: case 9:
    case 11: case 13: case 14: case 15: return number;
    case 7: return 10;
    case 10: return 16;
    case 12: return 17;
    case 17: return 18;
    case 18: return 25;
    case 19: return 23;
    case 20: return 24;
    case 21: return 26;
    case 22: return 27;
    case 23: return 21;
    case 24: return 30;
    case 25: return 31;
    case 26: return 28;
    case 27: return 29;
    case 28: return 20;
    case 29: return 22;
    case 30: return 19;
    case 31: return 12;
    default: return 0;
    }
}

int32_t guest_signal_send(pid_t process, int linux_signal)
{
    int host_signal;
    if (linux_signal == 0) host_signal = 0;
    else {
        host_signal = linux_signal_to_host(linux_signal);
        if (host_signal == 0) return -EINVAL;
    }
    return kill(process, host_signal) == 0 ? 0 :
        -(int32_t)linux_errno_number(errno);
}

static uint32_t load_u32(const unsigned char *buffer)
{
    uint32_t value;
    memcpy(&value, buffer, sizeof(value));
    return value;
}

int32_t guest_signal_action(int linux_signal, const void *guest_action,
    void *guest_old_action, size_t signal_set_size)
{
    struct sigaction host_action;
    int host_signal;
    const unsigned char *action = guest_action;
    uint32_t handler;
    uint32_t flags;

    if (signal_set_size != 8 || linux_signal <= 0 ||
        linux_signal > LINUX_SIGNAL_COUNT ||
        (host_signal = linux_signal_to_host(linux_signal)) == 0)
        return -EINVAL;
    if (guest_old_action != 0)
        memcpy(guest_old_action, guest_actions[linux_signal],
            LINUX_SIGACTION_SIZE);
    if (action == 0) return 0;
    handler = load_u32(action);
    flags = load_u32(action + 4);
    trace_action(linux_signal, handler, flags);
    if (linux_signal == 9 || linux_signal == 19) return -EINVAL;
    memcpy(guest_actions[linux_signal], action, LINUX_SIGACTION_SIZE);

    if (linux_signal == 4 || linux_signal == 11) return 0;
    memset(&host_action, 0, sizeof(host_action));
    if (handler == 1) host_action.sa_handler = SIG_IGN;
    else if (handler == 0) host_action.sa_handler = SIG_DFL;
    else host_action.sa_handler = host_signal_wakeup;
    if ((flags & 1u) != 0) host_action.sa_flags |= SA_NOCLDSTOP;
    if ((flags & 2u) != 0) host_action.sa_flags |= SA_NOCLDWAIT;
    sigemptyset(&host_action.sa_mask);
    if (sigaction(host_signal, &host_action, 0) != 0)
        return -(int32_t)linux_errno_number(errno);
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

int guest_signal_host_mask(const void *guest_set, size_t signal_set_size,
    sigset_t *host_set)
{
    uint32_t words[2];
    if (guest_set == 0 || signal_set_size != sizeof(words)) return -EINVAL;
    memcpy(words, guest_set, sizeof(words));
    linux_set_to_host(words, host_set);
    return 0;
}

static void host_set_to_linux(const sigset_t *host_set, uint32_t words[2])
{
    int number;
    words[0] = 0;
    words[1] = 0;
    for (number = 1; number <= LINUX_SIGNAL_COUNT; ++number) {
        int host_signal = linux_signal_to_host(number);
        if (host_signal != 0 && sigismember(host_set, host_signal) == 1)
            words[(number - 1) / 32] |= 1u << ((number - 1) % 32);
    }
}

int32_t guest_signal_mask(int how, const void *guest_set, void *guest_old_set,
    size_t signal_set_size)
{
    sigset_t host_set;
    sigset_t host_old_set;
    const sigset_t *host_set_pointer = 0;
    uint32_t words[2];

    if (signal_set_size != 8 || how < 0 || how > 2) return -EINVAL;
    if (guest_set != 0) {
        memcpy(words, guest_set, sizeof(words));
        linux_set_to_host(words, &host_set);
        host_set_pointer = &host_set;
    }
    if (sigprocmask(how, host_set_pointer,
            guest_old_set == 0 ? 0 : &host_old_set) != 0)
        return -(int32_t)linux_errno_number(errno);
    if (guest_old_set != 0) {
        host_set_to_linux(&host_old_set, words);
        memcpy(guest_old_set, words, sizeof(words));
    }
    return 0;
}

int32_t guest_signal_suspend(const void *guest_set, size_t signal_set_size)
{
    sigset_t host_set;
    uint32_t words[2];

    if (guest_set == 0 || signal_set_size != 8) return -EINVAL;
    memcpy(words, guest_set, sizeof(words));
    linux_set_to_host(words, &host_set);
    if (sigsuspend(&host_set) != 0) {
        int saved_errno = errno;
        int linux_signal = pending_linux_signal;
        if (linux_signal > 0 && linux_signal <= LINUX_SIGNAL_COUNT) {
            uint32_t handler = load_u32(guest_actions[linux_signal]);
            pending_linux_signal = 0;
            if (handler > 1u)
                ((void (*)(int))(uintptr_t)handler)(linux_signal);
        }
        return -(int32_t)linux_errno_number(saved_errno);
    }
    return 0;
}
