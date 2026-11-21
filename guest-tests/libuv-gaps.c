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
 *   13 connect() of an AF_UNIX stream socket, blocking and nonblocking, to
 *      a regular file fails ECONNREFUSED, and to a missing path ENOENT
 *      (libuv pipe_connect_to_file)
 *   14 getsockname() of an AF_UNIX socket bound to a relative name, from
 *      /tmp, returns that name as given (length 2 + strlen + 1), and so
 *      does a client's getpeername() after connecting by the same name;
 *      a name bound as "/tmp/./NAME" also comes back as given (libuv
 *      pipe_getsockname)
 *   15 fchmod() of a bound AF_UNIX socket fails EOPNOTSUPP (Linux would
 *      change only the socket inode's mode; QNX refuses it with EPERM and
 *      Linuxemu reports the refusal), and chmod() of the bound path sets
 *      the mode stat() shows (libuv pipe_set_chmod)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXEC_LINK "/tmp/linuxemu-libuv-gaps-link"
#define PLAIN_FILE "/tmp/linuxemu-libuv-gaps-file"
#define SOCKET_NAME "linuxemu-libuv-gaps-sock"
#define DOTTED_NAME "/tmp/./linuxemu-libuv-gaps-sock2"
#define CHMOD_NAME "/tmp/linuxemu-libuv-gaps-sock3"

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

/* connect(path) on a fresh AF_UNIX stream socket: 0 or the errno. */
static int connect_error(const char *path, int flags)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM | flags, 0);
    int result;

    if (fd < 0) return errno;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    result = connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0 ?
        0 : errno;
    close(fd);
    return result;
}

static int check_connect_file(void)
{
    int fd;
    int result;

    unlink(PLAIN_FILE);
    fd = open(PLAIN_FILE, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return fail(13, "create file");
    close(fd);
    result = connect_error(PLAIN_FILE, 0);
    errno = result;
    if (result != ECONNREFUSED) return fail(13, "connect to a file");
    result = connect_error(PLAIN_FILE, SOCK_NONBLOCK);
    errno = result;
    if (result != ECONNREFUSED)
        return fail(13, "nonblocking connect to a file");
    unlink(PLAIN_FILE);
    result = connect_error(PLAIN_FILE, 0);
    errno = result;
    if (result != ENOENT) return fail(13, "connect to a missing path");
    return 0;
}

static void unix_address(struct sockaddr_un *address, const char *path)
{
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    strcpy(address->sun_path, path);
}

/* getsockname or getpeername of fd is exactly the AF_UNIX name path. */
static int has_name(int fd, int peer, const char *path)
{
    struct sockaddr_un address;
    socklen_t length = sizeof(address);
    int result;

    memset(&address, 0, sizeof(address));
    result = peer ? getpeername(fd, (struct sockaddr *)&address, &length) :
        getsockname(fd, (struct sockaddr *)&address, &length);
    if (result != 0) return 0;
    if (length != offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1 ||
        strcmp(address.sun_path, path) != 0) {
        fprintf(stderr, "libuv-gaps: name is \"%s\" (length %u), want "
            "\"%s\"\n", address.sun_path, (unsigned)length, path);
        errno = 0;
        return 0;
    }
    return 1;
}

static int check_bound_names(void)
{
    struct sockaddr_un address;
    int server, client, dotted;

    arm(14, 10);
    if (chdir("/tmp") != 0) return fail(14, "chdir /tmp");
    unlink(SOCKET_NAME);
    unlink(DOTTED_NAME);
    server = socket(AF_UNIX, SOCK_STREAM, 0);
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    dotted = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || client < 0 || dotted < 0) return fail(14, "socket");
    unix_address(&address, SOCKET_NAME);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 1) != 0) return fail(14, "bind relative name");
    if (!has_name(server, 0, SOCKET_NAME))
        return fail(14, "getsockname of a relative name");
    if (connect(client, (struct sockaddr *)&address, sizeof(address)) != 0)
        return fail(14, "connect by relative name");
    if (!has_name(client, 1, SOCKET_NAME))
        return fail(14, "getpeername of a relative name");
    unix_address(&address, DOTTED_NAME);
    if (bind(dotted, (struct sockaddr *)&address, sizeof(address)) != 0)
        return fail(14, "bind dotted name");
    if (!has_name(dotted, 0, DOTTED_NAME))
        return fail(14, "getsockname of a dotted name");
    close(server);
    close(client);
    close(dotted);
    unlink(SOCKET_NAME);
    unlink(DOTTED_NAME);
    alarm(0);
    return 0;
}

static int check_socket_chmod(void)
{
    struct sockaddr_un address;
    struct stat status;
    int fd;

    unlink(CHMOD_NAME);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return fail(15, "socket");
    unix_address(&address, CHMOD_NAME);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        return fail(15, "bind");
    errno = 0;
    if (fchmod(fd, 0444) == 0 || errno != EOPNOTSUPP)
        return fail(15, "fchmod of a socket");
    if (chmod(CHMOD_NAME, 0444) != 0) return fail(15, "chmod 0444");
    if (stat(CHMOD_NAME, &status) != 0 || !S_ISSOCK(status.st_mode) ||
        (status.st_mode & 0777) != 0444) return fail(15, "stat after 0444");
    if (chmod(CHMOD_NAME, 0666) != 0) return fail(15, "chmod 0666");
    if (stat(CHMOD_NAME, &status) != 0 || (status.st_mode & 0777) != 0666)
        return fail(15, "stat after 0666");
    close(fd);
    unlink(CHMOD_NAME);
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
    if ((result = check_connect_file()) != 0) return result;
    if ((result = check_bound_names()) != 0) return result;
    if ((result = check_socket_chmod()) != 0) return result;
    return 0;
}
