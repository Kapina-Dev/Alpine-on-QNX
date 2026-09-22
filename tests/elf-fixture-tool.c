#define _QNX_SOURCE 1
#include <sys/elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length != 0) {
        ssize_t count = write(fd, cursor, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        cursor += count;
        length -= (size_t)count;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct stat status;
    unsigned char *data;
    Elf32_Ehdr *elf;
    Elf32_Phdr *program_headers;
    Elf32_Shdr *section_headers;
    size_t output_size;
    size_t i;
    int input;
    int output;
    int changed = 0;

    if (argc != 4) {
        fprintf(stderr, "usage: %s INPUT OUTPUT MODE\n", argv[0]);
        return 2;
    }
    input = open(argv[1], O_RDONLY);
    if (input < 0 || fstat(input, &status) != 0 ||
        status.st_size < (off_t)sizeof(Elf32_Ehdr)) {
        perror("fixture input");
        return 1;
    }
    data = malloc((size_t)status.st_size);
    if (data == 0 || read(input, data, (size_t)status.st_size) != status.st_size) {
        perror("fixture read");
        return 1;
    }
    close(input);
    elf = (Elf32_Ehdr *)data;
    if (elf->e_phentsize != sizeof(Elf32_Phdr) ||
        elf->e_phoff + (size_t)elf->e_phnum * sizeof(Elf32_Phdr) >
            (size_t)status.st_size) {
        fprintf(stderr, "fixture source is not a usable ELF32 file\n");
        return 1;
    }
    program_headers = (Elf32_Phdr *)(data + elf->e_phoff);
    section_headers = 0;
    if (elf->e_shentsize == sizeof(Elf32_Shdr) && elf->e_shoff != 0 &&
        elf->e_shoff + (size_t)elf->e_shnum * sizeof(Elf32_Shdr) <=
            (size_t)status.st_size)
        section_headers = (Elf32_Shdr *)(data + elf->e_shoff);
    output_size = (size_t)status.st_size;

    if (strcmp(argv[3], "magic") == 0) {
        elf->e_ident[0] = 0;
        changed = 1;
    } else if (strcmp(argv[3], "machine") == 0) {
        elf->e_machine = 3;
        changed = 1;
    } else if (strcmp(argv[3], "thumb-entry") == 0) {
        elf->e_entry |= 1u;
        changed = 1;
    } else if (strcmp(argv[3], "no-sections") == 0) {
        elf->e_shoff = 0;
        elf->e_shnum = 0;
        changed = 1;
    } else if (strcmp(argv[3], "truncated") == 0) {
        output_size = sizeof(Elf32_Ehdr);
        changed = 1;
    } else if (strcmp(argv[3], "section-mismatch") == 0 &&
        section_headers != 0) {
        for (i = 0; i < elf->e_shnum; ++i) {
            if ((section_headers[i].sh_flags & SHF_EXECINSTR) != 0) {
                section_headers[i].sh_offset += 4;
                changed = 1;
                break;
            }
        }
    } else if (strcmp(argv[3], "overlap-load") == 0) {
        Elf32_Addr first_address = 0;
        for (i = 0; i < elf->e_phnum; ++i) {
            if (program_headers[i].p_type != PT_LOAD ||
                program_headers[i].p_memsz == 0) continue;
            if (first_address == 0) first_address = program_headers[i].p_vaddr;
            else {
                program_headers[i].p_vaddr = first_address;
                changed = 1;
                break;
            }
        }
    } else {
        for (i = 0; i < elf->e_phnum; ++i) {
            Elf32_Phdr *header = &program_headers[i];
            if (strcmp(argv[3], "alignment") == 0 &&
                header->p_type == PT_LOAD && header->p_memsz != 0) {
                header->p_align = 3;
                changed = 1;
                break;
            }
            if (strcmp(argv[3], "segment-bounds") == 0 &&
                header->p_type == PT_LOAD && header->p_memsz != 0) {
                header->p_filesz = 0xffffffffu;
                header->p_memsz = 0xffffffffu;
                changed = 1;
                break;
            }
            if (strcmp(argv[3], "wx-segment") == 0 &&
                header->p_type == PT_LOAD && (header->p_flags & PF_X) != 0) {
                header->p_flags |= PF_W;
                changed = 1;
                break;
            }
            if (strcmp(argv[3], "relative-interp") == 0 &&
                header->p_type == PT_INTERP && header->p_filesz > 1 &&
                header->p_offset < (Elf32_Off)status.st_size) {
                data[header->p_offset] = 'x';
                changed = 1;
                break;
            }
        }
    }
    if (!changed) {
        fprintf(stderr, "mutation did not apply: %s\n", argv[3]);
        return 1;
    }
    output = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (output < 0 || write_all(output, data, output_size) != 0 ||
        close(output) != 0) {
        perror("fixture output");
        return 1;
    }
    free(data);
    return 0;
}
