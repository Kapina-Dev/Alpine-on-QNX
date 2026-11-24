#ifndef LINUXEMU_CODE_MAP_COMPARE_H
#define LINUXEMU_CODE_MAP_COMPARE_H

/*
 * Host-side helpers shared by tests/code-map-test.c and
 * tests/code-map-survey.c: read an ELF file, take its .symtab mapping
 * symbols as ground truth, count the patch sites the patcher would find,
 * and compare a decoded map against the truth byte by byte.
 */

#include <stddef.h>
#include <stdint.h>

#include "code_map.h"

uint8_t *cm_read_file(const char *path, size_t *size);
uint32_t cm_le32(const uint8_t *p);
uint16_t cm_le16(const uint8_t *p);

/* Mapping symbols from .symtab (sorted), or 0 when there are none. */
size_t cm_symtab_mapping(const uint8_t *file, struct mapping_symbol *out,
    size_t capacity);

/* Patch sites (address * 4 + 1 ARM / + 2 Thumb) the patcher would find in
   the file's executable PROGBITS sections, given mapping symbols. */
size_t cm_patch_sites(const uint8_t *file,
    const struct mapping_symbol *symbols, size_t symbol_count,
    uint32_t *out, size_t capacity);

struct cm_pair {
    int decoded;              /* code_map_from_elf returned 0 */
    int refused;
    uint32_t refused_address;
    size_t truth_symbols;
    size_t want_sites, got_sites;
    int same_sites;
    long first_difference;    /* index into the site lists, -1 if none */
    uint32_t want_at, got_at; /* the sites there (address * 4 + kind) */
    size_t total, data_as_code, wrong_mode, undecoded;
    size_t stub_bytes;        /* Thumb stub halfwords the truth calls ARM */
    uint32_t first_wrong;
};

/* Decode stripped (code_map_from_elf ignores .symtab, so the unstripped
   image itself may be passed) and compare it with full's mapping
   symbols. Returns 0, or -1 on allocation failure. */
int cm_compare_pair(const uint8_t *full, const uint8_t *stripped,
    size_t stripped_size, struct cm_pair *out);

#endif
