#include "guest_fd.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
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
    ino64_t inode;
    const struct guest_fd_ops *ops;
    union { long long integer; void *pointer; double real; } data[];
};

static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_fd_object *slots[GUEST_FD_SLOTS];
static volatile unsigned char typed[GUEST_FD_SLOTS];
static int hidden_base = HIDDEN_BASE_CAP;
static uint64_t nofile_soft = HIDDEN_BASE_CAP;
static uint64_t nofile_hard = HIDDEN_BASE_CAP;

static int valid_fd(int fd)
{
    return fd >= 0 && fd < GUEST_FD_SLOTS;
}

struct guest_fd_object *guest_fd_object_new(enum guest_fd_type type,
    const struct guest_fd_ops *ops, int backing_fd, size_t extra_size)
{
    struct stat64 status;
    struct guest_fd_object *object;
    if (ops == 0 || fstat64(backing_fd, &status) != 0) {
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

/* Narrow adoption (Phase 1): a received pipe-backed object that this
   process still holds, e.g. one inherited across fork, is the same object,
   because a pipe's (st_dev, st_ino) is unique to it.  Path records are
   never adopted: two opens of one directory share an inode but not an
   open-file description.  Anything else starts native. */
void guest_fd_received(int fd)
{
    struct stat64 status;
    struct guest_fd_object *match = 0;
    int other;
    guest_fd_clear(fd);
    if (!valid_fd(fd) || fstat64(fd, &status) != 0) return;
    pthread_mutex_lock(&table_lock);
    for (other = 0; other != GUEST_FD_SLOTS && match == 0; ++other) {
        struct guest_fd_object *object = slots[other];
        if (other != fd && object != 0 && object->type != GUEST_FD_PATH &&
                object->device == status.st_dev &&
                object->inode == status.st_ino) {
            match = object;
            ++match->references;
        }
    }
    pthread_mutex_unlock(&table_lock);
    if (match == 0) return;
    (void)guest_fd_attach(fd, match);
    guest_fd_put(match);
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
    struct stat64 status;
    struct guest_fd_object *object, *stale = 0;
    if (!valid_fd(fd)) return 0;
    pthread_mutex_lock(&table_lock);
    object = slots[fd];
    if (object != 0) {
        if (fstat64(fd, &status) != 0 || status.st_dev != object->device ||
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

struct directory_entry {
    uint64_t inode;
    char *name;
};

struct path_data {
    char *path;
    struct directory_entry *entries;
    size_t count, capacity, cursor;
    int loaded;
};

static void free_entries(struct path_data *data)
{
    size_t i;
    for (i = 0; i != data->count; ++i) free(data->entries[i].name);
    free(data->entries);
    data->entries = 0;
    data->count = data->capacity = 0;
}

static void path_release(struct guest_fd_object *object)
{
    struct path_data *data = guest_fd_object_data(object);
    free_entries(data);
    free(data->path);
}

static int path_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    struct path_data *data = guest_fd_object_data(object);
    size_t length = strlen(data->path), i;
    (void)fd;
    if (size < 3 + 2 * length + 1) return -1;
    memcpy(buffer, "-1:", 3);
    for (i = 0; i != length; ++i) {
        unsigned char byte = (unsigned char)data->path[i];
        buffer[3 + 2 * i] = digits[byte >> 4];
        buffer[3 + 2 * i + 1] = digits[byte & 15];
    }
    buffer[3 + 2 * length] = '\0';
    return (int)(3 + 2 * length);
}

static const struct guest_fd_ops path_ops = {
    0, 0, 0, 0, 0, path_release, path_serialize
};

int guest_fd_record_path(int fd, const char *host_path)
{
    struct guest_fd_object *object;
    struct path_data *data;
    object = guest_fd_object_new(GUEST_FD_PATH, &path_ops, fd,
        sizeof(struct path_data));
    if (object == 0) return -1;
    data = guest_fd_object_data(object);
    data->path = strdup(host_path);
    if (data->path == 0 || guest_fd_attach(fd, object) != 0) {
        guest_fd_put(object);
        return -1;
    }
    guest_fd_put(object);
    return 0;
}

int guest_fd_path(int fd, char *host_path, size_t size)
{
    struct guest_fd_object *object = guest_fd_acquire(fd);
    struct path_data *data;
    size_t length;
    int result = 0;
    if (object == 0 || object->type != GUEST_FD_PATH) {
        guest_fd_put(object);
        errno = EBADF;
        return -1;
    }
    data = guest_fd_object_data(object);
    length = strlen(data->path);
    if (length >= size) {
        errno = ENAMETOOLONG;
        result = -1;
    } else memcpy(host_path, data->path, length + 1);
    guest_fd_put(object);
    return result;
}

static int load_directory(struct path_data *data)
{
    DIR *directory = opendir(data->path);
    struct dirent *entry;
    if (directory == 0) return -1;
    errno = 0;
    while ((entry = readdir(directory)) != 0) {
        if (data->count == data->capacity) {
            size_t capacity = data->capacity == 0 ? 32 : 2 * data->capacity;
            struct directory_entry *grown = realloc(data->entries,
                capacity * sizeof(*grown));
            if (grown == 0) { errno = ENOMEM; break; }
            data->entries = grown;
            data->capacity = capacity;
        }
        data->entries[data->count].name = strdup(entry->d_name);
        if (data->entries[data->count].name == 0) { errno = ENOMEM; break; }
        data->entries[data->count++].inode = (uint64_t)entry->d_ino;
        errno = 0;
    }
    if (errno != 0) {
        int saved_errno = errno;
        closedir(directory);
        free_entries(data);
        errno = saved_errno;
        return -1;
    }
    closedir(directory);
    data->loaded = 1;
    data->cursor = 0;
    return 0;
}

int guest_fd_object_directory_entry(struct guest_fd_object *object,
    uint64_t *inode, const char **name, int64_t *offset)
{
    struct path_data *data;
    if (object == 0 || object->type != GUEST_FD_PATH) {
        errno = EBADF;
        return -1;
    }
    data = guest_fd_object_data(object);
    if (!data->loaded && load_directory(data) != 0) return -1;
    if (data->cursor >= data->count) return 0;
    *inode = data->entries[data->cursor].inode;
    *name = data->entries[data->cursor].name;
    *offset = (int64_t)data->cursor + 1;
    return 1;
}

void guest_fd_object_directory_advance(struct guest_fd_object *object)
{
    struct path_data *data = guest_fd_object_data(object);
    if (data->cursor < data->count) ++data->cursor;
}

static struct guest_fd_object *typed_object(int fd)
{
    return guest_fd_is_typed(fd) ? guest_fd_acquire(fd) : 0;
}

int guest_fd_dispatch_read(int fd, void *buffer, size_t length,
    int32_t *result)
{
    struct guest_fd_object *object = typed_object(fd);
    int handled = object != 0 && object->ops->read != 0;
    if (handled) *result = object->ops->read(object, fd, buffer, length);
    guest_fd_put(object);
    return handled;
}

int guest_fd_dispatch_write(int fd, const void *buffer, size_t length,
    int32_t *result)
{
    struct guest_fd_object *object = typed_object(fd);
    int handled = object != 0 && object->ops->write != 0;
    if (handled) *result = object->ops->write(object, fd, buffer, length);
    guest_fd_put(object);
    return handled;
}

int guest_fd_dispatch_get_status(int fd, int *host_flags)
{
    struct guest_fd_object *object = typed_object(fd);
    int handled = 0;
    if (object != 0 && object->ops->get_status != 0) {
        *host_flags = object->ops->get_status(object, fd);
        handled = *host_flags < 0 ? -1 : 1;
    }
    guest_fd_put(object);
    return handled;
}

int guest_fd_dispatch_set_status(int fd, int host_flags)
{
    struct guest_fd_object *object = typed_object(fd);
    int handled = 0;
    if (object != 0 && object->ops->set_status != 0)
        handled = object->ops->set_status(object, fd, host_flags) == 0 ?
            1 : -1;
    guest_fd_put(object);
    return handled;
}

#define RECORDS_PREFIX "LINUXEMU_FD_RECORDS="

static guest_fd_restore_fn restore_functions[GUEST_FD_BINDER + 1];
static int carried_hidden[GUEST_FD_SLOTS];
static int carried_hidden_count;

void guest_fd_register_restore(enum guest_fd_type type,
    guest_fd_restore_fn restore)
{
    if ((int)type > 0 && type <= GUEST_FD_BINDER)
        restore_functions[type] = restore;
}

int guest_fd_exec_records(char *buffer, size_t size)
{
    static struct guest_fd_object *seen[GUEST_FD_SLOTS];
    static int seen_fd[GUEST_FD_SLOTS];
    int seen_count = 0, fd, i, failed = 0;
    size_t used = sizeof(RECORDS_PREFIX) - 1;
    carried_hidden_count = 0;
    if (size <= used) return -1;
    memcpy(buffer, RECORDS_PREFIX, used);
    if (nofile_soft != (uint64_t)hidden_base ||
            nofile_hard != (uint64_t)hidden_base) {
        int written = snprintf(buffer + used, size - used, "limit:%llu:%llu;",
            (unsigned long long)nofile_soft, (unsigned long long)nofile_hard);
        if (written < 0 || (size_t)written >= size - used) return -1;
        used += (size_t)written;
    }
    for (fd = 0; fd != GUEST_FD_SLOTS && !failed; ++fd) {
        struct guest_fd_object *object = guest_fd_acquire(fd);
        char body[2 * PATH_MAX + 32];
        int flags, written, length = -1;
        if (object == 0) continue;
        flags = fcntl(fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) != 0 ||
                object->ops->serialize == 0) {
            guest_fd_put(object);
            continue;
        }
        for (i = 0; i != seen_count; ++i)
            if (seen[i] == object) break;
        if (i != seen_count)
            written = snprintf(buffer + used, size - used, "%d:0:%d:;", fd,
                seen_fd[i]);
        else {
            length = object->ops->serialize(object, fd, body, sizeof(body));
            written = length < 0 ? -1 : snprintf(buffer + used, size - used,
                "%d:%d:%s;", fd, (int)object->type, body);
        }
        if (written < 0 || (size_t)written >= size - used) failed = 1;
        else {
            used += (size_t)written;
            if (i == seen_count) {
                long hidden = strtol(body, 0, 10);
                seen[seen_count] = object;
                seen_fd[seen_count++] = fd;
                if (hidden >= 0)
                    carried_hidden[carried_hidden_count++] = (int)hidden;
            }
        }
        guest_fd_put(object);
    }
    if (failed) { carried_hidden_count = 0; return -1; }
    if (used == sizeof(RECORDS_PREFIX) - 1) { buffer[0] = '\0'; return 0; }
    return (int)used;
}

int guest_fd_exec_prepare(void)
{
    int i, result = 0;
    for (i = 0; i != carried_hidden_count; ++i) {
        int flags = fcntl(carried_hidden[i], F_GETFD);
        if (flags < 0 || fcntl(carried_hidden[i], F_SETFD,
                flags & ~FD_CLOEXEC) < 0) result = -1;
    }
    return result;
}

static int parse_number(const char **cursor, long minimum, long maximum,
    long *value)
{
    char *end;
    errno = 0;
    *value = strtol(*cursor, &end, 10);
    if (end == *cursor || *end != ':' || errno != 0 ||
            *value < minimum || *value > maximum) return -1;
    *cursor = end + 1;
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int restore_limit(const char *record, size_t length)
{
    char text[64];
    unsigned long long soft, hard;
    char extra;
    if (length >= sizeof(text)) return -1;
    memcpy(text, record, length);
    text[length] = '\0';
    if (sscanf(text, "limit:%llu:%llu%c", &soft, &hard, &extra) != 2)
        return -1;
    return guest_fd_nofile_set(soft, hard);
}

static int restore_one(const char *record, size_t length)
{
    char path[PATH_MAX];
    const char *cursor = record, *hex;
    long fd, type, hidden;
    size_t hex_length, i;
    if (parse_number(&cursor, 0, hidden_base - 1, &fd) != 0 ||
            parse_number(&cursor, 0, GUEST_FD_BINDER, &type) != 0 ||
            parse_number(&cursor, -1, INT_MAX, &hidden) != 0) return -1;
    hex = cursor;
    hex_length = length - (size_t)(hex - record);
    if (hex_length % 2 != 0 || hex_length / 2 >= sizeof(path)) return -1;
    for (i = 0; i != hex_length / 2; ++i) {
        int high = hex_value(hex[2 * i]), low = hex_value(hex[2 * i + 1]);
        if (high < 0 || low < 0) return -1;
        path[i] = (char)(high * 16 + low);
    }
    path[hex_length / 2] = '\0';
    if (fcntl((int)fd, F_GETFD) < 0) return -1;
    if (type == 0) {
        struct guest_fd_object *source;
        if (hidden < 0 || hidden >= GUEST_FD_SLOTS || hex_length != 0)
            return -1;
        source = guest_fd_acquire((int)hidden);
        if (source == 0) return -1;
        guest_fd_put(source);
        return guest_fd_duplicate((int)hidden, (int)fd);
    }
    if (hidden >= 0 && fcntl((int)hidden, F_GETFD) < 0) return -1;
    if (type == GUEST_FD_PATH) {
        if (hex_length == 0 || hidden != -1) return -1;
        return guest_fd_record_path((int)fd, path);
    }
    if (restore_functions[type] == 0 ||
            restore_functions[type]((int)fd, (int)hidden, path) != 0)
        return -1;
    if (hidden >= 0) {
        int flags = fcntl((int)hidden, F_GETFD);
        if (flags >= 0) fcntl((int)hidden, F_SETFD, flags | FD_CLOEXEC);
    }
    return 0;
}

int guest_fd_restore_records(const char *value)
{
    int restored = 0;
    if (value == 0) return 0;
    while (*value != '\0') {
        const char *end = strchr(value, ';');
        size_t length = end == 0 ? strlen(value) : (size_t)(end - value);
        if (end != 0 && (strncmp(value, "limit:", 6) == 0 ?
                restore_limit(value, length) : restore_one(value, length)) == 0)
            ++restored;
        if (end == 0) break;
        value = end + 1;
    }
    return restored;
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
    nofile_soft = nofile_hard = (uint64_t)hidden_base;
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

int guest_fd_is_hidden(int fd)
{
    return fd >= hidden_base;
}

int guest_fd_admit(int fd)
{
    if (fd >= 0 && guest_fd_is_hidden(fd)) {
        close(fd);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

int guest_fd_poll_target(int fd)
{
    struct guest_fd_object *object = typed_object(fd);
    int target = fd;
    if (object != 0 && object->ops->poll_fd != 0)
        target = object->ops->poll_fd(object, fd);
    guest_fd_put(object);
    return target;
}

void guest_fd_nofile_get(uint64_t *soft, uint64_t *hard)
{
    *soft = nofile_soft;
    *hard = nofile_hard;
}

int guest_fd_nofile_set(uint64_t soft, uint64_t hard)
{
    if (soft > hard) { errno = EINVAL; return -1; }
    if (hard > (uint64_t)hidden_base) hard = (uint64_t)hidden_base;
    if (soft > hard) soft = hard;
    nofile_soft = soft;
    nofile_hard = hard;
    return 0;
}

int guest_fd_hide(int fd)
{
    int hidden = fcntl(fd, F_DUPFD, hidden_base);
    if (hidden < 0) return -1;
    if (fcntl(hidden, F_SETFD, FD_CLOEXEC) < 0) {
        int saved_errno = errno;
        close(hidden);
        errno = saved_errno;
        return -1;
    }
    close(fd);
    return hidden;
}
