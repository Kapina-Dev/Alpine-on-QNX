/*
 * Host unit test for the stripped-code decoder (src/code_map.c).
 * Pure logic, builds and runs on the x86 host with the native compiler:
 *
 *   gcc -std=gnu99 -Isrc -o build/code-map-test tests/code-map-test.c \
 *       src/code_map.c src/thumb_decode.c
 *
 * Encodings and expected addresses were checked against GNU as/objdump
 * (arm-linux-gnueabihf), assembled at the addresses used below.
 */
#include "code_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *name)
{
    printf("%s: %s\n", condition ? "ok" : "FAIL", name);
    if (!condition) failures++;
}

static struct insn_info thumb_at(uint16_t hw0, uint16_t hw1, uint32_t address)
{
    uint16_t code[2] = { hw0, hw1 };
    struct insn_info info;
    memset(&info, 0xa5, sizeof(info));
    if (thumb_insn_info(code, 2, address, &info) != 0) info.width = 0;
    return info;
}

static struct insn_info arm_at(uint32_t word, uint32_t address)
{
    struct insn_info info;
    memset(&info, 0xa5, sizeof(info));
    if (arm_insn_info(word, address, &info) != 0) info.width = 0;
    return info;
}

static void check_thumb_flow(uint16_t hw0, uint16_t hw1, uint32_t address,
    unsigned width, enum insn_flow flow, uint32_t target, int target_thumb,
    const char *name)
{
    struct insn_info info = thumb_at(hw0, hw1, address);
    char label[128];
    snprintf(label, sizeof(label), "thumb %s: width", name);
    check(info.width == width, label);
    snprintf(label, sizeof(label), "thumb %s: flow", name);
    check(info.flow == flow, label);
    snprintf(label, sizeof(label), "thumb %s: target", name);
    check(info.target_valid && info.target == target &&
        info.target_thumb == target_thumb, label);
    snprintf(label, sizeof(label), "thumb %s: no literal", name);
    check(!info.literal_valid, label);
}

static void check_thumb_literal(uint16_t hw0, uint16_t hw1, uint32_t address,
    unsigned width, uint32_t literal, unsigned size, const char *name)
{
    struct insn_info info = thumb_at(hw0, hw1, address);
    char label[128];
    snprintf(label, sizeof(label), "thumb %s: width", name);
    check(info.width == width, label);
    snprintf(label, sizeof(label), "thumb %s: flow next", name);
    check(info.flow == FLOW_NEXT && !info.target_valid, label);
    snprintf(label, sizeof(label), "thumb %s: literal", name);
    check(info.literal_valid && info.literal == literal &&
        info.literal_size == size, label);
}

static void check_arm_flow(uint32_t word, uint32_t address,
    enum insn_flow flow, uint32_t target, int target_thumb, const char *name)
{
    struct insn_info info = arm_at(word, address);
    char label[128];
    snprintf(label, sizeof(label), "arm %s: width/flow", name);
    check(info.width == 4 && info.flow == flow, label);
    snprintf(label, sizeof(label), "arm %s: target", name);
    check(info.target_valid && info.target == target &&
        info.target_thumb == target_thumb, label);
}

static void check_arm_literal(uint32_t word, uint32_t address,
    uint32_t literal, unsigned size, const char *name)
{
    struct insn_info info = arm_at(word, address);
    char label[128];
    snprintf(label, sizeof(label), "arm %s: flow next", name);
    check(info.width == 4 && info.flow == FLOW_NEXT && !info.target_valid,
        label);
    snprintf(label, sizeof(label), "arm %s: literal", name);
    check(info.literal_valid && info.literal == literal &&
        info.literal_size == size, label);
}

