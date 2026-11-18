#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define LINUX_NR_SOCKETCALL 102
#define LINUX_NR_SOCKET 281
#define LINUX_NR_BIND 282
#define LINUX_NR_CONNECT 283
#define LINUX_NR_LISTEN 284
#define LINUX_NR_ACCEPT 285
#define LINUX_NR_GETSOCKNAME 286
#define LINUX_NR_GETPEERNAME 287
#define LINUX_NR_SOCKETPAIR 288
#define LINUX_NR_SEND 289
#define LINUX_NR_SENDTO 290
#define LINUX_NR_RECV 291
#define LINUX_NR_RECVFROM 292
#define LINUX_NR_SHUTDOWN 293
#define LINUX_NR_SETSOCKOPT 294
#define LINUX_NR_GETSOCKOPT 295
#define LINUX_NR_SENDMSG 296
#define LINUX_NR_RECVMSG 297
#define LINUX_NR_RECVMMSG 365
#define LINUX_NR_ACCEPT4 366
#define LINUX_NR_SENDMMSG 374

#define LINUX_AF_UNIX 1
#define LINUX_AF_INET 2
#define LINUX_AF_INET6 10
#define LINUX_SOCK_TYPE_MASK 0x0fu
#define LINUX_SOCK_NONBLOCK 0x00000800u
#define LINUX_SOCK_CLOEXEC 0x00080000u
#define LINUX_SOL_SOCKET 1
#define LINUX_SOL_IP 0
#define LINUX_SOL_TCP 6
#define LINUX_SCM_RIGHTS 1
#define LINUX_SO_PEERCRED 17
#define LINUX_MSG_CMSG_CLOEXEC 0x40000000u
#define LINUX_ENOPROTOOPT 92
#define LINUX_EOPNOTSUPP 95
#define LINUX_EAFNOSUPPORT 97
#define LINUX_UNIX_PATH_MAX 108

struct linux_msghdr {
    uint32_t name;
    uint32_t name_length;
    uint32_t iov;
    uint32_t iov_count;
    uint32_t control;
    uint32_t control_length;
    uint32_t flags;
};

struct linux_cmsghdr {
    uint32_t length;
    int32_t level;
    int32_t type;
};

static size_t linux_cmsg_align(size_t length)
{
    return (length + 3u) & ~3u;
}

static int32_t socket_result(ssize_t result)
{
    return result < 0 ? -(int32_t)linux_errno_number(errno) :
        (int32_t)result;
}

static int host_domain(int linux_domain)
{
    switch (linux_domain) {
    case LINUX_AF_UNIX: return AF_UNIX;
    case LINUX_AF_INET: return AF_INET;
    case LINUX_AF_INET6: return AF_INET6;
    default: return -1;
    }
}

static int linux_domain(int host)
{
    if (host == AF_UNIX) return LINUX_AF_UNIX;
    if (host == AF_INET) return LINUX_AF_INET;
    if (host == AF_INET6) return LINUX_AF_INET6;
    return -1;
}

