#include "thumb_decode.h"

/* Read TPIDRURO: mrc p15,0,Rt,c13,c0,3. As a 32-bit word the ARM and Thumb-2
   encodings share the bit pattern 0xee1d0f70, with Rt in bits [15:12]. */
#define THUMB_MRC_TPIDRURO_HW0 0xee1du
#define THUMB_MRC_TPIDRURO_HW1_MASK 0x0fffu
#define THUMB_MRC_TPIDRURO_HW1 0x0f70u
#define THUMB_SVC_0 0xdf00u

int thumb_is_32bit(uint16_t halfword)
{
    /* ARMv7 Thumb: a halfword starting with 0b11101, 0b11110 or 0b11111
       introduces a 32-bit instruction; everything else is 16-bit. */
    return (halfword & 0xf800u) >= 0xe800u;
}

size_t thumb_scan(const uint16_t *code, size_t halfword_count,
    struct thumb_patch *out, size_t out_capacity)
{
    size_t count = 0;
    size_t i = 0;

    while (i < halfword_count) {
        uint16_t hw0 = code[i];
        enum thumb_patch_kind kind = THUMB_PATCH_NONE;
        unsigned width = 2;

        if (thumb_is_32bit(hw0) && i + 1 < halfword_count) {
            uint16_t hw1 = code[i + 1];
            width = 4;
            if (hw0 == THUMB_MRC_TPIDRURO_HW0 &&
                (hw1 & THUMB_MRC_TPIDRURO_HW1_MASK) == THUMB_MRC_TPIDRURO_HW1) {
                kind = THUMB_PATCH_TLS_READ;
            }
        } else if (hw0 == THUMB_SVC_0) {
            kind = THUMB_PATCH_SVC;
        }

        if (kind != THUMB_PATCH_NONE) {
            if (count < out_capacity) {
                out[count].offset = (uint32_t)(i * 2u);
                out[count].width = width;
                out[count].kind = kind;
            }
            count++;
        }
        i += width / 2u;
    }
    return count;
}

int mapping_symbol_state(const char *name, enum code_state *state)
{
    if (name[0] != '$' || name[1] == '\0' ||
        (name[2] != '\0' && name[2] != '.')) return 0;
    switch (name[1]) {
    case 'a': *state = CODE_STATE_ARM; return 1;
    case 't': *state = CODE_STATE_THUMB; return 1;
    case 'd': *state = CODE_STATE_DATA; return 1;
    default: return 0;
    }
}

void mapping_sort(struct mapping_symbol *symbols, size_t count)
{
    size_t i;

    for (i = 1; i < count; ++i) {
        struct mapping_symbol current = symbols[i];
        size_t j = i;
        while (j > 0 && symbols[j - 1].address > current.address) {
            symbols[j] = symbols[j - 1];
            j--;
        }
        symbols[j] = current;
    }
}

enum code_state mapping_state_at(const struct mapping_symbol *symbols,
    size_t count, uint32_t address, uint32_t limit, uint32_t *run_end)
{
    size_t low = 0;
    size_t high = count;

    /* Binary search for the first symbol past address: a library can have
       a mapping symbol every few instructions, and callers walk it run by
       run. */
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (symbols[middle].address <= address) low = middle + 1;
        else high = middle;
    }
    *run_end = limit;
    if (low < count && symbols[low].address < limit)
        *run_end = symbols[low].address;
    return low == 0 ? CODE_STATE_ARM : symbols[low - 1].state;
}
