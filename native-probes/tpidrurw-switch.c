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
    uintptr_t host_tpidrurw;
    uintptr_t guest_tpidrurw;
    volatile uintptr_t signal_entry_tpidrurw;
    volatile uintptr_t signal_host_tpidrurw;
    volatile sig_atomic_t ready;
    volatile sig_atomic_t signals;
    unsigned long scheduling_mismatches;
    int library_tls_stable;
    int restored;
};

static struct thread_state states[THREAD_COUNT];
static uintptr_t native_tpidrurw;
static volatile sig_atomic_t start_workers;
static volatile sig_atomic_t unexpected_signals;

static uintptr_t read_tpidrurw(void)
{
    uintptr_t value;
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 2" : "=r" (value));
    return value;
}

static void write_tpidrurw(uintptr_t value)
{
    __asm__ volatile (
        "mcr p15, 0, %0, c13, c0, 2\n"
        "isb\n"
        : : "r" (value) : "memory");
}

static void signal_handler(int sig, siginfo_t *info, void *context)
{
    uintptr_t guest_value;
    void *library_tls;
    int i;

    (void)info;
    (void)context;
    if (sig != SIGUSR1) {
        return;
    }

    guest_value = read_tpidrurw();
    write_tpidrurw(native_tpidrurw);
    library_tls = __tls();

    for (i = 0; i < THREAD_COUNT; ++i) {
        if (states[i].library_tls == library_tls) {
            states[i].signal_entry_tpidrurw = guest_value;
            states[i].signal_host_tpidrurw = read_tpidrurw();
            states[i].signals++;
            write_tpidrurw(states[i].guest_tpidrurw);
            return;
        }
    }

    unexpected_signals++;
    write_tpidrurw(guest_value);
}

static void *worker_main(void *argument)
{
    struct thread_state *state = argument;
    int i;

    state->library_tls = __tls();
    state->host_tpidrurw = read_tpidrurw();
    state->guest_tpidrurw = 0x4c580100u + (unsigned)state->index;
    state->library_tls_stable = 1;

    write_tpidrurw(state->guest_tpidrurw);
    __sync_synchronize();
    state->ready = 1;

    while (!start_workers) {
        sched_yield();
    }
    for (i = 0; i < STRESS_ITERATIONS; ++i) {
        if (read_tpidrurw() != state->guest_tpidrurw) {
            state->scheduling_mismatches++;
        }
        sched_yield();
    }
    for (i = 0; i < WAIT_ITERATIONS && state->signals == 0; ++i) {
        sched_yield();
    }

    write_tpidrurw(state->host_tpidrurw);
    state->restored = read_tpidrurw() == state->host_tpidrurw;
    if (__tls() != state->library_tls) {
        state->library_tls_stable = 0;
    }
    return 0;
}

int main(void)
{
    struct sigaction action;
    pthread_t threads[THREAD_COUNT];
    int created = 0;
    int ready;
    int error;
    int i;
    int ok = 1;

    alarm(30);
    memset(states, 0, sizeof(states));
    memset(&action, 0, sizeof(action));
    native_tpidrurw = read_tpidrurw();
    action.sa_sigaction = signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) {
        perror("sigaction");
        return 1;
    }

    printf("main native_tpidrurw=%p library_tls=%p\n",
        (void *)native_tpidrurw, __tls());
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

    for (i = 0; i < created; ++i) {
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
        printf("worker=%d library_tls=%p host=%p guest=%p signal_entry=%p "
               "signal_host=%p scheduling_mismatches=%lu tls_stable=%d "
               "signals=%d restored=%d\n",
            i, states[i].library_tls, (void *)states[i].host_tpidrurw,
            (void *)states[i].guest_tpidrurw,
            (void *)states[i].signal_entry_tpidrurw,
            (void *)states[i].signal_host_tpidrurw,
            states[i].scheduling_mismatches,
            states[i].library_tls_stable, (int)states[i].signals,
            states[i].restored);
        if (states[i].host_tpidrurw != native_tpidrurw ||
            states[i].signal_entry_tpidrurw != states[i].guest_tpidrurw ||
            states[i].signal_host_tpidrurw != native_tpidrurw ||
            states[i].scheduling_mismatches != 0 ||
            !states[i].library_tls_stable || states[i].signals != 1 ||
            !states[i].restored) {
            ok = 0;
        }
    }

    if (created != THREAD_COUNT || unexpected_signals != 0 ||
        read_tpidrurw() != native_tpidrurw) {
        ok = 0;
    }
    printf("tpidrurw_switch=%s threads=%d iterations=%d unexpected_signals=%d\n",
        ok ? "PASS" : "FAIL", created, STRESS_ITERATIONS,
        (int)unexpected_signals);
    return ok ? 0 : 1;
}
