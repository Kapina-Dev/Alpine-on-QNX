/*
 * Gaps the libuv 1.52.1 event-loop subset found (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a wait that hangs is ended by the SIGALRM watchdog with exit code
 * 100 + the check number):
 *   10 readlink("/proc/self/exe") is this image's guest path (libuv
 *      spawn_*: uv_exepath)
 *   11 readlinkat(AT_FDCWD, "/proc/self/exe") and
 *      readlink("/proc/<getpid()>/exe") give the same path
 *   12 after execve through a relative symlink from another directory,
 *      the new image's /proc/self/exe is the real file's absolute path
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXEC_LINK "/tmp/linuxemu-libuv-gaps-link"

static volatile sig_atomic_t current_check;

static int fail(int check, const char *what)
{
    fprintf(stderr, "libuv-gaps: check %d failed: %s (errno %d %s)\n", check,
        what, errno, strerror(errno));
    return check;
}

static void watchdog(int signal_number)
{
    (void)signal_number;
    _exit(100 + current_check);
}

static void arm(int check, unsigned seconds)
{
    current_check = check;
    alarm(seconds);
}

/* readlink into a terminated buffer; -1 on error. */
static int read_link(int directory, const char *path, char *buffer,
    size_t size)
{
    ssize_t length = directory == -1 ? readlink(path, buffer, size - 1) :
        readlinkat(directory, path, buffer, size - 1);
    if (length < 0) return -1;
    buffer[length] = '\0';
    return 0;
}

static int check_exe(const char *expected)
{
    char target[PATH_MAX];
    char proc_path[64];

    errno = 0;
    if (read_link(-1, "/proc/self/exe", target, sizeof(target)) != 0)
        return fail(10, "readlink /proc/self/exe");
    if (strcmp(target, expected) != 0) {
        fprintf(stderr, "libuv-gaps: /proc/self/exe is \"%s\", want \"%s\"\n",
            target, expected);
        return fail(10, "readlink /proc/self/exe value");
    }
    errno = 0;
    if (read_link(AT_FDCWD, "/proc/self/exe", target, sizeof(target)) != 0 ||
        strcmp(target, expected) != 0)
        return fail(11, "readlinkat AT_FDCWD /proc/self/exe");
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", (int)getpid());
    errno = 0;
    if (read_link(-1, proc_path, target, sizeof(target)) != 0 ||
        strcmp(target, expected) != 0)
        return fail(11, "readlink /proc/<pid>/exe");
    return 0;
}

static int check_exec(const char *self)
{
    pid_t child;
    int status;

    unlink(EXEC_LINK);
    if (symlink(self, EXEC_LINK) != 0) return fail(12, "symlink");
    arm(12, 30);
    child = fork();
    if (child < 0) return fail(12, "fork");
    if (child == 0) {
        /* From "/", "tmp/<link>" names the file only through the link. */
        if (chdir("/") != 0) _exit(fail(12, "chdir /"));
        execl(EXEC_LINK + 1, EXEC_LINK + 1, "exec-12", self, (char *)0);
        _exit(fail(12, "execl"));
    }
    if (waitpid(child, &status, 0) != child) return fail(12, "waitpid");
    alarm(0);
    unlink(EXEC_LINK);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return WIFEXITED(status) ? WEXITSTATUS(status) : fail(12, "signal");
    return 0;
}

int main(int argc, char **argv)
{
    char self[PATH_MAX];
    int result;

    signal(SIGALRM, watchdog);
    if (argc == 3 && strcmp(argv[1], "exec-12") == 0) {
        result = check_exe(argv[2]);
        return result == 0 ? 0 : 12;
    }
    if (realpath(argv[0], self) == 0) return fail(10, "realpath argv[0]");
    if ((result = check_exe(self)) != 0) return result;
    if ((result = check_exec(self)) != 0) return result;
    return 0;
}
