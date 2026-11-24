/*
 * Phase 4 part 4 Android gate, static: a stripped Thumb NDK program with
 * its own entry point, so bionic's start-up (and NDK r27's
 * "pid <= 65535" check) never runs. It calls the NDK libc.a's syscall()
 * and makes one inline Thumb svc. Built by scripts/build-android-gate.sh.
 *
 * Exit 0: all passed. 2: write through syscall() failed. 3: getpid
 * through syscall() returned <= 0. 4: the inline svc's getpid differs.
 */
#include <sys/syscall.h>
#include <unistd.h>

long syscall(long number, ...);

static long inline_getpid(void)
{
    register long r0 __asm__("r0");
    /* r7 is clang's Thumb frame pointer: save it around the call. ip =
       0x7fff: an svc left unpatched names no real QNX kernel call. */
    __asm__ volatile (
        "push {r7}\n\t"
        "movs r7, %[number]\n\t"
        "movw ip, #0x7fff\n\t"
        "svc #0\n\t"
        "pop {r7}"
        : "=r"(r0) : [number] "i"(__NR_getpid) : "ip", "memory");
    return r0;
}

static void finish(long status)
{
    syscall(__NR_exit_group, status);
    for (;;) {
    }
}

void _start(void)
{
    static const char message[] = "android-syscall write\n";
    long pid;

    if (syscall(__NR_write, 1, message, sizeof(message) - 1) !=
        (long)(sizeof(message) - 1))
        finish(2);
    pid = syscall(__NR_getpid);
    if (pid <= 0) finish(3);
    if (inline_getpid() != pid) finish(4);
    finish(0);
}
