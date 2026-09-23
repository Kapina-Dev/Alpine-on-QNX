#include "linuxemu.h"

#include <errno.h>
#include <pthread.h>
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
static uintptr_t current_break;
static uintptr_t minimum_break;
static uintptr_t break_mapping_end;

#define MAX_RUNTIME_MAPPINGS 2048
struct runtime_mapping {
    uintptr_t start;
    uintptr_t end;
    int executable;
};

static struct runtime_mapping runtime_mappings[MAX_RUNTIME_MAPPINGS];
static struct runtime_mapping runtime_mapping_scratch[MAX_RUNTIME_MAPPINGS + 2];
static size_t runtime_mapping_count;
static pthread_mutex_t runtime_mapping_lock = PTHREAD_MUTEX_INITIALIZER;

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
        (header->p_filesz != 0 &&
            ((uint64_t)header->p_offset > (uint64_t)file_size ||
            (uint64_t)header->p_filesz >
                (uint64_t)file_size - (uint64_t)header->p_offset)) ||
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
        int saved_errno = errno;
        munmap(mapping, map_end - map_start);
        errno = saved_errno;
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
    for (i = 0; i < runtime_mapping_count; ++i)
        if (runtime_mappings[i].executable &&
            address >= runtime_mappings[i].start &&
            address + length <= runtime_mappings[i].end) {
            return 1;
        }
    return 0;
}

size_t guest_memory_segment_count(void)
{
    return load_segment_count;
}

void guest_memory_rollback(size_t segment_count)
{
    while (load_segment_count > segment_count) {
        struct load_segment *segment = &load_segments[load_segment_count - 1];
        munmap((void *)segment->map_start, segment->map_length);
        memset(segment, 0, sizeof(*segment));
        load_segment_count--;
    }
}

static int runtime_range_owned_locked(uintptr_t start, uintptr_t end)
{
    uintptr_t cursor = start;
    while (cursor < end) {
        uintptr_t covered_end = cursor;
        size_t i;
        for (i = 0; i < runtime_mapping_count; ++i)
            if (runtime_mappings[i].start <= cursor &&
                runtime_mappings[i].end > covered_end)
                covered_end = runtime_mappings[i].end;
        if (covered_end == cursor) return 0;
        cursor = covered_end;
    }
    return 1;
}

static int runtime_replace_locked(uintptr_t start, uintptr_t end,
    int add_replacement, int executable)
{
    size_t replacement_count = 0;
    size_t compacted_count = 0;
    size_t i;

    for (i = 0; i < runtime_mapping_count; ++i) {
        struct runtime_mapping old = runtime_mappings[i];
        if (!ranges_overlap(start, end, old.start, old.end))
            runtime_mapping_scratch[replacement_count++] = old;
        else {
            if (old.start < start) {
                runtime_mapping_scratch[replacement_count].start = old.start;
                runtime_mapping_scratch[replacement_count].end = start;
                runtime_mapping_scratch[replacement_count].executable =
                    old.executable;
                replacement_count++;
            }
            if (old.end > end) {
                runtime_mapping_scratch[replacement_count].start = end;
                runtime_mapping_scratch[replacement_count].end = old.end;
                runtime_mapping_scratch[replacement_count].executable =
                    old.executable;
                replacement_count++;
            }
        }
    }
    if (add_replacement) {
        runtime_mapping_scratch[replacement_count].start = start;
        runtime_mapping_scratch[replacement_count].end = end;
        runtime_mapping_scratch[replacement_count].executable = executable;
        replacement_count++;
    }
    for (i = 1; i < replacement_count; ++i) {
        struct runtime_mapping item = runtime_mapping_scratch[i];
        size_t position = i;
        while (position != 0 &&
            runtime_mapping_scratch[position - 1].start > item.start) {
            runtime_mapping_scratch[position] =
                runtime_mapping_scratch[position - 1];
            position--;
        }
        runtime_mapping_scratch[position] = item;
    }
    for (i = 0; i < replacement_count; ++i) {
        if (compacted_count != 0 &&
            runtime_mapping_scratch[compacted_count - 1].end ==
                runtime_mapping_scratch[i].start &&
            runtime_mapping_scratch[compacted_count - 1].executable ==
                runtime_mapping_scratch[i].executable) {
            runtime_mapping_scratch[compacted_count - 1].end =
                runtime_mapping_scratch[i].end;
        } else {
            runtime_mapping_scratch[compacted_count++] =
                runtime_mapping_scratch[i];
        }
    }
    if (compacted_count > MAX_RUNTIME_MAPPINGS) return -1;
    memcpy(runtime_mappings, runtime_mapping_scratch,
        compacted_count * sizeof(*runtime_mappings));
    runtime_mapping_count = compacted_count;
    return 0;
}

