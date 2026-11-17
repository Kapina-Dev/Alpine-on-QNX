#include "linuxemu.h"
#include "linux_diagnostics.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * Append this process's unsupported-syscall report to LINUXEMU_DIAGNOSTICS,
 * then start counting afresh. Called on every exit path (exit_group,
 * main-thread exit, execve, fatal signal). Uses only open/write/close so it
 * is usable from the fatal-signal path; every error is ignored so that
 * diagnostics never change guest behaviour.
 */
void linux_diag_flush(void)
{
    static char report[16384];
    const char *path = getenv("LINUXEMU_DIAGNOSTICS");
    const char *executable = guest_process_executable();
    int32_t pid = (int32_t)getpid();
    uint32_t cursor = 0;
    size_t length;
    int descriptor;

    if (path == 0 || path[0] == '\0') return;
    length = linux_diag_format(report, sizeof(report), executable, pid,
        &cursor);
    if (length == 0) return;
    descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    /* The report is written in buffer-sized chunks of whole lines, so no
       line is lost however long the executable path is. */
    while (length != 0) {
        if (descriptor >= 0) {
            ssize_t written = write(descriptor, report, length);
            (void)written;
        }
        length = linux_diag_format(report, sizeof(report), executable, pid,
            &cursor);
    }
    if (descriptor >= 0) close(descriptor);
    linux_diag_reset();
}
