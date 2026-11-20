/*
 * The Linux inotify: init flags, the event kinds, watch errors, read
 * sizes, FIONREAD, guest paths (relative and through a guest symlink) and
 * bad pointers (guest fixture, musl).
 *
 * Linuxemu backs inotify with QNX's native one, which batches events
 * (about 250 ms late), so every expected event is waited for with poll for
 * up to 2 s.  Background Passport processes open files after changes, so
 * each watch asks only for the bits under test, and OPEN, ACCESS and
 * CLOSE_NOWRITE records are skipped as noise anyway.  Fixture paths are
 * under the guest's /tmp.
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed (inotify_init, the fixture directory)
 *   11 inotify_init1(IN_NONBLOCK): read on an empty queue fails EAGAIN
 *   12 inotify_init1(IN_CLOEXEC) sets FD_CLOEXEC
 *   13 inotify_init1(0x1) fails EINVAL
 *   14 a directory watch (CREATE|MODIFY|CLOSE_WRITE|ATTRIB|DELETE): create,
 *      write, close of f give CREATE f, MODIFY f, CLOSE_WRITE f
 *   15 chmod f gives ATTRIB f; unlink f gives DELETE f
 *   16 rename a b (IN_MOVE) gives MOVED_FROM a, MOVED_TO b, one nonzero
 *      cookie
 *   17 rmdir of the watched directory gives DELETE_SELF then IGNORED
 *      (same wd)
 *   18 inotify_rm_watch gives IGNORED; a second rm_watch fails EINVAL
 *   19 mask 0, IN_MASK_ADD|IN_MASK_CREATE|IN_CREATE and
 *      IN_MASK_CREATE|IN_CREATE fail EINVAL
 *   20 a missing path fails ENOENT; IN_ONLYDIR on a file fails ENOTDIR
 *   21 add_watch on a pipe fd fails EINVAL, on closed fd 999 EBADF
 *   22 a 16-byte read with a named event queued fails EINVAL
 *   23 FIONREAD with one CREATE x queued is 32
 *   24 after chdir(dir), a watch on "." reports CREATE r
 *   25 through the guest symlink /tmp/inb-link -> /tmp/inb-dir a watch
 *      reports CREATE in the target; with IN_DONT_FOLLOW|IN_ATTRIB (the
 *      link itself) a chmod of the target reports nothing in 1 s (a watch
 *      that followed the link would report ATTRIB)
 *   26 add_watch with path (char *)1 fails EFAULT; a read into (void *)1
 *      with an event queued fails EFAULT
 *   27 20 files created: reads with a sizeof(struct inotify_event) +
 *      NAME_MAX + 1 buffer return whole records and consume exactly 20
 *      CREATE records; one more event read with readv (16 + 256 bytes)
 *      gives that record
 *   28 a watch on /dev/null (a native device: QNX says ENOTTY) fails
 *      EOPNOTSUPP; "skip 28" when the guest can't reach it
 *   29 a watch on a guest /dev/pts/N (the fixture's own terminal, else a
 *      fresh pty from /dev/ptmx) fails EOPNOTSUPP promptly: device nodes
 *      never reach QNX's call, which blocks on some ptys; "skip 29" when
 *      no pty is reachable
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define DIR_PATH "/tmp/inb-dir"
#define LINK_PATH "/tmp/inb-link"
#define MASK_CREATE_FLAG 0x10000000u
#define NOISE (IN_OPEN | IN_ACCESS | IN_CLOSE_NOWRITE)
#define EVENT_WAIT_MS 2000

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */

static int fail(int check, const char *what)
{
    fprintf(stderr, "inotify-basic check %d failed: %s (errno %d %s)\n",
        check, what, errno, strerror(errno));
    return check;
}

static void sleep_ms(int milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

/* Each check arms the watchdog for itself: BerryCore timeout never
   fires on the device. */
static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 150) {
            fprintf(stderr, "inotify-basic check %d timed out\n",
                current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

/* Starts the check's 15 s bound. */
static void arm(int check)
{
    static int started;
    current_check = check;
    watchdog_ticks = 0;
    __sync_synchronize();
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread, 0, watchdog, 0) == 0) started = 1;
    }
}

/* Buffered records of one inotify fd. */
struct reader {
    int fd;
    int position, length;
    char buffer[4096] __attribute__((aligned(4)));
};

struct record {
    int32_t wd;
    uint32_t mask, cookie, len;
    char name[NAME_MAX + 1];
};

