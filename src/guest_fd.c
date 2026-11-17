#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

/* Guest-visible fds stay below the hidden base, which is at most
   HIDDEN_BASE_CAP, so a fixed table covers every guest number and the
   lock-free typed check reads memory that never moves. */
#define GUEST_FD_SLOTS 4096
#define HIDDEN_BASE_CAP 4032
#define HIDDEN_RESERVE 64

struct guest_fd_object {
    enum guest_fd_type type;
    unsigned references;
    dev_t device;
    ino_t inode;
    const struct guest_fd_ops *ops;
    union { long long integer; void *pointer; double real; } data[];
};

static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_fd_object *slots[GUEST_FD_SLOTS];
static volatile unsigned char typed[GUEST_FD_SLOTS];
static int hidden_base = HIDDEN_BASE_CAP;

static int valid_fd(int fd)
{
    return fd >= 0 && fd < GUEST_FD_SLOTS;
}

struct guest_fd_object *guest_fd_object_new(enum guest_fd_type type,
    const struct guest_fd_ops *ops, int backing_fd, size_t extra_size)
{
    struct stat status;
    struct guest_fd_object *object;
    if (ops == 0 || fstat(backing_fd, &status) != 0) {
        if (ops == 0) errno = EINVAL;
        return 0;
    }
    object = calloc(1, sizeof(*object) + extra_size);
    if (object == 0) return 0;
    object->type = type;
    object->references = 1;
    object->device = status.st_dev;
    object->inode = status.st_ino;
    object->ops = ops;
    return object;
}

void *guest_fd_object_data(struct guest_fd_object *object)
{
    return object->data;
}

enum guest_fd_type guest_fd_object_type(const struct guest_fd_object *object)
{
    return object->type;
}

void guest_fd_put(struct guest_fd_object *object)
{
    int last;
    if (object == 0) return;
    pthread_mutex_lock(&table_lock);
    last = --object->references == 0;
    pthread_mutex_unlock(&table_lock);
    if (!last) return;
    if (object->ops->release != 0) object->ops->release(object);
    free(object);
}

/* Caller holds table_lock.  Returns the detached object for guest_fd_put
   after the lock is released. */
static struct guest_fd_object *detach_locked(int fd)
{
    struct guest_fd_object *old = slots[fd];
    slots[fd] = 0;
    typed[fd] = 0;
    return old;
}

int guest_fd_attach(int fd, struct guest_fd_object *object)
{
    struct guest_fd_object *old;
    if (!valid_fd(fd) || object == 0) { errno = EBADF; return -1; }
    pthread_mutex_lock(&table_lock);
    old = detach_locked(fd);
    ++object->references;
    slots[fd] = object;
    typed[fd] = object->type != GUEST_FD_PATH;
    pthread_mutex_unlock(&table_lock);
    guest_fd_put(old);
    return 0;
}

void guest_fd_clear(int fd)
{
    struct guest_fd_object *old;
    if (!valid_fd(fd)) return;
    pthread_mutex_lock(&table_lock);
    old = detach_locked(fd);
    pthread_mutex_unlock(&table_lock);
    guest_fd_put(old);
}

int guest_fd_duplicate(int source, int destination)
{
    struct guest_fd_object *old, *object;
    if (!valid_fd(destination)) { errno = EBADF; return -1; }
    if (source == destination) return 0;
    pthread_mutex_lock(&table_lock);
    object = valid_fd(source) ? slots[source] : 0;
    old = detach_locked(destination);
    if (object != 0) {
        ++object->references;
        slots[destination] = object;
        typed[destination] = object->type != GUEST_FD_PATH;
    }
    pthread_mutex_unlock(&table_lock);
    guest_fd_put(old);
    return 0;
}

struct guest_fd_object *guest_fd_acquire(int fd)
{
    struct stat status;
    struct guest_fd_object *object, *stale = 0;
    if (!valid_fd(fd)) return 0;
    pthread_mutex_lock(&table_lock);
    object = slots[fd];
    if (object != 0) {
        if (fstat(fd, &status) != 0 || status.st_dev != object->device ||
                status.st_ino != object->inode) {
            stale = detach_locked(fd);
            object = 0;
        } else ++object->references;
    }
    pthread_mutex_unlock(&table_lock);
    guest_fd_put(stale);
    return object;
}

int guest_fd_is_typed(int fd)
{
    return valid_fd(fd) && typed[fd] != 0;
}

int guest_fd_initialize(void)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return -1;
    if (limit.rlim_cur != limit.rlim_max) {
        struct rlimit raised = limit;
        raised.rlim_cur = limit.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &raised) == 0) limit = raised;
    }
    if (limit.rlim_cur == RLIM_INFINITY ||
            limit.rlim_cur > GUEST_FD_SLOTS)
        hidden_base = HIDDEN_BASE_CAP;
    else if (limit.rlim_cur > 2 * HIDDEN_RESERVE)
        hidden_base = (int)limit.rlim_cur - HIDDEN_RESERVE;
    else {
        errno = EMFILE;
        return -1;
    }
    return 0;
}

void guest_fd_after_fork(void)
{
    pthread_mutex_init(&table_lock, 0);
}

int guest_fd_hidden_base(void)
{
    return hidden_base;
}

int guest_fd_hide(int fd)
{
    int hidden = fcntl(fd, F_DUPFD, hidden_base);
    if (hidden < 0) return -1;
    if (fcntl(hidden, F_SETFD, FD_CLOEXEC) != 0) {
        int saved_errno = errno;
        close(hidden);
        errno = saved_errno;
        return -1;
    }
    close(fd);
    return hidden;
}