static int socket_type(uint32_t linux_type, int *host_type, int *status_flags,
    int *descriptor_flags)
{
    uint32_t flags = linux_type & ~LINUX_SOCK_TYPE_MASK;
    int type = linux_type & LINUX_SOCK_TYPE_MASK;
    if ((flags & ~(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) != 0 ||
        type < SOCK_STREAM || type > SOCK_SEQPACKET) return -1;
    *host_type = type;
    *status_flags = (flags & LINUX_SOCK_NONBLOCK) != 0 ? O_NONBLOCK : 0;
    *descriptor_flags = (flags & LINUX_SOCK_CLOEXEC) != 0 ? FD_CLOEXEC : 0;
    return 0;
}

static int configure_socket(int fd, int status_flags, int descriptor_flags)
{
    int current;
    if (status_flags != 0) {
        current = fcntl(fd, F_GETFL);
        if (current < 0 || fcntl(fd, F_SETFL, current | status_flags) != 0)
            return -1;
    }
    if (descriptor_flags != 0) {
        current = fcntl(fd, F_GETFD);
        if (current < 0 || fcntl(fd, F_SETFD,
                current | descriptor_flags) != 0) return -1;
    }
    return 0;
}

static size_t bounded_string_length(const char *text, size_t limit)
{
    size_t length = 0;
    while (length != limit && text[length] != '\0') length++;
    return length;
}

static int guest_address(const void *guest, uint32_t guest_length,
    int bind_address, struct sockaddr_storage *host, socklen_t *host_length)
{
    uint16_t family;
    int mapped;
    if (guest == 0 || guest_length < 2) return -EFAULT;
    memcpy(&family, guest, sizeof(family));
    mapped = host_domain(family);
    if (mapped < 0) return -LINUX_EAFNOSUPPORT;
    memset(host, 0, sizeof(*host));
    if (family == LINUX_AF_INET) {
        struct sockaddr_in *address = (struct sockaddr_in *)host;
        if (guest_length < 16) return -EINVAL;
        address->sin_len = sizeof(*address);
        address->sin_family = AF_INET;
        memcpy(&address->sin_port, (const unsigned char *)guest + 2, 14);
        *host_length = sizeof(*address);
    } else if (family == LINUX_AF_INET6) {
        struct sockaddr_in6 *address = (struct sockaddr_in6 *)host;
        if (guest_length < 28) return -EINVAL;
        address->sin6_len = sizeof(*address);
        address->sin6_family = AF_INET6;
        memcpy(&address->sin6_port, (const unsigned char *)guest + 2, 26);
        *host_length = sizeof(*address);
    } else {
        struct sockaddr_un *address = (struct sockaddr_un *)host;
        size_t path_length = guest_length - 2;
        size_t actual_length;
        char guest_path[LINUX_UNIX_PATH_MAX + 1];
        char host_path[PATH_MAX];
        if (path_length > LINUX_UNIX_PATH_MAX) return -EINVAL;
        if (path_length == 0) {
            address->sun_len = offsetof(struct sockaddr_un, sun_path);
            address->sun_family = AF_UNIX;
            *host_length = address->sun_len;
            return 0;
        }
        if (((const unsigned char *)guest)[2] == '\0')
            return -LINUX_EOPNOTSUPP;
        actual_length = bounded_string_length(
            (const char *)guest + 2, path_length);
        memcpy(guest_path, (const unsigned char *)guest + 2, actual_length);
        guest_path[actual_length] = '\0';
        if (guest_path_resolve(guest_path, bind_address, host_path,
                sizeof(host_path)) != 0)
            return -(int32_t)linux_errno_number(errno);
        actual_length = strlen(host_path);
        if (actual_length >= sizeof(address->sun_path)) return -ENAMETOOLONG;
        memcpy(address->sun_path, host_path, actual_length + 1);
        address->sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) +
            actual_length);
        address->sun_family = AF_UNIX;
        *host_length = address->sun_len;
    }
    return 0;
}

static int store_address(void *guest, uint32_t *guest_length,
    const struct sockaddr *host, socklen_t host_length)
{
    unsigned char converted[128];
    uint32_t capacity;
    uint32_t required;
    uint16_t family;
    int mapped;
    if (guest_length == 0) return guest == 0 ? 0 : -EFAULT;
    memcpy(&capacity, guest_length, sizeof(capacity));
    mapped = linux_domain(host->sa_family);
    if (mapped < 0) return -LINUX_EAFNOSUPPORT;
    family = (uint16_t)mapped;
    if (mapped == LINUX_AF_INET) required = 16;
    else if (mapped == LINUX_AF_INET6) required = 28;
    else {
        const struct sockaddr_un *address =
            (const struct sockaddr_un *)host;
        const char *path = address->sun_path;
        const char *root = guest_path_root();
        size_t available = host_length > offsetof(struct sockaddr_un, sun_path) ?
            host_length - offsetof(struct sockaddr_un, sun_path) : 0;
        size_t path_length;
        size_t root_length = strlen(root);
        if (available > sizeof(address->sun_path))
            available = sizeof(address->sun_path);
        path_length = bounded_string_length(path, available);
        if (path_length >= root_length && memcmp(path, root, root_length) == 0 &&
            (path_length == root_length || path[root_length] == '/')) {
            path += root_length;
            path_length -= root_length;
            if (path_length == 0) { path = "/"; path_length = 1; }
        }
        required = (uint32_t)(2 + path_length + (path_length != 0 ? 1 : 0));
        if (required > sizeof(converted)) return -EINVAL;
        memset(converted, 0, required);
        memcpy(converted, &family, 2);
        if (path_length != 0) memcpy(converted + 2, path, path_length);
        if (guest != 0 && capacity != 0)
            memcpy(guest, converted, capacity < required ? capacity : required);
        memcpy(guest_length, &required, sizeof(required));
        return 0;
    }
    if (required > sizeof(converted)) return -EINVAL;
    memset(converted, 0, required);
    memcpy(converted, &family, 2);
    if (required > 2) memcpy(converted + 2,
        (const unsigned char *)host + 2, required - 2);
    if (guest != 0 && capacity != 0)
        memcpy(guest, converted, capacity < required ? capacity : required);
    memcpy(guest_length, &required, sizeof(required));
    return 0;
}

