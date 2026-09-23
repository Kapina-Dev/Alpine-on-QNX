#include "linuxemu.h"

#include <errno.h>
#include <unistd.h>

uintptr_t align_down(uintptr_t value, uintptr_t alignment)
{
    return value & ~(alignment - 1u);
}

uintptr_t align_up(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

int read_exact_at(int fd, void *buffer, size_t length, off_t offset)
{
    unsigned char *cursor = buffer;

    while (length != 0) {
        ssize_t result = pread(fd, cursor, length, offset);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            return -1;
        }
        if (result == 0) {
            errno = EIO;
            return -1;
        }
        cursor += result;
        offset += result;
        length -= (size_t)result;
    }
    return 0;
}
