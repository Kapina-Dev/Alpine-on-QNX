/*
 * Host unit test for src/guest_fd.c (the guest fd record table).
 *
 *   cc -Isrc -Wall -Wextra -pthread -o build/guest-fd-test \
 *       tests/guest-fd-test.c src/guest_fd.c
 */
#include "guest_fd.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;
static int releases;

#define CHECK(name, condition) do { \
    if (!(condition)) { \
        printf("FAIL %s: %s (line %d)\n", name, #condition, __LINE__); \
        ++failures; \
    } \
} while (0)

static void count_release(struct guest_fd_object *object)
{
    (void)object;
    ++releases;
}

static const struct guest_fd_ops counting_ops = {
    0, 0, 0, 0, 0, count_release, 0, 0
};

static struct guest_fd_object *new_object(int backing_fd)
{
    return guest_fd_object_new(GUEST_FD_WAKE_EVENTFD, &counting_ops,
        backing_fd, 0);
}

static void attach_then_acquire(void)
{
    int p[2];
    struct guest_fd_object *object, *found;
    if (pipe(p) != 0) { ++failures; return; }
    releases = 0;
    object = new_object(p[0]);
    CHECK("attach_then_acquire", object != 0);
    CHECK("attach_then_acquire", guest_fd_attach(p[0], object) == 0);
    guest_fd_put(object);
    CHECK("attach_then_acquire", guest_fd_is_typed(p[0]));
    found = guest_fd_acquire(p[0]);
    CHECK("attach_then_acquire", found == object);
    CHECK("attach_then_acquire",
        found != 0 && guest_fd_object_type(found) == GUEST_FD_WAKE_EVENTFD);
    if (found != 0) guest_fd_put(found);
    guest_fd_clear(p[0]);
    CHECK("attach_then_acquire", guest_fd_acquire(p[0]) == 0);
    CHECK("attach_then_acquire", !guest_fd_is_typed(p[0]));
    CHECK("attach_then_acquire", releases == 1);
    close(p[0]); close(p[1]);
}

static void duplicate_shares_object(void)
{
    int p[2], d;
    struct guest_fd_object *object, *a, *b;
    if (pipe(p) != 0) { ++failures; return; }
    releases = 0;
    object = new_object(p[0]);
    guest_fd_attach(p[0], object);
    guest_fd_put(object);
    d = dup(p[0]);
    CHECK("duplicate_shares_object", guest_fd_duplicate(p[0], d) == 0);
    a = guest_fd_acquire(p[0]);
    b = guest_fd_acquire(d);
    CHECK("duplicate_shares_object", a != 0 && a == b);
    if (a != 0) guest_fd_put(a);
    if (b != 0) guest_fd_put(b);
    guest_fd_clear(p[0]);
    close(p[0]);
    CHECK("duplicate_shares_object", releases == 0);
    b = guest_fd_acquire(d);
    CHECK("duplicate_shares_object", b == object);
    if (b != 0) guest_fd_put(b);
    guest_fd_clear(d);
    close(d);
    CHECK("duplicate_shares_object", releases == 1);
    close(p[1]);
}

static void identity_mismatch_drops_record(void)
{
    int p1[2], p2[2];
    struct guest_fd_object *object;
    if (pipe(p1) != 0 || pipe(p2) != 0) { ++failures; return; }
    releases = 0;
    object = new_object(p1[0]);
    guest_fd_attach(p1[0], object);
    guest_fd_put(object);
    /* Replace the number behind the table's back. */
    dup2(p2[0], p1[0]);
    CHECK("identity_mismatch_drops_record", guest_fd_acquire(p1[0]) == 0);
    CHECK("identity_mismatch_drops_record", releases == 1);
    CHECK("identity_mismatch_drops_record", !guest_fd_is_typed(p1[0]));
    close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
}

