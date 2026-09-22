#define _QNX_SOURCE 1
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/storage.h>

#define THREAD_COUNT 4
#define READ_ITERATIONS 1000

struct thread_state {
    int index;
    void *library_tls;
    uintptr_t guest_tls;
    volatile sig_atomic_t ready;
    volatile sig_atomic_t traps;
    unsigned long mismatches;
};

static struct thread_state states[THREAD_COUNT];
static volatile sig_atomic_t start_workers;
static volatile sig_atomic_t unexpected_traps;

extern uintptr_t emulated_tls_read(void);
extern char linuxemu_tls_fault[];
extern char linuxemu_tls_resume[];

#if defined(PROBE_THUMB)
__asm__(
    ".text\n"
    ".align 2\n"
    ".thumb\n"
    ".thumb_func\n"
    ".global emulated_tls_read\n"
    ".type emulated_tls_read, %function\n"
    "emulated_tls_read:\n"
    ".global linuxemu_tls_fault\n"
    "linuxemu_tls_fault:\n"
    ".short 0xde00\n"
    ".global linuxemu_tls_resume\n"
    "linuxemu_tls_resume:\n"
    "bx lr\n"
    ".size emulated_tls_read, .-emulated_tls_read\n");
#else
__asm__(
    ".text\n"
    ".align 2\n"
    ".arm\n"
    ".global emulated_tls_read\n"
    ".type emulated_tls_read, %function\n"
    "emulated_tls_read:\n"
    ".global linuxemu_tls_fault\n"
    "linuxemu_tls_fault:\n"
    ".word 0xe7f000f0\n"
    ".global linuxemu_tls_resume\n"
    "linuxemu_tls_resume:\n"
    "bx lr\n"
    ".size emulated_tls_read, .-emulated_tls_read\n");
#endif

static void trap_handler(int sig, siginfo_t *info, void *argument)
{
    ucontext_t *context = argument;
    uintptr_t pc = context->uc_mcontext.cpu.gpr[15];
    void *library_tls;
    int i;

    (void)info;
    if (sig != SIGILL ||
        (pc & ~(uintptr_t)1) !=
            ((uintptr_t)linuxemu_tls_fault & ~(uintptr_t)1)) {
        unexpected_traps++;
        _exit(120);
    }

    library_tls = __tls();
    for (i = 0; i < THREAD_COUNT; ++i) {
        if (states[i].library_tls == library_tls) {
            context->uc_mcontext.cpu.gpr[0] = (uint32_t)states[i].guest_tls;
            context->uc_mcontext.cpu.gpr[15] =
                (uint32_t)((uintptr_t)linuxemu_tls_resume & ~(uintptr_t)1);
            states[i].traps++;
            return;
        }
    }
    unexpected_traps++;
    _exit(121);
}

static void *worker_main(void *argument)
{
    struct thread_state *state = argument;
    uintptr_t value;
    int i;

    state->library_tls = __tls();
    state->guest_tls = 0x71000000u + ((unsigned)state->index << 16);
    __sync_synchronize();
    state->ready = 1;
    while (!start_workers) {
        sched_yield();
    }

    for (i = 0; i < READ_ITERATIONS; ++i) {
        value = emulated_tls_read();
        if (value != state->guest_tls) {
            state->mismatches++;
        }
        sched_yield();
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
    action.sa_sigaction = trap_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, 0) != 0) {
        perror("sigaction");
        return 1;
    }

    printf("fault=%p resume=%p\n",
        (void *)linuxemu_tls_fault, (void *)linuxemu_tls_resume);
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
    __sync_synchronize();
    start_workers = 1;

    for (i = 0; i < created; ++i) {
        error = pthread_join(threads[i], 0);
        if (error != 0) {
            printf("pthread_join[%d]=%d\n", i, error);
            ok = 0;
        }
        printf("worker=%d library_tls=%p guest_tls=%p traps=%d "
               "mismatches=%lu\n",
            i, states[i].library_tls, (void *)states[i].guest_tls,
            (int)states[i].traps, states[i].mismatches);
        if (states[i].traps != READ_ITERATIONS ||
            states[i].mismatches != 0) {
            ok = 0;
        }
    }

    if (created != THREAD_COUNT || unexpected_traps != 0) {
        ok = 0;
    }
    printf("emulated_tls_read=%s threads=%d reads_per_thread=%d "
           "unexpected_traps=%d\n",
        ok ? "PASS" : "FAIL", created, READ_ITERATIONS,
        (int)unexpected_traps);
    return ok ? 0 : 1;
}
