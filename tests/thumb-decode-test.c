/*
 * Host unit test for the Thumb instruction classifier (src/thumb_decode.c).
 * Pure logic, builds and runs on the x86 host with the native compiler.
 *
 *   cc -I../src -o thumb-decode-test thumb-decode-test.c ../src/thumb_decode.c
 */
#include "thumb_decode.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *name)
{
    printf("%s: %s\n", condition ? "ok" : "FAIL", name);
    if (!condition) {
        failures++;
    }
}

/* Encodings, as halfwords in execution order (little-endian in memory). */
#define HW_MRC_TPIDRURO_R0_0 0xee1du /* mrc p15,0,r0,c13,c0,3 : halfword 0 */
#define HW_MRC_TPIDRURO_R0_1 0x0f70u /* ...                   : halfword 1 */
#define HW_MRC_TPIDRURO_R5_1 0x5f70u /* same read into r5 (Rt in bits 15:12) */
#define HW_SVC_0             0xdf00u /* svc #0 */
#define HW_SVC_12            0xdf0cu /* svc #12 (not a Linux syscall gate) */
#define HW_BX_LR             0x4770u /* bx lr (16-bit) */
#define HW_NOP               0xbf00u /* nop (16-bit) */
#define HW_BL_PREFIX         0xf000u /* first half of a 32-bit BL */
#define HW_MOVW_PREFIX       0xf240u /* first half of a 32-bit movw */

