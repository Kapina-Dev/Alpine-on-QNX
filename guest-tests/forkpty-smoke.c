#define _GNU_SOURCE 1
#include <errno.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    char buffer[64];
    int master;
    int status;
    ssize_t length;
    pid_t child = forkpty(&master, 0, 0, 0);
    if (child < 0) {
        fprintf(stderr, "forkpty: errno=%d %s\n", errno, strerror(errno));
        return 1;
    }
    if (child == 0) {
        static const char message[] = "forkpty-child-ok\n";
        write(STDOUT_FILENO, message, sizeof(message) - 1);
        _exit(23);
    }
    length = read(master, buffer, sizeof(buffer));
    if (length < 0) {
        fprintf(stderr, "read-master: errno=%d %s\n", errno, strerror(errno));
        return 2;
    }
    if (waitpid(child, &status, 0) != child) {
        fprintf(stderr, "waitpid: errno=%d %s\n", errno, strerror(errno));
        return 3;
    }
    printf("read=%.*s child_exit=%d\n", (int)length, buffer,
        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    close(master);
    return length > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 23 ? 0 : 4;
}
