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
    0, 0, 0, 0, 0, count_release, 0
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
    CHECK("path_record_shared_across_dup",
        object != 0 && guest_fd_object_directory(object) != 0);
    if (object != 0) guest_fd_put(object);
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
    printf("guest_fd_test_failures=%d\n", failures);
    return failures != 0;
}
