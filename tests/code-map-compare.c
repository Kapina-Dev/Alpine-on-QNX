/* Shared host helpers for the code map tests; see code-map-compare.h. */

#include "code-map-compare.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *cm_read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *bytes;
    long length;
    if (file == 0) return 0;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    bytes = length > 0 ? malloc((size_t)length) : 0;
    if (bytes != 0 && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = 0;
    }
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

uint32_t cm_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

uint16_t cm_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

#define SHDR(file, i) ((file) + cm_le32((file) + 32) + 40u * (i))

size_t cm_symtab_mapping(const uint8_t *file, struct mapping_symbol *out,
    size_t capacity)
{
    unsigned count = cm_le16(file + 48);
    unsigned i;
    size_t found = 0;
    for (i = 0; i < count; ++i) {
        const uint8_t *sh = SHDR(file, i);
        const uint8_t *strings;
        uint32_t j;
        if (cm_le32(sh + 4) != 2) continue; /* SHT_SYMTAB */
        strings = file + cm_le32(SHDR(file, cm_le32(sh + 24)) + 16);
        for (j = 0; j < cm_le32(sh + 20) / 16u; ++j) {
            const uint8_t *sym = file + cm_le32(sh + 16) + 16u * j;
            enum code_state state;
            uint16_t index = cm_le16(sym + 14);
            if (index == 0 || index >= count ||
                (cm_le32(SHDR(file, index) + 8) & 4u) == 0 ||
                !mapping_symbol_state((const char *)strings + cm_le32(sym),
                    &state) || found == capacity)
                continue;
            out[found].address = cm_le32(sym + 4);
            out[found].state = state;
            found++;
        }
    }
    mapping_sort(out, found);
    return found;
}

size_t cm_patch_sites(const uint8_t *file,
    const struct mapping_symbol *symbols, size_t symbol_count,
    uint32_t *out, size_t capacity)
{
    unsigned count = cm_le16(file + 48);
    unsigned i;
    size_t found = 0;
    for (i = 0; i < count; ++i) {
        const uint8_t *sh = SHDR(file, i);
        uint32_t address = cm_le32(sh + 12);
        uint32_t end = address + cm_le32(sh + 20);
        const uint8_t *bytes = file + cm_le32(sh + 16);
        if (cm_le32(sh + 4) != 1 || (cm_le32(sh + 8) & 4u) == 0) continue;
        while (address < end) {
            uint32_t run_end;
            enum code_state state = mapping_state_at(symbols, symbol_count,
                address, end, &run_end);
            uint32_t a;
            if (state == CODE_STATE_ARM) {
                for (a = (address + 3u) & ~3u; a + 4u <= run_end; a += 4u) {
                    uint32_t w = cm_le32(bytes + a - cm_le32(sh + 12));
                    if ((w == 0xef000000u || (w & 0xffff0fffu) == 0xee1d0f70u)
                        && found < capacity)
                        out[found++] = a * 4u + 1u;
                }
            } else if (state == CODE_STATE_THUMB) {
                uint32_t first = (address + 1u) & ~1u;
                struct thumb_patch sites[64];
                const uint16_t *code = (const uint16_t *)(bytes + first -
                    cm_le32(sh + 12));
                size_t halfwords = (run_end - first) / 2u;
                size_t done = 0;
                /* thumb_scan in chunks; a chunk ends on a whole instruction. */
                while (done < halfwords) {
                    size_t chunk = halfwords - done > 4096 ? 4096 :
                        halfwords - done;
                    size_t n, k;
                    if (done + chunk < halfwords &&
                        thumb_is_32bit(code[done + chunk - 1])) {
                        size_t j = done;
                        while (j < done + chunk)
                            j += thumb_is_32bit(code[j]) ? 2 : 1;
                        chunk = j - done;
                    }
                    n = thumb_scan(code + done, chunk, sites, 64);
                    for (k = 0; k < n && k < 64 && found < capacity; ++k)
                        out[found++] = (first + (uint32_t)(done * 2u) +
                            sites[k].offset) * 4u + 2u;
                    done += chunk;
                }
            }
            address = run_end;
        }
    }
    return found;
}

int cm_compare_pair(const uint8_t *full, const uint8_t *stripped,
    size_t stripped_size, struct cm_pair *out)
{
    static struct mapping_symbol truth[1u << 20];
    static uint32_t want[1u << 16], got[1u << 16];
    size_t truth_count, k;
    struct code_map_result r;
    unsigned count = cm_le16(full + 48);
    unsigned i;

    memset(out, 0, sizeof(*out));
    out->first_difference = -1;
    truth_count = cm_symtab_mapping(full, truth, 1u << 20);
    out->truth_symbols = truth_count;
    out->want_sites = cm_patch_sites(full, truth, truth_count, want, 1u << 16);
    if (code_map_from_elf(stripped, stripped_size, &r) != 0) return -1;
    out->decoded = 1;
    out->refused = r.refused;
    out->refused_address = r.refused_address;
    out->got_sites = cm_patch_sites(stripped, r.symbols, r.symbol_count, got,
        1u << 16);
    out->same_sites = out->got_sites == out->want_sites &&
        memcmp(got, want, out->want_sites * sizeof(want[0])) == 0;
    for (k = 0; k < out->want_sites || k < out->got_sites; ++k)
        if (k >= out->want_sites || k >= out->got_sites || want[k] != got[k]) {
            out->first_difference = (long)k;
            out->want_at = k < out->want_sites ? want[k] : 0;
            out->got_at = k < out->got_sites ? got[k] : 0;
            break;
        }
    /* Byte by byte: code the symbols call data (a pool or table decoded
       as instructions, where a false patch could land) must not happen;
       code left undecoded is only coverage. */
    for (i = 0; i < count; ++i) {
        const uint8_t *sh = SHDR(full, i);
        uint32_t a, end;
        if (cm_le32(sh + 4) != 1 || (cm_le32(sh + 8) & 4u) == 0) continue;
        end = cm_le32(sh + 12) + cm_le32(sh + 20);
        for (a = cm_le32(sh + 12); a < end; ++a) {
            uint32_t e1, e2;
            enum code_state t = mapping_state_at(truth, truth_count, a,
                end, &e1);
            enum code_state d = mapping_state_at(r.symbols,
                r.symbol_count, a, end, &e2);
            out->total++;
            if (t == CODE_STATE_DATA && d != CODE_STATE_DATA) {
                if (out->data_as_code++ == 0) out->first_wrong = a;
            } else if (t != CODE_STATE_DATA && d != CODE_STATE_DATA &&
                t != d) {
                if (out->wrong_mode++ == 0 && out->first_wrong == 0)
                    out->first_wrong = a;
            } else if (t != CODE_STATE_DATA && d == CODE_STATE_DATA)
                out->undecoded++;
        }
    }
    code_map_result_free(&r);
    return 0;
}
