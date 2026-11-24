#include "linuxemu.h"
#include "code_map.h"
#include "linux_diagnostics.h"
#include "thumb_decode.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

static int in_file(uint64_t offset, uint64_t length, uint64_t file_size)
{
    return offset <= file_size && length <= file_size - offset;
}

/* Executable code section: the only place mapping symbols and patching
   apply. */
static int code_section(const Elf32_Shdr *section)
{
    return section->sh_type == SHT_PROGBITS &&
        (section->sh_flags & SHF_EXECINSTR) != 0 && section->sh_size != 0;
}

/* The last file decoded: musl maps a library 1-3 times while loading it,
   and each mapping that holds code asks for its symbols. */
static pthread_mutex_t synthesis_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int valid;
    dev_t device;
    ino_t inode;
    off64_t size;
    time_t modified;
    struct code_map_result map;
} synthesis_cache;

/* Decode the file, or copy the cached result for it. */
static int decode_file(int fd, uint64_t file_size, struct code_map_result *map)
{
    struct stat64 status;
    void *file;
    int result;
    int keyed = fstat64(fd, &status) == 0;

    memset(map, 0, sizeof(*map));
    pthread_mutex_lock(&synthesis_lock);
    if (keyed && synthesis_cache.valid &&
        synthesis_cache.device == status.st_dev &&
        synthesis_cache.inode == status.st_ino &&
        synthesis_cache.size == status.st_size &&
        synthesis_cache.modified == status.st_mtime) {
        *map = synthesis_cache.map;
        map->symbols = 0;
        if (map->symbol_count != 0) {
            map->symbols = malloc(map->symbol_count * sizeof(*map->symbols));
            if (map->symbols == 0) {
                pthread_mutex_unlock(&synthesis_lock);
                return -1;
            }
            memcpy(map->symbols, synthesis_cache.map.symbols,
                map->symbol_count * sizeof(*map->symbols));
        }
        pthread_mutex_unlock(&synthesis_lock);
        return 0;
    }
    pthread_mutex_unlock(&synthesis_lock);

    file = mmap(0, (size_t)file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) return -1;
    result = code_map_from_elf(file, (size_t)file_size, map);
    munmap(file, (size_t)file_size);
    if (result != 0) return -1;

    if (keyed) {
        struct mapping_symbol *copy = 0;
        if (map->symbol_count != 0) {
            copy = malloc(map->symbol_count * sizeof(*copy));
            if (copy != 0)
                memcpy(copy, map->symbols,
                    map->symbol_count * sizeof(*copy));
        }
        if (map->symbol_count == 0 || copy != 0) {
            pthread_mutex_lock(&synthesis_lock);
            if (synthesis_cache.valid)
                code_map_result_free(&synthesis_cache.map);
            synthesis_cache.valid = 1;
            synthesis_cache.device = status.st_dev;
            synthesis_cache.inode = status.st_ino;
            synthesis_cache.size = status.st_size;
            synthesis_cache.modified = status.st_mtime;
            synthesis_cache.map = *map;
            synthesis_cache.map.symbols = copy;
            pthread_mutex_unlock(&synthesis_lock);
        }
    }
    return 0;
}

/* No mapping symbols: decode the file (src/code_map.c). Refuses with
   ENOEXEC when a possible patch site can't be classified. */
static int synthesize_mapping_symbols(int fd, uint64_t file_size,
    struct mapping_symbol **out, size_t *count)
{
    struct code_map_result map;

    if (file_size == 0 || file_size > SIZE_MAX) return 0;
    if (decode_file(fd, file_size, &map) != 0) {
        if (errno == 0) errno = ENOMEM;
        return -1;
    }
    if (map.refused) {
        uint32_t args[6] = { map.refused_address, 0, 0, 0, 0, 0 };
        linux_diag_record(0, 0, "undecodable-code", args);
        if (getenv("LINUXEMU_VERBOSE") != 0)
            fprintf(stderr, "linuxemu: stripped code at link address 0x%x "
                "can't be decoded safely%s; refusing the file\n",
                (unsigned)map.refused_address, map.unsettled ?
                " (the decoding didn't settle)" : "");
        code_map_result_free(&map);
        errno = ENOEXEC;
        return -1;
    }
    if (map.symbol_count == 0) {
        code_map_result_free(&map);
        return 0;
    }
    *out = map.symbols;
    *count = map.symbol_count;
    return 0;
}