static void reader_init(struct reader *reader, int fd)
{
    reader->fd = fd;
    reader->position = reader->length = 0;
}

/* The next record within timeout_ms (any kind): 1, 0 on timeout, -1 on
   error. */
static int next_record(struct reader *reader, int timeout_ms,
    struct record *out)
{
    const struct inotify_event *event;
    if (reader->position >= reader->length) {
        struct pollfd entry;
        ssize_t got;
        entry.fd = reader->fd;
        entry.events = POLLIN;
        entry.revents = 0;
        if (poll(&entry, 1, timeout_ms) <= 0) return 0;
        got = read(reader->fd, reader->buffer, sizeof(reader->buffer));
        if (got < 0 && errno == EAGAIN) return 0;
        if (got <= 0) return -1;
        reader->position = 0;
        reader->length = (int)got;
    }
    event = (const struct inotify_event *)(reader->buffer + reader->position);
    out->wd = event->wd;
    out->mask = event->mask;
    out->cookie = event->cookie;
    out->len = event->len;
    out->name[0] = '\0';
    if (event->len != 0) {
        size_t length = strnlen(event->name, event->len);
        memcpy(out->name, event->name, length);
        out->name[length] = '\0';
    }
    reader->position += (int)(sizeof(*event) + event->len);
    return 1;
}

/* The next record that isn't noise, within EVENT_WAIT_MS. */
static int next_event(struct reader *reader, struct record *out)
{
    int result;
    do result = next_record(reader, EVENT_WAIT_MS, out);
    while (result == 1 && (out->mask & ~NOISE) == 0);
    return result;
}

/* The next non-noise event is mask (exact, ignoring noise bits and
   IN_ISDIR, which Linux 5.9+ and QNX add to a directory's DELETE_SELF) on
   name (0: no name) for wd. */
static int expect(int check, struct reader *reader, int wd, uint32_t mask,
    const char *name, struct record *out)
{
    struct record local;
    if (out == 0) out = &local;
    if (next_event(reader, out) != 1) {
        fprintf(stderr, "check %d: no event 0x%x '%s' within %d ms\n",
            check, mask, name ? name : "", EVENT_WAIT_MS);
        return fail(check, "event missing");
    }
    if (out->wd != wd || (out->mask & ~(NOISE | IN_ISDIR)) != mask ||
            strcmp(out->name, name ? name : "") != 0) {
        fprintf(stderr, "check %d: got wd %d mask 0x%x '%s', want wd %d "
            "mask 0x%x '%s'\n", check, out->wd, out->mask, out->name, wd,
            mask, name ? name : "");
        errno = 0;
        return fail(check, "unexpected event");
    }
    return 0;
}

/* Nothing but noise within milliseconds. */
static int expect_quiet(int check, struct reader *reader, int milliseconds)
{
    struct record record;
    int result;
    do result = next_record(reader, milliseconds, &record);
    while (result == 1 && (record.mask & ~NOISE) == 0);
    if (result == 0) return 0;
    if (result == 1)
        fprintf(stderr, "check %d: unexpected wd %d mask 0x%x '%s'\n",
            check, record.wd, record.mask, record.name);
    return fail(check, "expected no event");
}

static int make_file(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    return close(fd);
}

/* Removes the fixture's tree (one directory level is enough). */
static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    if (directory != 0) {
        struct dirent *entry;
        char child[PATH_MAX];
        while ((entry = readdir(directory)) != 0) {
            if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) continue;
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            if (unlink(child) != 0) rmdir(child);
        }
        closedir(directory);
    }
    rmdir(path);
}

static void cleanup(void)
{
    unlink(LINK_PATH);
    remove_tree(DIR_PATH);
}

static int expect_error(int check, int result, int wanted, const char *what)
{
    if (result != -1 || errno != wanted) {
        fprintf(stderr, "check %d: %s returned %d errno %d, want -1/%d\n",
            check, what, result, errno, wanted);
        return fail(check, what);
    }
    return 0;
}

static int check_nonblock(void)
{
    char buffer[512];
    int fd = inotify_init1(IN_NONBLOCK), result;
    ssize_t got;
    if (fd < 0) return fail(11, "inotify_init1(IN_NONBLOCK)");
    got = read(fd, buffer, sizeof(buffer));
    result = expect_error(11, (int)got, EAGAIN, "read on an empty queue");
    close(fd);
    return result;
}

