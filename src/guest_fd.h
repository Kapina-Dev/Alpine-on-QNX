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

#include <dirent.h>
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
/* A number that arrived from outside the table (SCM_RIGHTS): drop any
   stale record so it starts native. */
void guest_fd_received(int fd);
/* Lock-free hint: the number carries an object with its own I/O behaviour
   (any type except GUEST_FD_PATH).  Gates the slower dispatch paths. */
int guest_fd_is_typed(int fd);

/* Path records (GUEST_FD_PATH): the host path an fd was opened by, for
   *at directory fds, fchdir and getdents. */
int guest_fd_record_path(int fd, const char *host_path);
/* 0, or -1 with errno EBADF (no record) or ENAMETOOLONG (buffer too small). */
int guest_fd_path(int fd, char *host_path, size_t size);
/* A directory stream on a referenced GUEST_FD_PATH object, positioned where
   the last getdents stopped; 0 + errno.  Hand it back with
   guest_fd_object_directory_done, which saves the position and closes it. */
DIR *guest_fd_object_directory(struct guest_fd_object *object);
void guest_fd_object_directory_done(struct guest_fd_object *object,
    DIR *directory);

/* Dispatch to a typed object's ops.  Return 0 when the fd is native (or
   the object lacks the op): the caller does the native call.  The read and
   write forms return 1 with *result holding the Linux result.  The status
   forms return 1 on success and -1 with errno set on failure. */
int guest_fd_dispatch_read(int fd, void *buffer, size_t length,
    int32_t *result);
int guest_fd_dispatch_write(int fd, const void *buffer, size_t length,
    int32_t *result);
int guest_fd_dispatch_get_status(int fd, int *host_flags);
int guest_fd_dispatch_set_status(int fd, int host_flags);

/* Exec carry-over.  Each fd that survives exec (no FD_CLOEXEC) and carries
   an object with a serialize op becomes one record "fd:type:hidden:hexpath;"
   in LINUXEMU_FD_RECORDS; a later fd sharing an already-written object
   becomes an alias record "fd:0:first_fd:;".  ops->serialize writes the
   "hidden:hexpath" body for its object. */
typedef int (*guest_fd_restore_fn)(int fd, int hidden_fd,
    const char *host_path);
/* Restores one record of the given type in the new image (0 or -1). */
void guest_fd_register_restore(enum guest_fd_type type,
    guest_fd_restore_fn restore);
/* Writes "LINUXEMU_FD_RECORDS=..." into buffer; returns its length, 0 when
   nothing survives exec, or -1 when it does not fit. */
int guest_fd_exec_records(char *buffer, size_t size);
/* Clears FD_CLOEXEC on the hidden fds of the objects just recorded. */
int guest_fd_exec_prepare(void);
/* Parses LINUXEMU_FD_RECORDS in the new image; malformed records are
   skipped.  Restored hidden fds get FD_CLOEXEC again.  Returns how many
   records were restored. */
int guest_fd_restore_records(const char *value);

int guest_fd_initialize(void);
void guest_fd_after_fork(void);

/* Emulator-owned fds live at or above the hidden base. */
int guest_fd_hidden_base(void);
/* Moves an emulator-owned fd to >= the hidden base with close-on-exec set;
   returns the new number and closes the old one, or -1 (errno set). */
int guest_fd_hide(int fd);

#endif