int main(void)
{
    /* 16 vs 32-bit classification. */
    check(thumb_is_32bit(HW_MRC_TPIDRURO_R0_0), "mrc halfword is 32-bit");
    check(thumb_is_32bit(HW_BL_PREFIX), "bl prefix is 32-bit");
    check(thumb_is_32bit(0xe800u), "0xe800 is 32-bit");
    check(thumb_is_32bit(0xf800u), "0xf800 is 32-bit");
    check(!thumb_is_32bit(HW_BX_LR), "bx lr is 16-bit");
    check(!thumb_is_32bit(HW_SVC_0), "svc is 16-bit");
    check(!thumb_is_32bit(0xe7ffu), "0xe7ff (b.n) is 16-bit");

    /* A lone TPIDRURO read into r0. */
    {
        uint16_t code[] = { HW_MRC_TPIDRURO_R0_0, HW_MRC_TPIDRURO_R0_1 };
        struct thumb_patch found[4];
        size_t count = thumb_scan(code, 2, found, 4);
        check(count == 1, "tls read: one patch");
        check(count == 1 && found[0].kind == THUMB_PATCH_TLS_READ,
            "tls read: kind");
        check(count == 1 && found[0].offset == 0, "tls read: offset 0");
        check(count == 1 && found[0].width == 4, "tls read: width 4");
    }

    /* TPIDRURO read into a non-zero register still matches. */
    {
        uint16_t code[] = { HW_MRC_TPIDRURO_R0_0, HW_MRC_TPIDRURO_R5_1 };
        struct thumb_patch found[4];
        size_t count = thumb_scan(code, 2, found, 4);
        check(count == 1 && found[0].kind == THUMB_PATCH_TLS_READ,
            "tls read r5: matched");
    }

    /* svc #0 is a patch; svc #12 is not. */
    {
        uint16_t code[] = { HW_SVC_0 };
        struct thumb_patch found[4];
        size_t count = thumb_scan(code, 1, found, 4);
        check(count == 1 && found[0].kind == THUMB_PATCH_SVC
            && found[0].width == 2, "svc #0: patched width 2");
    }
    {
        uint16_t code[] = { HW_SVC_12 };
        struct thumb_patch found[4];
        size_t count = thumb_scan(code, 1, found, 4);
        check(count == 0, "svc #12: not patched");
    }

    /* The crux: a 32-bit instruction whose SECOND halfword equals svc #0
       must not be misread as an svc. */
    {
        uint16_t code[] = { HW_BL_PREFIX, HW_SVC_0 };
        struct thumb_patch found[4];
        size_t count = thumb_scan(code, 2, found, 4);
        check(count == 0, "svc-shaped tail of 32-bit instr: not patched");
    }

    /* Likewise a 32-bit instruction whose second halfword looks like the
       low half of a TPIDRURO read must not match. */
    {
        uint16_t code[] = { HW_MOVW_PREFIX, HW_MRC_TPIDRURO_R0_1 };
        struct thumb_patch found[4];
        size_t count = thumb_scan(code, 2, found, 4);
        check(count == 0, "tls-shaped tail of 32-bit instr: not patched");
    }

    /* A realistic mix: nop; tls read; bx lr; svc #0. Two patches, right
       offsets and widths. */
    {
        uint16_t code[] = {
            HW_NOP,                                    /* +0, 16-bit */
            HW_MRC_TPIDRURO_R0_0, HW_MRC_TPIDRURO_R0_1,/* +2, 32-bit tls */
            HW_BX_LR,                                  /* +6, 16-bit */
            HW_SVC_0                                   /* +8, 16-bit svc */
        };
        struct thumb_patch found[8];
        size_t count = thumb_scan(code, 5, found, 8);
        check(count == 2, "mix: two patches");
        check(count == 2 && found[0].kind == THUMB_PATCH_TLS_READ
            && found[0].offset == 2 && found[0].width == 4,
            "mix: tls at offset 2 width 4");
        check(count == 2 && found[1].kind == THUMB_PATCH_SVC
            && found[1].offset == 8 && found[1].width == 2,
            "mix: svc at offset 8 width 2");
    }

    /* out_capacity is respected but the full count is still returned. */
    {
        uint16_t code[] = { HW_SVC_0, HW_SVC_0, HW_SVC_0 };
        struct thumb_patch found[2];
        size_t count = thumb_scan(code, 3, found, 2);
        check(count == 3, "capacity: full count returned");
    }

    /* Mapping symbol names. */
    {
        enum code_state state = CODE_STATE_DATA;
        check(mapping_symbol_state("$a", &state) && state == CODE_STATE_ARM,
            "mapping: $a is arm");
        check(mapping_symbol_state("$t", &state)
            && state == CODE_STATE_THUMB, "mapping: $t is thumb");
        check(mapping_symbol_state("$d", &state) && state == CODE_STATE_DATA,
            "mapping: $d is data");
        check(mapping_symbol_state("$t.foo", &state)
            && state == CODE_STATE_THUMB, "mapping: $t.foo is thumb");
        check(!mapping_symbol_state("$x", &state), "mapping: $x ignored");
        check(!mapping_symbol_state("$ab", &state), "mapping: $ab ignored");
        check(!mapping_symbol_state("main", &state), "mapping: main ignored");
    }

    /* State runs. Symbols are given unsorted on purpose. */
    {
        struct mapping_symbol symbols[] = {
            { 0x1100, CODE_STATE_DATA },
            { 0x1000, CODE_STATE_THUMB },
            { 0x1200, CODE_STATE_ARM },
        };
        uint32_t end = 0;
        mapping_sort(symbols, 3);
        check(symbols[0].address == 0x1000 && symbols[2].address == 0x1200,
            "mapping: sorted");
        check(mapping_state_at(symbols, 3, 0x0f00, 0x2000, &end)
            == CODE_STATE_ARM && end == 0x1000,
            "mapping: before first symbol is arm until 0x1000");
        check(mapping_state_at(symbols, 3, 0x1000, 0x2000, &end)
            == CODE_STATE_THUMB && end == 0x1100,
            "mapping: thumb run 0x1000-0x1100");
        check(mapping_state_at(symbols, 3, 0x1080, 0x2000, &end)
            == CODE_STATE_THUMB && end == 0x1100,
            "mapping: inside thumb run");
        check(mapping_state_at(symbols, 3, 0x1100, 0x2000, &end)
            == CODE_STATE_DATA && end == 0x1200,
            "mapping: data run 0x1100-0x1200");
        check(mapping_state_at(symbols, 3, 0x1200, 0x1300, &end)
            == CODE_STATE_ARM && end == 0x1300,
            "mapping: last run clipped to limit");
        check(mapping_state_at(symbols, 0, 0x1200, 0x1300, &end)
            == CODE_STATE_ARM && end == 0x1300,
            "mapping: no symbols means arm to limit");
    }

    printf("thumb_decode_test_failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
