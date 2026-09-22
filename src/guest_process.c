#include "linuxemu.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_EXEC_VECTOR 4096

static char emulator_path[PATH_MAX];
static char *host_arguments[MAX_EXEC_VECTOR + 6];
static char *host_environment[MAX_EXEC_VECTOR + 1];

static int hidden_environment(const char *value)
{
    return strncmp(value, "LINUXEMU_ROOT=", 14) == 0 ||
        strncmp(value, "LINUXEMU_GUEST_", 15) == 0 ||
        strncmp(value, "LD_LIBRARY_PATH=", 16) == 0;
}

int guest_process_initialize(const char *emulator)
{
    if (emulator == 0 || realpath(emulator, emulator_path) == 0) return -1;
    return 0;
}

int guest_process_exec(const char *host_executable, char *const guest_argv[],
    char *const guest_envp[])
{
    const char *root = guest_path_root();
    const char *cwd = guest_path_cwd();
    size_t argument_count = 0;
    size_t environment_count = 0;
    size_t i;
    sigset_t trap_signals;
    sigset_t old_mask;

    if (host_executable == 0 || guest_argv == 0 || guest_argv[0] == 0 ||
        guest_envp == 0 || root[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    while (guest_argv[argument_count] != 0) {
        if (argument_count == MAX_EXEC_VECTOR) {
            errno = E2BIG;
            return -1;
        }
        argument_count++;
    }
    for (i = 0; guest_envp[i] != 0; ++i) {
        if (hidden_environment(guest_envp[i])) continue;
        if (environment_count == MAX_EXEC_VECTOR) {
            errno = E2BIG;
            return -1;
        }
        host_environment[environment_count++] = guest_envp[i];
    }
    host_environment[environment_count] = 0;

    host_arguments[0] = emulator_path;
    host_arguments[1] = "--linuxemu-exec";
    host_arguments[2] = (char *)root;
    host_arguments[3] = (char *)cwd;
    host_arguments[4] = (char *)host_executable;
    for (i = 0; i < argument_count; ++i)
        host_arguments[5 + i] = guest_argv[i];
    host_arguments[5 + argument_count] = 0;
    sigemptyset(&trap_signals);
    sigaddset(&trap_signals, SIGILL);
    sigaddset(&trap_signals, SIGSEGV);
    if (sigprocmask(SIG_UNBLOCK, &trap_signals, &old_mask) != 0) return -1;
    execve(emulator_path, host_arguments, host_environment);
    {
        int saved_errno = errno;
        sigprocmask(SIG_SETMASK, &old_mask, 0);
        errno = saved_errno;
    }
    return -1;
}