static void fd_above_256(void)
{
    int p[2], high;
    struct guest_fd_object *object, *found;
    if (pipe(p) != 0) { ++failures; return; }
    releases = 0;
    high = dup2(p[0], 300);
    CHECK("fd_above_256", high == 300);
    object = new_object(high);
    CHECK("fd_above_256", guest_fd_attach(high, object) == 0);
    guest_fd_put(object);
    found = guest_fd_acquire(high);
    CHECK("fd_above_256", found == object);
    if (found != 0) guest_fd_put(found);
    guest_fd_clear(high);
    CHECK("fd_above_256", releases == 1);
    close(high); close(p[0]); close(p[1]);
}

static void hide_moves_above_base(void)
{
    int p[2], hidden, flags;
    if (pipe(p) != 0) { ++failures; return; }
    hidden = guest_fd_hide(p[1]);
    CHECK("hide_moves_above_base", hidden >= guest_fd_hidden_base());
    flags = fcntl(hidden, F_GETFD);
    CHECK("hide_moves_above_base", flags >= 0 && (flags & FD_CLOEXEC) != 0);
    CHECK("hide_moves_above_base", fcntl(p[1], F_GETFD) < 0);
    close(hidden); close(p[0]);
}

static void path_record_shared_across_dup(void)
{
    char path[64];
    int d, copy;
    struct guest_fd_object *object;
    d = open("/", O_RDONLY);
    if (d < 0) { ++failures; return; }
    CHECK("path_record_shared_across_dup", guest_fd_record_path(d, "/") == 0);
    CHECK("path_record_shared_across_dup", !guest_fd_is_typed(d));
    copy = dup(d);
    guest_fd_duplicate(d, copy);
    guest_fd_clear(d);
    close(d);
    CHECK("path_record_shared_across_dup",
        guest_fd_path(copy, path, sizeof(path)) == 0 && strcmp(path, "/") == 0);
    object = guest_fd_acquire(copy);
    if (object != 0) {
        uint64_t inode;
        const char *name;
        int64_t offset;
        CHECK("path_record_shared_across_dup",
            guest_fd_object_directory_entry(object, &inode, &name,
                &offset) == 1);
        guest_fd_put(object);
    } else CHECK("path_record_shared_across_dup", object != 0);
    CHECK("path_record_shared_across_dup",
        guest_fd_path(copy, path, 1) != 0);
    guest_fd_clear(copy);
    CHECK("path_record_shared_across_dup",
        guest_fd_path(copy, path, sizeof(path)) != 0);
    close(copy);
}

static void stale_slot_cleared_on_create(void)
{
    int p[2], q[2], n;
    struct guest_fd_object *object;
    if (pipe(p) != 0) { ++failures; return; }
    releases = 0;
    n = p[0];
    object = new_object(n);
    guest_fd_attach(n, object);
    guest_fd_put(object);
    /* A creation path that forgot to clear: close natively, reuse N. */
    close(n);
    if (pipe(q) != 0) { ++failures; return; }
    if (q[0] != n) { close(q[0]); q[0] = dup2(q[1], n); }
    guest_fd_received(n);
    CHECK("stale_slot_cleared_on_create", releases == 1);
    CHECK("stale_slot_cleared_on_create", guest_fd_acquire(n) == 0);
    CHECK("stale_slot_cleared_on_create", !guest_fd_is_typed(n));
    close(n); close(p[1]); close(q[1]);
}

/* A typed test object that carries a hidden fd across exec. */
struct carried { int hidden; };
static int restored_hidden, restored_second;

static void carried_release(struct guest_fd_object *object)
{
    struct carried *data = guest_fd_object_data(object);
    close(data->hidden);
}

static int carried_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    struct carried *data = guest_fd_object_data(object);
    (void)fd;
    return snprintf(buffer, size, "%d:", data->hidden);
}

static const struct guest_fd_ops carried_ops = {
    0, 0, 0, 0, 0, carried_release, carried_serialize, 0
};

static int carried_restore(int fd, int hidden_fd, int second_hidden_fd,
    const char *host_path)
{
    struct guest_fd_object *object;
    (void)host_path;
    restored_second = second_hidden_fd;
    object = guest_fd_object_new(GUEST_FD_WAKE_EVENTFD, &carried_ops, fd,
        sizeof(struct carried));
    if (object == 0) return -1;
    ((struct carried *)guest_fd_object_data(object))->hidden = hidden_fd;
    restored_hidden = hidden_fd;
    guest_fd_attach(fd, object);
    guest_fd_put(object);
    return 0;
}

