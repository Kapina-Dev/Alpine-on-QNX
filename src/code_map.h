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

enum fn_mode { MODE_UNKNOWN, MODE_ARM, MODE_THUMB };

struct code_function {
    uint32_t start; /* link address, no Thumb bit */
    uint32_t end;
    enum fn_mode mode;
};

struct code_map_result {
    struct mapping_symbol *symbols; /* sorted $a/$t/$d */
    size_t symbol_count;
    int refused;
    uint32_t refused_address;
};

/*
 * Decode [base, base + size) of one executable section. functions: sorted,
 * non-overlapping, ends set. Modes are propagated in place: a call or
 * branch to another function's start sets its mode, to a fixpoint; a
 * function reached in both modes becomes MODE_UNKNOWN. Words read by
 * PC-relative loads and bounded tbb/tbh tables are data. Bytes left
 * undecoded are $d; if any holds a possible patch site in either mode,
 * refused is set (refused_address is the first such site).
 * Returns 0, or -1 on allocation failure.
 */
int code_map_build(const uint8_t *bytes, uint32_t base, size_t size,
    struct code_function *functions, size_t function_count,
    struct code_map_result *out);
void code_map_result_free(struct code_map_result *result);

/*
 * A whole little-endian ARM ELF file image, read without its .symtab:
 * when it shows Thumb evidence (an odd .dynsym function, e_entry,
 * DT_INIT/DT_FINI, or an R_ARM_RELATIVE word naming an unwind-table or
 * .dynsym function with the Thumb bit), decode every executable section
 * from its .ARM.exidx function starts and that evidence. out gets the
 * synthesized mapping symbols, sorted, at link addresses, or refused.
 * No Thumb evidence, or no usable section headers: no symbols (the file
 * is ARM as before). Returns 0, or -1 on allocation failure.
 */
int code_map_from_elf(const uint8_t *file, size_t file_size,
    struct code_map_result *out);

#endif