static int check_cloexec(void)
{
    int fd = inotify_init1(IN_CLOEXEC), flags;
    if (fd < 0) return fail(12, "inotify_init1(IN_CLOEXEC)");
    flags = fcntl(fd, F_GETFD);
    close(fd);
    if (flags < 0 || (flags & FD_CLOEXEC) == 0)
        return fail(12, "FD_CLOEXEC not set");
    return 0;
}

static int check_bad_flags(void)
{
    int fd = inotify_init1(0x1);
    if (fd >= 0) close(fd);
    return expect_error(13, fd, EINVAL, "inotify_init1(0x1)");
}

/* Checks 14 and 15 share one directory watch. */
static int check_file_events(void)
{
    struct reader reader;
    int fd = inotify_init1(IN_NONBLOCK), wd, file, result;
    if (fd < 0) return fail(14, "inotify_init1");
    reader_init(&reader, fd);
    wd = inotify_add_watch(fd, DIR_PATH, IN_CREATE | IN_MODIFY |
        IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE);
    if (wd < 0) { result = fail(14, "add_watch"); goto done; }
    file = open(DIR_PATH "/f", O_CREAT | O_WRONLY, 0644);
    if (file < 0) { result = fail(14, "create f"); goto done; }
    if (write(file, "data", 4) != 4) {
        close(file);
        result = fail(14, "write f");
        goto done;
    }
    close(file);
    if ((result = expect(14, &reader, wd, IN_CREATE, "f", 0)) != 0 ||
            (result = expect(14, &reader, wd, IN_MODIFY, "f", 0)) != 0 ||
            (result = expect(14, &reader, wd, IN_CLOSE_WRITE, "f", 0)) != 0)
        goto done;
    arm(15);
    if (chmod(DIR_PATH "/f", 0600) != 0) { result = fail(15, "chmod"); goto done; }
    if ((result = expect(15, &reader, wd, IN_ATTRIB, "f", 0)) != 0) goto done;
    if (unlink(DIR_PATH "/f") != 0) { result = fail(15, "unlink"); goto done; }
    result = expect(15, &reader, wd, IN_DELETE, "f", 0);
done:
    close(fd);
    return result;
}

static int check_rename(void)
{
    struct reader reader;
    struct record from, to;
    int fd = inotify_init1(IN_NONBLOCK), wd, result;
    if (fd < 0) return fail(16, "inotify_init1");
    reader_init(&reader, fd);
    if (make_file(DIR_PATH "/a") != 0) { result = fail(16, "create a"); goto done; }
    wd = inotify_add_watch(fd, DIR_PATH, IN_MOVE);
    if (wd < 0) { result = fail(16, "add_watch"); goto done; }
    if (rename(DIR_PATH "/a", DIR_PATH "/b") != 0) {
        result = fail(16, "rename");
        goto done;
    }
    if ((result = expect(16, &reader, wd, IN_MOVED_FROM, "a", &from)) != 0 ||
            (result = expect(16, &reader, wd, IN_MOVED_TO, "b", &to)) != 0)
        goto done;
    if (from.cookie == 0 || from.cookie != to.cookie) {
        fprintf(stderr, "check 16: cookies %u and %u\n", from.cookie,
            to.cookie);
        errno = 0;
        result = fail(16, "cookie");
    }
    unlink(DIR_PATH "/b");
done:
    close(fd);
    return result;
}

static int check_delete_self(void)
{
    struct reader reader;
    int fd = inotify_init1(IN_NONBLOCK), wd, result;
    if (fd < 0) return fail(17, "inotify_init1");
    reader_init(&reader, fd);
    if (mkdir(DIR_PATH "/sub", 0755) != 0) { result = fail(17, "mkdir"); goto done; }
    wd = inotify_add_watch(fd, DIR_PATH "/sub", IN_DELETE_SELF);
    if (wd < 0) { result = fail(17, "add_watch"); goto done; }
    if (rmdir(DIR_PATH "/sub") != 0) { result = fail(17, "rmdir"); goto done; }
    if ((result = expect(17, &reader, wd, IN_DELETE_SELF, 0, 0)) == 0)
        result = expect(17, &reader, wd, IN_IGNORED, 0, 0);
done:
    close(fd);
    return result;
}

static int check_rm_watch(void)
{
    struct reader reader;
    int fd = inotify_init1(IN_NONBLOCK), wd, result;
    if (fd < 0) return fail(18, "inotify_init1");
    reader_init(&reader, fd);
    wd = inotify_add_watch(fd, DIR_PATH, IN_CREATE);
    if (wd < 0) { result = fail(18, "add_watch"); goto done; }
    if (inotify_rm_watch(fd, wd) != 0) { result = fail(18, "rm_watch"); goto done; }
    if ((result = expect(18, &reader, wd, IN_IGNORED, 0, 0)) != 0) goto done;
    result = expect_error(18, inotify_rm_watch(fd, wd), EINVAL,
        "second rm_watch");
done:
    close(fd);
    return result;
}

