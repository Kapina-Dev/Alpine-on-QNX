/*
 * prctl state across fork and exec (guest fixture, musl).
 *
 * Parent: sets NO_NEW_PRIVS and PDEATHSIG, then forks.  The child must see
 * NO_NEW_PRIVS still set (check 2) and PDEATHSIG cleared (check 3), as on
 * Linux.  The parent then execs itself with an argument; the new image must
 * still see NO_NEW_PRIVS (check 5).  Exit 0 when every check holds; a failing
 * check prints its number on stderr and is the exit status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#endif

static int fail(int check)
{
    fprintf(stderr, "prctl-exec: check %d failed\n", check);
    return check;
}

static int pdeathsig(void)
{
    int value = -1;
    if (prctl(PR_GET_PDEATHSIG, &value, 0, 0, 0) != 0) return -1;
    return value;
}

int main(int argc, char **argv)
{
    pid_t child;
    int status;
    if (argc > 1) {
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return fail(5);
        return 0;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return fail(1);
    if (prctl(PR_SET_PDEATHSIG, 15, 0, 0, 0) != 0) return fail(1);
    if (pdeathsig() != 15) return fail(1);
    child = fork();
    if (child < 0) return fail(4);
    if (child == 0) {
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) _exit(fail(2));
        if (pdeathsig() != 0) _exit(fail(3));
        _exit(0);
    }
    if (waitpid(child, &status, 0) != child) return fail(4);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return WIFEXITED(status) ? WEXITSTATUS(status) : fail(4);
    execl(argv[0], argv[0], "exec", (char *)0);
    return fail(6);
}