static void exec_records_round_trip(void)
{
    char records[512], path[64];
    int d, p[2], hidden, flags;
    struct guest_fd_object *object, *found;
    d = open("/", O_RDONLY);
    if (d < 0 || pipe(p) != 0) { ++failures; return; }
    guest_fd_register_restore(GUEST_FD_WAKE_EVENTFD, carried_restore);
    guest_fd_record_path(d, "/");
    hidden = guest_fd_hide(p[1]);
    object = guest_fd_object_new(GUEST_FD_WAKE_EVENTFD, &carried_ops, p[0],
        sizeof(struct carried));
    ((struct carried *)guest_fd_object_data(object))->hidden = hidden;
    guest_fd_attach(p[0], object);
    guest_fd_put(object);
    CHECK("exec_records_round_trip",
        guest_fd_exec_records(records, sizeof(records)) > 0);
    CHECK("exec_records_round_trip",
        strncmp(records, "LINUXEMU_FD_RECORDS=", 20) == 0);
    CHECK("exec_records_round_trip", guest_fd_exec_prepare() == 0);
    flags = fcntl(hidden, F_GETFD);
    CHECK("exec_records_round_trip", flags >= 0 && (flags & FD_CLOEXEC) == 0);
    /* Simulate the new image: the table is empty and the guest fds are
       still open.  Clearing released the old object, which closed its
       hidden fd, so stand up a fresh inherited one. */
    guest_fd_clear(d);
    guest_fd_clear(p[0]);
    hidden = guest_fd_hide(dup(p[0]));
    snprintf(records, sizeof(records), "%d:%d:-1:2f;%d:%d:%d:;", d,
        (int)GUEST_FD_PATH, p[0], (int)GUEST_FD_WAKE_EVENTFD, hidden);
    CHECK("exec_records_round_trip", guest_fd_restore_records(records) == 2);
    CHECK("exec_records_round_trip",
        guest_fd_path(d, path, sizeof(path)) == 0 && strcmp(path, "/") == 0);
    found = guest_fd_acquire(p[0]);
    CHECK("exec_records_round_trip", found != 0 &&
        guest_fd_object_type(found) == GUEST_FD_WAKE_EVENTFD);
    if (found != 0) guest_fd_put(found);
    CHECK("exec_records_round_trip", restored_hidden == hidden);
    flags = fcntl(hidden, F_GETFD);
    CHECK("exec_records_round_trip", flags >= 0 && (flags & FD_CLOEXEC) != 0);
    guest_fd_clear(d);
    guest_fd_clear(p[0]);
    close(d); close(p[0]);
}

/* Body "hidden:second:": both hidden fds survive exec and are restored
   close-on-exec again. */
struct carried_two { int hidden, second; };

static void carried_two_release(struct guest_fd_object *object)
{
    struct carried_two *data = guest_fd_object_data(object);
    close(data->hidden);
    close(data->second);
}

static int carried_two_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    struct carried_two *data = guest_fd_object_data(object);
    (void)fd;
    return snprintf(buffer, size, "%d:%d:", data->hidden, data->second);
}

static const struct guest_fd_ops carried_two_ops = {
    0, 0, 0, 0, 0, carried_two_release, carried_two_serialize, 0
};