int arm_mapping_symbols_load(int fd, const Elf32_Shdr *sections,
    size_t section_count, uint64_t file_size, struct mapping_symbol **out,
    size_t *count)
{
    struct mapping_symbol *symbols = 0;
    int result = 0;
    Elf32_Sym *table = 0;
    char *strings = 0;
    size_t i;

    *count = 0;
    for (i = 0; i < section_count; ++i) {
        const Elf32_Shdr *symtab = &sections[i];
        const Elf32_Shdr *strtab;
        size_t entries;
        size_t j;

        if (symtab->sh_type != SHT_SYMTAB) continue;
        if (symtab->sh_entsize != sizeof(Elf32_Sym) ||
            symtab->sh_link >= section_count) break;
        strtab = &sections[symtab->sh_link];
        if (strtab->sh_size == 0 ||
            !in_file(symtab->sh_offset, symtab->sh_size, file_size) ||
            !in_file(strtab->sh_offset, strtab->sh_size, file_size))
            break;
        entries = symtab->sh_size / sizeof(Elf32_Sym);
        table = malloc(symtab->sh_size);
        strings = malloc(strtab->sh_size);
        symbols = malloc((entries ? entries : 1) * sizeof(*symbols));
        if (table == 0 || strings == 0 || symbols == 0 ||
            read_exact_at(fd, table, symtab->sh_size,
                (off64_t)symtab->sh_offset) != 0 ||
            read_exact_at(fd, strings, strtab->sh_size,
                (off64_t)strtab->sh_offset) != 0) {
            /* A table that is there but can't be read: patching the file
               as ARM would rewrite its Thumb data, so fail instead. */
            if (errno == 0) errno = ENOMEM;
            free(symbols);
            symbols = 0;
            result = -1;
            break;
        }
        strings[strtab->sh_size - 1] = '\0';
        for (j = 0; j < entries; ++j) {
            enum code_state state;
            if (table[j].st_name >= strtab->sh_size ||
                table[j].st_shndx == SHN_UNDEF ||
                table[j].st_shndx >= section_count ||
                !code_section(&sections[table[j].st_shndx]) ||
                !mapping_symbol_state(strings + table[j].st_name, &state))
                continue;
            symbols[*count].address = table[j].st_value;
            symbols[*count].state = state;
            (*count)++;
        }
        mapping_sort(symbols, *count);
        break;
    }
    free(table);
    free(strings);
    if (symbols != 0 && *count == 0) {
        free(symbols);
        symbols = 0;
    }
    *out = symbols;
    if (result == 0 && *count == 0)
        result = synthesize_mapping_symbols(fd, file_size, out, count);
    return result;
}

int arm_patch_code_runs(uintptr_t start, size_t size, uintptr_t bias,
    const struct mapping_symbol *symbols, size_t count)
{
    uintptr_t position = start;
    uintptr_t end = start + size;

    while (position < end) {
        uint32_t symbol_end;
        uintptr_t run_end;
        uintptr_t run_start = position;
        enum code_state state = mapping_state_at(symbols, count,
            (uint32_t)(position - bias), (uint32_t)(end - bias), &symbol_end);

        run_end = (uintptr_t)symbol_end + bias;
        if (run_end <= position || run_end > end) run_end = end;
        position = run_end;
        if (state == CODE_STATE_ARM) {
            uintptr_t first = (run_start + 3u) & ~(uintptr_t)3u;
            uintptr_t last = run_end & ~(uintptr_t)3u;
            if (last > first && arm_patch_range(first, last - first) != 0)
                return -1;
        } else if (state == CODE_STATE_THUMB) {
            uintptr_t first = (run_start + 1u) & ~(uintptr_t)1u;
            uintptr_t last = run_end & ~(uintptr_t)1u;
            if (last > first &&
                arm_patch_thumb_range(first, last - first) != 0)
                return -1;
        }
    }
    return 0;
}

void arm_patch_report(int fd, uint64_t offset, uint64_t length, size_t sites)
{
    struct stat64 status;
    if (getenv("LINUXEMU_VERBOSE") == 0 || fstat64(fd, &status) != 0) return;
    fprintf(stderr, "linuxemu: patch-sites ino=%llu offset=0x%llx "
        "length=0x%llx sites=%lu\n", (unsigned long long)status.st_ino,
        (unsigned long long)offset, (unsigned long long)length,
        (unsigned long)sites);
}

int arm_patch_elf_mapping(int fd, off64_t offset, uintptr_t address,
    size_t length)
{
    size_t sites_before = arm_patch_count();
    Elf32_Ehdr header;
    Elf32_Shdr *sections = 0;
    struct mapping_symbol *symbols;
    size_t symbol_count;
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
    /* Read the symbol table only when the mapping holds code: libraries
       are also mapped for their data segments. */
    for (i = 0; i < header.e_shnum; ++i) {
        uint64_t section_start = sections[i].sh_offset;
        uint64_t section_end = section_start + sections[i].sh_size;
        if (code_section(&sections[i]) && section_end >= section_start &&
            section_start < mapping_end && section_end > mapping_start) break;
    }
    if (i == header.e_shnum) {
        free(sections);
        return 0;
    }
    if (arm_mapping_symbols_load(fd, sections, header.e_shnum,
            (uint64_t)status.st_size, &symbols, &symbol_count) != 0) {
        free(sections);
        return -1;
    }
    for (i = 0; i < header.e_shnum; ++i) {
        uint64_t section_start = sections[i].sh_offset;
        uint64_t section_end = section_start + sections[i].sh_size;
        uint64_t patch_start;
        uint64_t patch_end;
        uintptr_t bias;
        if (!code_section(&sections[i]) || section_end < section_start ||
            section_start >= mapping_end || section_end <= mapping_start)
            continue;
        patch_start = section_start > mapping_start ?
            section_start : mapping_start;
        patch_end = section_end < mapping_end ? section_end : mapping_end;
        /* Runtime address minus link address, for this section. */
        bias = address + (uintptr_t)section_start - (uintptr_t)mapping_start -
            (uintptr_t)sections[i].sh_addr;
        if (arm_patch_code_runs(
                address + (uintptr_t)(patch_start - mapping_start),
                (size_t)(patch_end - patch_start), bias, symbols,
                symbol_count) != 0) {
            result = -1;
            break;
        }
    }
    free(symbols);
    free(sections);
    if (result == 0)
        arm_patch_report(fd, (uint64_t)offset, (uint64_t)length,
            arm_patch_count() - sites_before);
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