static void test_insn_info(void)
{
    /* Thumb control flow. */
    check_thumb_flow(0xf000, 0xf87e, 0x100, 4, FLOW_CALL, 0x200, 1, "bl");
    /* blx: target is Align(0x104 + 4, 4) + 0x2f8 = 0x400, ARM. */
    check_thumb_flow(0xf000, 0xe97c, 0x104, 4, FLOW_CALL, 0x400, 0, "blx");
    check_thumb_flow(0xf000, 0xb87a, 0x108, 4, FLOW_BRANCH, 0x200, 1, "b.w");
    check_thumb_flow(0xe01c, 0, 0x10c, 2, FLOW_BRANCH, 0x148, 1, "b.n");
    check_thumb_flow(0xd01b, 0, 0x10e, 2, FLOW_BRANCH, 0x148, 1, "beq.n");
    check_thumb_flow(0xf000, 0x8076, 0x110, 4, FLOW_BRANCH, 0x200, 1, "beq.w");
    /* Negative offsets: bl back 0x1000 from 0x2000 -> 0x1000. */
    check_thumb_flow(0xf7fe, 0xfffe, 0x2000, 4, FLOW_CALL, 0x1000, 1,
        "bl backwards");
    check_thumb_flow(0xe7fe, 0, 0x300, 2, FLOW_BRANCH, 0x300, 1, "b.n self");

    /* svc and udf are not conditional branches. */
    {
        struct insn_info info = thumb_at(0xdf00, 0, 0x100);
        check(info.width == 2 && info.flow == FLOW_NEXT && !info.target_valid,
            "thumb svc: not a branch");
        info = thumb_at(0xdeff, 0, 0x100);
        check(info.width == 2 && !info.target_valid, "thumb udf: not a branch");
    }

    /* Thumb literal loads: Align(address + 4, 4) +/- offset. */
    check_thumb_literal(0x4902, 0, 0x114, 2, 0x120, 4, "ldr T1");
    check_thumb_literal(0xf85f, 0x2004, 0x116, 4, 0x114, 4, "ldr.w U=0");
    check_thumb_literal(0xf8df, 0x2010, 0x116, 4, 0x128, 4, "ldr.w U=1");
    check_thumb_literal(0xf89f, 0x300c, 0x11a, 4, 0x128, 1, "ldrb.w");
    check_thumb_literal(0xf93f, 0x3006, 0x11e, 4, 0x11a, 2, "ldrsh.w");
    check_thumb_literal(0xf8bf, 0x0002, 0x140, 4, 0x146, 2, "ldrh.w");
    check_thumb_literal(0xf99f, 0x0002, 0x144, 4, 0x14a, 1, "ldrsb.w");
    check_thumb_literal(0xe9df, 0x4504, 0x122, 4, 0x134, 8, "ldrd U=1");
    check_thumb_literal(0xe95f, 0x4504, 0x126, 4, 0x118, 8, "ldrd U=0");
    check_thumb_literal(0xed9f, 0x0b02, 0x12a, 4, 0x134, 8, "vldr d0");
    check_thumb_literal(0xed5f, 0x0a02, 0x12e, 4, 0x128, 4, "vldr s1");
    /* pld [pc, #imm] (ldrb form with Rt=pc) reads nothing. */
    {
        struct insn_info info = thumb_at(0xf89f, 0xf00c, 0x11a);
        check(info.width == 4 && !info.literal_valid, "thumb pld: no literal");
    }

    /* Table branches. */
    {
        struct insn_info info = thumb_at(0xe8df, 0xf003, 0x132);
        check(info.width == 4 && info.flow == FLOW_TABLE &&
            info.table_reg == 3 && info.table_entry_size == 1,
            "thumb tbb [pc, r3]");
        info = thumb_at(0xe8df, 0xf015, 0x136);
        check(info.width == 4 && info.flow == FLOW_TABLE &&
            info.table_reg == 5 && info.table_entry_size == 2,
            "thumb tbh [pc, r5]");
        /* tbb [r1, r3] is not a pc-relative table. */
        info = thumb_at(0xe8d1, 0xf003, 0x132);
        check(info.width == 4 && info.flow != FLOW_TABLE,
            "thumb tbb [r1, r3]: not inline");
    }

    /* cmp with an immediate. */
    {
        uint16_t t1[] = { 0x2b07 };
        uint16_t t2[] = { 0xf5b9, 0x7f96 };
        uint16_t other[] = { 0x4770 };
        unsigned reg = 99;
        uint32_t imm = 99;
        check(thumb_cmp_imm(t1, 1, &reg, &imm) && reg == 3 && imm == 7,
            "thumb cmp r3, #7");
        check(thumb_cmp_imm(t2, 2, &reg, &imm) && reg == 9 && imm == 300,
            "thumb cmp.w r9, #300");
        check(!thumb_cmp_imm(other, 1, &reg, &imm), "thumb bx lr: not cmp");
        check(!thumb_cmp_imm(t2, 1, &reg, &imm), "thumb cmp.w cut: not cmp");
    }

    /* A 32-bit prefix with no second halfword. */
    {
        uint16_t code[1] = { 0xf000 };
        struct insn_info info;
        check(thumb_insn_info(code, 1, 0x100, &info) != 0,
            "thumb cut 32-bit: error");
    }

    /* ARM. */
    check_arm_flow(0xeb00003e, 0x300, FLOW_CALL, 0x400, 0, "bl");
    check_arm_flow(0xfb00007d, 0x304, FLOW_CALL, 0x502, 1, "blx");
    check_arm_flow(0xea00003c, 0x308, FLOW_BRANCH, 0x400, 0, "b");
    check_arm_flow(0x1a00003b, 0x30c, FLOW_BRANCH, 0x400, 0, "bne");
    check_arm_flow(0xebfffffe, 0x1000, FLOW_CALL, 0x1000, 0, "bl self");
    check_arm_literal(0xe59f0008, 0x310, 0x320, 4, "ldr U=1");
    check_arm_literal(0xe51f0008, 0x314, 0x314, 4, "ldr U=0");
    check_arm_literal(0xe5df0004, 0x324, 0x330, 1, "ldrb");
    check_arm_literal(0xe1cf20d8, 0x318, 0x328, 8, "ldrd");
    check_arm_literal(0xed1f1b03, 0x31c, 0x318, 8, "vldr d1");
    check_arm_literal(0xed9f1a04, 0x320, 0x338, 4, "vldr s2");
    /* ldrh r0, [pc, #4] = e1df00b4; ldrsh = e1df00f4; ldrsb = e1df00d4. */
    check_arm_literal(0xe1df00b4, 0x400, 0x40c, 2, "ldrh");
    check_arm_literal(0xe1df00f4, 0x400, 0x40c, 2, "ldrsh");
    check_arm_literal(0xe1df00d4, 0x400, 0x40c, 1, "ldrsb");
    {
        struct insn_info info = arm_at(0xef000000, 0x400);
        check(info.width == 4 && info.flow == FLOW_NEXT && !info.target_valid &&
            !info.literal_valid, "arm svc: plain");
        info = arm_at(0xf5dff004, 0x400); /* pld [pc, #4] */
        check(info.width == 4 && !info.literal_valid, "arm pld: no literal");
        info = arm_at(0xe59f1004, 0x400); /* ldr r1, [pc, #4] */
        check(info.literal_valid && info.literal == 0x40c, "arm ldr r1");
        info = arm_at(0xe5901004, 0x400); /* ldr r1, [r0, #4] */
        check(!info.literal_valid, "arm ldr [r0]: no literal");
    }
}