static void exec_records_two_hidden(void)
{
    char records[512], expected[64];
    int p[2], hidden, second, flags;
    struct guest_fd_object *object;
    struct carried_two *data;
    if (pipe(p) != 0) { ++failures; return; }
    guest_fd_register_restore(GUEST_FD_EVENTFD, carried_restore);
    hidden = guest_fd_hide(p[1]);
    second = guest_fd_hide(dup(p[0]));
    object = guest_fd_object_new(GUEST_FD_EVENTFD, &carried_two_ops, p[0],
        sizeof(struct carried_two));
    data = guest_fd_object_data(object);
    data->hidden = hidden;
    data->second = second;
    guest_fd_attach(p[0], object);
    guest_fd_put(object);
    snprintf(expected, sizeof(expected), "%d:%d:%d:%d:;", p[0],
        (int)GUEST_FD_EVENTFD, hidden, second);
    CHECK("exec_records_two_hidden",
        guest_fd_exec_records(records, sizeof(records)) > 0 &&
        strstr(records, expected) != 0);
    CHECK("exec_records_two_hidden", guest_fd_exec_prepare() == 0);
    flags = fcntl(hidden, F_GETFD);
    CHECK("exec_records_two_hidden", flags >= 0 && (flags & FD_CLOEXEC) == 0);
    flags = fcntl(second, F_GETFD);
    CHECK("exec_records_two_hidden", flags >= 0 && (flags & FD_CLOEXEC) == 0);
    /* New image: stand up fresh inherited hidden fds (clearing closed the
       old ones) and restore the record. */
    guest_fd_clear(p[0]);
    hidden = guest_fd_hide(dup(p[0]));
    second = guest_fd_hide(dup(p[0]));
    fcntl(hidden, F_SETFD, 0);
    fcntl(second, F_SETFD, 0);
    snprintf(records, sizeof(records), "%d:%d:%d:%d:;", p[0],
        (int)GUEST_FD_EVENTFD, hidden, second);
    restored_hidden = restored_second = -2;
    CHECK("exec_records_two_hidden", guest_fd_restore_records(records) == 1);
    CHECK("exec_records_two_hidden",
        restored_hidden == hidden && restored_second == second);
    flags = fcntl(second, F_GETFD);
    CHECK("exec_records_two_hidden", flags >= 0 && (flags & FD_CLOEXEC) != 0);
    /* A one-number body still restores with no second fd. */
    guest_fd_clear(p[0]);
    hidden = guest_fd_hide(dup(p[0]));
    snprintf(records, sizeof(records), "%d:%d:%d:;", p[0],
        (int)GUEST_FD_EVENTFD, hidden);
    CHECK("exec_records_two_hidden", guest_fd_restore_records(records) == 1 &&
        restored_hidden == hidden && restored_second == -1);
    /* Malformed second numbers are skipped. */
    CHECK("exec_records_two_hidden",
        guest_fd_restore_records("5:3:1:x:;5:3:1:2:ab;5:3:1:2:3:;") == 0);
    guest_fd_clear(p[0]);
    close(p[0]);
}

static void exec_records_alias_shares_object(void)
{
    char records[512];
    int d, copy;
    struct guest_fd_object *a, *b;
    d = open("/", O_RDONLY);
    if (d < 0) { ++failures; return; }
    copy = dup(d);
    guest_fd_record_path(d, "/");
    guest_fd_duplicate(d, copy);
    CHECK("exec_records_alias_shares_object",
        guest_fd_exec_records(records, sizeof(records)) > 0);
    guest_fd_clear(d);
    guest_fd_clear(copy);
    CHECK("exec_records_alias_shares_object",
        guest_fd_restore_records(records + 20) == 2);
    a = guest_fd_acquire(d);
    b = guest_fd_acquire(copy);
    CHECK("exec_records_alias_shares_object", a != 0 && a == b);
    if (a != 0) guest_fd_put(a);
    if (b != 0) guest_fd_put(b);
    guest_fd_clear(d); guest_fd_clear(copy);
    close(d); close(copy);
}

static void nofile_virtual_limit(void)
{
    uint64_t soft, hard, base = (uint64_t)guest_fd_hidden_base();
    guest_fd_nofile_get(&soft, &hard);
    CHECK("nofile_virtual_limit", soft == base && hard == base);
    CHECK("nofile_virtual_limit", guest_fd_nofile_set(base + 100, base + 100) == 0);
    guest_fd_nofile_get(&soft, &hard);
    CHECK("nofile_virtual_limit", soft == base && hard == base);
    CHECK("nofile_virtual_limit", guest_fd_nofile_set(10, 20) == 0);
    guest_fd_nofile_get(&soft, &hard);
    CHECK("nofile_virtual_limit", soft == 10 && hard == 20);
    CHECK("nofile_virtual_limit", guest_fd_nofile_set(30, 20) != 0);
    CHECK("nofile_virtual_limit", guest_fd_is_hidden((int)base) &&
        !guest_fd_is_hidden((int)base - 1));
    guest_fd_nofile_set(base, base);
}