static int message_flags(uint32_t linux_flags, int *host_flags)
{
    uint32_t known = 0x00000001u | 0x00000002u | 0x00000004u |
        0x00000008u | 0x00000020u | 0x00000040u | 0x00000080u |
        0x00000100u | 0x00004000u | 0x00010000u |
        LINUX_MSG_CMSG_CLOEXEC;
    int result = 0;
    if ((linux_flags & ~known) != 0) return -1;
    if (linux_flags & 0x0001u) result |= MSG_OOB;
    if (linux_flags & 0x0002u) result |= MSG_PEEK;
    if (linux_flags & 0x0004u) result |= MSG_DONTROUTE;
    if (linux_flags & 0x0008u) result |= MSG_CTRUNC;
    if (linux_flags & 0x0020u) result |= MSG_TRUNC;
    if (linux_flags & 0x0040u) result |= MSG_DONTWAIT;
    if (linux_flags & 0x0080u) result |= MSG_EOR;
    if (linux_flags & 0x0100u) result |= MSG_WAITALL;
    if (linux_flags & 0x4000u) result |= MSG_NOSIGNAL;
#ifdef MSG_WAITFORONE
    if (linux_flags & 0x10000u) result |= MSG_WAITFORONE;
#else
    if (linux_flags & 0x10000u) return -1;
#endif
    *host_flags = result;
    return 0;
}

static int send_control(const void *guest_control, uint32_t guest_length,
    void **host_control, size_t *host_length)
{
    const unsigned char *guest = guest_control;
    size_t guest_offset = 0;
    size_t required = 0;
    unsigned char *converted;
    size_t host_offset = 0;
    while (guest_offset != guest_length) {
        struct linux_cmsghdr header;
        size_t payload_length;
        size_t space;
        if (guest_length - guest_offset < sizeof(header))
            return -EINVAL;
        memcpy(&header, guest + guest_offset, sizeof(header));
        if (header.length < sizeof(header) ||
            header.length > guest_length - guest_offset)
            return -EINVAL;
        if (header.level != LINUX_SOL_SOCKET ||
            header.type != LINUX_SCM_RIGHTS)
            return -LINUX_EOPNOTSUPP;
        payload_length = header.length - sizeof(header);
        if (payload_length == 0 || payload_length % sizeof(int) != 0)
            return -EINVAL;
        space = linux_cmsg_align(header.length);
        if (space > guest_length - guest_offset) {
            if (header.length != guest_length - guest_offset) return -EINVAL;
            space = header.length;
        }
        if (CMSG_SPACE(payload_length) > SIZE_MAX - required) return -EINVAL;
        required += CMSG_SPACE(payload_length);
        guest_offset += space;
    }
    converted = calloc(1, required);
    if (converted == 0) return -ENOMEM;
    guest_offset = 0;
    while (guest_offset != guest_length) {
        struct linux_cmsghdr header;
        struct cmsghdr *host_header;
        size_t payload_length;
        size_t guest_space;
        memcpy(&header, guest + guest_offset, sizeof(header));
        payload_length = header.length - sizeof(header);
        host_header = (struct cmsghdr *)(converted + host_offset);
        host_header->cmsg_len = CMSG_LEN(payload_length);
        host_header->cmsg_level = SOL_SOCKET;
        host_header->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(host_header), guest + guest_offset + sizeof(header),
            payload_length);
        host_offset += CMSG_SPACE(payload_length);
        guest_space = linux_cmsg_align(header.length);
        guest_offset += guest_space <= guest_length - guest_offset ?
            guest_space : header.length;
    }
    *host_control = converted;
    *host_length = required;
    return 0;
}

static int receive_control(const struct msghdr *host, void *guest_control,
    uint32_t guest_capacity, int close_on_exec, uint32_t *guest_length,
    uint32_t *linux_flags)
{
    struct cmsghdr *header;
    unsigned char *guest = guest_control;
    size_t guest_offset = 0;
    for (header = CMSG_FIRSTHDR(host); header != 0;
            header = CMSG_NXTHDR(host, header)) {
        struct linux_cmsghdr converted;
        size_t minimum = CMSG_LEN(0);
        size_t payload_length;
        size_t converted_length;
        size_t converted_space;
        size_t index;
        int *descriptors;
        if (header->cmsg_len < minimum) return -EINVAL;
        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS)
            return -LINUX_EOPNOTSUPP;
        payload_length = header->cmsg_len - minimum;
        if (payload_length == 0 || payload_length % sizeof(int) != 0)
            return -EINVAL;
        descriptors = (int *)CMSG_DATA(header);
        /* A received fd in the hidden range means the guest is out of
           numbers: drop them all and report MSG_CTRUNC, as Linux does when
           it cannot install a received fd. */
        for (index = 0; index != payload_length / sizeof(int); ++index)
            if (guest_fd_is_hidden(descriptors[index])) break;
        if (index != payload_length / sizeof(int)) {
            *linux_flags |= 0x0008u;
            for (index = 0; index != payload_length / sizeof(int); ++index)
                close(descriptors[index]);
            continue;
        }
        for (index = 0; index != payload_length / sizeof(int); ++index)
            guest_fd_received(descriptors[index]);
        if (close_on_exec)
            for (index = 0; index != payload_length / sizeof(int); ++index) {
                int flags = fcntl(descriptors[index], F_GETFD);
                if (flags < 0 || fcntl(descriptors[index], F_SETFD,
                        flags | FD_CLOEXEC) != 0)
                    return -(int32_t)linux_errno_number(errno);
            }
        converted_length = sizeof(converted) + payload_length;
        converted_space = linux_cmsg_align(converted_length);
        if (converted_space > guest_capacity - guest_offset) {
            *linux_flags |= 0x0008u;
            for (index = 0; index != payload_length / sizeof(int); ++index)
                close(descriptors[index]);
            continue;
        }
        converted.length = (uint32_t)converted_length;
        converted.level = LINUX_SOL_SOCKET;
        converted.type = LINUX_SCM_RIGHTS;
        memset(guest + guest_offset, 0, converted_space);
        memcpy(guest + guest_offset, &converted, sizeof(converted));
        memcpy(guest + guest_offset + sizeof(converted), descriptors,
            payload_length);
        guest_offset += converted_space;
    }
    *guest_length = (uint32_t)guest_offset;
    return 0;
}

