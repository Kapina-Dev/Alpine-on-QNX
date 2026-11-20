/*
 * Linux inotify modes: one-shot, mask add/replace, queue overflow,
 * poll/select/ppoll/epoll readiness, blocking reads, bursts from several
 * writers and fd number reuse under epoll (guest fixture, musl).
 *
 * Linuxemu backs inotify with QNX's native one, which batches events
 * (about 250 ms late), so waits are bounded by poll timeouts of 2 s.  Each
 * watch asks only for the bits under test.  Every check arms a watchdog
 * that ends the process with 100 + the check number.
 *
 * Checks (exit code = check number, printed on stderr with errno):
 *   10 setup failed
 *   11 IN_ONESHOT|IN_CREATE: two creates give one CREATE, then IGNORED,
 *      and nothing more within 1 s
 *   12 IN_CREATE watch, then IN_DELETE|IN_MASK_ADD on the same path: the
 *      same wd; a create and a delete give both events
 *   13 re-adding without IN_MASK_ADD returns the same wd and replaces the
 *      mask (IN_DELETE only: a create gives nothing in 1 s, a delete gives
 *      DELETE)
 *   14 overflow: 1500 create+unlink pairs with no read, then a nonblocking
 *      drain finds an IN_Q_OVERFLOW record with wd -1 and len 0
 *   15 poll, select and ppoll report the fd readable within 2 s of a
 *      create, and not readable once drained
 *   16 epoll level-triggered reports EPOLLIN until drained; edge-triggered
 *      reports once per burst and again after a read plus a new event
 *   17 a blocking read in thread B is woken by a create from thread A; a
 *      blocking read interrupted by a SIGUSR1 handler without SA_RESTART
 *      fails EINTR
 *   18 4 threads create 25 distinct files each: all 100 names arrive
 *      exactly once within 5 s, no IN_Q_OVERFLOW
 *   19 an inotify fd with a pending event in an epoll set is closed and a
 *      pipe takes its number (dup2): epoll_wait(0) reports nothing for
 *      that number until the pipe is written
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DIR_PATH "/tmp/inmo-dir"
#define EVENT_WAIT_MS 2000

static volatile int current_check;
static volatile int watchdog_ticks;

static int fail(int check, const char *what)
{
    fprintf(stderr, "inotify-modes check %d failed: %s (errno %d %s)\n",
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

static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 300) {
            fprintf(stderr, "inotify-modes check %d timed out\n",
                current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

/* Starts the check's bound: 30 s, or 3 minutes for check 14 (the tmp
   filesystem manages about 30 create+unlink pairs a second). */
static void arm(int check)
{
    static int started;
    current_check = check;
    watchdog_ticks = check == 14 ? -1500 : 0;
    __sync_synchronize();
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread, 0, watchdog, 0) == 0) started = 1;
    }
}

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

/* The next record within timeout_ms (0: no wait): 1, 0 on timeout, -1 on
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
        if (timeout_ms != 0 && poll(&entry, 1, timeout_ms) <= 0) return 0;
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

static int expect(int check, struct reader *reader, int wd, uint32_t mask,
    const char *name)
{
    struct record record;
    if (next_record(reader, EVENT_WAIT_MS, &record) != 1) {
        fprintf(stderr, "check %d: no event 0x%x '%s' within %d ms\n",
            check, mask, name ? name : "", EVENT_WAIT_MS);
        return fail(check, "event missing");
    }
    if (record.wd != wd || record.mask != mask ||
            strcmp(record.name, name ? name : "") != 0) {
        fprintf(stderr, "check %d: got wd %d mask 0x%x '%s', want wd %d "
            "mask 0x%x '%s'\n", check, record.wd, record.mask, record.name,
            wd, mask, name ? name : "");
        errno = 0;
        return fail(check, "unexpected event");
    }
    return 0;
}

static int expect_quiet(int check, struct reader *reader, int milliseconds)
{
    struct record record;
    int result = next_record(reader, milliseconds, &record);
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
    remove_tree(DIR_PATH);
}

/* Fresh inotify fd with one watch on DIR_PATH; -1 after reporting. */
static int open_watch(int check, int flags, uint32_t mask, int *wd)
{
    int fd = inotify_init1(flags);
    if (fd < 0) { fail(check, "inotify_init1"); return -1; }
    *wd = inotify_add_watch(fd, DIR_PATH, mask);
    if (*wd < 0) { fail(check, "inotify_add_watch"); close(fd); return -1; }
    return fd;
}

