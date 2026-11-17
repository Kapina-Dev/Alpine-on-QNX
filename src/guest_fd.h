#ifndef GUEST_FD_H
#define GUEST_FD_H

/*
 * Guest file-descriptor record table (roadmap Phase 1, approach B).
 *
 * Guest fd numbers are native QNX numbers.  A number may carry a shared,
 * reference-counted object describing an emulator-managed Linux object or
 * extra per-description state; no object means a plain native fd.  Objects
 * are identified by their backing native fd's (st_dev, st_ino), so a record
 * whose number was reused behind the table's back is detected and dropped.
 */

#include <stddef.h>
#include <stdint.h>

enum guest_fd_type {
    GUEST_FD_PATH = 1,
    GUEST_FD_WAKE_EVENTFD,
    GUEST_FD_EVENTFD,
    GUEST_FD_EPOLL,
    GUEST_FD_TIMERFD,
    GUEST_FD_SIGNALFD,
    GUEST_FD_INOTIFY,
    GUEST_FD_ASHMEM,
    GUEST_FD_BINDER
};

struct guest_fd_object;

struct guest_fd_ops {
    int32_t (*read)(struct guest_fd_object *, int fd, void *, size_t);
    int32_t (*write)(struct guest_fd_object *, int fd, const void *, size_t);
    int (*poll_fd)(struct guest_fd_object *, int fd);
    int (*get_status)(struct guest_fd_object *, int fd);
    int (*set_status)(struct guest_fd_object *, int fd, int host_flags);
    void (*release)(struct guest_fd_object *);
    int (*serialize)(struct guest_fd_object *, int fd, char *, size_t);
};

/* Created with one reference owned by the caller; identity is taken from
   fstat(backing_fd).  Returns 0 on failure (errno set). */
struct guest_fd_object *guest_fd_object_new(enum guest_fd_type type,
    const struct guest_fd_ops *ops, int backing_fd, size_t extra_size);
void *guest_fd_object_data(struct guest_fd_object *object);
enum guest_fd_type guest_fd_object_type(const struct guest_fd_object *object);
/* Drop one reference; the last one runs ops->release and frees. */
void guest_fd_put(struct guest_fd_object *object);

/* The slot takes its own reference and drops any previous object. */
int guest_fd_attach(int fd, struct guest_fd_object *object);
/* The number becomes native again. */
void guest_fd_clear(int fd);
/* After a native dup: destination shares source's object, or becomes native. */
int guest_fd_duplicate(int source, int destination);
/* Referenced object or 0.  Validates identity; drops a mismatched record. */
struct guest_fd_object *guest_fd_acquire(int fd);
/* Lock-free hint: the number carries an object with its own I/O behaviour
   (any type except GUEST_FD_PATH).  Gates the slower dispatch paths. */
int guest_fd_is_typed(int fd);

int guest_fd_initialize(void);
void guest_fd_after_fork(void);

/* Emulator-owned fds live at or above the hidden base. */
int guest_fd_hidden_base(void);
/* Moves an emulator-owned fd to >= the hidden base with close-on-exec set;
   returns the new number and closes the old one, or -1 (errno set). */
int guest_fd_hide(int fd);

#endif
