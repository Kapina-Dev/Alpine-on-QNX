#ifndef LINUXEMU_LINUX_DIAGNOSTICS_H
#define LINUXEMU_LINUX_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Structured unsupported-syscall diagnostics (roadmap Phase 0). Pure: no
 * stdio, no malloc, no QNX headers, so it is host-tested
 * (tests/diagnostics-test.c) and safe to format from a fatal-signal path.
 *
 * Line format (one per distinct number/error/detail, per process):
 *   linuxemu-unsupported exe=<exe> pid=<pid> nr=<n> name=<name>
 *   error=<ENAME> count=<n> detail=<detail> args=<r0>,...,<r5>
 * (a single line; exe is percent-encoded). Overflow beyond
 * LINUX_DIAG_MAX_ENTRIES distinct entries adds:
 *   linuxemu-unsupported-overflow exe=<exe> pid=<pid> dropped=<n>
 */

#define LINUX_DIAG_MAX_ENTRIES 64

const char *linux_syscall_name(uint32_t number);
const char *linux_errno_name(int32_t linux_error);
void linux_diag_record(uint32_t number, int32_t linux_error,
    const char *detail, const uint32_t args[6]);
/* Write whole report lines into buffer, starting at line *cursor (0 to
   begin), and advance *cursor past them. Returns the bytes written; 0 means
   the report is complete. Call it in a loop to emit a report of any size
   through a fixed buffer. */
size_t linux_diag_format(char *buffer, size_t capacity,
    const char *executable, int32_t pid, uint32_t *cursor);
void linux_diag_reset(void);

/* Recording is off until enabled (Linuxemu enables it when
   LINUXEMU_DIAGNOSTICS is set), so the unset path records nothing. */
void linux_diag_set_enabled(int enabled);
int linux_diag_is_enabled(void);

/* Empty the table in a fork child without taking the lock: another thread
   of the parent may have held it at fork time, and that thread does not
   exist in the child. */
void linux_diag_after_fork(void);

#endif