static void close_received_rights(const struct msghdr *host)
{
    struct cmsghdr *header;
    for (header = CMSG_FIRSTHDR(host); header != 0;
            header = CMSG_NXTHDR(host, header)) {
        size_t minimum = CMSG_LEN(0);
        size_t payload_length;
        size_t index;
        int *descriptors;
        if (header->cmsg_len < minimum || header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS) continue;
        payload_length = header->cmsg_len - minimum;
        if (payload_length % sizeof(int) != 0) continue;
        descriptors = (int *)CMSG_DATA(header);
        for (index = 0; index != payload_length / sizeof(int); ++index)
            close(descriptors[index]);
    }
}

static uint32_t linux_message_flags(int host_flags)
{
    uint32_t result = 0;
    if (host_flags & MSG_OOB) result |= 0x0001u;
    if (host_flags & MSG_PEEK) result |= 0x0002u;
    if (host_flags & MSG_DONTROUTE) result |= 0x0004u;
    if (host_flags & MSG_CTRUNC) result |= 0x0008u;
    if (host_flags & MSG_TRUNC) result |= 0x0020u;
    if (host_flags & MSG_DONTWAIT) result |= 0x0040u;
    if (host_flags & MSG_EOR) result |= 0x0080u;
    if (host_flags & MSG_WAITALL) result |= 0x0100u;
    return result;
}

static int socket_option(int linux_level, int linux_option, int *host_level,
    int *host_option)
{
    if (linux_level == LINUX_SOL_SOCKET) {
        *host_level = SOL_SOCKET;
        switch (linux_option) {
        case 1: *host_option = SO_DEBUG; return 0;
        case 2: *host_option = SO_REUSEADDR; return 0;
        case 3: *host_option = SO_TYPE; return 0;
        case 4: *host_option = SO_ERROR; return 0;
        case 5: *host_option = SO_DONTROUTE; return 0;
        case 6: *host_option = SO_BROADCAST; return 0;
        case 7: *host_option = SO_SNDBUF; return 0;
        case 8: *host_option = SO_RCVBUF; return 0;
        case 9: *host_option = SO_KEEPALIVE; return 0;
        case 10: *host_option = SO_OOBINLINE; return 0;
        case 13: *host_option = SO_LINGER; return 0;
        case 15: *host_option = SO_REUSEPORT; return 0;
        case 18: *host_option = SO_RCVLOWAT; return 0;
        case 19: *host_option = SO_SNDLOWAT; return 0;
        case 20: *host_option = SO_RCVTIMEO; return 0;
        case 21: *host_option = SO_SNDTIMEO; return 0;
        case 30: *host_option = SO_ACCEPTCONN; return 0;
        default: return -1;
        }
    }
    if (linux_level == LINUX_SOL_IP) {
        *host_level = IPPROTO_IP;
        switch (linux_option) {
        case 1: *host_option = IP_TOS; return 0;
        case 2: *host_option = IP_TTL; return 0;
        case 3: *host_option = IP_HDRINCL; return 0;
        case 4: *host_option = IP_OPTIONS; return 0;
        case 32: *host_option = IP_MULTICAST_IF; return 0;
        case 33: *host_option = IP_MULTICAST_TTL; return 0;
        case 34: *host_option = IP_MULTICAST_LOOP; return 0;
        case 35: *host_option = IP_ADD_MEMBERSHIP; return 0;
        case 36: *host_option = IP_DROP_MEMBERSHIP; return 0;
        default: return -1;
        }
    }
    if (linux_level == LINUX_SOL_TCP) {
        *host_level = IPPROTO_TCP;
        switch (linux_option) {
        case 1: *host_option = TCP_NODELAY; return 0;
        case 2: *host_option = TCP_MAXSEG; return 0;
        case 4: *host_option = TCP_KEEPALIVE; return 0;
        default: return -1;
        }
    }
    return -1;
}

