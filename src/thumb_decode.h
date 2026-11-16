#ifndef LINUXEMU_THUMB_DECODE_H
#define LINUXEMU_THUMB_DECODE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Pure Thumb instruction classification for the patcher. No QNX or ELF
 * dependencies, so it is unit-tested on the host (tests/thumb-decode-test.c)
 * as well as cross-compiled into the emulator.
 */

enum thumb_patch_kind {
    THUMB_PATCH_NONE = 0,
    THUMB_PATCH_TLS_READ, /* mrc p15,0,Rt,c13,c0,3 (read TPIDRURO), 32-bit */
    THUMB_PATCH_SVC       /* svc #0, 16-bit */
};

struct thumb_patch {
    uint32_t offset;        /* byte offset of the instruction in the range */
    unsigned width;         /* instruction width in bytes: 2 or 4 */
    enum thumb_patch_kind kind;
};

/* True when a Thumb halfword begins a 32-bit Thumb-2 instruction. */
int thumb_is_32bit(uint16_t halfword);

/*
 * Scan halfword_count halfwords of Thumb code, stepping instruction by
 * instruction, and record every TPIDRURO read and svc #0. Returns the number
 * of patch sites found; writes up to out_capacity of them into out.
 */
size_t thumb_scan(const uint16_t *code, size_t halfword_count,
    struct thumb_patch *out, size_t out_capacity);

/* ELF ARM mapping symbols ($a, $t, $d, optionally with a ".suffix"). */
enum code_state {
    CODE_STATE_ARM = 0,
    CODE_STATE_THUMB,
    CODE_STATE_DATA
};

struct mapping_symbol {
    uint32_t address;
    enum code_state state;
};

/* Returns 1 and sets *state when name is a mapping symbol, else 0. */
int mapping_symbol_state(const char *name, enum code_state *state);

/* Stable sort by address (a later entry wins a tie). */
void mapping_sort(struct mapping_symbol *symbols, size_t count);

/*
 * State in effect at address, given sorted symbols. Code before the first
 * symbol (or with no symbols at all) is ARM. *run_end is set to where the
 * state next changes, clipped to limit.
 */
enum code_state mapping_state_at(const struct mapping_symbol *symbols,
    size_t count, uint32_t address, uint32_t limit, uint32_t *run_end);

#endif
