/*
 * code-map-survey [--pairs] DIR...: run the stripped-code decoder over
 * every ARM ELF file under each DIR and print one line per file:
 *
 *   survey file=<path> mapsyms=<0|1> thumb=<0|1> refused=<0|1> at=0x<a>
 *       unsettled=<0|1> sites=<n>
 *
 * refused/at/unsettled come from code_map_from_elf, which ignores
 * .symtab: they describe the file as if fully stripped. sites is the
 * patch-site count under the file's own mapping symbols when it has them,
 * else under the decoded map (-1 when refused): what Linuxemu patches.
 * With --pairs, files with mapping symbols are also compared with their
 * decoded map (pair file=... wrong_mode= data_as_code= undecoded=).
 * With --pairs-symtab, they are compared again with their mapping symbols
 * hidden, so their .symtab functions are the evidence (symtab-pair ...).
 *
 * Build: gcc -std=gnu99 -O2 -Isrc -Itests -o build/code-map-survey \
 *   tests/code-map-survey.c tests/code-map-compare.c src/code_map.c \
 *   src/thumb_decode.c
 */

#define _GNU_SOURCE
#include "code-map-compare.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int pairs;
static int pairs_symtab;

/* A copy of file whose mapping symbols are renamed ("$a" -> "Xa"), so
   code_map_from_elf takes its .symtab functions as evidence (the
   linker's case) while the original still holds the truth. */
static uint8_t *hide_mapping_symbols(const uint8_t *file, size_t size)
{
    uint8_t *copy = malloc(size);
    unsigned count = cm_le16(file + 48), i;
    if (copy == 0) return 0;
    memcpy(copy, file, size);
    for (i = 0; i < count; ++i) {
        const uint8_t *sh = file + cm_le32(file + 32) + 40u * i;
        uint32_t offset, entries, j, strings;
        if (cm_le32(sh + 4) != 2) continue;
        offset = cm_le32(sh + 16);
        entries = cm_le32(sh + 20) / 16u;
        strings = cm_le32(file + cm_le32(file + 32) + 40u *
            cm_le32(sh + 24) + 16);
        for (j = 0; j < entries; ++j) {
            uint32_t name = strings + cm_le32(file + offset + 16u * j);
            enum code_state state;
            if (name < size && file[name] == '$' &&
                memchr(file + name, 0, size - name) != 0 &&
                mapping_symbol_state((const char *)file + name, &state))
                copy[name] = 'X';
        }
    }
    return copy;
}
static size_t root_length;
static unsigned files, refused_files, no_thumb_files;

static struct mapping_symbol symbols[1u << 20];
static uint32_t sites[1u << 16];

static void survey(const char *path)
{
    size_t size;
    uint8_t *file = cm_read_file(path, &size);
    struct code_map_result r;
    size_t own;
    long count;
    const char *name = path + root_length;

    while (*name == '/') name++;
    if (file == 0 || size < 52 || memcmp(file, "\177ELF\001\001", 6) != 0 ||
        cm_le16(file + 18) != 40u) {
        free(file);
        return;
    }
    if (code_map_from_elf(file, size, &r) != 0) {
        printf("survey file=%s error=decode\n", name);
        free(file);
        return;
    }
    own = cm_symtab_mapping(file, symbols, 1u << 20);
    if (own != 0)
        count = (long)cm_patch_sites(file, symbols, own, sites, 1u << 16);
    else if (r.refused)
        count = -1;
    else
        count = (long)cm_patch_sites(file, r.symbols, r.symbol_count, sites,
            1u << 16);
    files++;
    if (r.refused) refused_files++;
    if (!r.refused && r.symbol_count == 0) no_thumb_files++;
    printf("survey file=%s mapsyms=%d thumb=%d refused=%d at=0x%x "
        "unsettled=%d sites=%ld\n", name, own != 0,
        r.refused || r.symbol_count != 0, r.refused,
        (unsigned)r.refused_address, r.unsettled, count);
    code_map_result_free(&r);
    if (pairs && own != 0) {
        struct cm_pair p;
        if (cm_compare_pair(file, file, size, &p) == 0)
            printf("pair file=%s wrong_mode=%zu data_as_code=%zu "
                "undecoded=%zu stub_bytes=%zu first_wrong=0x%x same_sites=%d\n",
                name, p.wrong_mode, p.data_as_code, p.undecoded, p.stub_bytes,
                (unsigned)p.first_wrong, p.same_sites);
    }
    if (pairs_symtab && own != 0) {
        uint8_t *hidden = hide_mapping_symbols(file, size);
        struct cm_pair p;
        if (hidden != 0 && cm_compare_pair(file, hidden, size, &p) == 0)
            printf("symtab-pair file=%s wrong_mode=%zu data_as_code=%zu "
                "undecoded=%zu stub_bytes=%zu first_wrong=0x%x same_sites=%d"
                " want_sites=%zu got_sites=%zu first_difference=%ld"
                " want=0x%x/%u got=0x%x/%u\n",
                name, p.wrong_mode, p.data_as_code, p.undecoded, p.stub_bytes,
                (unsigned)p.first_wrong, p.same_sites, p.want_sites,
                p.got_sites, p.first_difference, p.want_at / 4u,
                p.want_at % 4u, p.got_at / 4u, p.got_at % 4u);
        free(hidden);
    }
    free(file);
}

static int visit(const char *path, const struct stat *status, int type,
    struct FTW *walk)
{
    (void)walk;
    if (type == FTW_F && S_ISREG(status->st_mode)) survey(path);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; ++i) {
        if (strcmp(argv[i], "--pairs") == 0) pairs = 1;
        else if (strcmp(argv[i], "--pairs-symtab") == 0) pairs_symtab = 1;
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: code-map-survey [--pairs] [--pairs-symtab] DIR...\n");
        return 2;
    }
    for (; i < argc; ++i) {
        root_length = strlen(argv[i]);
        if (nftw(argv[i], visit, 16, FTW_PHYS) != 0) {
            perror(argv[i]);
            return 1;
        }
    }
    printf("survey_total files=%u refused=%u nothumb=%u\n", files,
        refused_files, no_thumb_files);
    return 0;
}