/* QNX's AF_UNIX stream defaults (SO_SNDBUF 7168, SO_RCVBUF 5120) let a
   sender queue more unread data than the receiver's buffer holds.  A
   descriptor-bearing message that no longer fits is then dropped, payload
   included, while sendmsg reports success; Linux never loses it.  A large
   receive buffer keeps every sender's backlog within the receiver's space.
   Accepted sockets inherit it from the listener.  Best effort: a refusal
   leaves QNX's default. */
#define UNIX_STREAM_RECEIVE_BUFFER (256 * 1024)

static void widen_unix_receive_buffer(int fd, int domain, int type)
{
    int size = UNIX_STREAM_RECEIVE_BUFFER;
    if (domain != AF_UNIX) return;
    if (type != SOCK_STREAM
#ifdef SOCK_SEQPACKET
        && type != SOCK_SEQPACKET
#endif
        ) return;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

static int32_t make_socket(uint32_t *arguments)
{
    int domain = host_domain((int)arguments[0]);
    int type, status_flags, descriptor_flags;
    int fd;
    if (domain < 0 || socket_type(arguments[1], &type, &status_flags,
            &descriptor_flags) != 0) return -EINVAL;
    fd = guest_fd_admit(socket(domain, type, (int)arguments[2]));
    if (fd < 0) return -(int32_t)linux_errno_number(errno);
    guest_fd_clear(fd);
    widen_unix_receive_buffer(fd, domain, type);
    if (configure_socket(fd, status_flags, descriptor_flags) != 0) {
        int saved_errno = errno; close(fd); errno = saved_errno;
        return -(int32_t)linux_errno_number(errno);
    }
    return fd;
}

static int32_t address_call(uint32_t number, uint32_t *arguments)
{
    struct sockaddr_storage address;
    socklen_t length;
    int converted;
    if (number == LINUX_NR_BIND || number == LINUX_NR_CONNECT) {
        converted = guest_address((void *)arguments[1], arguments[2],
            number == LINUX_NR_BIND, &address, &length);
        if (converted != 0) return converted;
        return socket_result(number == LINUX_NR_BIND ?
            bind((int)arguments[0], (struct sockaddr *)&address, length) :
            connect((int)arguments[0], (struct sockaddr *)&address, length));
    }
    length = sizeof(address);
    converted = number == LINUX_NR_GETSOCKNAME ?
        getsockname((int)arguments[0], (struct sockaddr *)&address, &length) :
        getpeername((int)arguments[0], (struct sockaddr *)&address, &length);
    if (converted != 0) return -(int32_t)linux_errno_number(errno);
    return store_address((void *)arguments[1], (uint32_t *)arguments[2],
        (struct sockaddr *)&address, length);
}

static int32_t accept_socket(uint32_t *arguments, int with_flags)
{
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    int status_flags = 0, descriptor_flags = 0, ignored_type;
    int fd;
    int converted;
    /* The flags share bits with the socket type, so they are checked on
       their own: accept4(..., 1) must not pass as SOCK_STREAM. */
    if (with_flags && ((arguments[3] & ~(LINUX_SOCK_NONBLOCK |
            LINUX_SOCK_CLOEXEC)) != 0 ||
            socket_type(SOCK_STREAM | arguments[3], &ignored_type,
            &status_flags, &descriptor_flags) != 0)) return -EINVAL;
    fd = guest_fd_admit(accept((int)arguments[0], arguments[1] == 0 ? 0 :
        (struct sockaddr *)&address, arguments[1] == 0 ? 0 : &length));
    if (fd < 0) return -(int32_t)linux_errno_number(errno);
    guest_fd_clear(fd);
    if (configure_socket(fd, status_flags, descriptor_flags) != 0) {
        int saved_errno = errno; close(fd); errno = saved_errno;
        return -(int32_t)linux_errno_number(errno);
    }
    if (arguments[1] != 0) {
        converted = store_address((void *)arguments[1],
            (uint32_t *)arguments[2], (struct sockaddr *)&address, length);
        if (converted != 0) { close(fd); return converted; }
    }
    return fd;
}

static int32_t socket_pair(uint32_t *arguments)
{
    int domain = host_domain((int)arguments[0]);
    int type, status_flags, descriptor_flags;
    int descriptors[2];
    if (arguments[3] == 0) return -EFAULT;
    if (domain < 0 || socket_type(arguments[1], &type, &status_flags,
            &descriptor_flags) != 0) return -EINVAL;
    if (socketpair(domain, type, (int)arguments[2], descriptors) != 0)
        return -(int32_t)linux_errno_number(errno);
    if (guest_fd_is_hidden(descriptors[0]) || guest_fd_is_hidden(descriptors[1])) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -EMFILE;
    }
    guest_fd_clear(descriptors[0]);
    guest_fd_clear(descriptors[1]);
    widen_unix_receive_buffer(descriptors[0], domain, type);
    widen_unix_receive_buffer(descriptors[1], domain, type);
    if (configure_socket(descriptors[0], status_flags, descriptor_flags) != 0 ||
        configure_socket(descriptors[1], status_flags, descriptor_flags) != 0) {
        int saved_errno = errno;
        close(descriptors[0]); close(descriptors[1]); errno = saved_errno;
        return -(int32_t)linux_errno_number(errno);
    }
    memcpy((void *)arguments[3], descriptors, sizeof(descriptors));
    return 0;
}

