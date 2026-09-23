#include "linuxemu.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ARM_LINUX_SVC_0 0xef000000u
#define ARM_TPIDRURO_MASK 0xffff0fffu
#define ARM_TPIDRURO_READ 0xee1d0f70u
#define ARM_UDF_TRAP 0xe7f000f0u

struct patch_record {
    uintptr_t address;
    uint32_t original;
};

static struct patch_record patches[MAX_PATCHES];
static size_t patch_count;

int arm_patch_range(uintptr_t start, size_t length)
{
    uintptr_t address;
    uintptr_t end;

    if ((start & 3u) != 0 || (length & 3u) != 0 ||
        length > UINTPTR_MAX - start) {
        errno = ENOEXEC;
        return -1;
    }
    end = start + length;
    for (address = start; address < end; address += 4) {
        uint32_t *instruction = (uint32_t *)address;
        uint32_t word = *instruction;

        if (word != ARM_LINUX_SVC_0 &&
            (word & ARM_TPIDRURO_MASK) != ARM_TPIDRURO_READ) {
            continue;
        }
        if (patch_count == ARRAY_COUNT(patches)) {
            errno = E2BIG;
            return -1;
        }
        patches[patch_count].address = address;
        patches[patch_count].original = word;
        patch_count++;
        *instruction = ARM_UDF_TRAP;
    }
    return 0;
}

const uint32_t *arm_patch_original(uintptr_t address)
{
    size_t i;

    for (i = 0; i < patch_count; ++i) {
        if (patches[i].address == address) {
            return &patches[i].original;
        }
    }
    return 0;
}

size_t arm_patch_count(void)
{
    return patch_count;
}

int arm_patch_elf_mapping(int fd, off_t offset, uintptr_t address,
    size_t length)
{
    Elf32_Ehdr header;
    Elf32_Shdr *sections = 0;
    struct stat status;
    size_t table_size;
    size_t i;
    int result = 0;
    uint64_t mapping_start;
    uint64_t mapping_end;

    if (fd < 0 || offset < 0 || length == 0 ||
        (uint64_t)length > UINT64_MAX - (uint64_t)offset) return 0;
    if (fstat(fd, &status) != 0 || status.st_size < 0 ||
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
            (off_t)header.e_shoff) != 0) {
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

void arm_patch_rollback(size_t retained_count)
{
    while (patch_count > retained_count) {
        patch_count--;
        *(uint32_t *)patches[patch_count].address = patches[patch_count].original;
    }
}
