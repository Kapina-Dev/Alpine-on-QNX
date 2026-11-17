/*
 * Delete-while-iterating (guest fixture, musl): the rm -r / find -delete
 * pattern.  Read a directory in small getdents chunks and unlink every
 * entry each chunk returns before reading the next; afterwards the
 * directory must be empty (rmdir succeeds).  A resume position that skips
 * entries after deletions leaves files behind.
 *
 * Exits: 2 setup, 3 getdents error, 4 entries left behind, 5 loop bound.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FILES 200

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

int main(void)
{
    const char *dir = "/tmp/linuxemu-getdents-unlink";
    char path[256], buffer[512];
    int i, fd, rounds = 0;
    mkdir(dir, 0755);
    for (i = 0; i < FILES; ++i) {
        snprintf(path, sizeof(path), "%s/entry-%03d", dir, i);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return 2;
        close(fd);
    }
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return 2;
    for (;;) {
        long count = syscall(SYS_getdents64, fd, buffer, sizeof(buffer));
        long offset = 0;
        if (count < 0) return 3;
        if (count == 0) break;
        if (++rounds > 10000) return 5;
        while (offset < count) {
            struct linux_dirent64 *entry =
                (struct linux_dirent64 *)(buffer + offset);
            if (strcmp(entry->d_name, ".") != 0 &&
                    strcmp(entry->d_name, "..") != 0) {
                snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
                unlink(path);
            }
            offset += entry->d_reclen;
        }
    }
    close(fd);
    if (rmdir(dir) != 0) {
        int left = 0;
        for (i = 0; i < FILES; ++i) {
            snprintf(path, sizeof(path), "%s/entry-%03d", dir, i);
            if (access(path, F_OK) == 0) { ++left; unlink(path); }
        }
        rmdir(dir);
        fprintf(stderr, "entries left behind: %d of %d (errno %d)\n", left,
            FILES, errno);
        return 4;
    }
    return 0;
}
