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
size_t linux_diag_format(char *buffer, size_t capacity,
    const char *executable, int32_t pid);
void linux_diag_reset(void);

#endif
