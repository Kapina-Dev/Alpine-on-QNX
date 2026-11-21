#include "linuxemu.h"
#include "thumb_decode.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ARM_LINUX_SVC_0 0xef000000u
#define ARM_TPIDRURO_MASK 0xffff0fffu
#define ARM_TPIDRURO_READ 0xee1d0f70u
#define ARM_UDF_TRAP 0xe7f000f0u
#define THUMB_UDF_TRAP 0xde00u

static struct arm_patch_site patches[MAX_PATCHES];
static size_t patch_count;

int arm_patch_range(uintptr_t start, size_t length)
{
    uintptr_t address;
    uintptr_t end;
    int may_repeat;

    if ((start & 3u) != 0 || (length & 3u) != 0 ||
        length > UINTPTR_MAX - start) {
        errno = ENOEXEC;
        return -1;
    }
    end = start + length;
    /* A runtime page flipped back to executable may hold a site recorded on
       an earlier flip (a JIT rewrote the svc in place).  That entry is
       overwritten, not appended: an appended duplicate would fill the table
       and arm_patch_lookup would keep returning the stale first one. */
    may_repeat = arm_patch_any_in_range(start, length);
    for (address = start; address < end; address += 4) {
        uint32_t *instruction = (uint32_t *)address;
        uint32_t word = *instruction;
        struct arm_patch_site *site = 0;

        if (word != ARM_LINUX_SVC_0 &&
            (word & ARM_TPIDRURO_MASK) != ARM_TPIDRURO_READ) {
            continue;
        }
        if (may_repeat) {
            size_t i;
            for (i = 0; i < patch_count; ++i)
                if (patches[i].address == address) {
                    site = &patches[i];
                    break;
                }
        }
        if (site == 0) {
            if (patch_count == ARRAY_COUNT(patches)) {
                errno = E2BIG;
                return -1;
            }
            site = &patches[patch_count++];
        }
        site->address = address;
        site->original = word;
        site->width = 4;
        site->thumb = 0;
        *instruction = ARM_UDF_TRAP;
    }
    return 0;
}

int arm_patch_candidate(uintptr_t start, size_t length, uintptr_t *first)
{
    uintptr_t address = (start + 3u) & ~(uintptr_t)3u;
    uintptr_t end;

    if (length > UINTPTR_MAX - start) return 0;
    end = start + length;
    for (; address + 4u <= end && address >= start; address += 4) {
        uint32_t word = *(const uint32_t *)address;
        if (word == ARM_LINUX_SVC_0 ||
            (word & ARM_TPIDRURO_MASK) == ARM_TPIDRURO_READ) {
            *first = address;
            return 1;
        }
    }
    return 0;
}

int arm_patch_thumb_range(uintptr_t start, size_t length)
{
    const uint16_t *code = (const uint16_t *)start;
    struct thumb_patch *found;
    size_t count;
    size_t i;

    if ((start & 1u) != 0 || (length & 1u) != 0 ||
        length > UINTPTR_MAX - start) {
        errno = ENOEXEC;
        return -1;
    }
    count = thumb_scan(code, length / 2u, 0, 0);
    if (count == 0) return 0;
    if (count > ARRAY_COUNT(patches) - patch_count) {
        errno = E2BIG;
        return -1;
    }
    found = malloc(count * sizeof(*found));
    if (found == 0) return -1;
    thumb_scan(code, length / 2u, found, count);
    for (i = 0; i < count; ++i) {
        uint16_t *instruction = (uint16_t *)(start + found[i].offset);
        struct arm_patch_site *site = &patches[patch_count++];

        site->address = (uintptr_t)instruction;
        site->original = found[i].width == 4 ?
            ((uint32_t)instruction[0] << 16) | instruction[1] : instruction[0];
        site->width = (uint8_t)found[i].width;
        site->thumb = 1;
        instruction[0] = THUMB_UDF_TRAP;
    }
    free(found);
    return 0;
}