static int32_t transfer(uint32_t number, uint32_t *arguments)
{
    int flags;
    struct sockaddr_storage address;
    socklen_t length;
    int converted;
    ssize_t result;
    if (message_flags(arguments[3], &flags) != 0) return -EINVAL;
    if (number == LINUX_NR_SEND)
        result = send((int)arguments[0], (void *)arguments[1], arguments[2],
            flags);
    else if (number == LINUX_NR_RECV)
        result = recv((int)arguments[0], (void *)arguments[1], arguments[2],
            flags);
    else if (number == LINUX_NR_SENDTO) {
        if (arguments[4] == 0 && arguments[5] == 0)
            result = send((int)arguments[0], (void *)arguments[1],
                arguments[2], flags);
        else {
            converted = guest_address((void *)arguments[4], arguments[5], 0,
                &address, &length);
            if (converted != 0) return converted;
            result = sendto((int)arguments[0], (void *)arguments[1],
                arguments[2], flags, (struct sockaddr *)&address, length);
        }
    } else {
        length = sizeof(address);
        result = recvfrom((int)arguments[0], (void *)arguments[1], arguments[2],
            flags, arguments[4] == 0 ? 0 : (struct sockaddr *)&address,
            arguments[4] == 0 ? 0 : &length);
        if (result >= 0 && arguments[4] != 0) {
            converted = store_address((void *)arguments[4],
                (uint32_t *)arguments[5], (struct sockaddr *)&address, length);
            if (converted != 0) return converted;
        }
    }
    return socket_result(result);
}

static int32_t socket_option_call(uint32_t number, uint32_t *arguments)
{
    int level, option;
    socklen_t length;
    int result;
    if (number == LINUX_NR_GETSOCKOPT &&
        (int)arguments[1] == LINUX_SOL_SOCKET &&
        (int)arguments[2] == LINUX_SO_PEERCRED) {
        struct unpcbid host_peer;
        uint32_t capacity;
        uint32_t required = 12;
        int32_t linux_pid;
        uint32_t linux_uid;
        uint32_t linux_gid;
        unsigned char linux_peer[12];
        if (arguments[3] == 0 || arguments[4] == 0) return -EFAULT;
        memcpy(&capacity, (void *)arguments[4], sizeof(capacity));
        length = sizeof(host_peer);
        if (getsockopt((int)arguments[0], 0, LOCAL_PEEREID,
                &host_peer, &length) != 0)
            return -(int32_t)linux_errno_number(errno);
        linux_pid = (int32_t)host_peer.unp_pid;
        linux_uid = (uint32_t)host_peer.unp_euid;
        linux_gid = (uint32_t)host_peer.unp_egid;
        memset(linux_peer, 0, sizeof(linux_peer));
        memcpy(linux_peer, &linux_pid, 4);
        memcpy(linux_peer + 4, &linux_uid, 4);
        memcpy(linux_peer + 8, &linux_gid, 4);
        if (capacity != 0)
            memcpy((void *)arguments[3], linux_peer,
                capacity < required ? capacity : required);
        memcpy((void *)arguments[4], &required, sizeof(required));
        return 0;
    }
    if (socket_option((int)arguments[1], (int)arguments[2], &level, &option) != 0)
        return -LINUX_ENOPROTOOPT;
    if (number == LINUX_NR_SETSOCKOPT)
        result = setsockopt((int)arguments[0], level, option,
            (void *)arguments[3], arguments[4]);
    else {
        if (arguments[4] == 0) return -EFAULT;
        memcpy(&length, (void *)arguments[4], sizeof(length));
        result = getsockopt((int)arguments[0], level, option,
            (void *)arguments[3], &length);
        if (result == 0 && (int)arguments[1] == LINUX_SOL_SOCKET &&
            (int)arguments[2] == 4 && arguments[3] != 0 &&
            length >= sizeof(int)) {
            int host_error;
            int linux_error;
            memcpy(&host_error, (void *)arguments[3], sizeof(host_error));
            linux_error = linux_errno_number(host_error);
            memcpy((void *)arguments[3], &linux_error, sizeof(linux_error));
        }
        if (result == 0) memcpy((void *)arguments[4], &length, sizeof(length));
    }
    return result == 0 ? 0 : -(int32_t)linux_errno_number(errno);
}

