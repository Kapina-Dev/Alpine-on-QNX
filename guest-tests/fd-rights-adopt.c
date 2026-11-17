/*
 * Phase 1 extra gate (guest fixture, musl): a wake eventfd and a native pipe
 * created before fork are sent parent -> child over AF_UNIX SCM_RIGHTS, and
 * both still work in the child, which still holds its inherited copies.
 *
 * Exits: 2 child did not receive both fds, 3 eventfd write through the
 * received number failed, 4 pipe read through the received number failed,
 * 5 parent read of the eventfd failed, 6 child failed, 7 setup.
 */
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int child(int socket_fd)
{
    char byte, control[CMSG_SPACE(2 * sizeof(int))], text[3] = { 0 };
    struct iovec iov = { &byte, 1 };
    struct msghdr message;
    struct cmsghdr *header;
    int fds[2];
    uint64_t value = 1;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(socket_fd, &message, 0) != 1) return 2;
    header = CMSG_FIRSTHDR(&message);
    if (header == 0 || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len != CMSG_LEN(2 * sizeof(int))) return 2;
    memcpy(fds, CMSG_DATA(header), sizeof(fds));
    if (write(fds[0], &value, sizeof(value)) != sizeof(value)) return 3;
    if (read(fds[1], text, 2) != 2 || strcmp(text, "ok") != 0) return 4;
    return 0;
}

int main(void)
{
    char byte = 'x', control[CMSG_SPACE(2 * sizeof(int))];
    struct iovec iov = { &byte, 1 };
    struct msghdr message;
    struct cmsghdr *header;
    int pair[2], pipe_fds[2], event, status;
    uint64_t value;
    pid_t pid;
    event = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event < 0 || pipe(pipe_fds) != 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 7;
    pid = fork();
    if (pid < 0) return 7;
    if (pid == 0) _exit(child(pair[1]));
    memset(&message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(2 * sizeof(int));
    memcpy(CMSG_DATA(header), &event, sizeof(int));
    memcpy(CMSG_DATA(header) + sizeof(int), &pipe_fds[0], sizeof(int));
    if (sendmsg(pair[0], &message, 0) != 1) return 7;
    if (write(pipe_fds[1], "ok", 2) != 2) return 7;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
        return WIFEXITED(status) && WEXITSTATUS(status) < 6 ?
            WEXITSTATUS(status) : 6;
    if (read(event, &value, sizeof(value)) != sizeof(value)) return 5;
    return 0;
}