/* Reads everything queued, nonblocking, returning the record count. */
static int drain(struct reader *reader)
{
    struct record record;
    int count = 0;
    while (next_record(reader, 0, &record) == 1) count++;
    return count;
}

static int check_oneshot(void)
{
    struct reader reader;
    int wd, result, fd = open_watch(11, IN_NONBLOCK,
        IN_ONESHOT | IN_CREATE, &wd);
    if (fd < 0) return 11;
    reader_init(&reader, fd);
    if (make_file(DIR_PATH "/a") != 0) { result = fail(11, "create a"); goto done; }
    if (make_file(DIR_PATH "/b") != 0) { result = fail(11, "create b"); goto done; }
    if ((result = expect(11, &reader, wd, IN_CREATE, "a")) != 0 ||
            (result = expect(11, &reader, wd, IN_IGNORED, 0)) != 0)
        goto done;
    result = expect_quiet(11, &reader, 1000);
done:
    close(fd);
    return result;
}

static int check_mask_add(void)
{
    struct reader reader;
    int wd, again, result, fd = open_watch(12, IN_NONBLOCK, IN_CREATE, &wd);
    if (fd < 0) return 12;
    reader_init(&reader, fd);
    again = inotify_add_watch(fd, DIR_PATH, IN_DELETE | IN_MASK_ADD);
    if (again != wd) {
        fprintf(stderr, "check 12: re-add returned %d, want %d\n", again, wd);
        result = fail(12, "mask add wd");
        goto done;
    }
    if (make_file(DIR_PATH "/m") != 0) { result = fail(12, "create"); goto done; }
    if (unlink(DIR_PATH "/m") != 0) { result = fail(12, "unlink"); goto done; }
    if ((result = expect(12, &reader, wd, IN_CREATE, "m")) != 0) goto done;
    result = expect(12, &reader, wd, IN_DELETE, "m");
done:
    close(fd);
    return result;
}

static int check_mask_replace(void)
{
    struct reader reader;
    int wd, again, result, fd = open_watch(13, IN_NONBLOCK, IN_CREATE, &wd);
    if (fd < 0) return 13;
    reader_init(&reader, fd);
    again = inotify_add_watch(fd, DIR_PATH, IN_DELETE);
    if (again != wd) {
        fprintf(stderr, "check 13: re-add returned %d, want %d\n", again, wd);
        result = fail(13, "replace wd");
        goto done;
    }
    if (make_file(DIR_PATH "/r") != 0) { result = fail(13, "create"); goto done; }
    if ((result = expect_quiet(13, &reader, 1000)) != 0) goto done;
    if (unlink(DIR_PATH "/r") != 0) { result = fail(13, "unlink"); goto done; }
    result = expect(13, &reader, wd, IN_DELETE, "r");
done:
    close(fd);
    return result;
}

static int check_overflow(void)
{
    struct reader reader;
    struct record record;
    int wd, i, found = 0, total = 0, result = 0, fd;
    char path[64];
    fd = open_watch(14, IN_NONBLOCK, IN_CREATE | IN_DELETE, &wd);
    if (fd < 0) return 14;
    reader_init(&reader, fd);
    for (i = 0; i < 1500; i++) {
        snprintf(path, sizeof(path), DIR_PATH "/o%d", i);
        if (make_file(path) != 0 || unlink(path) != 0) {
            result = fail(14, "create/unlink burst");
            goto done;
        }
    }
    sleep_ms(1000);
    for (;;) {
        int got = next_record(&reader, total == 0 ? EVENT_WAIT_MS : 500,
            &record);
        if (got < 0) { result = fail(14, "read"); goto done; }
        if (got == 0) break;
        total++;
        if ((record.mask & IN_Q_OVERFLOW) != 0 && record.wd == -1 &&
                record.len == 0) found = 1;
    }
    if (!found) {
        fprintf(stderr, "check 14: %d records, no IN_Q_OVERFLOW\n", total);
        errno = 0;
        result = fail(14, "no overflow record");
    }
done:
    close(fd);
    return result;
}