/* One sendmsg or recvmsg on a guest msghdr copy.  Shared by sendmsg,
   recvmsg, sendmmsg and recvmmsg.  On a successful receive *guest holds the
   updated header and the caller stores it back. */
static int32_t message_one(int fd, int receiving, struct linux_msghdr *header,
    uint32_t linux_flags)
{
    struct linux_msghdr guest = *header;
    struct msghdr host;
    struct sockaddr_storage address;
    socklen_t address_length;
    void *control = 0;
    size_t control_length = 0;
    size_t payload_length = 0;
    int flags;
    int converted;
    ssize_t result;
    uint32_t iov_index;
    if (message_flags(linux_flags, &flags) != 0) return -EINVAL;
    memset(&host, 0, sizeof(host));
    host.msg_iov = (struct iovec *)guest.iov;
    host.msg_iovlen = guest.iov_count;
    for (iov_index = 0; iov_index != guest.iov_count; ++iov_index) {
        if (host.msg_iov[iov_index].iov_len > SIZE_MAX - payload_length)
            return -EINVAL;
        payload_length += host.msg_iov[iov_index].iov_len;
    }
    if (!receiving && guest.control_length != 0) {
        if (guest.control == 0) return -EFAULT;
        converted = send_control((void *)guest.control, guest.control_length,
            &control, &control_length);
        if (converted != 0) return converted;
        host.msg_control = control;
        host.msg_controllen = control_length;
    } else if (receiving && guest.control_length != 0) {
        if (guest.control == 0) return -EFAULT;
        control = calloc(1, guest.control_length);
        if (control == 0) return -ENOMEM;
        host.msg_control = control;
        host.msg_controllen = guest.control_length;
    }
    if (!receiving && guest.name != 0) {
        converted = guest_address((void *)guest.name, guest.name_length, 0,
            &address, &address_length);
        if (converted != 0) { free(control); return converted; }
        host.msg_name = &address;
        host.msg_namelen = address_length;
    } else if (receiving && guest.name != 0) {
        host.msg_name = &address;
        host.msg_namelen = sizeof(address);
    }
    result = !receiving ?
        sendmsg(fd, &host, flags) :
        recvmsg(fd, &host, flags);
    if (result < 0) {
        int saved_errno = errno;
        free(control);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    if (receiving) {
        guest.flags = linux_message_flags(host.msg_flags);
        if (guest.name != 0) {
            uint32_t capacity = guest.name_length;
            converted = store_address((void *)guest.name, &capacity,
                (struct sockaddr *)&address, host.msg_namelen);
            if (converted != 0) {
                close_received_rights(&host);
                free(control);
                return converted;
            }
            guest.name_length = capacity;
        }
        if (guest.control_length != 0) {
            uint32_t received_length = 0;
            converted = receive_control(&host, (void *)guest.control,
                guest.control_length,
                (linux_flags & LINUX_MSG_CMSG_CLOEXEC) != 0,
                &received_length, &guest.flags);
            if (converted != 0) {
                close_received_rights(&host);
                free(control);
                return converted;
            }
            guest.control_length = received_length;
        }
        *header = guest;
    }
    free(control);
    return (int32_t)result;
}

static int32_t message_call(uint32_t number, uint32_t *arguments)
{
    struct linux_msghdr guest;
    int32_t result;
    if (arguments[1] == 0) return -EFAULT;
    if (guest_fd_is_hidden((int)arguments[0])) return -EBADF;
    memcpy(&guest, (void *)arguments[1], sizeof(guest));
    result = message_one((int)arguments[0], number == LINUX_NR_RECVMSG,
        &guest, arguments[2]);
    if (result >= 0 && number == LINUX_NR_RECVMSG)
        memcpy((void *)arguments[1], &guest, sizeof(guest));
    return result;
}

/* struct mmsghdr on ARM: a msghdr and the byte count of that message. */
struct linux_mmsghdr {
    struct linux_msghdr header;
    uint32_t length;
};

struct linux_timespec32 {
    int32_t seconds;
    int32_t nanoseconds;
};

#define LINUX_UIO_MAXIOV 1024u
#define LINUX_MSG_DONTWAIT 0x00000040u
#define LINUX_MSG_WAITFORONE 0x00010000u

/* sendmmsg (receiving 0) and recvmmsg (receiving 1) as a loop over
   message_one.  An error on the first message is returned; one after at
   least one message returns the count so far.  The recvmmsg timeout is
   checked only between messages. */
static int32_t multi_message_call(int receiving, uint32_t *arguments)
{
    struct linux_mmsghdr *vector = (struct linux_mmsghdr *)arguments[1];
    uint32_t count = arguments[2];
    uint32_t flags = arguments[3];
    struct timespec deadline;
    struct timespec now;
    int has_timeout = 0;
    uint32_t done = 0;
    if (guest_fd_is_hidden((int)arguments[0])) return -EBADF;
    if (count > LINUX_UIO_MAXIOV) count = LINUX_UIO_MAXIOV;
    if (receiving && arguments[4] != 0) {
        struct linux_timespec32 timeout;
        memcpy(&timeout, (void *)arguments[4], sizeof(timeout));
        if (timeout.seconds < 0 || timeout.nanoseconds < 0 ||
            timeout.nanoseconds >= 1000000000)
            return -EINVAL;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout.seconds;
        deadline.tv_nsec += timeout.nanoseconds;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        has_timeout = 1;
    }
    if (count == 0) return 0;
    if (vector == 0) return -EFAULT;
    while (done != count) {
        struct linux_msghdr header;
        int32_t result;
        uint32_t length;
        memcpy(&header, &vector[done].header, sizeof(header));
        result = message_one((int)arguments[0], receiving, &header,
            receiving ? flags & ~LINUX_MSG_WAITFORONE : flags);
        if (result < 0) return done != 0 ? (int32_t)done : result;
        if (receiving) memcpy(&vector[done].header, &header, sizeof(header));
        length = (uint32_t)result;
        memcpy(&vector[done].length, &length, sizeof(length));
        ++done;
        if (receiving) {
            if ((flags & LINUX_MSG_WAITFORONE) != 0)
                flags |= LINUX_MSG_DONTWAIT;
            if (has_timeout) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                if (now.tv_sec > deadline.tv_sec ||
                    (now.tv_sec == deadline.tv_sec &&
                        now.tv_nsec >= deadline.tv_nsec))
                    break;
            }
        }
    }
    return (int32_t)done;
}

