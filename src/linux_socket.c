#include "linuxemu.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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
#define LINUX_NR_ACCEPT4 366

#define LINUX_AF_UNIX 1
#define LINUX_AF_INET 2
#define LINUX_AF_INET6 10
#define LINUX_SOCK_TYPE_MASK 0x0fu
#define LINUX_SOCK_NONBLOCK 0x00000800u
#define LINUX_SOCK_CLOEXEC 0x00080000u
#define LINUX_SOL_SOCKET 1
#define LINUX_SOL_IP 0
#define LINUX_SOL_TCP 6
#define LINUX_ENOPROTOOPT 92
#define LINUX_EOPNOTSUPP 95
#define LINUX_EAFNOSUPPORT 97

struct linux_msghdr {
    uint32_t name;
    uint32_t name_length;
    uint32_t iov;
    uint32_t iov_count;
    uint32_t control;
    uint32_t control_length;
    uint32_t flags;
};

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

static int guest_address(const void *guest, uint32_t guest_length,
    struct sockaddr_storage *host, socklen_t *host_length)
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
        if (path_length > sizeof(address->sun_path)) return -EINVAL;
        address->sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) +
            path_length);
        address->sun_family = AF_UNIX;
        memcpy(address->sun_path, (const unsigned char *)guest + 2,
            path_length);
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
    else required = host_length;
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
        0x00000100u | 0x00004000u | 0x00010000u;
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

static int32_t make_socket(uint32_t *arguments)
{
    int domain = host_domain((int)arguments[0]);
    int type, status_flags, descriptor_flags;
    int fd;
    if (domain < 0 || socket_type(arguments[1], &type, &status_flags,
            &descriptor_flags) != 0) return -EINVAL;
    fd = socket(domain, type, (int)arguments[2]);
    if (fd < 0) return -(int32_t)linux_errno_number(errno);
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
            &address, &length);
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
    if (with_flags && socket_type(SOCK_STREAM | arguments[3], &ignored_type,
            &status_flags, &descriptor_flags) != 0) return -EINVAL;
    fd = accept((int)arguments[0], arguments[1] == 0 ? 0 :
        (struct sockaddr *)&address, arguments[1] == 0 ? 0 : &length);
    if (fd < 0) return -(int32_t)linux_errno_number(errno);
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
            converted = guest_address((void *)arguments[4], arguments[5],
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

static int32_t message_call(uint32_t number, uint32_t *arguments)
{
    struct linux_msghdr guest;
    struct msghdr host;
    struct sockaddr_storage address;
    socklen_t address_length;
    int flags;
    int converted;
    ssize_t result;
    if (arguments[1] == 0) return -EFAULT;
    memcpy(&guest, (void *)arguments[1], sizeof(guest));
    if (guest.control != 0 || guest.control_length != 0)
        return -LINUX_EOPNOTSUPP;
    if (message_flags(arguments[2], &flags) != 0) return -EINVAL;
    memset(&host, 0, sizeof(host));
    host.msg_iov = (struct iovec *)guest.iov;
    host.msg_iovlen = guest.iov_count;
    if (number == LINUX_NR_SENDMSG && guest.name != 0) {
        converted = guest_address((void *)guest.name, guest.name_length,
            &address, &address_length);
        if (converted != 0) return converted;
        host.msg_name = &address;
        host.msg_namelen = address_length;
    } else if (number == LINUX_NR_RECVMSG && guest.name != 0) {
        host.msg_name = &address;
        host.msg_namelen = sizeof(address);
    }
    result = number == LINUX_NR_SENDMSG ?
        sendmsg((int)arguments[0], &host, flags) :
        recvmsg((int)arguments[0], &host, flags);
    if (result < 0) return -(int32_t)linux_errno_number(errno);
    if (number == LINUX_NR_RECVMSG) {
        guest.flags = linux_message_flags(host.msg_flags);
        if (guest.name != 0) {
            uint32_t capacity = guest.name_length;
            converted = store_address((void *)guest.name, &capacity,
                (struct sockaddr *)&address, host.msg_namelen);
            if (converted != 0) return converted;
            guest.name_length = capacity;
        }
        memcpy((void *)arguments[1], &guest, sizeof(guest));
    }
    return (int32_t)result;
}

static uint32_t socketcall_number(uint32_t call)
{
    static const uint16_t calls[] = { 0, LINUX_NR_SOCKET, LINUX_NR_BIND,
        LINUX_NR_CONNECT, LINUX_NR_LISTEN, LINUX_NR_ACCEPT,
        LINUX_NR_GETSOCKNAME, LINUX_NR_GETPEERNAME, LINUX_NR_SOCKETPAIR,
        LINUX_NR_SEND, LINUX_NR_RECV, LINUX_NR_SENDTO, LINUX_NR_RECVFROM,
        LINUX_NR_SHUTDOWN, LINUX_NR_SETSOCKOPT, LINUX_NR_GETSOCKOPT,
        LINUX_NR_SENDMSG, LINUX_NR_RECVMSG, LINUX_NR_ACCEPT4 };
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
    default: return -ENOSYS;
    }
}
