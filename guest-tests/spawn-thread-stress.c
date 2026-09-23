#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static volatile int stop_workers;
static volatile unsigned worker_progress[4];

static void *worker(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument;
    while (!stop_workers) {
        __sync_add_and_fetch(&worker_progress[index], 1);
        usleep(1000);
    }
    return 0;
}

int main(int argc, char **argv)
{
    pthread_t workers[4];
    int spawn_count = 100;
    int iteration;
    unsigned index;
    if (argc == 2) {
        char *end = 0;
        long value = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || value < 1 || value > 1000)
            return 5;
        spawn_count = (int)value;
    } else if (argc != 1) return 5;
    for (index = 0; index != 4; ++index)
        if (pthread_create(&workers[index], 0, worker,
                (void *)(uintptr_t)index) != 0) return 1;
    for (iteration = 0; iteration != spawn_count; ++iteration) {
        char *arguments[] = { "/bin/true", 0 };
        pid_t child;
        int status;
        int error = posix_spawn(&child, "/bin/true", 0, 0, arguments,
            environ);
        if (error != 0) {
            fprintf(stderr, "posix_spawn iteration=%d error=%d\n",
                iteration, error);
            return 2;
        }
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) return 3;
    }
    stop_workers = 1;
    for (index = 0; index != 4; ++index) {
        if (pthread_join(workers[index], 0) != 0 ||
            worker_progress[index] == 0) return 4;
    }
    printf("spawn_thread_stress=PASS spawns=%d workers=4\n", spawn_count);
    return 0;
}
