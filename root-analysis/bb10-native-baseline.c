/* BB10 native baseline: no TLS-register writes, trap injection, or global changes. */
#define _QNX_SOURCE 1
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <ucontext.h>
#include <sys/storage.h>
static pthread_key_t key;
static volatile sig_atomic_t seen;
static void handler(int sig, siginfo_t *info, void *context) {
    (void)info; (void)context;
    if (sig == SIGUSR1) seen++;
}
struct result { void *tls; int ok; int error; };
static void *worker(void *arg) {
    struct result *r = arg;
    int token = 123;
    void *before = __tls();
    int e = pthread_setspecific(key, &token);
    if (e) { r->error = e; return 0; }
    errno = 137;
    sched_yield();
    r->tls = __tls();
    r->ok = before == r->tls && pthread_getspecific(key) == &token && errno == 137;
    return 0;
}
int main(void) {
    struct sigaction sa;
    struct result r[2];
    pthread_t t;
    void *main_tls = __tls();
    int i, e, ok = 1;
    memset(r, 0, sizeof(r));
    alarm(10);
    printf("uid=%lu euid=%lu gid=%lu egid=%lu page=%ld\n",
        (unsigned long)getuid(), (unsigned long)geteuid(),
        (unsigned long)getgid(), (unsigned long)getegid(), sysconf(_SC_PAGESIZE));
    printf("ucontext=%u mcontext=%u cpu=%u pc_offset=%u spsr_offset=%u main_tls=%p\n",
        (unsigned)sizeof(ucontext_t), (unsigned)sizeof(mcontext_t),
        (unsigned)sizeof(ARM_CPU_REGISTERS),
        (unsigned)offsetof(ucontext_t, uc_mcontext.cpu.gpr[15]),
        (unsigned)offsetof(ucontext_t, uc_mcontext.cpu.spsr), main_tls);
    memset(&sa, 0, sizeof(sa)); sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, 0) || raise(SIGUSR1)) { perror("signal"); return 1; }
    e = pthread_key_create(&key, 0);
    if (e) { printf("key_create=%d\n", e); return 1; }
    /* Sequential create/join tests retirement; reused TLS addresses are valid. */
    for (i = 0; i < 2; i++) {
        e = pthread_create(&t, 0, worker, &r[i]);
        if (e) { printf("create=%d\n", e); return 1; }
        e = pthread_join(t, 0);
        if (e) { printf("join=%d\n", e); return 1; }
        printf("worker=%d tls=%p stable=%d error=%d\n", i, r[i].tls, r[i].ok, r[i].error);
        ok &= r[i].ok && r[i].tls != main_tls;
    }
    ok &= __tls() == main_tls && pthread_getspecific(key) == 0 && seen == 1;
    pthread_key_delete(key);
    printf("baseline=%s signal_count=%d\n", ok ? "PASS" : "FAIL", (int)seen);
    return ok ? 0 : 1;
}
