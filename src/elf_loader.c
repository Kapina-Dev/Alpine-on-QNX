#include "linuxemu.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int range_in_file(Elf32_Off offset, Elf32_Word length, off_t file_size)
{
    return file_size >= 0 && (uint64_t)offset <= (uint64_t)file_size &&
        (uint64_t)length <= (uint64_t)file_size - (uint64_t)offset;
}

static int valid_alignment(Elf32_Word alignment)
{
    return alignment == 0 || alignment == 1 ||
        (alignment & (alignment - 1u)) == 0;
}

static int validate_program_header(const Elf32_Phdr *header, off_t file_size)
{
    if (header->p_type == PT_LOAD && header->p_memsz != 0) {
        if (header->p_memsz < header->p_filesz ||
            !range_in_file(header->p_offset, header->p_filesz, file_size) ||
            !valid_alignment(header->p_align) ||
            (header->p_align > 1 &&
                ((header->p_vaddr - header->p_offset) &
                    (header->p_align - 1u)) != 0) ||
            (header->p_flags & ~(PF_R | PF_W | PF_X)) != 0 ||
            (header->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
            errno = ENOEXEC;
            return -1;
        }
    } else if (header->p_type == PT_INTERP &&
        !range_in_file(header->p_offset, header->p_filesz, file_size)) {
        errno = ENOEXEC;
        return -1;
    }
    return 0;
}

static int safe_interpreter_path(const char *path)
{
    const char *component;

    if (path == 0 || path[0] != '/' || path[1] == '\0' ||
        path[strlen(path) - 1] == '/') return 0;
    for (component = path + 1; *component != '\0';) {
        const char *end = strchr(component, '/');
        size_t length = end == 0 ? strlen(component) : (size_t)(end - component);
        if (length == 0 || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return 0;
        }
        if (end == 0) break;
        component = end + 1;
    }
    return 1;
}

static int section_in_executable_load(const Elf32_Shdr *section,
    const Elf32_Phdr *program_headers, size_t program_header_count)
{
    size_t i;
    for (i = 0; i < program_header_count; ++i) {
        const Elf32_Phdr *header = &program_headers[i];
        Elf32_Addr address_delta;
        Elf32_Off file_delta;
        if (header->p_type != PT_LOAD || (header->p_flags & PF_X) == 0 ||
            section->sh_addr < header->p_vaddr ||
            section->sh_offset < header->p_offset) continue;
        address_delta = section->sh_addr - header->p_vaddr;
        file_delta = section->sh_offset - header->p_offset;
        if (address_delta == file_delta && file_delta <= header->p_filesz &&
            section->sh_size <= header->p_filesz - file_delta) return 1;
    }
    return 0;
}

static int patch_executable_sections(int fd, const Elf32_Ehdr *elf_header,
    const Elf32_Phdr *program_headers, uintptr_t load_bias, off_t file_size)
{
    Elf32_Shdr *sections;
    size_t table_size;
    size_t i;
    size_t executable_sections = 0;
    int result = -1;

    if (elf_header->e_shnum == 0 ||
        elf_header->e_shentsize != sizeof(Elf32_Shdr) ||
        elf_header->e_shoff > (Elf32_Off)file_size) {
        errno = ENOEXEC;
        return -1;
    }
    table_size = (size_t)elf_header->e_shnum * sizeof(Elf32_Shdr);
    if (table_size > (size_t)file_size - elf_header->e_shoff) {
        errno = ENOEXEC;
        return -1;
    }
    sections = malloc(table_size);
    if (sections == 0 || read_exact_at(fd, sections, table_size,
            (off_t)elf_header->e_shoff) != 0) {
        free(sections);
        return -1;
    }
    for (i = 0; i < elf_header->e_shnum; ++i) {
        uintptr_t start;
        Elf32_Shdr *section = &sections[i];

        if ((section->sh_flags & SHF_EXECINSTR) == 0 || section->sh_size == 0)
            continue;
        if (section->sh_type != SHT_PROGBITS ||
            !range_in_file(section->sh_offset, section->sh_size, file_size) ||
            !valid_alignment(section->sh_addralign) ||
            (section->sh_flags & SHF_WRITE) != 0 ||
            !section_in_executable_load(section, program_headers,
                elf_header->e_phnum) ||
            load_bias > UINTPTR_MAX - section->sh_addr) {
            errno = ENOEXEC;
            goto done;
        }
        start = load_bias + section->sh_addr;
        if ((start & 3u) != 0 || (section->sh_size & 3u) != 0 ||
            !guest_memory_is_executable(start, section->sh_size) ||
            arm_patch_range(start, section->sh_size) != 0) {
            if (errno == 0) errno = ENOEXEC;
            goto done;
        }
        executable_sections++;
    }
    if (executable_sections == 0) {
        errno = ENOEXEC;
        goto done;
    }
    result = 0;
done:
    free(sections);
    return result;
}

static int load_elf_object(const char *path, uintptr_t dynamic_bias,
    int allow_executable, struct elf_object *object,
    char *interpreter, size_t interpreter_capacity)
{
    Elf32_Ehdr elf_header;
    Elf32_Phdr *program_headers = 0;
    struct stat file_status;
    size_t headers_size;
    size_t i;
    size_t interpreter_count = 0;
    size_t loads_before = guest_memory_segment_count();
    int fd = -1;
    int result = -1;
    uintptr_t load_bias;

    memset(object, 0, sizeof(*object));
    if (interpreter != 0 && interpreter_capacity != 0) interpreter[0] = '\0';
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &file_status) != 0 || file_status.st_size < 0 ||
        (uint64_t)file_status.st_size > SIZE_MAX ||
        read_exact_at(fd, &elf_header, sizeof(elf_header), 0) != 0) goto done;

    if (memcmp(elf_header.e_ident, ELFMAG, SELFMAG) != 0 ||
        elf_header.e_ident[EI_CLASS] != ELFCLASS32 ||
        elf_header.e_ident[EI_DATA] != ELFDATA2LSB ||
        elf_header.e_ident[EI_VERSION] != EV_CURRENT ||
        elf_header.e_version != EV_CURRENT ||
        elf_header.e_machine != EM_ARM ||
        (elf_header.e_type != ET_DYN &&
            !(allow_executable && elf_header.e_type == ET_EXEC)) ||
        elf_header.e_ehsize != sizeof(Elf32_Ehdr) ||
        elf_header.e_phentsize != sizeof(Elf32_Phdr) ||
        elf_header.e_phnum == 0 || elf_header.e_phnum > MAX_LOAD_SEGMENTS ||
        elf_header.e_phoff > (Elf32_Off)file_status.st_size ||
        (elf_header.e_entry & 1u) != 0) {
        errno = ENOEXEC;
        goto done;
    }
    headers_size = (size_t)elf_header.e_phnum * sizeof(Elf32_Phdr);
    if (headers_size > (size_t)file_status.st_size - elf_header.e_phoff) {
        errno = ENOEXEC;
        goto done;
    }
    program_headers = malloc(headers_size);
    if (program_headers == 0 || read_exact_at(fd, program_headers,
            headers_size, (off_t)elf_header.e_phoff) != 0) goto done;

    for (i = 0; i < elf_header.e_phnum; ++i) {
        if (validate_program_header(&program_headers[i], file_status.st_size) != 0)
            goto done;
        if (program_headers[i].p_type == PT_INTERP) interpreter_count++;
    }
    if (interpreter_count > 1 || (interpreter_count != 0 && interpreter == 0)) {
        errno = ENOEXEC;
        goto done;
    }
    load_bias = elf_header.e_type == ET_DYN ? dynamic_bias : 0;

    for (i = 0; i < elf_header.e_phnum; ++i) {
        Elf32_Phdr *header = &program_headers[i];
        if (header->p_type == PT_INTERP) {
            if (header->p_filesz < 2 || header->p_filesz > interpreter_capacity ||
                read_exact_at(fd, interpreter, header->p_filesz,
                    (off_t)header->p_offset) != 0 ||
                interpreter[header->p_filesz - 1] != '\0' ||
                memchr(interpreter, '\0', header->p_filesz - 1) != 0 ||
                !safe_interpreter_path(interpreter)) {
                errno = ENOEXEC;
                goto done;
            }
        }
        if (header->p_type == PT_LOAD && header->p_memsz != 0 &&
            guest_memory_map_load_segment(fd, header, load_bias,
                file_status.st_size) != 0) goto done;
    }
    if (guest_memory_segment_count() == loads_before ||
        patch_executable_sections(fd, &elf_header, program_headers, load_bias,
            file_status.st_size) != 0 ||
        load_bias > UINTPTR_MAX - elf_header.e_entry) {
        if (errno == 0) errno = ENOEXEC;
        goto done;
    }

    object->entry = load_bias + elf_header.e_entry;
    object->load_bias = load_bias;
    object->program_header_size = elf_header.e_phentsize;
    object->program_header_count = elf_header.e_phnum;
    for (i = 0; i < elf_header.e_phnum; ++i) {
        Elf32_Phdr *header = &program_headers[i];
        Elf32_Off delta;
        if (header->p_type != PT_LOAD || elf_header.e_phoff < header->p_offset)
            continue;
        delta = elf_header.e_phoff - header->p_offset;
        if (delta <= header->p_filesz &&
            headers_size <= header->p_filesz - delta) {
            object->program_headers = load_bias + header->p_vaddr + delta;
            break;
        }
    }
    result = 0;
