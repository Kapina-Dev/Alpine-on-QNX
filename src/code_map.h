#ifndef LINUXEMU_CODE_MAP_H
#define LINUXEMU_CODE_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "thumb_decode.h"

/*
 * Decoding of code that has no mapping symbols (stripped files). Pure
 * logic: no QNX or ELF dependencies, unit-tested on the host
 * (tests/code-map-test.c) and cross-compiled into the emulator.
 */

enum insn_flow { FLOW_NEXT, FLOW_CALL, FLOW_BRANCH, FLOW_TABLE, FLOW_STOP };

struct insn_info {
    unsigned width;            /* 2 or 4 */
    enum insn_flow flow;
    int target_valid;          /* call/branch target known */
    uint32_t target;           /* absolute address */
    int target_thumb;          /* mode at target */
    int literal_valid;         /* reads PC-relative data */
    uint32_t literal;          /* absolute address of the data */
    unsigned literal_size;     /* 1, 2, 4 or 8 */
    unsigned table_reg;        /* FLOW_TABLE: index register */
    unsigned table_entry_size; /* FLOW_TABLE: 1 (tbb) or 2 (tbh) */
};

/* Decode the Thumb instruction at code[0] (address is its link address).
   Returns 0, or -1 when a 32-bit instruction is cut by the end. */
int thumb_insn_info(const uint16_t *code, size_t halfwords, uint32_t address,
    struct insn_info *out);
int arm_insn_info(uint32_t word, uint32_t address, struct insn_info *out);
/* For tbb/tbh bounds: if the instruction at code is `cmp rN, #imm`, set
   *reg and *imm and return 1. */
int thumb_cmp_imm(const uint16_t *code, size_t halfwords, unsigned *reg,
    uint32_t *imm);

#endif
