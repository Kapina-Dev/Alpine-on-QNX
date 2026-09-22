#define _QNX_SOURCE 1
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/storage.h>

#define THREAD_COUNT 4
#define STRESS_ITERATIONS 20000
#define WAIT_ITERATIONS 100000

struct thread_state {
    int index;
    void *library_tls;
    uintptr_t tpidrurw;
    uintptr_t tpidruro;
    volatile uintptr_t signal_tpidrurw;
    volatile uintptr_t signal_tpidruro;
    volatile sig_atomic_t ready;
    volatile sig_atomic_t signals;
    int library_tls_stable;
    unsigned long tpidrurw_changes;
    unsigned long tpidruro_changes;
};

static struct thread_state states[THREAD_COUNT];
static volatile sig_atomic_t start_workers;
static volatile sig_atomic_t unexpected_signals;
static volatile uintptr_t unexpected_tpidrurw;
static volatile uintptr_t unexpected_tpidruro;
static void *main_library_tls;
static uintptr_t main_tpidrurw;
static volatile uintptr_t main_signal_tpidrurw;
static volatile uintptr_t main_signal_tpidruro;
static volatile sig_atomic_t main_signals;

static uintptr_t read_tpidrurw(void)
{
    uintptr_t value;
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 2" : "=r" (value));
    return value;
}

static uintptr_t read_tpidruro(void)
{
    uintptr_t value;
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3" : "=r" (value));
    return value;
}

static void signal_handler(int sig, siginfo_t *info, void *context)
{
    uintptr_t read_write = read_tpidrurw();
    uintptr_t read_only = read_tpidruro();
    void *library_tls = __tls();
    int i;

    (void)info;
    (void)context;
    if (sig != SIGUSR1) {
        return;
    }
    if (library_tls == main_library_tls) {
        main_signal_tpidrurw = read_write;
        main_signal_tpidruro = read_only;
        main_signals++;
        return;
    }
    for (i = 0; i < THREAD_COUNT; ++i) {
        if (states[i].library_tls == library_tls) {
            states[i].signal_tpidrurw = read_write;
            states[i].signal_tpidruro = read_only;
            states[i].signals++;
            return;
        }
    }
    unexpected_tpidrurw = read_write;
    unexpected_tpidruro = read_only;
    unexpected_signals++;
}

static void *worker_main(void *argument)
{
    struct thread_state *state = argument;
    int i;

    state->library_tls = __tls();
    state->tpidrurw = read_tpidrurw();
    state->tpidruro = read_tpidruro();
    __sync_synchronize();
    state->ready = 1;

    while (!start_workers) {
        sched_yield();
    }

    state->library_tls_stable = 1;
    for (i = 0; i < STRESS_ITERATIONS; ++i) {
        if (__tls() != state->library_tls) {
            state->library_tls_stable = 0;
        }
        if (read_tpidrurw() != state->tpidrurw) {
            state->tpidrurw_changes++;
        }
        if (read_tpidruro() != state->tpidruro) {
            state->tpidruro_changes++;
        }
        sched_yield();
    }

    for (i = 0; i < WAIT_ITERATIONS && state->signals == 0; ++i) {
        sched_yield();
    }
    return 0;
}

int main(void)
{
    struct sigaction action;
    pthread_t threads[THREAD_COUNT];
    uintptr_t main_tpidruro;
    int created = 0;
    int ready;
    int error;
    int i;
    int j;
    int ok = 1;

    alarm(30);
    memset(states, 0, sizeof(states));
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) {
        perror("sigaction");
        return 1;
    }

    main_library_tls = __tls();
    main_tpidrurw = read_tpidrurw();
    main_tpidruro = read_tpidruro();
    if (raise(SIGUSR1) != 0) {
        perror("raise");
        return 1;
    }
    if (main_signals != 1) {
        ok = 0;
    }
    printf("main library_tls=%p tpidrurw=%p tpidruro=%p "
           "signal_tpidrurw=%p signal_tpidruro=%p signals=%d\n",
        main_library_tls, (void *)main_tpidrurw, (void *)main_tpidruro,
        (void *)main_signal_tpidrurw, (void *)main_signal_tpidruro,
        (int)main_signals);
    fflush(stdout);

    for (i = 0; i < THREAD_COUNT; ++i) {
        states[i].index = i;
        error = pthread_create(&threads[i], 0, worker_main, &states[i]);
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
            if (!states[i].ready) {
                ready--;
            }
        }
        if (ready != created) {
            sched_yield();
        }
    } while (ready != created);

    printf("workers_ready=%d\n", ready);
    fflush(stdout);

    __sync_synchronize();
    for (i = 0; i < created; ++i) {
        if (states[i].library_tls == main_library_tls) {
            ok = 0;
        }
        for (j = 0; j < i; ++j) {
            if (states[i].library_tls == states[j].library_tls) {
                ok = 0;
            }
        }
        error = pthread_kill(threads[i], SIGUSR1);
        if (error != 0) {
            printf("pthread_kill[%d]=%d\n", i, error);
            ok = 0;
        }
    }
    printf("signals_sent=%d unexpected_signals=%d unexpected_tpidrurw=%p "
           "unexpected_tpidruro=%p\n",
        created, (int)unexpected_signals, (void *)unexpected_tpidrurw,
        (void *)unexpected_tpidruro);
    fflush(stdout);

    __sync_synchronize();
    start_workers = 1;
    for (i = 0; i < created; ++i) {
        error = pthread_join(threads[i], 0);
        if (error != 0) {
            printf("pthread_join[%d]=%d\n", i, error);
            ok = 0;
        }
        printf("worker=%d library_tls=%p tpidrurw=%p tpidruro=%p "
               "signal_tpidrurw=%p signal_tpidruro=%p tls_stable=%d "
               "tpidrurw_changes=%lu tpidruro_changes=%lu signals=%d\n",
            i, states[i].library_tls, (void *)states[i].tpidrurw,
            (void *)states[i].tpidruro,
            (void *)states[i].signal_tpidrurw,
            (void *)states[i].signal_tpidruro,
            states[i].library_tls_stable, states[i].tpidrurw_changes,
            states[i].tpidruro_changes, (int)states[i].signals);
        fflush(stdout);
        if (!states[i].library_tls_stable || states[i].signals != 1) {
            ok = 0;
        }
    }

    if (created != THREAD_COUNT || unexpected_signals != 0) {
        ok = 0;
    }
    printf("tls_registers=%s threads=%d iterations=%d unexpected_signals=%d\n",
        ok ? "PASS" : "FAIL", created, STRESS_ITERATIONS,
        (int)unexpected_signals);
    return ok ? 0 : 1;
}
