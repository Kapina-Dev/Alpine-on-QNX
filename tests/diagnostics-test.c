/*
 * Host unit test for src/linux_diagnostics.c (pure logic).
 *
 *   cc -Isrc -Wall -Wextra -pthread -o diagnostics-test \
 *       tests/diagnostics-test.c src/linux_diagnostics.c
 */
#include "linux_diagnostics.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *name)
{
    printf("%s: %s\n", condition ? "ok" : "FAIL", name);
    if (!condition) {
        failures++;
    }
}

static int count_lines(const char *text, const char *prefix)
{
    int count = 0;
    const char *line = text;
    size_t length = strlen(prefix);

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        if (strncmp(line, prefix, length) == 0) {
            count++;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return count;
}

/* Format the whole report in chunks of at most chunk bytes, the way the
   runtime writes it, and return the total length. */
static size_t format_all(char *out, size_t capacity, const char *exe,
    int32_t pid, size_t chunk)
{
    size_t total = 0;
    uint32_t cursor = 0;

    for (;;) {
        size_t room = capacity - total;
        size_t length = linux_diag_format(out + total,
            chunk < room ? chunk : room, exe, pid, &cursor);
        if (length == 0) break;
        total += length;
    }
    return total;
}

/* One call from the start of the report. */
static size_t format_once(char *out, size_t capacity, const char *exe,
    int32_t pid)
{
    uint32_t cursor = 0;
    return linux_diag_format(out, capacity, exe, pid, &cursor);
}

static volatile int recorder_running;

static void *recorder(void *argument)
{
    static const uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t number = 2000;
    (void)argument;
    while (recorder_running) {
        linux_diag_record(number, 38, 0, args);
        number = number == 2063 ? 2000 : number + 1;
    }
    return NULL;
}

/* A fork child inherits the table lock in whatever state another thread
   left it. linux_diag_after_fork must not wait for it. Returns the number
   of children that did not exit 0 within the alarm. */
static int fork_while_recording(int forks)
{
    pthread_t thread;
    int bad = 0;
    int i;

    recorder_running = 1;
    pthread_create(&thread, NULL, recorder, NULL);
    for (i = 0; i < forks; ++i) {
        int status = 0;
        pid_t child = fork();
        if (child == 0) {
            static const uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
            alarm(2);
            linux_diag_after_fork();
            linux_diag_record(1, 38, 0, args);
            _exit(0);
        }
        if (child < 0 || waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            bad++;
    }
    recorder_running = 0;
    pthread_join(thread, NULL);
    return bad;
}

int main(void)
{
    static char buffer[16384];
    static const uint32_t args_a[6] = { 1, 2, 3, 4, 5, 6 };
    static const uint32_t args_b[6] = { 9, 9, 9, 9, 9, 9 };
    size_t length;

    linux_diag_set_enabled(1);

    /* names */
    check(strcmp(linux_syscall_name(136), "personality") == 0,
        "names: 136 personality");
    check(strcmp(linux_syscall_name(363), "rt_tgsigqueueinfo") == 0,
        "names: 363 rt_tgsigqueueinfo");
    check(strcmp(linux_syscall_name(0xf0005), "ARM_set_tls") == 0,
        "names: 0xf0005 ARM_set_tls");
    check(strcmp(linux_syscall_name(999), "unknown") == 0,
        "names: 999 unknown");

    /* errno names */
    check(linux_errno_name(38) != NULL
        && strcmp(linux_errno_name(38), "ENOSYS") == 0, "errno: 38 ENOSYS");
    check(linux_errno_name(95) != NULL
        && strcmp(linux_errno_name(95), "EOPNOTSUPP") == 0,
        "errno: 95 EOPNOTSUPP");
    check(linux_errno_name(200) == NULL, "errno: 200 unnamed");

    /* aggregate */
    linux_diag_reset();
    linux_diag_record(999, 38, 0, args_a);
    linux_diag_record(999, 38, 0, args_b);
    length = format_once(buffer, sizeof(buffer), "/bin/x", 42);
    buffer[length] = '\0';
    check(strcmp(buffer, "linuxemu-unsupported exe=/bin/x pid=42 nr=999 "
        "name=unknown error=ENOSYS count=2 detail=- args=0x00000001,"
        "0x00000002,0x00000003,0x00000004,0x00000005,0x00000006\n") == 0,
        "aggregate: exact line, first args kept");
    check(length == strlen(buffer), "aggregate: length matches");

    /* distinct detail */
    linux_diag_reset();
    linux_diag_record(172, 22, "prctl-option", args_a);
    linux_diag_record(172, 22, "prctl-set-vma", args_a);
    length = format_once(buffer, sizeof(buffer), "/bin/x", 42);
    buffer[length] = '\0';
    check(count_lines(buffer, "linuxemu-unsupported ") == 2,
        "distinct detail: two lines");
    check(strstr(buffer, "count=1 detail=prctl-option ") != NULL
        && strstr(buffer, "count=1 detail=prctl-set-vma ") != NULL,
        "distinct detail: each count=1");

    /* unknown errno */
    linux_diag_reset();
    linux_diag_record(1, 200, 0, args_a);
    length = format_once(buffer, sizeof(buffer), "/bin/x", 42);
    buffer[length] = '\0';
    check(strstr(buffer, " error=E200 ") != NULL, "unknown errno: E200");

    /* exe encoding */
    linux_diag_reset();
    linux_diag_record(999, 38, 0, args_a);
    length = format_once(buffer, sizeof(buffer), "/a b/%x", 42);
    buffer[length] = '\0';
    check(strstr(buffer, " exe=/a%20b/%25x ") != NULL,
        "exe encoding: space and percent");

    /* overflow */
    {
        uint32_t number;
        const char *last;
        linux_diag_reset();
        for (number = 1000; number < 1070; ++number) {
            linux_diag_record(number, 38, 0, args_a);
        }
        length = format_once(buffer, sizeof(buffer), "/bin/x", 42);
        buffer[length] = '\0';
        check(count_lines(buffer, "linuxemu-unsupported ") == 64,
            "overflow: 64 entry lines");
        last = strstr(buffer, "linuxemu-unsupported-overflow");
        check(last != NULL && strcmp(last, "linuxemu-unsupported-overflow "
            "exe=/bin/x pid=42 dropped=6\n") == 0,
            "overflow: final dropped=6 line");
    }

    /* short buffer */
    linux_diag_reset();
    linux_diag_record(999, 38, 0, args_a);
    memset(buffer, 'Z', 32);
    length = format_once(buffer, 10, "/bin/x", 42);
    check(length == 0, "short buffer: returns 0");
    check(buffer[0] == 'Z', "short buffer: nothing written");

    /* reset */
    linux_diag_reset();
    check(format_once(buffer, sizeof(buffer), "/bin/x", 42) == 0,
        "reset: empty");

    /* overflow with a long exe: the runtime's 16 KiB report buffer must not
       lose entry lines or the overflow line */
    {
        static char report[16384 * 8];
        const char *long_exe = "/accounts/1000/shared/documents/My Linux "
            "Projects/rootfs/usr/lib/jvm/java-17-openjdk/bin/java";
        uint32_t number;
        linux_diag_reset();
        for (number = 1000; number < 1070; ++number)
            linux_diag_record(number, 38, 0, args_a);
        /* QNX pids are nine digits on the Passport (pid=171241710). */
        length = format_all(report, sizeof(report), long_exe, 171241710,
            16384);
        report[length] = '\0';
        check(count_lines(report, "linuxemu-unsupported ") == 64,
            "long exe overflow: 64 entry lines");
        check(strstr(report, "linuxemu-unsupported-overflow ") != NULL &&
            strstr(report, " dropped=6\n") != NULL,
            "long exe overflow: overflow line kept");
    }

    /* disabled: recording is a no-op (the LINUXEMU_DIAGNOSTICS-unset path) */
    linux_diag_reset();
    linux_diag_set_enabled(0);
    linux_diag_record(999, 38, 0, args_a);
    check(format_once(buffer, sizeof(buffer), "/bin/x", 42) == 0,
        "disabled: record is a no-op");
    linux_diag_set_enabled(1);

    /* fork while another thread holds the lock */
    {
        int bad = fork_while_recording(200);
        printf("fork_while_recording bad_children=%d\n", bad);
        check(bad == 0, "after fork: child never waits on an inherited lock");
    }

    printf("diagnostics_test_failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