static int is_readable(int fd, int timeout_ms, int how)
{
    if (how == 0) {
        struct pollfd entry;
        entry.fd = fd;
        entry.events = POLLIN;
        entry.revents = 0;
        return poll(&entry, 1, timeout_ms) > 0 && (entry.revents & POLLIN);
    } else if (how == 1) {
        fd_set set;
        struct timeval timeout;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        return select(fd + 1, &set, 0, 0, &timeout) > 0 && FD_ISSET(fd, &set);
    } else {
        struct pollfd entry;
        struct timespec timeout;
        entry.fd = fd;
        entry.events = POLLIN;
        entry.revents = 0;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        return ppoll(&entry, 1, &timeout, 0) > 0 && (entry.revents & POLLIN);
    }
}

static int check_readiness(void)
{
    static const char *const names[] = { "poll", "select", "ppoll" };
    struct reader reader;
    int wd, how, result = 0, fd = open_watch(15, IN_NONBLOCK, IN_CREATE, &wd);
    char path[64];
    if (fd < 0) return 15;
    reader_init(&reader, fd);
    for (how = 0; how < 3; how++) {
        snprintf(path, sizeof(path), DIR_PATH "/p%d", how);
        if (is_readable(fd, 0, how)) {
            fprintf(stderr, "check 15: %s readable before any event\n",
                names[how]);
            result = fail(15, "readable while empty");
            break;
        }
        if (make_file(path) != 0) { result = fail(15, "create"); break; }
        if (!is_readable(fd, EVENT_WAIT_MS, how)) {
            fprintf(stderr, "check 15: %s not readable within 2 s\n",
                names[how]);
            result = fail(15, "not readable after create");
            break;
        }
        if (drain(&reader) < 1) { result = fail(15, "drain empty"); break; }
        if (is_readable(fd, 0, how)) {
            fprintf(stderr, "check 15: %s readable after drain\n",
                names[how]);
            result = fail(15, "readable after drain");
            break;
        }
    }
    close(fd);
    return result;
}

static int epoll_count(int epfd, int timeout_ms)
{
    struct epoll_event events[4];
    return epoll_wait(epfd, events, 4, timeout_ms);
}

static int check_epoll(void)
{
    struct reader reader;
    struct epoll_event event;
    int wd, i, epfd, result = 0, fd = open_watch(16, IN_NONBLOCK, IN_CREATE, &wd);
    char path[64];
    if (fd < 0) return 16;
    reader_init(&reader, fd);
    epfd = epoll_create1(0);
    event.events = EPOLLIN;
    event.data.fd = fd;
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) != 0) {
        close(fd);
        return fail(16, "epoll setup");
    }
    /* level-triggered */
    if (epoll_count(epfd, 0) != 0) { result = fail(16, "LT ready while empty"); goto done; }
    if (make_file(DIR_PATH "/l0") != 0) { result = fail(16, "create"); goto done; }
    if (epoll_count(epfd, EVENT_WAIT_MS) != 1) { result = fail(16, "LT not reported"); goto done; }
    if (epoll_count(epfd, 0) != 1) { result = fail(16, "LT not repeated"); goto done; }
    if (drain(&reader) < 1) { result = fail(16, "LT drain"); goto done; }
    if (epoll_count(epfd, 200) != 0) { result = fail(16, "LT after drain"); goto done; }
    /* edge-triggered */
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event) != 0) { result = fail(16, "MOD ET"); goto done; }
    if (epoll_count(epfd, 200) != 0) { result = fail(16, "ET ready while empty"); goto done; }
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), DIR_PATH "/e%d", i);
        if (make_file(path) != 0) { result = fail(16, "create burst"); goto done; }
    }
    if (epoll_count(epfd, EVENT_WAIT_MS) != 1) { result = fail(16, "ET not reported"); goto done; }
    if (epoll_count(epfd, 700) != 0) { result = fail(16, "ET reported twice"); goto done; }
    if (drain(&reader) < 1) { result = fail(16, "ET drain"); goto done; }
    if (make_file(DIR_PATH "/e9") != 0) { result = fail(16, "create after read"); goto done; }
    if (epoll_count(epfd, EVENT_WAIT_MS) != 1) { result = fail(16, "ET not re-armed"); goto done; }
