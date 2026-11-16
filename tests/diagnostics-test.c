/*
 * Host unit test for src/linux_diagnostics.c (pure logic).
 *
 *   cc -Isrc -Wall -Wextra -o diagnostics-test tests/diagnostics-test.c \
 *       src/linux_diagnostics.c
 */
#include "linux_diagnostics.h"

#include <stdio.h>
#include <string.h>

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

int main(void)
{
    static char buffer[16384];
    static const uint32_t args_a[6] = { 1, 2, 3, 4, 5, 6 };
    static const uint32_t args_b[6] = { 9, 9, 9, 9, 9, 9 };
    size_t length;

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
    length = linux_diag_format(buffer, sizeof(buffer), "/bin/x", 42);
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
    length = linux_diag_format(buffer, sizeof(buffer), "/bin/x", 42);
    buffer[length] = '\0';
    check(count_lines(buffer, "linuxemu-unsupported ") == 2,
        "distinct detail: two lines");
    check(strstr(buffer, "count=1 detail=prctl-option ") != NULL
        && strstr(buffer, "count=1 detail=prctl-set-vma ") != NULL,
        "distinct detail: each count=1");

    /* unknown errno */
    linux_diag_reset();
    linux_diag_record(1, 200, 0, args_a);
    length = linux_diag_format(buffer, sizeof(buffer), "/bin/x", 42);
    buffer[length] = '\0';
    check(strstr(buffer, " error=E200 ") != NULL, "unknown errno: E200");

    /* exe encoding */
    linux_diag_reset();
    linux_diag_record(999, 38, 0, args_a);
    length = linux_diag_format(buffer, sizeof(buffer), "/a b/%x", 42);
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
        length = linux_diag_format(buffer, sizeof(buffer), "/bin/x", 42);
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
    length = linux_diag_format(buffer, 10, "/bin/x", 42);
    check(length == 0, "short buffer: returns 0");
    check(buffer[0] == 'Z', "short buffer: nothing written");

    /* reset */
    linux_diag_reset();
    check(linux_diag_format(buffer, sizeof(buffer), "/bin/x", 42) == 0,
        "reset: empty");

    printf("diagnostics_test_failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
