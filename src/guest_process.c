#include "linuxemu.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_EXEC_VECTOR 4096

static char emulator_path[PATH_MAX];
static char *host_arguments[MAX_EXEC_VECTOR + 6];
static char *host_environment[MAX_EXEC_VECTOR + 2];
static char shebang_interpreter[PATH_MAX];
static char shebang_option[256];
static char shebang_host_path[PATH_MAX];
extern char **environ;

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

int guest_process_retry_load(char *const arguments[])
{
    const char *value = getenv("LINUXEMU_LOAD_RETRY");
    char retry_value[16];
    char *end = 0;
    unsigned long attempt = value == 0 ? 0 : strtoul(value, &end, 10);
    int saved_errno;
    if ((value != 0 && (end == value || *end != '\0')) || attempt >= 31) {
        errno = EEXIST;
        return -1;
    }
    snprintf(retry_value, sizeof(retry_value), "%lu", attempt + 1);
    if (setenv("LINUXEMU_LOAD_RETRY", retry_value, 1) != 0) return -1;
    execve(emulator_path, arguments, environ);
    saved_errno = errno;
    unsetenv("LINUXEMU_LOAD_RETRY");
    errno = saved_errno;
    return -1;
}

static int read_shebang(const char *host_executable)
{
    char buffer[256];
    char *cursor;
    char *end;
    char *option;
    ssize_t length;
    int descriptor = open(host_executable, O_RDONLY);

    if (descriptor < 0) return -1;
    length = read(descriptor, buffer, sizeof(buffer) - 1);
    close(descriptor);
    if (length < 0) return -1;
    if (length < 2 || buffer[0] != '#' || buffer[1] != '!') return 0;
    buffer[length] = '\0';
    cursor = buffer + 2;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    end = cursor;
    while (*end != '\0' && *end != '\n' && *end != ' ' && *end != '\t')
        end++;
    if (end == cursor) { errno = ENOEXEC; return -1; }
    if ((size_t)(end - cursor) >= sizeof(shebang_interpreter)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(shebang_interpreter, cursor, (size_t)(end - cursor));
    shebang_interpreter[end - cursor] = '\0';
    option = end;
    while (*option == ' ' || *option == '\t') option++;
    end = option;
    while (*end != '\0' && *end != '\n') end++;
    while (end > option && (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r')) end--;
    if ((size_t)(end - option) >= sizeof(shebang_option)) {
        errno = E2BIG;
        return -1;
    }
    memcpy(shebang_option, option, (size_t)(end - option));
    shebang_option[end - option] = '\0';
    if (guest_path_resolve(shebang_interpreter, 0, shebang_host_path,
            sizeof(shebang_host_path)) != 0) return -1;
    return 1;
}

int guest_process_exec(const char *host_executable, const char *guest_executable,
    char *const guest_argv[], char *const guest_envp[])
{
    const char *root = guest_path_root();
    const char *cwd = guest_path_cwd();
    size_t argument_count = 0;
    size_t environment_count = 0;
    size_t i;
    size_t host_index;
    int shebang;
    sigset_t trap_signals;
    sigset_t old_mask;

    if (host_executable == 0 || guest_executable == 0 || guest_argv == 0 ||
        guest_argv[0] == 0 || guest_envp == 0 || root[0] == '\0') {
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
    shebang = read_shebang(host_executable);
    if (shebang < 0) return -1;
    if (shebang != 0 && argument_count +
            (shebang_option[0] != '\0' ? 2 : 1) > MAX_EXEC_VECTOR) {
        errno = E2BIG;
        return -1;
    }
    for (i = 0; guest_envp[i] != 0; ++i) {
        if (hidden_environment(guest_envp[i])) continue;
        if (environment_count == MAX_EXEC_VECTOR) {
            errno = E2BIG;
            return -1;
        }
        host_environment[environment_count++] = guest_envp[i];
    }
    if (runtime_trace_enabled())
        host_environment[environment_count++] = "LINUXEMU_SYSTRACE=1";
    host_environment[environment_count] = 0;

    host_arguments[0] = emulator_path;
    host_arguments[1] = "--linuxemu-exec";
    host_arguments[2] = (char *)root;
    host_arguments[3] = (char *)cwd;
    host_arguments[4] = shebang != 0 ? shebang_host_path :
        (char *)host_executable;
    host_index = 5;
    if (shebang != 0) {
        host_arguments[host_index++] = shebang_interpreter;
        if (shebang_option[0] != '\0')
            host_arguments[host_index++] = shebang_option;
        host_arguments[host_index++] = (char *)guest_executable;
        for (i = 1; i < argument_count; ++i)
            host_arguments[host_index++] = guest_argv[i];
    } else {
        for (i = 0; i < argument_count; ++i)
            host_arguments[host_index++] = guest_argv[i];
    }
    host_arguments[host_index] = 0;
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