const struct arm_patch_site *arm_patch_lookup(uintptr_t address)
{
    size_t i;

    for (i = 0; i < patch_count; ++i) {
        if (patches[i].address == address) {
            return &patches[i];
        }
    }
    return 0;
}

size_t arm_patch_count(void)
{
    return patch_count;
}

int arm_patch_elf_mapping(int fd, off64_t offset, uintptr_t address,
    size_t length)
{
    Elf32_Ehdr header;
    Elf32_Shdr *sections = 0;
    struct stat64 status;
    size_t table_size;
    size_t i;
    int result = 0;
    uint64_t mapping_start;
    uint64_t mapping_end;

    if (fd < 0 || offset < 0 || length == 0 ||
        (uint64_t)length > UINT64_MAX - (uint64_t)offset) return 0;
    if (fstat64(fd, &status) != 0 || status.st_size < 0 ||
        read_exact_at(fd, &header, sizeof(header), 0) != 0) return 0;
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS32 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_machine != EM_ARM || header.e_shnum == 0 ||
        header.e_shentsize != sizeof(Elf32_Shdr)) return 0;
    table_size = (size_t)header.e_shnum * sizeof(Elf32_Shdr);
    if ((uint64_t)header.e_shoff > (uint64_t)status.st_size ||
        (uint64_t)table_size >
            (uint64_t)status.st_size - (uint64_t)header.e_shoff) return 0;
    sections = malloc(table_size);
    if (sections == 0 || read_exact_at(fd, sections, table_size,
            (off64_t)header.e_shoff) != 0) {
        free(sections);
        return -1;
    }
    mapping_start = (uint64_t)offset;
    mapping_end = mapping_start + length;
    for (i = 0; i < header.e_shnum; ++i) {
        uint64_t section_start = sections[i].sh_offset;
        uint64_t section_end = section_start + sections[i].sh_size;
        uint64_t patch_start;
        uint64_t patch_end;
        if (sections[i].sh_type != SHT_PROGBITS ||
            (sections[i].sh_flags & SHF_EXECINSTR) == 0 ||
            sections[i].sh_size == 0 || section_end < section_start ||
            section_start >= mapping_end || section_end <= mapping_start)
            continue;
        patch_start = section_start > mapping_start ?
            section_start : mapping_start;
        patch_end = section_end < mapping_end ? section_end : mapping_end;
        while (patch_start < patch_end &&
            ((address + (uintptr_t)(patch_start - mapping_start)) & 3u) != 0)
            patch_start++;
        patch_end -= (patch_end - patch_start) & 3u;
        if (patch_end > patch_start &&
            arm_patch_range(address + (uintptr_t)(patch_start - mapping_start),
                (size_t)(patch_end - patch_start)) != 0) {
            result = -1;
            break;
        }
    }
    free(sections);
    return result;
}

void arm_patch_forget_range(uintptr_t address, size_t length)
{
    uintptr_t end;
    size_t source;
    size_t destination = 0;
    if (length > UINTPTR_MAX - address) end = UINTPTR_MAX;
    else end = address + length;
    for (source = 0; source < patch_count; ++source)
        if (patches[source].address < address || patches[source].address >= end)
            patches[destination++] = patches[source];
    patch_count = destination;
}

/* Nonzero if any patch site lies in [start, start + length). */
int arm_patch_any_in_range(uintptr_t start, size_t length)
{
    uintptr_t end;
    size_t i;
    if (length > UINTPTR_MAX - start) end = UINTPTR_MAX;
    else end = start + length;
    for (i = 0; i < patch_count; ++i)
        if (patches[i].address >= start && patches[i].address < end) return 1;
    return 0;
}

void arm_patch_rollback(size_t retained_count)
{
    while (patch_count > retained_count) {
        patch_count--;
        if (!patches[patch_count].thumb)
            *(uint32_t *)patches[patch_count].address =
                patches[patch_count].original;
        else
            *(uint16_t *)patches[patch_count].address =
                (uint16_t)(patches[patch_count].width == 4 ?
                    patches[patch_count].original >> 16 :
                    patches[patch_count].original);
    }
}