static int check_masks(void)
{
    int fd = inotify_init1(IN_NONBLOCK), result;
    if (fd < 0) return fail(19, "inotify_init1");
    if ((result = expect_error(19, inotify_add_watch(fd, DIR_PATH, 0),
            EINVAL, "mask 0")) == 0 &&
        (result = expect_error(19, inotify_add_watch(fd, DIR_PATH,
            IN_MASK_ADD | MASK_CREATE_FLAG | IN_CREATE), EINVAL,
            "IN_MASK_ADD|IN_MASK_CREATE")) == 0)
        result = expect_error(19, inotify_add_watch(fd, DIR_PATH,
            MASK_CREATE_FLAG | IN_CREATE), EINVAL, "IN_MASK_CREATE");
    close(fd);
    return result;
}

static int check_path_errors(void)
{
    int fd = inotify_init1(IN_NONBLOCK), result;
    if (fd < 0) return fail(20, "inotify_init1");
    if (make_file(DIR_PATH "/plain") != 0) {
        close(fd);
        return fail(20, "create plain");
    }
    if ((result = expect_error(20, inotify_add_watch(fd, DIR_PATH "/missing",
            IN_CREATE), ENOENT, "missing path")) == 0)
        result = expect_error(20, inotify_add_watch(fd, DIR_PATH "/plain",
            IN_ONLYDIR | IN_ATTRIB), ENOTDIR, "IN_ONLYDIR on a file");
    unlink(DIR_PATH "/plain");
    close(fd);
    return result;
}

static int check_fd_errors(void)
{
    int pipe_fds[2], result;
    if (pipe(pipe_fds) != 0) return fail(21, "pipe");
    close(999);
    if ((result = expect_error(21, inotify_add_watch(pipe_fds[0], DIR_PATH,
            IN_CREATE), EINVAL, "add_watch on a pipe")) == 0)
        result = expect_error(21, inotify_add_watch(999, DIR_PATH,
            IN_CREATE), EBADF, "add_watch on fd 999");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return result;
}

/* Waits until fd has a record queued (poll, bounded). */
static int wait_queued(int check, int fd)
{
    struct pollfd entry;
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    if (poll(&entry, 1, EVENT_WAIT_MS) != 1)
        return fail(check, "no event queued");
    return 0;
}

static int check_small_read(void)
{
    char buffer[16];
    int fd = inotify_init1(IN_NONBLOCK), result;
    if (fd < 0) return fail(22, "inotify_init1");
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE) < 0) {
        close(fd);
        return fail(22, "add_watch");
    }
    if (make_file(DIR_PATH "/s") != 0 || (result = wait_queued(22, fd)) != 0) {
        close(fd);
        return result ? result : fail(22, "create s");
    }
    result = expect_error(22, (int)read(fd, buffer, sizeof(buffer)), EINVAL,
        "16-byte read");
    unlink(DIR_PATH "/s");
    close(fd);
    return result;
}

static int check_fionread(void)
{
    int fd = inotify_init1(IN_NONBLOCK), result, queued = -1;
    if (fd < 0) return fail(23, "inotify_init1");
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE) < 0) {
        close(fd);
        return fail(23, "add_watch");
    }
    if (make_file(DIR_PATH "/x") != 0 || (result = wait_queued(23, fd)) != 0) {
        close(fd);
        return result ? result : fail(23, "create x");
    }
    result = 0;
    if (ioctl(fd, FIONREAD, &queued) != 0) result = fail(23, "FIONREAD");
    else if (queued != 32) {
        fprintf(stderr, "check 23: FIONREAD %d, want 32\n", queued);
        errno = 0;
        result = fail(23, "FIONREAD value");
    }
    unlink(DIR_PATH "/x");
    close(fd);
    return result;
}

static int check_relative(void)
{
    struct reader reader;
    int fd = inotify_init1(IN_NONBLOCK), wd, result;
    if (fd < 0) return fail(24, "inotify_init1");
    reader_init(&reader, fd);
    if (chdir(DIR_PATH) != 0) { close(fd); return fail(24, "chdir"); }
    wd = inotify_add_watch(fd, ".", IN_CREATE);
    if (wd < 0) result = fail(24, "add_watch(\".\")");
    else if (make_file("r") != 0) result = fail(24, "create r");
    else result = expect(24, &reader, wd, IN_CREATE, "r", 0);
    unlink("r");
    if (chdir("/") != 0 && result == 0) result = fail(24, "chdir /");
    close(fd);
    return result;
}

