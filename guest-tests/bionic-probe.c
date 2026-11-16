/*
 * Static bionic (Android NDK) guest probe for Linuxemu.
 *
 * Built ARM-mode, static, against API 23 by scripts/build-bionic-probe.sh.
 * Run under Linuxemu with LINUXEMU_SYSTRACE=1 to see which syscalls bionic
 * needs. Each step prints one line before and after, so if one kills the
 * process the last "begin" line names it. Expected failures (epoll, /proc,
 * properties) are recorded, not treated as errors: this is a gap survey.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void begin(const char *step)
{
    printf("step=%s begin\n", step);
    fflush(stdout);
}

static void end(const char *step, int ok, int saved_errno, const char *detail)
{
    printf("step=%s %s errno=%d (%s) %s\n", step, ok ? "ok" : "fail",
        saved_errno, saved_errno ? strerror(saved_errno) : "-",
        detail ? detail : "");
    fflush(stdout);
    if (!ok) {
        failures++;
    }
}

static void *thread_main(void *arg)
{
    *(int *)arg = (int)gettid();
    return arg;
}

int main(void)
{
    char detail[128];
    struct timespec ts;
    int fd;
    int result;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("probe=bionic-probe api=%d\n", __ANDROID_API__);

    begin("getpid_gettid");
    snprintf(detail, sizeof(detail), "pid=%d tid=%d", (int)getpid(),
        (int)gettid());
    end("getpid_gettid", 1, 0, detail);

    begin("clock_monotonic");
    errno = 0;
    result = clock_gettime(CLOCK_MONOTONIC, &ts);
    end("clock_monotonic", result == 0, result ? errno : 0, NULL);

    begin("clock_boottime");
    errno = 0;
    result = clock_gettime(CLOCK_BOOTTIME, &ts);
    end("clock_boottime", result == 0, result ? errno : 0, NULL);

    begin("malloc_large");
    {
        size_t size = 8u * 1024u * 1024u;
        char *block = malloc(size);
        if (block != NULL) {
            memset(block, 0x5a, size);
        }
        end("malloc_large", block != NULL && block[size - 1] == 0x5a,
            block ? 0 : errno, NULL);
        free(block);
    }

    begin("pthread_create_join");
    {
        pthread_t thread;
        int tid = 0;
        void *ret = NULL;
        result = pthread_create(&thread, NULL, thread_main, &tid);
        if (result == 0) {
            result = pthread_join(thread, &ret);
        }
        snprintf(detail, sizeof(detail), "thread_tid=%d", tid);
        end("pthread_create_join", result == 0 && tid > 0, result, detail);
    }

    begin("prctl_name");
    {
        char name[17] = { 0 };
        errno = 0;
        result = prctl(PR_SET_NAME, "bionic-probe", 0, 0, 0);
        if (result == 0) {
            result = prctl(PR_GET_NAME, name, 0, 0, 0);
        }
        snprintf(detail, sizeof(detail), "name=%s", name);
        end("prctl_name", result == 0 && strcmp(name, "bionic-probe") == 0,
            result ? errno : 0, detail);
    }

    begin("proc_self_maps");
    errno = 0;
    fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    end("proc_self_maps", fd >= 0, fd < 0 ? errno : 0, NULL);
    if (fd >= 0) {
        close(fd);
    }

    begin("system_property_get");
    {
        char value[PROP_VALUE_MAX] = { 0 };
        int length = __system_property_get("ro.build.version.sdk", value);
        snprintf(detail, sizeof(detail), "length=%d value=%s", length, value);
        end("system_property_get", 1, 0, detail);
    }

    begin("eventfd");
    errno = 0;
    fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    end("eventfd", fd >= 0, fd < 0 ? errno : 0, NULL);
    if (fd >= 0) {
        close(fd);
    }

    begin("epoll_create1");
    errno = 0;
    fd = epoll_create1(EPOLL_CLOEXEC);
    end("epoll_create1", fd >= 0, fd < 0 ? errno : 0, NULL);
    if (fd >= 0) {
        close(fd);
    }

    begin("fork_wait");
    {
        pid_t child = fork();
        int status = 0;
        if (child == 0) {
            _exit(7);
        }
        errno = 0;
        result = child > 0 ? (int)waitpid(child, &status, 0) : -1;
        snprintf(detail, sizeof(detail), "child=%d status=0x%x",
            (int)child, status);
        end("fork_wait", result == child && WIFEXITED(status)
            && WEXITSTATUS(status) == 7, result < 0 ? errno : 0, detail);
    }

    printf("bionic_probe_failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
