#include "linuxemu.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

struct load_segment {
    uintptr_t map_start;
    size_t map_length;
    int final_protection;
};

static struct load_segment load_segments[MAX_LOAD_SEGMENTS];
static size_t load_segment_count;
static uintptr_t page_size;

static int ranges_overlap(uintptr_t first_start, uintptr_t first_end,
    uintptr_t second_start, uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static int validate_load_range(uintptr_t start, uintptr_t end)
{
    size_t i;

    if (start < GUEST_MIN_ADDRESS || end <= start || end > GUEST_MAX_ADDRESS) {
        errno = ENOEXEC;
        return -1;
    }
    for (i = 0; i < load_segment_count; ++i) {
        uintptr_t existing_end = load_segments[i].map_start +
            load_segments[i].map_length;
        if (ranges_overlap(start, end, load_segments[i].map_start,
                existing_end)) {
            errno = ENOEXEC;
            return -1;
        }
    }
    return 0;
}

static int protection_from_flags(Elf32_Word flags)
{
    int protection = PROT_NONE;

    if ((flags & PF_R) != 0) protection |= PROT_READ;
    if ((flags & PF_W) != 0) protection |= PROT_WRITE;
    if ((flags & PF_X) != 0) protection |= PROT_EXEC;
    return protection;
}

int guest_memory_initialize(long host_page_size)
{
    if (host_page_size <= 0 ||
        ((uintptr_t)host_page_size & ((uintptr_t)host_page_size - 1u)) != 0) {
        errno = EINVAL;
        return -1;
    }
    page_size = (uintptr_t)host_page_size;
    return 0;
}

int guest_memory_map_load_segment(int fd, const Elf32_Phdr *header,
    uintptr_t load_bias, off_t file_size)
{
    uintptr_t segment_start;
    uintptr_t segment_end;
    uintptr_t map_start;
    uintptr_t map_end;
    void *mapping;

    if (header->p_memsz < header->p_filesz || file_size < 0 ||
        (uint64_t)header->p_offset > (uint64_t)file_size ||
        (uint64_t)header->p_filesz >
            (uint64_t)file_size - (uint64_t)header->p_offset ||
        load_bias > UINTPTR_MAX - (uintptr_t)header->p_vaddr) {
        errno = ENOEXEC;
        return -1;
    }
    segment_start = load_bias + (uintptr_t)header->p_vaddr;
    if ((uintptr_t)header->p_memsz > UINTPTR_MAX - segment_start) {
        errno = ENOEXEC;
        return -1;
    }
    segment_end = segment_start + header->p_memsz;
    map_start = align_down(segment_start, page_size);
    if (segment_end > UINTPTR_MAX - (page_size - 1u)) {
        errno = ENOEXEC;
        return -1;
    }
    map_end = align_up(segment_end, page_size);
    if (validate_load_range(map_start, map_end) != 0) {
        return -1;
    }
    if (load_segment_count == ARRAY_COUNT(load_segments)) {
        errno = E2BIG;
        return -1;
    }

    mapping = mmap((void *)map_start, map_end - map_start,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        return -1;
    }
    if ((uintptr_t)mapping != map_start) {
        munmap(mapping, map_end - map_start);
        errno = EEXIST;
        return -1;
    }

    if (header->p_filesz != 0 &&
        read_exact_at(fd, (void *)segment_start, header->p_filesz,
            (off_t)header->p_offset) != 0) {
        return -1;
    }
    if (header->p_memsz > header->p_filesz) {
        memset((void *)(segment_start + header->p_filesz), 0,
            header->p_memsz - header->p_filesz);
    }
    if (segment_start != map_start) {
        memset((void *)map_start, 0, segment_start - map_start);
    }

    load_segments[load_segment_count].map_start = map_start;
    load_segments[load_segment_count].map_length = map_end - map_start;
    load_segments[load_segment_count].final_protection =
        protection_from_flags(header->p_flags);
    load_segment_count++;
    return 0;
}

int guest_memory_finalize(void)
{
    size_t i;

    for (i = 0; i < load_segment_count; ++i) {
        if (mprotect((void *)load_segments[i].map_start,
                load_segments[i].map_length,
                load_segments[i].final_protection) != 0) {
            return -1;
        }
        if ((load_segments[i].final_protection & PROT_EXEC) != 0 &&
            msync((void *)load_segments[i].map_start,
                load_segments[i].map_length,
                MS_INVALIDATE_ICACHE) != 0) {
            return -1;
        }
    }
    return 0;
}

int guest_memory_is_executable(uintptr_t address, size_t length)
{
    size_t i;

    if (length > UINTPTR_MAX - address) return 0;
    for (i = 0; i < load_segment_count; ++i) {
        uintptr_t end = load_segments[i].map_start +
            load_segments[i].map_length;
        if ((load_segments[i].final_protection & PROT_EXEC) != 0 &&
            address >= load_segments[i].map_start && address + length <= end) {
            return 1;
        }
    }
    return 0;
}

size_t guest_memory_segment_count(void)
{
    return load_segment_count;
}