static void exec_records_carry_limit(void)
{
    char records[256];
    uint64_t soft, hard, base = (uint64_t)guest_fd_hidden_base();
    guest_fd_nofile_set(50, 60);
    CHECK("exec_records_carry_limit",
        guest_fd_exec_records(records, sizeof(records)) > 0 &&
        strstr(records, "limit:50:60;") != 0);
    guest_fd_nofile_set(base, base);
    CHECK("exec_records_carry_limit",
        guest_fd_restore_records("limit:50:60;") == 1);
    guest_fd_nofile_get(&soft, &hard);
    CHECK("exec_records_carry_limit", soft == 50 && hard == 60);
    CHECK("exec_records_carry_limit",
        guest_fd_restore_records("limit:x:1;limit:5;limit:1:2:3;") == 0);
    guest_fd_nofile_set(base, base);
}

static void received_fd_adopts_known_object(void)
{
    int p[2], q[2], received, native_copy, d, dir_copy;
    struct guest_fd_object *object, *a, *b;
    if (pipe(p) != 0 || pipe(q) != 0) { ++failures; return; }
    object = new_object(p[0]);
    guest_fd_attach(p[0], object);
    guest_fd_put(object);
    /* A number that arrived over SCM_RIGHTS for a known eventfd pipe. */
    received = dup(p[0]);
    guest_fd_received(received);
    a = guest_fd_acquire(p[0]);
    b = guest_fd_acquire(received);
    CHECK("received_fd_adopts_known_object", a != 0 && a == b);
    if (a != 0) guest_fd_put(a);
    if (b != 0) guest_fd_put(b);
    /* A native pipe is never adopted. */
    native_copy = dup(q[0]);
    guest_fd_received(native_copy);
    CHECK("received_fd_adopts_known_object",
        guest_fd_acquire(native_copy) == 0);
    /* Nor is a path record: two opens of one directory share an inode
       but not an open-file description. */
    d = open("/", O_RDONLY);
    guest_fd_record_path(d, "/");
    dir_copy = open("/", O_RDONLY);
    guest_fd_received(dir_copy);
    CHECK("received_fd_adopts_known_object", guest_fd_acquire(dir_copy) == 0);
    guest_fd_clear(received); guest_fd_clear(p[0]); guest_fd_clear(d);
    close(received); close(p[0]); close(p[1]); close(native_copy);
    close(q[0]); close(q[1]); close(d); close(dir_copy);
}

static void exec_records_reject_malformed(void)
{
    static char huge[100001];
    const char *bad[] = { "5:2:x:;", "5:99:-1:;", "5:1:-1:zz;", "7",
        "-3:1:-1:2f;", "5:1:-1:2;", "999999:1:-1:2f;", huge };
    size_t i;
    memset(huge, '9', sizeof(huge) - 1);
    for (i = 0; i != sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK("exec_records_reject_malformed",
            guest_fd_restore_records(bad[i]) == 0);
    CHECK("exec_records_reject_malformed", guest_fd_restore_records(0) == 0);
}

int main(void)
{
    if (guest_fd_initialize() != 0) {
        printf("FAIL initialize\n");
        return 1;
    }
    attach_then_acquire();
    duplicate_shares_object();
    identity_mismatch_drops_record();
    fd_above_256();
    hide_moves_above_base();
    path_record_shared_across_dup();
    stale_slot_cleared_on_create();
    exec_records_round_trip();
    exec_records_alias_shares_object();
    exec_records_two_hidden();
    exec_records_reject_malformed();
    nofile_virtual_limit();
    exec_records_carry_limit();
    received_fd_adopts_known_object();
    printf("guest_fd_test_failures=%d\n", failures);
    return failures != 0;
}
