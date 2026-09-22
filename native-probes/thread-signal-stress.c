#define _QNX_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/storage.h>

#define THREAD_COUNT 4
#define STRESS_ITERATIONS 20000
#define WAIT_ITERATIONS 1000000

struct worker_state {
    int index;
    void *tls;
    volatile sig_atomic_t ready;
    volatile sig_atomic_t signals;
    int error;
    int ok;
};

static pthread_key_t key;
static struct worker_state workers[THREAD_COUNT];
static volatile sig_atomic_t start_workers;
static volatile sig_atomic_t unexpected_signals;

static void signal_handler(int sig, siginfo_t *info, void *context)
{
    void *tls;
    int i;

    (void)info;
    (void)context;
    if (sig != SIGUSR1) {
        return;
    }

    tls = __tls();
    for (i = 0; i < THREAD_COUNT; ++i) {
        if (workers[i].tls == tls) {
            workers[i].signals++;
            return;
        }
    }
    unexpected_signals++;
}

static void *worker_main(void *argument)
{
    struct worker_state *state = argument;
    int token = 0x1000 + state->index;
    int expected_errno = 200 + state->index;
    int i;

    state->error = pthread_setspecific(key, &token);
    if (state->error != 0) {
        state->ready = 1;
        return 0;
    }

    errno = expected_errno;
    state->tls = __tls();
    __sync_synchronize();
    state->ready = 1;

    while (!start_workers) {
        sched_yield();
    }

    state->ok = 1;
    for (i = 0; i < STRESS_ITERATIONS; ++i) {
        if (__tls() != state->tls ||
            pthread_getspecific(key) != &token ||
            errno != expected_errno) {
            state->ok = 0;
            break;
        }
        sched_yield();
    }

    for (i = 0; i < WAIT_ITERATIONS && state->signals == 0; ++i) {
        sched_yield();
    }
    if (state->signals != 1) {
        state->ok = 0;
    }
    return 0;
}

int main(void)
{
    struct sigaction action;
    pthread_t threads[THREAD_COUNT];
    void *main_tls = __tls();
    int created = 0;
    int ready;
    int i;
    int j;
    int error;
    int ok = 1;

    alarm(20);
    memset(workers, 0, sizeof(workers));
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) {
        perror("sigaction");
        return 1;
    }

    error = pthread_key_create(&key, 0);
    if (error != 0) {
        printf("pthread_key_create=%d\n", error);
        return 1;
    }

    for (i = 0; i < THREAD_COUNT; ++i) {
        workers[i].index = i;
        error = pthread_create(&threads[i], 0, worker_main, &workers[i]);
        if (error != 0) {
            printf("pthread_create[%d]=%d\n", i, error);
            ok = 0;
            break;
        }
        created++;
    }

    do {
        ready = created;
        for (i = 0; i < created; ++i) {
            if (!workers[i].ready) {
                ready--;
            }
        }
        if (ready != created) {
            sched_yield();
        }
    } while (ready != created);

    __sync_synchronize();
    for (i = 0; i < created; ++i) {
        if (workers[i].tls == 0 || workers[i].tls == main_tls) {
            ok = 0;
        }
        for (j = 0; j < i; ++j) {
            if (workers[i].tls == workers[j].tls) {
                ok = 0;
            }
        }
        error = pthread_kill(threads[i], SIGUSR1);
        if (error != 0) {
            printf("pthread_kill[%d]=%d\n", i, error);
            ok = 0;
        }
    }

    __sync_synchronize();
    start_workers = 1;
    for (i = 0; i < created; ++i) {
        error = pthread_join(threads[i], 0);
        if (error != 0) {
            printf("pthread_join[%d]=%d\n", i, error);
            ok = 0;
        }
        printf("worker=%d tls=%p stable=%d signals=%d error=%d\n",
            i, workers[i].tls, workers[i].ok,
            (int)workers[i].signals, workers[i].error);
        if (!workers[i].ok || workers[i].error != 0) {
            ok = 0;
        }
    }

    if (created != THREAD_COUNT || unexpected_signals != 0) {
        ok = 0;
    }
    pthread_key_delete(key);
    printf("thread_signal_stress=%s threads=%d iterations=%d unexpected_signals=%d\n",
        ok ? "PASS" : "FAIL", created, STRESS_ITERATIONS,
        (int)unexpected_signals);
    return ok ? 0 : 1;
}
