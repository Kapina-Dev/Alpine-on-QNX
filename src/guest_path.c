#include "linuxemu.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char root_path[PATH_MAX];
static char guest_cwd[PATH_MAX] = "/";

static int normalize_guest_path(const char *path, char *output, size_t size);

static int resolve_guest_symlinks(const char *normalized, int allow_missing_leaf,
    char *resolved, size_t resolved_size)
{
    char current[PATH_MAX];
    char parent[PATH_MAX];
    char component[PATH_MAX];
    char host_candidate[PATH_MAX];
    char target[PATH_MAX];
    char combined[PATH_MAX * 2];
    unsigned link_count = 0;

    if (strlen(normalized) + 1 > sizeof(current)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(current, normalized);
restart:
    parent[0] = '\0';
    {
        const char *cursor = current + 1;
        while (*cursor != '\0') {
            const char *end = strchr(cursor, '/');
            size_t component_length = end == 0 ? strlen(cursor) :
                (size_t)(end - cursor);
            int last = end == 0;
            struct stat status;
            int length;

            if (component_length + 1 > sizeof(component)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(component, cursor, component_length);
            component[component_length] = '\0';
            length = snprintf(host_candidate, sizeof(host_candidate), "%s%s/%s",
                root_path, parent, component);
            if (length < 0 || (size_t)length >= sizeof(host_candidate)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            if (lstat(host_candidate, &status) != 0) {
                if (allow_missing_leaf && last && errno == ENOENT) break;
                return -1;
            }
            if (S_ISLNK(status.st_mode)) {
                ssize_t target_length = readlink(host_candidate, target,
                    sizeof(target) - 1);
                if (target_length < 0) return -1;
                target[target_length] = '\0';
                if (++link_count > 40) {
                    errno = ELOOP;
                    return -1;
                }
                if (target[0] == '/')
                    length = snprintf(combined, sizeof(combined), "%s%s%s",
                        target, last ? "" : "/", last ? "" : end + 1);
                else
                    length = snprintf(combined, sizeof(combined), "%s/%s%s%s",
                        parent[0] == '\0' ? "/" : parent, target,
                        last ? "" : "/", last ? "" : end + 1);
                if (length < 0 || (size_t)length >= sizeof(combined) ||
                    normalize_guest_path(combined, current, sizeof(current)) != 0)
                    return -1;
                goto restart;
            }
            {
                size_t parent_length = strlen(parent);
                length = snprintf(parent + parent_length,
                    sizeof(parent) - parent_length, "/%s", component);
                if (length < 0 ||
                    (size_t)length >= sizeof(parent) - parent_length) {
                    errno = ENAMETOOLONG;
                    return -1;
                }
            }
            if (last) break;
            cursor = end + 1;
        }
    }
    if (strlen(current) + 1 > resolved_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(resolved, current);
    return 0;
}

static int normalize_guest_path(const char *path, char *output, size_t size)
{
    char combined[PATH_MAX * 2];
    char *components[PATH_MAX / 2];
    char *cursor;
    size_t count = 0;
    size_t position = 0;
    size_t index;
    int length;

    if (path == 0 || path[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    length = path[0] == '/' ? snprintf(combined, sizeof(combined), "%s", path) :
        snprintf(combined, sizeof(combined), "%s/%s", guest_cwd, path);
    if (length < 0 || (size_t)length >= sizeof(combined)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    cursor = combined;
    while (*cursor != '\0') {
        char *start;
        while (*cursor == '/') cursor++;
        if (*cursor == '\0') break;
        start = cursor;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        if (*cursor != '\0') *cursor++ = '\0';
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) {
            if (count != 0) count--;
            continue;
        }
        components[count++] = start;
    }
    if (size < 2) {
        errno = ERANGE;
        return -1;
    }
    output[position++] = '/';
    for (index = 0; index < count; ++index) {
        size_t component_length = strlen(components[index]);
        if (position + component_length + (index + 1 < count ? 1 : 0) >= size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(output + position, components[index], component_length);
        position += component_length;
        if (index + 1 < count) output[position++] = '/';
    }
    output[position] = '\0';
    return 0;
}

int guest_path_initialize(const char *root)
{
    if (root == 0 || root[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (realpath(root, root_path) == 0) return -1;
    return 0;
}

int guest_path_resolve(const char *path, int allow_missing_leaf,
    char *host_path, size_t host_path_size)
{
    char guest_path[PATH_MAX];
    char resolved_guest_path[PATH_MAX];
    char candidate[PATH_MAX];
    int length;

    if (root_path[0] == '\0' ||
        normalize_guest_path(path, guest_path, sizeof(guest_path)) != 0) return -1;
    if (resolve_guest_symlinks(guest_path, allow_missing_leaf,
            resolved_guest_path, sizeof(resolved_guest_path)) != 0) return -1;
    length = snprintf(candidate, sizeof(candidate), "%s%s", root_path,
        resolved_guest_path);
    if (length < 0 || (size_t)length >= sizeof(candidate)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (strlen(candidate) + 1 > host_path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(host_path, candidate);
    return 0;
}

int guest_path_resolve_nofollow(const char *path, char *host_path,
    size_t host_path_size)
{
    char guest_path[PATH_MAX];
    char parent[PATH_MAX];
    char resolved_parent[PATH_MAX];
    char leaf[PATH_MAX];
    char *slash;
    int length;

    if (root_path[0] == '\0' ||
        normalize_guest_path(path, guest_path, sizeof(guest_path)) != 0)
        return -1;
    if (strcmp(guest_path, "/") == 0) {
        if (strlen(root_path) + 1 > host_path_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(host_path, root_path);
        return 0;
    }
    strcpy(parent, guest_path);
    slash = strrchr(parent, '/');
    strcpy(leaf, slash + 1);
    if (slash == parent) strcpy(parent, "/");
    else *slash = '\0';
    if (resolve_guest_symlinks(parent, 0, resolved_parent,
            sizeof(resolved_parent)) != 0) return -1;
    length = snprintf(host_path, host_path_size, "%s%s%s%s", root_path,
        resolved_parent, strcmp(resolved_parent, "/") == 0 ? "" : "/", leaf);
    if (length < 0 || (size_t)length >= host_path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int guest_path_resolve_at(const char *host_directory, const char *path,
    int allow_missing_leaf, int nofollow, char *host_path,
    size_t host_path_size)
{
    char combined[PATH_MAX * 2];
    const char *guest_directory;
    size_t root_length;
    int length;

    if (path == 0 || path[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (path[0] == '/')
        return nofollow ? guest_path_resolve_nofollow(path, host_path,
            host_path_size) : guest_path_resolve(path, allow_missing_leaf,
            host_path, host_path_size);
    if (host_directory == 0 || root_path[0] == '\0') {
        errno = EBADF;
        return -1;
    }
    root_length = strlen(root_path);
    if (strncmp(host_directory, root_path, root_length) != 0 ||
        (host_directory[root_length] != '\0' &&
            host_directory[root_length] != '/')) {
        errno = EACCES;
        return -1;
    }
    guest_directory = host_directory + root_length;
    if (guest_directory[0] == '\0') guest_directory = "/";
    length = snprintf(combined, sizeof(combined), "%s/%s", guest_directory,
        path);
    if (length < 0 || (size_t)length >= sizeof(combined)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return nofollow ? guest_path_resolve_nofollow(combined, host_path,
        host_path_size) : guest_path_resolve(combined, allow_missing_leaf,
        host_path, host_path_size);
}

int guest_path_getcwd(char *buffer, size_t size)
{
    size_t length = strlen(guest_cwd) + 1;
    if (length > size) {
        errno = ERANGE;
        return -1;
    }
    memcpy(buffer, guest_cwd, length);
    return (int)length;
}

int guest_path_readlink(const char *path, char *buffer, size_t size)
{
    char candidate[PATH_MAX];
    ssize_t count;

    if (guest_path_resolve_nofollow(path, candidate, sizeof(candidate)) != 0)
        return -1;
    count = readlink(candidate, buffer, size);
    return count < 0 ? -1 : (int)count;
}
