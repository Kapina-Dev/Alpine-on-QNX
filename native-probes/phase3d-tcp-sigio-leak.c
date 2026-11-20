/*
 * QNX io-pkt SIGIO leak after TCP async I/O (Phase 3 part 4, Ruling 4).
 *
 * Usage: phase3d-tcp-sigio-leak arm [clear]   one TCP loopback connection,
 *            FIOASYNC 1 and F_SETOWN(getpid()) on the accepted socket, one
 *            write, close, exit ("clear": FIOASYNC 0 and F_SETOWN 0 first)
 *        phase3d-tcp-sigio-leak arm-unix      the same on an AF_UNIX
 *            socketpair
 *        phase3d-tcp-sigio-leak scan          a fresh process that never
 *            asks for async I/O makes 400 TCP loopback connections (poll,
 *            accept, write) with a SIGIO handler and reports what arrived
 *
 * Run "arm" and then "scan" as separate processes, many times: on the
 * Passport a scan after an arm often receives SIGIO; scans alone don't.
 * Prints key=value lines.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef FIOASYNC
#define FIOASYNC 0x8004667d
#endif
#define SCAN_ROUNDS 400

static volatile sig_atomic_t sigio_count;
static volatile int sigio_pid, sigio_code;

static void on_sigio(int signo, siginfo_t *information, void *context)
{
    (void)signo;
    (void)context;
    sigio_pid = information->si_pid;
    sigio_code = information->si_code;
    sigio_count = sigio_count + 1;
}

static void wait_readable(int fd, int milliseconds)
{
    struct pollfd poller;
    poller.fd = fd;
    poller.events = POLLIN;
    poller.revents = 0;
    (void)poll(&poller, 1, milliseconds);
}

/* A connected TCP loopback pair; 0 or -1. */
static int tcp_pair(int *listener, int *client, int *accepted, int reuse)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int one = 1;
    *listener = socket(AF_INET, SOCK_STREAM, 0);
    if (*listener < 0) return -1;
    if (reuse)
        setsockopt(*listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(*listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(*listener, 4) != 0 ||
            getsockname(*listener, (struct sockaddr *)&address,
                &length) != 0)
        return -1;
    *client = socket(AF_INET, SOCK_STREAM, 0);
    if (*client < 0 ||
            connect(*client, (struct sockaddr *)&address,
                sizeof(address)) != 0)
        return -1;
    wait_readable(*listener, 1000);
    *accepted = accept(*listener, 0, 0);
    return *accepted < 0 ? -1 : 0;
}

static int arm(int unix_pair, int clear)
{
    int listener = -1, client, accepted, on = 1, off = 0, pair[2];
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_sigio;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGIO, &action, 0);
    if (unix_pair) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 1;
        accepted = pair[0];
        client = pair[1];
    } else if (tcp_pair(&listener, &client, &accepted, 0) != 0) {
        perror("tcp_pair");
        return 1;
    }
    if (ioctl(accepted, FIOASYNC, &on) != 0 ||
            fcntl(accepted, F_SETOWN, getpid()) != 0) {
        perror("async");
        return 1;
    }
    if (write(client, "x", 1) != 1) return 1;
    usleep(100000);
    if (clear) {
        ioctl(accepted, FIOASYNC, &off);
        fcntl(accepted, F_SETOWN, 0);
    }
    close(accepted);
    close(client);
    if (listener >= 0) close(listener);
    printf("arm pid=%d kind=%s clear=%d own_sigio=%d\n", (int)getpid(),
        unix_pair ? "unix" : "tcp", clear, (int)sigio_count);
    return 0;
}

static int scan(void)
{
    struct sigaction action;
    int round, first = -1;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_sigio;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGIO, &action, 0);
    for (round = 0; round < SCAN_ROUNDS; ++round) {
        int listener = -1, client = -1, accepted = -1;
        if (tcp_pair(&listener, &client, &accepted, 1) == 0) {
            wait_readable(accepted, 20);
            if (write(client, "x", 1) != 1) perror("write");
            wait_readable(accepted, 1000);
        }
        close(accepted);
        close(client);
        close(listener);
        if (first < 0 && sigio_count != 0) first = round;
    }
    printf("scan pid=%d sigio=%d first_round=%d si_pid_is_self=%d "
        "si_code=%d\n", (int)getpid(), (int)sigio_count, first,
        sigio_count != 0 && sigio_pid == getpid(), sigio_code);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "arm") == 0)
        return arm(0, argc > 2 && strcmp(argv[2], "clear") == 0);
    if (argc > 1 && strcmp(argv[1], "arm-unix") == 0) return arm(1, 0);
    if (argc > 1 && strcmp(argv[1], "scan") == 0) return scan();
    fprintf(stderr, "usage: %s arm [clear] | arm-unix | scan\n", argv[0]);
    return 2;
}