done:
    free(program_headers);
    if (fd >= 0) close(fd);
    return result;
}

int load_guest_image(const char *path, struct guest_image *image)
{
    struct elf_object main_object;
    struct elf_object interpreter_object;
    char interpreter[PATH_MAX];
    char interpreter_path[PATH_MAX];
    char canonical_root[PATH_MAX];
    char canonical_interpreter[PATH_MAX];
    const char *root;
    int length;

    memset(image, 0, sizeof(*image));
    if (load_elf_object(path, MAIN_ET_DYN_BIAS, 1, &main_object,
            interpreter, sizeof(interpreter)) != 0) return -1;
    image->entry = main_object.entry;
    image->start_entry = main_object.entry;
    image->program_headers = main_object.program_headers;
    image->program_header_size = main_object.program_header_size;
    image->program_header_count = main_object.program_header_count;

    if (interpreter[0] != '\0') {
        root = getenv("LINUXEMU_ROOT");
        if (root == 0 || root[0] == '\0') {
            errno = ENOENT;
            return -1;
        }
        length = snprintf(interpreter_path, sizeof(interpreter_path),
            "%s%s", root, interpreter);
        if (length < 0 || (size_t)length >= sizeof(interpreter_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (realpath(root, canonical_root) == 0 ||
            realpath(interpreter_path, canonical_interpreter) == 0) return -1;
        length = (int)strlen(canonical_root);
        if (strncmp(canonical_root, canonical_interpreter, (size_t)length) != 0 ||
            (canonical_root[1] != '\0' && canonical_interpreter[length] != '/')) {
            errno = EACCES;
            return -1;
        }
        if (load_elf_object(canonical_interpreter, INTERPRETER_BIAS, 0,
                &interpreter_object, 0, 0) != 0) return -1;
        image->start_entry = interpreter_object.entry;
        image->interpreter_base = interpreter_object.load_bias;
    }

    if (arm_patch_count() == 0 ||
        !guest_memory_is_executable(image->entry, 4) ||
        !guest_memory_is_executable(image->start_entry, 4) ||
        guest_memory_finalize() != 0) {
        if (errno == 0) errno = ENOEXEC;
        return -1;
    }
    return 0;
}