done:
    close(epfd);
    close(fd);
    return result;
}

static volatile int signal_seen;

static void on_usr1(int number)
{
    (void)number;
    signal_seen = 1;
}

struct blocker {
    int fd;
    volatile int started, done;
    volatile ssize_t result;
    volatile int error;
};

static void *blocker_thread(void *argument)
{
    struct blocker *blocker = argument;
    char buffer[512] __attribute__((aligned(4)));
    ssize_t got;
    blocker->started = 1;
    got = read(blocker->fd, buffer, sizeof(buffer));
    blocker->error = errno;
    blocker->result = got;
    __sync_synchronize();
    blocker->done = 1;
    return 0;
}

static int wait_done(struct blocker *blocker, int milliseconds)
{
    int waited;
    for (waited = 0; waited < milliseconds && !blocker->done; waited += 20)
        sleep_ms(20);
    return blocker->done;
}

static int check_blocking(void)
{
    struct blocker blocker;
    struct sigaction action;
    pthread_t thread;
    int wd, result = 0, fd = open_watch(17, 0, IN_CREATE, &wd);
    if (fd < 0) return 17;
    memset(&blocker, 0, sizeof(blocker));
    blocker.fd = fd;
    if (pthread_create(&thread, 0, blocker_thread, &blocker) != 0) {
        close(fd);
        return fail(17, "pthread_create");
    }
    while (!blocker.started) sleep_ms(10);
    sleep_ms(300);
    if (blocker.done) { result = fail(17, "read returned on an empty queue"); goto done; }
    if (make_file(DIR_PATH "/w") != 0) { result = fail(17, "create"); goto done; }
    if (!wait_done(&blocker, EVENT_WAIT_MS + 1000)) { result = fail(17, "blocking read not woken"); goto done; }
    if (blocker.result < (ssize_t)sizeof(struct inotify_event)) {
        errno = blocker.error;
        result = fail(17, "woken read returned too little");
        goto done;
    }
    pthread_join(thread, 0);
    /* EINTR */
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_usr1;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) { result = fail(17, "sigaction"); goto done; }
    memset(&blocker, 0, sizeof(blocker));
    blocker.fd = fd;
    if (pthread_create(&thread, 0, blocker_thread, &blocker) != 0) { result = fail(17, "pthread_create 2"); goto done; }
    while (!blocker.started) sleep_ms(10);
    sleep_ms(300);
    pthread_kill(thread, SIGUSR1);
    if (!wait_done(&blocker, 2000)) { result = fail(17, "read not interrupted"); goto done; }
    pthread_join(thread, 0);
    if (blocker.result != -1 || blocker.error != EINTR || !signal_seen) {
        fprintf(stderr, "check 17: result %ld errno %d handler %d\n",
            (long)blocker.result, blocker.error, signal_seen);
        errno = blocker.error;
        result = fail(17, "interrupted read");
    }
done:
    close(fd);
    return result;
}

#define WRITERS 4
#define PER_WRITER 25

static void *writer_thread(void *argument)
{
    int index = (int)(intptr_t)argument, i;
    char path[64];
    for (i = 0; i < PER_WRITER; i++) {
        snprintf(path, sizeof(path), DIR_PATH "/t%df%d", index, i);
        make_file(path);
    }
    return 0;
}

