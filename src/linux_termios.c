#include "linuxemu.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define LINUX_TCGETS 0x5401u
#define LINUX_TCSETS 0x5402u
#define LINUX_TCSETSW 0x5403u
#define LINUX_TCSETSF 0x5404u
#define LINUX_TIOCGPGRP 0x540fu
#define LINUX_TIOCSPGRP 0x5410u
#define LINUX_TIOCGWINSZ 0x5413u
#define LINUX_TIOCSWINSZ 0x5414u
#define LINUX_FIONREAD 0x541bu
#define LINUX_FIONBIO 0x5421u
#define QNX_DISABLED_CHARACTER 0xffu

struct linux_termios {
    uint32_t input, output, control, local;
    unsigned char line;
    unsigned char characters[19];
};

static const unsigned char linux_cc[] =
    { VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, VSWTCH, VSTART,
      VSTOP, VSUSP, VEOL, VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2 };

static uint32_t speed_to_linux(speed_t speed)
{
    static const speed_t rates[] = { B0, B50, B75, B110, B134, B150, B200,
        B300, B600, B1200, B1800, B2400, B4800, B9600, B19200, B38400 };
    unsigned i;
    for (i = 0; i != ARRAY_COUNT(rates); ++i)
        if (speed == rates[i]) return i;
    if (speed == B57600) return 0x1001;
    if (speed == B115200) return 0x1002;
    return 15;
}

static int linux_to_speed(uint32_t control, speed_t *speed)
{
    static const speed_t rates[] = { B0, B50, B75, B110, B134, B150, B200,
        B300, B600, B1200, B1800, B2400, B4800, B9600, B19200, B38400 };
    uint32_t encoded = control & 0x100fu;
    if (encoded < ARRAY_COUNT(rates)) { *speed = rates[encoded]; return 0; }
    if (encoded == 0x1001) { *speed = B57600; return 0; }
    if (encoded == 0x1002) { *speed = B115200; return 0; }
    return -1;
}

static void termios_to_linux(const struct termios *host,
    struct linux_termios *guest)
{
    unsigned i;
    memset(guest, 0, sizeof(*guest));
    guest->input = (uint32_t)host->c_iflag & 0x3fffu;
    guest->output = (uint32_t)host->c_oflag & 0xffffu;
    guest->control = ((uint32_t)host->c_cflag & 0xff0u) |
        speed_to_linux(cfgetospeed(host));
    guest->local = (uint32_t)host->c_lflag & 0x8fffu;
    for (i = 0; i != ARRAY_COUNT(linux_cc); ++i)
        guest->characters[i] = i != 5 && i != 6 &&
            host->c_cc[linux_cc[i]] == QNX_DISABLED_CHARACTER ? 0 :
            host->c_cc[linux_cc[i]];
}

static int termios_from_linux(const struct linux_termios *guest,
    struct termios *host)
{
    speed_t speed;
    unsigned i;
    if (linux_to_speed(guest->control, &speed) != 0) return -EINVAL;
    host->c_iflag = guest->input & 0x3fffu;
    host->c_oflag = guest->output & 0xffffu;
    host->c_cflag = guest->control & 0xff0u;
    host->c_lflag = guest->local & 0x8fffu;
    for (i = 0; i != ARRAY_COUNT(linux_cc); ++i)
        host->c_cc[linux_cc[i]] = i != 5 && i != 6 &&
            guest->characters[i] == 0 ? QNX_DISABLED_CHARACTER :
            guest->characters[i];
    if (cfsetispeed(host, speed) != 0 || cfsetospeed(host, speed) != 0)
        return -(int32_t)linux_errno_number(errno);
    return 0;
}

static int32_t termios_get(int fd, void *argument)
{
    struct termios host;
    struct linux_termios guest;
    if (argument == 0) return -EFAULT;
    if (tcgetattr(fd, &host) != 0)
        return -(int32_t)linux_errno_number(errno);
    termios_to_linux(&host, &guest);
    memcpy(argument, &guest, sizeof(guest));
    return 0;
}

static int32_t termios_set(int fd, uint32_t request, const void *argument)
{
    struct termios host;
    struct linux_termios guest;
    int action = request == LINUX_TCSETS ? TCSANOW :
        request == LINUX_TCSETSW ? TCSADRAIN : TCSAFLUSH;
    int result;
    if (argument == 0) return -EFAULT;
    if (tcgetattr(fd, &host) != 0)
        return -(int32_t)linux_errno_number(errno);
    memcpy(&guest, argument, sizeof(guest));
    result = termios_from_linux(&guest, &host);
    if (result != 0) return result;
    return tcsetattr(fd, action, &host) == 0 ? 0 :
        -(int32_t)linux_errno_number(errno);
}

int32_t linux_ioctl(int fd, uint32_t request, void *guest_argument)
{
    int value;
    struct winsize window;
    switch (request) {
    case LINUX_TCGETS: return termios_get(fd, guest_argument);
    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF: return termios_set(fd, request, guest_argument);
    case LINUX_TIOCGWINSZ:
        if (guest_argument == 0) return -EFAULT;
        if (ioctl(fd, TIOCGWINSZ, &window) != 0)
            return -(int32_t)linux_errno_number(errno);
        memcpy(guest_argument, &window, 8);
        return 0;
    case LINUX_TIOCSWINSZ:
        if (guest_argument == 0) return -EFAULT;
        memcpy(&window, guest_argument, 8);
        return ioctl(fd, TIOCSWINSZ, &window) == 0 ? 0 :
            -(int32_t)linux_errno_number(errno);
    case LINUX_FIONREAD:
        if (guest_argument == 0) return -EFAULT;
        if (ioctl(fd, FIONREAD, &value) != 0)
            return -(int32_t)linux_errno_number(errno);
        memcpy(guest_argument, &value, sizeof(value));
        return 0;
    case LINUX_FIONBIO:
        if (guest_argument == 0) return -EFAULT;
        {
            int requested;
            memcpy(&requested, guest_argument, sizeof(requested));
            value = fcntl(fd, F_GETFL);
            if (value < 0) return -(int32_t)linux_errno_number(errno);
            if (requested) value |= O_NONBLOCK;
            else value &= ~O_NONBLOCK;
            return fcntl(fd, F_SETFL, value) == 0 ? 0 :
                -(int32_t)linux_errno_number(errno);
        }
    case LINUX_TIOCGPGRP:
        if (guest_argument == 0) return -EFAULT;
        value = (int)tcgetpgrp(fd);
        if (value < 0) return -(int32_t)linux_errno_number(errno);
        memcpy(guest_argument, &value, sizeof(value));
        return 0;
    case LINUX_TIOCSPGRP:
        if (guest_argument == 0) return -EFAULT;
        memcpy(&value, guest_argument, sizeof(value));
        return tcsetpgrp(fd, value) == 0 ? 0 :
            -(int32_t)linux_errno_number(errno);
    default: return -ENOTTY;
    }
}
