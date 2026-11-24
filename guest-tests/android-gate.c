/*
 * Phase 4 part 4 Android gate, dynamic: run by Android 6.0's own
 * /system/bin/linker, which loads libc, libm and libutils (and libutils'
 * seven dependencies) from /system/lib before any constructor runs. Built
 * Thumb with NDK r27c by scripts/build-android-gate.sh.
 *
 * Exit 0: main ran and raw syscall() works. 2: getpid through syscall()
 * differs from getpid(). 3: write through syscall() failed.
 */
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    static const char message[] = "android-gate syscall\n";

    printf("android-gate main\n");
    fflush(stdout);
    if (syscall(__NR_getpid) != getpid()) return 2;
    if (syscall(__NR_write, 1, message, sizeof(message) - 1) !=
        (long)(sizeof(message) - 1))
        return 3;
    return 0;
}