static int check_symlink(void)
{
    struct reader reader;
    int fd = inotify_init1(IN_NONBLOCK), wd, result;
    if (fd < 0) return fail(25, "inotify_init1");
    reader_init(&reader, fd);
    unlink(LINK_PATH);
    if (symlink(DIR_PATH, LINK_PATH) != 0) {
        close(fd);
        return fail(25, "symlink");
    }
    wd = inotify_add_watch(fd, LINK_PATH, IN_CREATE);
    if (wd < 0) result = fail(25, "add_watch through the link");
    else if (make_file(DIR_PATH "/t") != 0) result = fail(25, "create t");
    else result = expect(25, &reader, wd, IN_CREATE, "t", 0);
    close(fd);
    unlink(DIR_PATH "/t");
    if (result != 0) return result;
    fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) return fail(25, "inotify_init1");
    reader_init(&reader, fd);
    wd = inotify_add_watch(fd, LINK_PATH, IN_DONT_FOLLOW | IN_ATTRIB);
    if (wd < 0) result = fail(25, "add_watch IN_DONT_FOLLOW");
    else if (chmod(DIR_PATH, 0700) != 0) result = fail(25, "chmod target");
    else result = expect_quiet(25, &reader, 1000);
    if (chmod(DIR_PATH, 0755) != 0 && result == 0)
        result = fail(25, "restore target mode");
    close(fd);
    return result;
}

static int check_bad_pointers(void)
{
    int fd = inotify_init1(IN_NONBLOCK), result;
    if (fd < 0) return fail(26, "inotify_init1");
    if ((result = expect_error(26, inotify_add_watch(fd, (const char *)1,
            IN_CREATE), EFAULT, "add_watch with path 1")) != 0) goto done;
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE) < 0) {
        result = fail(26, "add_watch");
        goto done;
    }
    if (make_file(DIR_PATH "/v") != 0) { result = fail(26, "create v"); goto done; }
    if ((result = wait_queued(26, fd)) != 0) goto done;
    result = expect_error(26, (int)read(fd, (void *)1, 4096), EFAULT,
        "read into 1");
done:
    unlink(DIR_PATH "/v");
    close(fd);
    return result;
}

static int check_burst(void)
{
    char buffer[sizeof(struct inotify_event) + NAME_MAX + 1]
        __attribute__((aligned(4)));
    char header[16] __attribute__((aligned(4))), name[256];
    struct iovec vectors[2];
    const struct inotify_event *event;
    struct pollfd entry;
    char path[PATH_MAX];
    int fd = inotify_init1(IN_NONBLOCK), wd, i, creates = 0, result = 0;
    ssize_t got;
    if (fd < 0) return fail(27, "inotify_init1");
    wd = inotify_add_watch(fd, DIR_PATH, IN_CREATE);
    if (wd < 0) { result = fail(27, "add_watch"); goto done; }
    for (i = 0; i != 20; ++i) {
        snprintf(path, sizeof(path), DIR_PATH "/b%02d", i);
        if (make_file(path) != 0) { result = fail(27, "create"); goto done; }
    }
    while (creates < 20) {
        ssize_t position = 0;
        entry.fd = fd;
        entry.events = POLLIN;
        entry.revents = 0;
        if (poll(&entry, 1, EVENT_WAIT_MS) != 1) {
            fprintf(stderr, "check 27: %d CREATE records, want 20\n", creates);
            result = fail(27, "records missing");
            goto done;
        }
        got = read(fd, buffer, sizeof(buffer));
        if (got <= 0) { result = fail(27, "read"); goto done; }
        while (position < got) {
            event = (const struct inotify_event *)(buffer + position);
            if (got - position < (ssize_t)sizeof(*event) ||
                    got - position < (ssize_t)(sizeof(*event) + event->len)) {
                errno = 0;
                result = fail(27, "partial record");
                goto done;
            }
            if (event->mask & IN_CREATE) ++creates;
            position += (ssize_t)(sizeof(*event) + event->len);
        }
    }
    if (creates != 20) {
        fprintf(stderr, "check 27: %d CREATE records, want 20\n", creates);
        errno = 0;
        result = fail(27, "extra records");
        goto done;
    }
    if (make_file(DIR_PATH "/g") != 0) { result = fail(27, "create g"); goto done; }
    if ((result = wait_queued(27, fd)) != 0) goto done;
    vectors[0].iov_base = header;
    vectors[0].iov_len = sizeof(header);
    vectors[1].iov_base = name;
    vectors[1].iov_len = sizeof(name);
    got = readv(fd, vectors, 2);
    event = (const struct inotify_event *)header;
    if (got < (ssize_t)sizeof(header)) {
        fprintf(stderr, "check 27: readv %d\n", (int)got);
        result = fail(27, "readv");
    } else if (got != (ssize_t)(16 + event->len) || event->wd != wd ||
            (event->mask & IN_CREATE) == 0 || event->len == 0 ||
            strcmp(name, "g") != 0) {
        fprintf(stderr, "check 27: readv %d wd %d mask 0x%x len %u\n",
            (int)got, event->wd, event->mask, event->len);
        result = fail(27, "readv record");
    }
done:
    for (i = 0; i != 20; ++i) {
        snprintf(path, sizeof(path), DIR_PATH "/b%02d", i);
        unlink(path);
    }
    unlink(DIR_PATH "/g");
    close(fd);
    return result;
}

