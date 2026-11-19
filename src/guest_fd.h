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

/* Every guest fd number is below this (the hidden base is at most 4032). */
#define GUEST_FD_SLOTS 4096

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
    /* Host poll bits (POLLOUT, ...) that are ready without the native
       poll target's help, or 0.  Used by poll, select and epoll. */
    short (*ready_now)(struct guest_fd_object *, int fd);
    /* SCM_RIGHTS: writes the hidden fds a receiving process needs to
       rebuild the object (at most max; still owned by the object) and
       returns how many, or -1. */
    int (*companions)(struct guest_fd_object *, int fd, int *out, int max);
};

/* Most companions one object sends with it. */
#define GUEST_FD_COMPANIONS_MAX 4

/* Created with one reference owned by the caller; identity is taken from
   fstat64(backing_fd).  Returns 0 on failure (errno set). */
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
/* clear, attach and duplicate (its destination) also bump the number's
   epoch and call linux_epoll_note_close: every path that closes, replaces
   or hands out a guest number goes through one of them.  The epoch changes
   whenever the number may name another file than before. */
unsigned guest_fd_epoch(int fd);
/* Bumps the epoch and notifies epoll without touching the record: called
   before a native dup2 replaces fd, so the number never names the new file
   under the old epoch. */
void guest_fd_retire(int fd);
/* In linux_epoll.c: drops every epoll item registered on fd and wakes the
   instances' waiters.  Cheap when no epoll watches fd.  Takes epoll locks,
   so it is never called with table_lock or an epoll instance lock held. */
void linux_epoll_note_close(int fd);
/* After a native dup: destination shares source's object, or becomes native. */
int guest_fd_duplicate(int source, int destination);
/* Referenced object or 0.  Validates identity; drops a mismatched record. */
struct guest_fd_object *guest_fd_acquire(int fd);
/* A number that arrived from outside the table (SCM_RIGHTS): drop any
   stale record, then adopt a live pipe-backed object of this process with
   the same (st_dev, st_ino); otherwise the number starts native. */
void guest_fd_received(int fd);

/* SCM_RIGHTS between Linuxemu processes.  When at least one sent fd has
   companions, the sender appends, after the guest's fds, every companion
   fd and then one descriptor-map fd: a fresh unnamed shm page holding
   "LXRIGHT1", the guest fd count, the companion total, and per guest fd
   its object type and companion count.  The receiver recognises the map
   as the last fd, rebuilds each object from its companions (or adopts the
   live object of this process with the same identity), closes the map and
   the companions, and hands the guest only its own fds.  A non-Linuxemu
   peer gets the extra fds (documented limit). */

/* Rebuilds an object of its type on the received fd from copies of the
   companions (the caller closes the companions afterwards).  0 or -1. */
typedef int (*guest_fd_receive_fn)(int fd, const int *companions, int count);
void guest_fd_register_receive(enum guest_fd_type type,
    guest_fd_receive_fn receive);
/* Sender: fds[0..count) are the guest's fds.  Writes the companions and
   then the map fd into extra (room for count * GUEST_FD_COMPANIONS_MAX + 1)
   and returns how many, or 0 when no fd has companions (send unchanged),
   or -1 with errno.  The objects whose companions are listed stay
   referenced in held[] (room for count) until guest_fd_rights_sent, which
   also closes the map fd. */
int guest_fd_rights_extra(const int *fds, int count, int *extra,
    struct guest_fd_object **held, int *held_count);
void guest_fd_rights_sent(int map_fd, struct guest_fd_object **held,
    int held_count);
/* Receiver: fds[0..count) arrived in one SCM_RIGHTS message.  Strips a
   trailing descriptor map (closing it and the companions; their slots
   become -1), gives every guest fd its object, and returns the guest's fd
   count.  When a guest fd landed in the hidden range (the guest is out of
   numbers) every fd is closed and -1 returned: the caller reports
   MSG_CTRUNC, as Linux does when it cannot install a received fd. */
int guest_fd_rights_received(int *fds, int count);
/* A fresh shm object of size bytes, already unlinked; a plain (not
   hidden) fd, or -1 with errno. */
int guest_fd_unnamed_shm(const char *prefix, size_t size);
/* Lock-free hint: the number carries an object with its own I/O behaviour
   (any type except GUEST_FD_PATH).  Gates the slower dispatch paths. */
int guest_fd_is_typed(int fd);

/* Path records (GUEST_FD_PATH): the host path an fd was opened by, for
   *at directory fds, fchdir and getdents. */
int guest_fd_record_path(int fd, const char *host_path);
/* 0, or -1 with errno EBADF (no record) or ENAMETOOLONG (buffer too small). */
int guest_fd_path(int fd, char *host_path, size_t size);
/* getdents on a referenced GUEST_FD_PATH object.  The first call takes a
   snapshot of the directory into the shared object; entries are then served
   from it, so deleting entries while iterating (rm -r) never shifts the
   position, and no directory stream stays open between calls.
   Returns 1 with the entry at the cursor (not consumed), 0 at the end, or
   -1 with errno.  *offset is the d_off cookie of the following entry. */
int guest_fd_object_directory_entry(struct guest_fd_object *object,
    uint64_t *inode, const char **name, int64_t *offset);
void guest_fd_object_directory_advance(struct guest_fd_object *object);

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
   "hidden:hexpath" body for its object, or "hidden:second:" for an object
   with two hidden fds (both survive exec; hex digits never contain ':', so
   the forms are unambiguous).  second_hidden_fd is -1 in the first form. */
typedef int (*guest_fd_restore_fn)(int fd, int hidden_fd,
    int second_hidden_fd, const char *host_path);
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
/* Numbers at or above the hidden base are not the guest's. */
int guest_fd_is_hidden(int fd);
/* A descriptor the host just created for the guest: one that landed in the
   hidden range (the guest is out of numbers below its limit) is closed and
   -1/EMFILE returned, as Linux returns EMFILE at RLIMIT_NOFILE. */
int guest_fd_admit(int fd);
/* The native fd QNX poll()/select() should watch for guest fd. */
int guest_fd_poll_target(int fd);
/* -1 when fd has no ready_now op (native, or a type without one); else
   the host poll bits ops->ready_now reports.  For such an fd the op is
   authoritative for write readiness (POLLOUT, POLLWRBAND): poll and select
   take those bits from it, not from the native poll target. */
int guest_fd_ready_now(int fd);
/* The guest's RLIMIT_NOFILE is virtual: it starts at the hidden base, a
   raise is clamped to the base, and the host limit is never changed.  A
   lowered limit is stored and reported but not enforced. */
void guest_fd_nofile_get(uint64_t *soft, uint64_t *hard);
int guest_fd_nofile_set(uint64_t soft, uint64_t hard);
/* Moves an emulator-owned fd to >= the hidden base with close-on-exec set;
   returns the new number and closes the old one, or -1 (errno set). */
int guest_fd_hide(int fd);

#endif