/* ---- map builder ---- */

static struct code_map_result build_thumb(const uint16_t *code, size_t halfwords,
    struct code_function *functions, size_t count)
{
    struct code_map_result result;
    uint8_t bytes[256];
    memcpy(bytes, code, halfwords * 2u); /* host is little-endian */
    memset(&result, 0, sizeof(result));
    if (code_map_build(bytes, 0x1000, halfwords * 2u, functions, count,
            &result) != 0)
        result.refused = -1;
    return result;
}

static enum code_state state_at(const struct code_map_result *result,
    uint32_t address)
{
    uint32_t end;
    return mapping_state_at(result->symbols, result->symbol_count, address,
        0x2000, &end);
}

static void test_map_builder(void)
{
    /* (a) forward pool holding 0xdf00: data; the real svc is Thumb code. */
    {
        uint16_t code[] = { 0x4801 /* ldr r0, [pc, #4] -> 0x1008 */,
            0xdf00 /* svc 0 */, 0x4770, 0xbf00, 0xdf00, 0xdf00 };
        struct code_function f[] = { { 0x1000, 0x100c, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 6, f, 1);
        check(r.refused == 0, "map a: not refused");
        check(state_at(&r, 0x1000) == CODE_STATE_THUMB &&
            state_at(&r, 0x1002) == CODE_STATE_THUMB, "map a: svc is Thumb");
        check(state_at(&r, 0x1008) == CODE_STATE_DATA &&
            state_at(&r, 0x100a) == CODE_STATE_DATA, "map a: pool is data");
        code_map_result_free(&r);
    }
    /* (b) pool referenced only backwards, after being walked as code. */
    {
        uint16_t code[] = { 0xe002 /* b.n 0x1008 */, 0xbf00,
            0xdf00, 0xdf00 /* pool */,
            0xf85f, 0x0008 /* ldr.w r0, [pc, #-8] -> 0x1004 */,
            0x4770, 0xbf00 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(r.refused == 0, "map b: not refused");
        check(state_at(&r, 0x1004) == CODE_STATE_DATA &&
            state_at(&r, 0x1006) == CODE_STATE_DATA,
            "map b: backward pool is data");
        check(state_at(&r, 0x1008) == CODE_STATE_THUMB &&
            state_at(&r, 0x100c) == CODE_STATE_THUMB, "map b: code after pool");
        code_map_result_free(&r);
    }
    /* (c) tbb bounded by cmp of its index: the table is data. */
    {
        uint16_t code[] = { 0x2b03 /* cmp r3, #3 */,
            0xd803 /* bhi.n 0x100c */, 0xe8df, 0xf003 /* tbb [pc, r3] */,
            0xdf00, 0xdf00 /* 4 table bytes */, 0x4770, 0xbf00 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(r.refused == 0, "map c: not refused");
        check(state_at(&r, 0x1008) == CODE_STATE_DATA &&
            state_at(&r, 0x100a) == CODE_STATE_DATA, "map c: table is data");
        check(state_at(&r, 0x100c) == CODE_STATE_THUMB,
            "map c: code after the table");
        code_map_result_free(&r);
    }
    /* (c2) cmp of another register: unbounded, the table bytes hold a
       pattern -> refused at the table. */
    {
        uint16_t code[] = { 0x2a03 /* cmp r2, #3 */, 0xd803,
            0xe8df, 0xf003, 0xdf00, 0xdf00, 0x4770, 0xbf00 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(r.refused == 1 && r.refused_address == 0x1008,
            "map c2: cmp of another register, refused at table");
        code_map_result_free(&r);
    }
    /* (c3) cmp too far back (5 instructions): unbounded. */
    {
        uint16_t code[] = { 0x2b03, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0xbf00,
            0xe8df, 0xf003, 0xdf00, 0xdf00, 0x4770, 0xbf00 };
        struct code_function f[] = { { 0x1000, 0x1018, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 12, f, 1);
        check(r.refused == 1 && r.refused_address == 0x1010,
            "map c3: cmp out of the window, refused");
        code_map_result_free(&r);
    }
    /* (d) unbounded tbb, an svc after the table: refused at the svc. */
    {
        uint16_t code[] = { 0xbf00, 0xbf00, 0xe8df, 0xf003, 0x0101, 0x0101,
            0xdf00, 0x4770 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(r.refused == 1 && r.refused_address == 0x100c,
            "map d: unbounded table then svc, refused at svc");
        code_map_result_free(&r);
    }
    /* (e) unbounded tbb, no pattern after: not refused, rest not code. */
    {
        uint16_t code[] = { 0xbf00, 0xbf00, 0xe8df, 0xf003, 0x0101, 0x0101,
            0xbf00, 0x4770 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(r.refused == 0, "map e: not refused");
        check(state_at(&r, 0x1000) == CODE_STATE_THUMB &&
            state_at(&r, 0x100c) == CODE_STATE_DATA,
            "map e: decoded up to the table only");
        code_map_result_free(&r);
    }
    /* (f) bl into a function of unknown mode makes it Thumb. */
    {
        uint16_t code[] = { 0xf000, 0xf806 /* bl 0x1010 */, 0x4770, 0xbf00,
            0xbf00, 0xbf00, 0xbf00, 0xbf00,
            0xdf00 /* svc */, 0x4770 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB },
            { 0x1010, 0x1014, MODE_UNKNOWN } };
        struct code_map_result r = build_thumb(code, 10, f, 2);
        check(r.refused == 0, "map f: not refused");
        check(f[1].mode == MODE_THUMB, "map f: callee became Thumb");
        check(state_at(&r, 0x1010) == CODE_STATE_THUMB, "map f: callee is $t");
        code_map_result_free(&r);
    }
    /* (f2) blx into a function of unknown mode makes it ARM. */
    {
        uint16_t code[] = { 0xf000, 0xe806 /* blx 0x1010 */, 0x4770, 0xbf00,
            0xbf00, 0xbf00, 0xbf00, 0xbf00,
            0x0000, 0xef00 /* svc 0 */, 0xff1e, 0xe12f /* bx lr */ };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB },
            { 0x1010, 0x1018, MODE_UNKNOWN } };
        struct code_map_result r = build_thumb(code, 12, f, 2);
        check(r.refused == 0, "map f2: not refused");
        check(f[1].mode == MODE_ARM, "map f2: callee became ARM");
        check(state_at(&r, 0x1010) == CODE_STATE_ARM, "map f2: callee is $a");
        code_map_result_free(&r);
    }
    /* (g) unknown-mode function holding an ARM svc: refused. */
    {
        uint16_t code[] = { 0x4770, 0xbf00, 0x0000, 0xef00, 0xff1e, 0xe12f };
        struct code_function f[] = { { 0x1000, 0x1004, MODE_THUMB },
            { 0x1004, 0x100c, MODE_UNKNOWN } };
        struct code_map_result r = build_thumb(code, 6, f, 2);
        check(r.refused == 1 && r.refused_address == 0x1004,
            "map g: unknown mode with svc, refused");
        code_map_result_free(&r);
    }
    /* (h) the same without a pattern: not refused, left as data. */
    {
        uint16_t code[] = { 0x4770, 0xbf00, 0x0000, 0xe1a0, 0xff1e, 0xe12f };
        struct code_function f[] = { { 0x1000, 0x1004, MODE_THUMB },
            { 0x1004, 0x100c, MODE_UNKNOWN } };
        struct code_map_result r = build_thumb(code, 6, f, 2);
        check(r.refused == 0, "map h: not refused");
        check(state_at(&r, 0x1004) == CODE_STATE_DATA,
            "map h: unknown function is not patched");
        code_map_result_free(&r);
    }
    /* (i) ARM function whose literal is 0xef000000: data. */
    {
        uint16_t code[] = { 0x0000, 0xe59f /* ldr r0, [pc, #0] -> 0x1008 */,
            0xff1e, 0xe12f, 0x0000, 0xef00 };
        struct code_function f[] = { { 0x1000, 0x100c, MODE_ARM } };
        struct code_map_result r = build_thumb(code, 6, f, 1);
        check(r.refused == 0, "map i: not refused");
        check(state_at(&r, 0x1000) == CODE_STATE_ARM &&
            state_at(&r, 0x1008) == CODE_STATE_DATA, "map i: literal is data");
        code_map_result_free(&r);
    }
    /* (j) callee reached as Thumb by bl and as ARM by blx: conflict, the
       callee's mode is unknown and its svc refuses the section. */
    {
        uint16_t code[] = { 0xf000, 0xf806 /* bl 0x1010 */, 0x4770, 0xbf00,
            0xf000, 0xe802 /* blx 0x1010 */, 0x4770, 0xbf00,
            0xdf00, 0x4770 };
        struct code_function f[] = { { 0x1000, 0x1004, MODE_THUMB },
            { 0x1008, 0x1010, MODE_THUMB }, { 0x1010, 0x1014, MODE_UNKNOWN } };
        struct code_map_result r = build_thumb(code, 10, f, 3);
        check(f[2].mode == MODE_UNKNOWN, "map j: conflicting mode is unknown");
        check(r.refused == 1 && r.refused_address == 0x1010,
            "map j: refused at the svc");
        code_map_result_free(&r);
    }
    /* (k) bytes no function covers, holding a Thumb TLS read: refused. */
    {
        uint16_t code[] = { 0xee1d, 0x0f70, 0x4770, 0xbf00 };
        struct code_function f[] = { { 0x1004, 0x1008, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 4, f, 1);
        check(r.refused == 1 && r.refused_address == 0x1000,
            "map k: uncovered TLS read, refused");
        code_map_result_free(&r);
    }
    /* (l) one unwind extent covering Thumb code and, after it, ARM code
       reached by blx (ld merges identical adjacent exidx entries): the
       call target starts an ARM function. */
    {
        uint16_t code[] = { 0xf000, 0xe806 /* blx 0x1010 */, 0x4770, 0xbf00,
            0xbf00, 0xbf00, 0xbf00, 0xbf00,
            0x0000, 0xef00 /* svc 0 */, 0xff1e, 0xe12f /* bx lr */ };
        struct code_function f[] = { { 0x1000, 0x1018, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 12, f, 1);
        check(r.refused == 0, "map l: not refused");
        check(state_at(&r, 0x1000) == CODE_STATE_THUMB &&
            state_at(&r, 0x1010) == CODE_STATE_ARM &&
            state_at(&r, 0x1014) == CODE_STATE_ARM,
            "map l: call target splits the extent into ARM");
        code_map_result_free(&r);
    }
    /* (m) a tail call (b.w) to another function's start hands on the
       mode, without being a call. */
    {
        uint16_t code[] = { 0xf000, 0xb806 /* b.w 0x1010 */, 0xbf00, 0xbf00,
            0xbf00, 0xbf00, 0xbf00, 0xbf00, 0xdf00, 0x4770 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB },
            { 0x1010, 0x1014, MODE_UNKNOWN } };
        struct code_map_result r = build_thumb(code, 10, f, 2);
        check(r.refused == 0 && f[1].mode == MODE_THUMB &&
            state_at(&r, 0x1010) == CODE_STATE_THUMB, "map m: tail call");
        code_map_result_free(&r);
    }
}

/* ---- whole files: stripped fixtures against their unstripped twins ---- */

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *bytes;
    long length;
    if (file == 0) return 0;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    bytes = malloc((size_t)length);
    if (bytes != 0 && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = 0;
    }
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

#define SHDR(file, i) ((file) + le32((file) + 32) + 40u * (i))

/* Mapping symbols from .symtab (the ground truth of an unstripped file). */
static size_t symtab_mapping(const uint8_t *file, struct mapping_symbol *out,
    size_t capacity)
{
    unsigned count = le16(file + 48);
    unsigned i;
    size_t found = 0;
    for (i = 0; i < count; ++i) {
        const uint8_t *sh = SHDR(file, i);
        const uint8_t *strings;
        uint32_t j;
        if (le32(sh + 4) != 2) continue; /* SHT_SYMTAB */
        strings = file + le32(SHDR(file, le32(sh + 24)) + 16);
        for (j = 0; j < le32(sh + 20) / 16u; ++j) {
            const uint8_t *sym = file + le32(sh + 16) + 16u * j;
            enum code_state state;
            uint16_t index = le16(sym + 14);
            if (index == 0 || index >= count ||
                (le32(SHDR(file, index) + 8) & 4u) == 0 ||
                !mapping_symbol_state((const char *)strings + le32(sym),
                    &state) || found == capacity)
                continue;
            out[found].address = le32(sym + 4);
            out[found].state = state;
            found++;
        }
    }
    mapping_sort(out, found);
    return found;
}

/* Patch sites (address * 4 + kind) the patcher would find in the file's
   code sections, given mapping symbols. */
static size_t patch_sites(const uint8_t *file, const struct mapping_symbol *symbols,
    size_t symbol_count, uint32_t *out, size_t capacity)
{
    unsigned count = le16(file + 48);
    unsigned i;
    size_t found = 0;
    for (i = 0; i < count; ++i) {
        const uint8_t *sh = SHDR(file, i);
        uint32_t address = le32(sh + 12);
        uint32_t end = address + le32(sh + 20);
        const uint8_t *bytes = file + le32(sh + 16);
        if (le32(sh + 4) != 1 || (le32(sh + 8) & 4u) == 0) continue;
        while (address < end) {
            uint32_t run_end;
            enum code_state state = mapping_state_at(symbols, symbol_count,
                address, end, &run_end);
            uint32_t a;
            if (state == CODE_STATE_ARM) {
                for (a = (address + 3u) & ~3u; a + 4u <= run_end; a += 4u) {
                    uint32_t w = le32(bytes + a - le32(sh + 12));
                    if ((w == 0xef000000u || (w & 0xffff0fffu) == 0xee1d0f70u)
                        && found < capacity)
                        out[found++] = a * 4u + 1u;
                }
            } else if (state == CODE_STATE_THUMB) {
                uint32_t first = (address + 1u) & ~1u;
                struct thumb_patch sites[64];
                size_t n = thumb_scan((const uint16_t *)(bytes + first -
                    le32(sh + 12)), (run_end - first) / 2u, sites, 64);
                size_t k;
                for (k = 0; k < n && k < 64 && found < capacity; ++k)
                    out[found++] = (first + sites[k].offset) * 4u + 2u;
            }
            address = run_end;
        }
    }
    return found;
}

static void test_fixture_files(void)
{
    size_t full_size, stripped_size, unknown_size;
    uint8_t *full = read_file("guest-tests/fixtures/stripped-mixed-armhf.so",
        &full_size);
    uint8_t *stripped = read_file(
        "guest-tests/fixtures/stripped-mixed-armhf.stripped.so", &stripped_size);
    uint8_t *unknown = read_file("guest-tests/fixtures/stripped-unknown-armhf.so",
        &unknown_size);
    struct mapping_symbol truth[4096];
    size_t truth_count;
    uint32_t want[256], got[256];
    size_t want_count, got_count;
    struct code_map_result r;

    check(full != 0 && stripped != 0 && unknown != 0, "fixtures: readable");
    if (full == 0 || stripped == 0 || unknown == 0) return;

    truth_count = symtab_mapping(full, truth, 4096);
    check(truth_count > 0, "fixtures: unstripped has mapping symbols");
    want_count = patch_sites(full, truth, truth_count, want, 256);
    check(want_count >= 8, "fixtures: unstripped has patch sites");
    printf("  unstripped patch sites: %zu\n", want_count);

    check(code_map_from_elf(stripped, stripped_size, &r) == 0 && !r.refused &&
        r.symbol_count > 0, "fixtures: stripped decodes");
    got_count = patch_sites(stripped, r.symbols, r.symbol_count, got, 256);
    check(got_count == want_count &&
        memcmp(got, want, want_count * sizeof(want[0])) == 0,
        "fixtures: stripped patch sites equal the unstripped ones");
    if (got_count != want_count ||
        memcmp(got, want, want_count * sizeof(want[0])) != 0) {
        size_t k;
        for (k = 0; k < want_count || k < got_count; ++k)
            printf("  site %zu: want %#x/%u got %#x/%u\n", k,
                k < want_count ? want[k] / 4u : 0, k < want_count ? want[k] % 4u : 0,
                k < got_count ? got[k] / 4u : 0, k < got_count ? got[k] % 4u : 0);
    }
    code_map_result_free(&r);

    /* The unstripped file read without its symtab decodes the same way. */
    check(code_map_from_elf(full, full_size, &r) == 0 && !r.refused,
        "fixtures: unstripped decodes without its symtab");
    got_count = patch_sites(full, r.symbols, r.symbol_count, got, 256);
    check(got_count == want_count &&
        memcmp(got, want, want_count * sizeof(want[0])) == 0,
        "fixtures: unstripped by decoding equals its mapping symbols");
    code_map_result_free(&r);

    check(code_map_from_elf(unknown, unknown_size, &r) == 0 && r.refused,
        "fixtures: unknown-mode svc refused");
    code_map_result_free(&r);

    /* An ARM-only file (no Thumb evidence): no symbols, ARM scan as before. */
    {
        size_t size;
        uint8_t *arm = read_file("guest-tests/fixtures/phase9-module-armhf.so",
            &size);
        check(arm != 0 && code_map_from_elf(arm, size, &r) == 0 &&
            !r.refused && r.symbol_count == 0,
            "fixtures: ARM-only library has no Thumb evidence");
        code_map_result_free(&r);
        free(arm);
    }
    /* Not an ELF file: nothing. */
    check(code_map_from_elf((const uint8_t *)"hello", 5, &r) == 0 &&
        r.symbol_count == 0 && !r.refused, "fixtures: non-ELF ignored");
    free(full);
    free(stripped);
    free(unknown);
}

int main(void)
{
    test_insn_info();
    test_map_builder();
    test_fixture_files();
    printf("code_map_test_failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