int guest_memory_range_owned(uintptr_t address, size_t length)
{
    uintptr_t end;
    size_t i;
    int owned;
    if (length == 0 || length > UINTPTR_MAX - address) return 0;
    end = address + length;
    for (i = 0; i < load_segment_count; ++i)
        if (address >= load_segments[i].map_start &&
            end <= load_segments[i].map_start + load_segments[i].map_length)
            return 1;
    pthread_mutex_lock(&runtime_mapping_lock);
    owned = runtime_range_owned_locked(address, end);
    pthread_mutex_unlock(&runtime_mapping_lock);
    return owned;
}

int guest_memory_runtime_owned(uintptr_t address, size_t length)
{
    int owned;
    if (length == 0 || length > UINTPTR_MAX - address) return 0;
    pthread_mutex_lock(&runtime_mapping_lock);
    owned = runtime_range_owned_locked(address, address + length);
    pthread_mutex_unlock(&runtime_mapping_lock);
    return owned;
}

int guest_memory_runtime_map(uintptr_t address, size_t length, int executable)
{
    uintptr_t end;
    int result = -1;
    if (length == 0 || length > UINTPTR_MAX - address) {
        errno = EINVAL;
        return -1;
    }
    end = address + length;
    pthread_mutex_lock(&runtime_mapping_lock);
    if (runtime_replace_locked(address, end, 1, executable) == 0) {
        result = 0;
    } else errno = ENOMEM;
    pthread_mutex_unlock(&runtime_mapping_lock);
    return result;
}

int guest_memory_runtime_protect(uintptr_t address, size_t length,
    int executable)
{
    uintptr_t end;
    int result = -1;
    if (length == 0 || length > UINTPTR_MAX - address) {
        errno = EINVAL;
        return -1;
    }
    end = address + length;
    pthread_mutex_lock(&runtime_mapping_lock);
    if (!runtime_range_owned_locked(address, end)) {
        pthread_mutex_unlock(&runtime_mapping_lock);
        return 1;
    }
    else if (runtime_replace_locked(address, end, 1, executable) != 0)
        errno = ENOMEM;
    else {
        result = 0;
    }
    pthread_mutex_unlock(&runtime_mapping_lock);
    return result;
}

void guest_memory_runtime_unmap(uintptr_t address, size_t length)
{
    if (length == 0 || length > UINTPTR_MAX - address) return;
    pthread_mutex_lock(&runtime_mapping_lock);
    runtime_replace_locked(address, address + length, 0, 0);
    pthread_mutex_unlock(&runtime_mapping_lock);
}

void guest_memory_after_fork(void)
{
    pthread_mutex_t fresh_lock = PTHREAD_MUTEX_INITIALIZER;
    runtime_mapping_lock = fresh_lock;
}

int guest_brk_initialize(uintptr_t initial_break)
{
    if (initial_break < GUEST_MIN_ADDRESS || initial_break >= GUEST_MAX_ADDRESS) {
        errno = ENOEXEC;
        return -1;
    }
    current_break = initial_break;
    minimum_break = initial_break;
    break_mapping_end = align_up(initial_break, page_size);
    return 0;
}

uintptr_t guest_brk_set(uintptr_t requested)
{
    uintptr_t requested_mapping_end;
    void *mapping;

    if (requested == 0) return current_break;
    if (requested < minimum_break) return current_break;
    if (requested < current_break) {
        requested_mapping_end = align_up(requested, page_size);
        if (requested_mapping_end < break_mapping_end) {
            munmap((void *)requested_mapping_end,
                break_mapping_end - requested_mapping_end);
            guest_memory_runtime_unmap(requested_mapping_end,
                break_mapping_end - requested_mapping_end);
            break_mapping_end = requested_mapping_end;
        }
        current_break = requested;
        return current_break;
    }
    if (requested >= GUEST_MAX_ADDRESS ||
        requested > UINTPTR_MAX - (page_size - 1u)) return current_break;
    requested_mapping_end = align_up(requested, page_size);
    if (requested_mapping_end > break_mapping_end) {
        mapping = mmap((void *)break_mapping_end,
            requested_mapping_end - break_mapping_end,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapping == MAP_FAILED) return current_break;
        if ((uintptr_t)mapping != break_mapping_end) {
            munmap(mapping, requested_mapping_end - break_mapping_end);
            return current_break;
        }
        if (guest_memory_runtime_map(break_mapping_end,
                requested_mapping_end - break_mapping_end, 0) != 0) {
            munmap(mapping, requested_mapping_end - break_mapping_end);
            return current_break;
        }
        break_mapping_end = requested_mapping_end;
    }
    current_break = requested;
    return current_break;
}