static int check_burst(void)
{
    struct reader reader;
    struct record record;
    pthread_t threads[WRITERS];
    char seen[WRITERS][PER_WRITER];
    int wd, i, count = 0, result = 0, started = 0,
        fd = open_watch(18, IN_NONBLOCK, IN_CREATE, &wd);
    long elapsed = 0;
    if (fd < 0) return 18;
    reader_init(&reader, fd);
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < WRITERS; i++)
        if (pthread_create(&threads[i], 0, writer_thread,
                (void *)(intptr_t)i) == 0) started++;
    if (started != WRITERS) { result = fail(18, "pthread_create"); goto join; }
    while (count < WRITERS * PER_WRITER && elapsed < 5000) {
        int writer, file, got;
        got = next_record(&reader, 100, &record);
        if (got < 0) { result = fail(18, "read"); goto join; }
        if (got == 0) { elapsed += 100; continue; }
        if (record.mask & IN_Q_OVERFLOW) {
            fprintf(stderr, "check 18: overflow after %d events\n", count);
            errno = 0;
            result = fail(18, "IN_Q_OVERFLOW");
            goto join;
        }
        if (sscanf(record.name, "t%df%d", &writer, &file) != 2 ||
                writer < 0 || writer >= WRITERS || file < 0 ||
                file >= PER_WRITER || record.mask != IN_CREATE) {
            fprintf(stderr, "check 18: bad record mask 0x%x '%s'\n",
                record.mask, record.name);
            errno = 0;
            result = fail(18, "unexpected record");
            goto join;
        }
        if (seen[writer][file]++) {
            fprintf(stderr, "check 18: '%s' twice\n", record.name);
            errno = 0;
            result = fail(18, "duplicate event");
            goto join;
        }
        count++;
    }
    if (count != WRITERS * PER_WRITER) {
        fprintf(stderr, "check 18: %d of %d names arrived\n", count,
            WRITERS * PER_WRITER);
        errno = 0;
        result = fail(18, "names missing");
        goto join;
    }
    result = expect_quiet(18, &reader, 1000);
join:
    for (i = 0; i < started; i++) pthread_join(threads[i], 0);
    close(fd);
    return result;
}

static int check_fd_reuse(void)
{
    struct epoll_event event, got[4];
    int wd, epfd, number, pipefd[2], ready, result = 0,
        fd = open_watch(19, IN_NONBLOCK, IN_CREATE, &wd);
    if (fd < 0) return 19;
    number = fd;
    epfd = epoll_create1(0);
    event.events = EPOLLIN;
    event.data.fd = number;
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) != 0) {
        close(fd);
        return fail(19, "epoll setup");
    }
    if (make_file(DIR_PATH "/u") != 0) { result = fail(19, "create"); goto done; }
    if (epoll_count(epfd, EVENT_WAIT_MS) != 1) { result = fail(19, "inotify not ready"); goto done; }
    close(fd);
    if (pipe(pipefd) != 0) { result = fail(19, "pipe"); goto done; }
    if (pipefd[0] != number) {
        if (dup2(pipefd[0], number) != number) { result = fail(19, "dup2"); goto done; }
        close(pipefd[0]);
    }
    ready = epoll_wait(epfd, got, 4, 300);
    if (ready != 0) {
        fprintf(stderr, "check 19: epoll_wait returned %d for reused fd %d "
            "(events 0x%x)\n", ready, number, ready > 0 ? got[0].events : 0);
        errno = 0;
        result = fail(19, "stale inotify readiness");
        goto done;
    }
    /* Linux has dropped the registration with the old description, so the
       pipe is not reported either; a report here would be harmless. */
    if (write(pipefd[1], "x", 1) != 1) result = fail(19, "pipe write");
    close(number);
    close(pipefd[1]);
done:
    close(epfd);
    return result;
}

int main(void)
{
    int result, probe;
    probe = inotify_init1(0);
    if (probe < 0) return fail(10, "inotify_init1");
    close(probe);
    cleanup();
    if (mkdir(DIR_PATH, 0755) != 0) return fail(10, "mkdir " DIR_PATH);
    signal(SIGPIPE, SIG_IGN);
    arm(10);
    arm(11); if ((result = check_oneshot()) != 0) goto out;
    arm(12); if ((result = check_mask_add()) != 0) goto out;
    arm(13); if ((result = check_mask_replace()) != 0) goto out;
    arm(14); if ((result = check_overflow()) != 0) goto out;
    arm(15); if ((result = check_readiness()) != 0) goto out;
    arm(16); if ((result = check_epoll()) != 0) goto out;
    arm(17); if ((result = check_blocking()) != 0) goto out;
    arm(18); if ((result = check_burst()) != 0) goto out;
    arm(19); if ((result = check_fd_reuse()) != 0) goto out;
out:
    cleanup();
    return result;
}
