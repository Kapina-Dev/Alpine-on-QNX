#include "linuxemu.h"
#include "guest_fd.h"

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
static char *host_environment[MAX_EXEC_VECTOR + 12];
/* Fd records carried into the new image (guest_fd_exec_records). */
static char fd_records_variable[32768];
static char personality_variable[48];
static char comm_variable[48];
static char nice_variable[48];
static char pending_variable[80];
/* Close-on-exec fds the new image closes (guest_fd_exec_defer_cloexec). */
static char exec_close_variable[32 + GUEST_EXEC_CLOSE_MAX * 5];
static char diagnostics_variable[PATH_MAX + 32];
static char executable_variable[PATH_MAX + 32];
static char guest_executable_path[PATH_MAX];
static char guest_image_path[PATH_MAX];
static uint32_t guest_personality;
static int guest_no_new_privs;
static int guest_nice;

#define PERSONALITY_VARIABLE "LINUXEMU_GUEST_PERSONALITY="
#define COMM_VARIABLE "LINUXEMU_GUEST_COMM="
#define NO_NEW_PRIVS_VARIABLE "LINUXEMU_GUEST_NO_NEW_PRIVS=1"
#define NICE_VARIABLE "LINUXEMU_GUEST_NICE="
static char shebang_interpreter[PATH_MAX];
static char shebang_option[256];
static char shebang_host_path[PATH_MAX];
extern char **environ;

static int hidden_environment(const char *value)
{
    return strncmp(value, "LINUXEMU_ROOT=", 14) == 0 ||
        strncmp(value, "LINUXEMU_GUEST_", 15) == 0 ||
        strncmp(value, "LD_LIBRARY_PATH=", 16) == 0 ||
        strncmp(value, "LINUXEMU_FD_RECORDS=", 20) == 0;
}

int guest_process_initialize(const char *emulator)
{
    const char *value = getenv("LINUXEMU_GUEST_PERSONALITY");
    if (emulator == 0 || realpath(emulator, emulator_path) == 0) return -1;
    /* Linux keeps the personality across execve. */
    guest_personality = value == 0 ? 0 : (uint32_t)strtoul(value, 0, 0);
    /* PR_SET_NO_NEW_PRIVS and the nice value also survive execve. */
    guest_no_new_privs = getenv("LINUXEMU_GUEST_NO_NEW_PRIVS") != 0;
    value = getenv("LINUXEMU_GUEST_NICE");
    guest_nice = 0;
    if (value != 0 && value[0] != '\0') {
        char *end;
        long parsed = strtol(value, &end, 10);
        if (*end == '\0') {
            if (parsed < -20) parsed = -20;
            if (parsed > 19) parsed = 19;
            guest_nice = (int)parsed;
        }
    }
    return 0;
}

int guest_process_no_new_privs(void)
{
    return guest_no_new_privs;
}

void guest_process_set_no_new_privs(void)
{
    guest_no_new_privs = 1;
}

int guest_process_nice(void)
{
    return guest_nice;
}

void guest_process_set_nice(int nice)
{
    guest_nice = nice;
}

uint32_t guest_process_personality(uint32_t persona)
{
    uint32_t old = guest_personality;
    if (persona != 0xffffffffu) guest_personality = persona;
    return old;
}

/* The guest path of the running image, for diagnostics: the path given to
   execve when this image was exec'd, else the path Linuxemu was started on. */
void guest_process_record_executable(const char *loaded_path)
{
    const char *value = getenv("LINUXEMU_GUEST_EXE");
    snprintf(guest_executable_path, sizeof(guest_executable_path), "%s",
        value != 0 ? value : loaded_path);
    /* Linux's /proc/self/exe names the file actually loaded (the
       interpreter of a #! script), absolute and with symlinks resolved.
       loaded_path is a host path under the root (execve passes the
       resolved one). */
    if (guest_path_from_host(loaded_path, guest_image_path,
            sizeof(guest_image_path)) != 0)
        guest_image_path[0] = '\0';
}

const char *guest_process_executable(void)
{
    return guest_executable_path;
}

const char *guest_process_image(void)
{
    return guest_image_path;
}

/* Linux names a process after the basename of the file passed to execve,
   truncated to 15 characters. */
const char *guest_process_initial_name(const char *loaded_path)
{
    const char *value = getenv("LINUXEMU_GUEST_COMM");
    const char *slash;
    if (value != 0) return value;
    slash = strrchr(loaded_path, '/');
    return slash != 0 ? slash + 1 : loaded_path;
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
    if (guest_personality != 0) {
        snprintf(personality_variable, sizeof(personality_variable),
            PERSONALITY_VARIABLE "0x%08x", (unsigned)guest_personality);
        host_environment[environment_count++] = personality_variable;
    }
    if (guest_no_new_privs)
        host_environment[environment_count++] = (char *)NO_NEW_PRIVS_VARIABLE;
    if (guest_nice != 0) {
        snprintf(nice_variable, sizeof(nice_variable), NICE_VARIABLE "%d",
            guest_nice);
        host_environment[environment_count++] = nice_variable;
    }
    {
        const char *slash = strrchr(guest_executable, '/');
        snprintf(comm_variable, sizeof(comm_variable), COMM_VARIABLE "%.15s",
            slash != 0 ? slash + 1 : guest_executable);
        host_environment[environment_count++] = comm_variable;
    }
    {
        const char *diagnostics = getenv("LINUXEMU_DIAGNOSTICS");
        if (diagnostics != 0 && diagnostics[0] != '\0') {
            snprintf(diagnostics_variable, sizeof(diagnostics_variable),
                "LINUXEMU_DIAGNOSTICS=%s", diagnostics);
            host_environment[environment_count++] = diagnostics_variable;
        }
    }
    {
        /* A host variable, not one the guest sees: carry it over the
           guest environment the new image is given. */
        const char *patch = getenv("LINUXEMU_PATCH_ANONYMOUS");
        if (patch != 0 && strcmp(patch, "1") == 0)
            host_environment[environment_count++] =
                "LINUXEMU_PATCH_ANONYMOUS=1";
    }
    snprintf(executable_variable, sizeof(executable_variable),
        "LINUXEMU_GUEST_EXE=%s", guest_executable);
    host_environment[environment_count++] = executable_variable;
    /* Records that do not fit are dropped: the fds still survive exec,
       they just start native in the new image. */
    if (guest_fd_exec_records(fd_records_variable,
            sizeof(fd_records_variable)) > 0)
        host_environment[environment_count++] = fd_records_variable;
    /* QNX drops pending signals at exec; Linux keeps them. */
    if (guest_signal_exec_pending(pending_variable,
            sizeof(pending_variable)) > 0)
        host_environment[environment_count++] = pending_variable;
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
    linux_diag_flush();
    (void)guest_fd_exec_prepare();
    /* Last, so no failure above leaves the fds without FD_CLOEXEC. */
    if (guest_fd_exec_defer_cloexec(exec_close_variable,
            sizeof(exec_close_variable)) > 0) {
        host_environment[environment_count++] = exec_close_variable;
        host_environment[environment_count] = 0;
    }
    execve(emulator_path, host_arguments, host_environment);
    {
        int saved_errno = errno;
        guest_fd_exec_defer_undo();
        sigprocmask(SIG_SETMASK, &old_mask, 0);
        errno = saved_errno;
    }
    return -1;
}