static uint32_t socketcall_number(uint32_t call)
{
    static const uint16_t calls[] = { 0, LINUX_NR_SOCKET, LINUX_NR_BIND,
        LINUX_NR_CONNECT, LINUX_NR_LISTEN, LINUX_NR_ACCEPT,
        LINUX_NR_GETSOCKNAME, LINUX_NR_GETPEERNAME, LINUX_NR_SOCKETPAIR,
        LINUX_NR_SEND, LINUX_NR_RECV, LINUX_NR_SENDTO, LINUX_NR_RECVFROM,
        LINUX_NR_SHUTDOWN, LINUX_NR_SETSOCKOPT, LINUX_NR_GETSOCKOPT,
        LINUX_NR_SENDMSG, LINUX_NR_RECVMSG, LINUX_NR_ACCEPT4,
        LINUX_NR_RECVMMSG, LINUX_NR_SENDMMSG };
    return call < ARRAY_COUNT(calls) ? calls[call] : 0;
}

int32_t linux_socket_syscall(uint32_t number, uint32_t arguments[6])
{
    uint32_t socket_arguments[6];
    if (number == LINUX_NR_SOCKETCALL) {
        number = socketcall_number(arguments[0]);
        if (number == 0 || arguments[1] == 0) return -EINVAL;
        memcpy(socket_arguments, (void *)arguments[1], sizeof(socket_arguments));
        arguments = socket_arguments;
    }
    switch (number) {
    case LINUX_NR_SOCKET: return make_socket(arguments);
    case LINUX_NR_BIND:
    case LINUX_NR_CONNECT:
    case LINUX_NR_GETSOCKNAME:
    case LINUX_NR_GETPEERNAME: return address_call(number, arguments);
    case LINUX_NR_LISTEN:
        return socket_result(listen((int)arguments[0], (int)arguments[1]));
    case LINUX_NR_ACCEPT: return accept_socket(arguments, 0);
    case LINUX_NR_ACCEPT4: return accept_socket(arguments, 1);
    case LINUX_NR_SOCKETPAIR: return socket_pair(arguments);
    case LINUX_NR_SEND:
    case LINUX_NR_SENDTO:
    case LINUX_NR_RECV:
    case LINUX_NR_RECVFROM: return transfer(number, arguments);
    case LINUX_NR_SHUTDOWN:
        return socket_result(shutdown((int)arguments[0], (int)arguments[1]));
    case LINUX_NR_SETSOCKOPT:
    case LINUX_NR_GETSOCKOPT: return socket_option_call(number, arguments);
    case LINUX_NR_SENDMSG:
    case LINUX_NR_RECVMSG: return message_call(number, arguments);
    case LINUX_NR_SENDMMSG: return multi_message_call(0, arguments);
    case LINUX_NR_RECVMMSG: return multi_message_call(1, arguments);
    default: return -ENOSYS;
    }
}
