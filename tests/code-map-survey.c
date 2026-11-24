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
    if (i < argc && strcmp(argv[i], "--pairs") == 0) {
        pairs = 1;
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: code-map-survey [--pairs] DIR...\n");
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