static int check_device(void)
{
    int fd = inotify_init1(IN_NONBLOCK), wd, result = 0;
    if (fd < 0) return fail(28, "inotify_init1");
    wd = inotify_add_watch(fd, "/dev/null", IN_ATTRIB);
    if (wd < 0 && errno == ENOENT) printf("skip 28\n");
    else result = expect_error(28, wd, EOPNOTSUPP, "watch on /dev/null");
    close(fd);
    return result;
}

/* The fixture's own terminal, else a fresh pty (master kept in *master). */
static const char *pty_path(int *master)
{
    static char path[64];
    const char *name;
    int fd;
    *master = -1;
    for (fd = 0; fd != 3; ++fd) {
        name = ttyname(fd);
        if (name != 0 && strncmp(name, "/dev/pts/", 9) == 0) return name;
    }
    *master = posix_openpt(O_RDWR | O_NOCTTY);
    if (*master < 0) return 0;
    name = ptsname(*master);
    if (name == 0 || strncmp(name, "/dev/pts/", 9) != 0) return 0;
    snprintf(path, sizeof(path), "%s", name);
    return path;
}

static int check_pty(void)
{
    int fd = inotify_init1(IN_NONBLOCK), master, result = 0;
    const char *path;
    if (fd < 0) return fail(29, "inotify_init1");
    path = pty_path(&master);
    if (path == 0) printf("skip 29: no /dev/pts/N reachable\n");
    else result = expect_error(29, inotify_add_watch(fd, path, IN_ATTRIB),
        EOPNOTSUPP, path);
    if (master >= 0) close(master);
    close(fd);
    return result;
}

static int run(void)
{
    int result, fd;
    arm(10);
    cleanup();
    fd = inotify_init();
    if (fd < 0) return fail(10, "inotify_init");
    close(fd);
    if (mkdir(DIR_PATH, 0755) != 0) return fail(10, "mkdir " DIR_PATH);
    arm(11);
    if ((result = check_nonblock()) != 0) return result;
    arm(12);
    if ((result = check_cloexec()) != 0) return result;
    arm(13);
    if ((result = check_bad_flags()) != 0) return result;
    arm(14);
    if ((result = check_file_events()) != 0) return result;
    arm(16);
    if ((result = check_rename()) != 0) return result;
    arm(17);
    if ((result = check_delete_self()) != 0) return result;
    arm(18);
    if ((result = check_rm_watch()) != 0) return result;
    arm(19);
    if ((result = check_masks()) != 0) return result;
    arm(20);
    if ((result = check_path_errors()) != 0) return result;
    arm(21);
    if ((result = check_fd_errors()) != 0) return result;
    arm(22);
    if ((result = check_small_read()) != 0) return result;
    arm(23);
    if ((result = check_fionread()) != 0) return result;
    arm(24);
    if ((result = check_relative()) != 0) return result;
    arm(25);
    if ((result = check_symlink()) != 0) return result;
    arm(26);
    if ((result = check_bad_pointers()) != 0) return result;
    arm(27);
    if ((result = check_burst()) != 0) return result;
    arm(28);
    if ((result = check_device()) != 0) return result;
    arm(29);
    return check_pty();
}

int main(void)
{
    int result = run();
    cleanup();
    return result;
}
