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
    check_thumb_flow(0xf000, 0xb87a, 0x108, 4, FLOW_JUMP, 0x200, 1, "b.w");
    check_thumb_flow(0xe01c, 0, 0x10c, 2, FLOW_JUMP, 0x148, 1, "b.n");
    check_thumb_flow(0xd01b, 0, 0x10e, 2, FLOW_BRANCH, 0x148, 1, "beq.n");
    check_thumb_flow(0xf000, 0x8076, 0x110, 4, FLOW_BRANCH, 0x200, 1, "beq.w");
    /* Negative offsets: bl back 0x1000 from 0x2000 -> 0x1000. */
    check_thumb_flow(0xf7fe, 0xfffe, 0x2000, 4, FLOW_CALL, 0x1000, 1,
        "bl backwards");
    check_thumb_flow(0xe7fe, 0, 0x300, 2, FLOW_JUMP, 0x300, 1, "b.n self");


    /* Where straight-line execution ends (checked against GNU as). */
    {
        static const struct { uint16_t hw0, hw1; enum insn_flow flow;
            const char *name; } thumb[] = {
            { 0x4770, 0, FLOW_STOP, "bx lr" },
            { 0x4798, 0, FLOW_CALL, "blx r3" },
            { 0xbd10, 0, FLOW_STOP, "pop {r4, pc}" },
            { 0xe8bd, 0x8ff0, FLOW_STOP, "pop.w {..., pc}" },
            { 0xe890, 0x8010, FLOW_STOP, "ldmia.w r0, {r4, pc}" },
            { 0xf85d, 0xfb04, FLOW_STOP, "ldr.w pc, [sp], #4" },
            { 0xf8d1, 0xf008, FLOW_STOP, "ldr.w pc, [r1, #8]" },
            { 0xf850, 0xf021, FLOW_STOP, "ldr.w pc, [r0, r1, lsl #2]" },
            { 0x469f, 0, FLOW_STOP, "mov pc, r3" },
            { 0x4497, 0, FLOW_STOP, "add pc, r2" },
            { 0xde00, 0, FLOW_STOP, "udf #0" },
            { 0x4679, 0, FLOW_NEXT, "mov r1, pc" },
            { 0xbd00 & 0x00ff, 0, FLOW_NEXT, "movs r0, r0" },
            { 0xf8d1, 0x3008, FLOW_NEXT, "ldr.w r3, [r1, #8]" },
        };
        static const struct { uint32_t word; enum insn_flow flow;
            const char *name; } arm[] = {
            { 0xe12fff1e, FLOW_STOP, "bx lr" },
            { 0xe12fff33, FLOW_CALL, "blx r3" },
            { 0xe8bd8010, FLOW_STOP, "pop {r4, pc}" },
            { 0xe8908002, FLOW_STOP, "ldm r0, {r1, pc}" },
            { 0xe49df004, FLOW_STOP, "ldr pc, [sp], #4" },
            { 0xe791f102, FLOW_STOP, "ldr pc, [r1, r2, lsl #2]" },
            { 0xe1a0f00e, FLOW_STOP, "mov pc, lr" },
            { 0xe7f000f0, FLOW_STOP, "udf" },
            { 0xe08ff100, FLOW_TABLE, "add pc, pc, r0, lsl #2" },
            { 0x908ff100, FLOW_TABLE, "addls pc, pc, r0, lsl #2" },
            { 0x012fff1e, FLOW_NEXT, "bxeq lr" },
            { 0x149df004, FLOW_NEXT, "popne {pc}" },
            { 0xe15f0000, FLOW_NEXT, "cmp pc, r0" },
            { 0xe1a00000, FLOW_NEXT, "nop (mov r0, r0)" },
            { 0xe0010392, FLOW_NEXT, "mul r1, r2, r3" },
        };
        unsigned i;
        char label[96];
        for (i = 0; i < sizeof(thumb) / sizeof(thumb[0]); ++i) {
            struct insn_info info = thumb_at(thumb[i].hw0, thumb[i].hw1, 0x100);
            snprintf(label, sizeof(label), "thumb %s: flow", thumb[i].name);
            check(info.flow == thumb[i].flow && !info.target_valid &&
                info.it_length == 0, label);
        }
        for (i = 0; i < sizeof(arm) / sizeof(arm[0]); ++i) {
            struct insn_info info = arm_at(arm[i].word, 0x200);
            snprintf(label, sizeof(label), "arm %s: flow", arm[i].name);
            check(info.flow == arm[i].flow && !info.target_valid, label);
        }
    }
    check_thumb_flow(0xb15a, 0, 0x11e, 2, FLOW_BRANCH, 0x138, 1, "cbz");
    check_thumb_flow(0xb953, 0, 0x120, 2, FLOW_BRANCH, 0x138, 1, "cbnz");
    {
        struct insn_info info = thumb_at(0xbf04, 0, 0x122);
        check(info.flow == FLOW_NEXT && info.it_length == 2, "thumb itt eq");
        info = thumb_at(0xbf14, 0, 0x128);
        check(info.it_length == 2, "thumb ite ne");
        info = thumb_at(0xbfc8, 0, 0x12e);
        check(info.it_length == 1, "thumb it gt");
        info = thumb_at(0xbf00, 0, 0x130);
        check(info.it_length == 0, "thumb nop: no it block");
    }

    /* adr: an address, not a load (literal_size 0). */
    {
        static const struct { uint16_t hw0, hw1; uint32_t address, target;
            const char *name; } adr[] = {
            { 0xa303, 0, 0x1000, 0x1010, "adr r3 (T1)" },
            { 0xf20f, 0x0010, 0x1002, 0x1014, "addw r0, pc, #16" },
            { 0xf2af, 0x0106, 0x1006, 0x1002, "subw r1, pc, #6" },
        };
        unsigned i;
        char label[96];
        for (i = 0; i < 3; ++i) {
            struct insn_info info = thumb_at(adr[i].hw0, adr[i].hw1,
                adr[i].address);
            snprintf(label, sizeof(label), "thumb %s", adr[i].name);
            check(info.flow == FLOW_NEXT && info.literal_valid &&
                info.literal == adr[i].target && info.literal_size == 0,
                label);
        }
        {
            struct insn_info info = arm_at(0xe28f0008, 0x2000);
            check(info.literal_valid && info.literal == 0x2010 &&
                info.literal_size == 0, "arm add r0, pc, #8");
            info = arm_at(0xe24f1008, 0x2004);
            check(info.literal_valid && info.literal == 0x2004 &&
                info.literal_size == 0, "arm sub r1, pc, #8");
            info = arm_at(0xe28f0c01, 0x2000); /* add r0, pc, #256 */
            check(info.literal_valid && info.literal == 0x2108,
                "arm add r0, pc, #256 (rotated)");
            info = arm_at(0xe08f2003, 0x2008);
            check(!info.literal_valid, "arm add r2, pc, r3: no address");
        }
    }

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
    check_arm_flow(0xea00003c, 0x308, FLOW_JUMP, 0x400, 0, "b");
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
        /* Entries 0: a target inside the table, so no length can be
           inferred either. */
        uint16_t code[] = { 0xbf00, 0xbf00, 0xe8df, 0xf003, 0x0000, 0x0000,
            0xdf00, 0x4770 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(r.refused == 1 && r.refused_address == 0x100c,
            "map d: unbounded table then svc, refused at svc");
        code_map_result_free(&r);
    }
    /* (e) unbounded tbb, no pattern after: not refused, rest not code. */
    {
        uint16_t code[] = { 0xbf00, 0xbf00, 0xe8df, 0xf003, 0x0000, 0x0000,
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
    /* (r) a pool after a call that never returns, reached only through
       adr: decoding stops at it; it isn't trusted as data, so a pattern in
       it refuses the section. */
    {
        uint16_t code[] = { 0xa302 /* adr r3, 0x100c */, 0x4798 /* blx r3 */,
            0xbf00, 0xbf00, 0xf3af, 0x8000 /* nop.w */, 0xdf00, 0xdf00 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(state_at(&r, 0x100c) == CODE_STATE_DATA &&
            state_at(&r, 0x1008) == CODE_STATE_THUMB,
            "map r: adr target not decoded as code");
        check(r.refused == 1 && r.refused_address == 0x100c,
            "map r: pattern in an adr pool refuses");
        code_map_result_free(&r);
    }
    {
        uint16_t code[] = { 0xa302, 0x4798, 0xbf00, 0xbf00, 0xf3af, 0x8000,
            0x5c00, 0x0526 };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(r.refused == 0 && state_at(&r, 0x100c) == CODE_STATE_DATA,
            "map r2: adr pool without a pattern");
        code_map_result_free(&r);
    }
    /* (n) tbh bounded by a register compare: the length is inferred
       from the entries (the first case starts right after the table). */
    {
        uint16_t code[] = { 0x4299 /* cmp r1, r3 */, 0xd804 /* bhi 0x100e */,
            0xe8df, 0xf011 /* tbh [pc, r1, lsl #1], table at 0x1008 */,
            0x0002, 0x0003 /* -> 0x100c, 0x100e */,
            0xdf00 /* case 0: svc */, 0x4770 /* case 1: bx lr */ };
        struct code_function f[] = { { 0x1000, 0x1010, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 8, f, 1);
        check(r.refused == 0, "map n: inferred table, not refused");
        check(state_at(&r, 0x1008) == CODE_STATE_DATA &&
            state_at(&r, 0x100a) == CODE_STATE_DATA &&
            state_at(&r, 0x100c) == CODE_STATE_THUMB &&
            state_at(&r, 0x100e) == CODE_STATE_THUMB,
            "map n: table data, cases Thumb");
        code_map_result_free(&r);
    }
    /* (o) a return inside an IT block is conditional: decoding goes on. */
    {
        uint16_t code[] = { 0xbf08 /* it eq */, 0xbd00 /* popeq {pc} */,
            0xdf00 /* svc */, 0x4770 };
        struct code_function f[] = { { 0x1000, 0x1008, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 4, f, 1);
        check(r.refused == 0 && state_at(&r, 0x1004) == CODE_STATE_THUMB,
            "map o: conditional return in an IT block");
        code_map_result_free(&r);
    }
    /* (p) a pool after a return, reached only through adr (not a load
       the decoder knows): never decoded as code; it holds a pattern, so
       the section is refused rather than the pool patched. */
    {
        uint16_t code[] = { 0xa301 /* adr r3, 0x1008 */, 0xbf00, 0x4770,
            0xbf00, 0xdf00, 0xdf00 };
        struct code_function f[] = { { 0x1000, 0x100c, MODE_THUMB } };
        struct code_map_result r = build_thumb(code, 6, f, 1);
        check(state_at(&r, 0x1008) == CODE_STATE_DATA,
            "map p: adr pool not decoded as code");
        check(r.refused == 1 && r.refused_address == 0x1008,
            "map p: refused at the pool's pattern");
        code_map_result_free(&r);
    }
    /* (q) ARM switch: addls pc, pc, r0, lsl #2 then a row of branches. */
    {
        uint16_t code[] = {
            0x0001, 0xe350 /* 0x1000 cmp r0, #1 */,
            0xf100, 0x908f /* 0x1004 addls pc, pc, r0, lsl #2 */,
            0x0001, 0xea00 /* 0x1008 b 0x1014 (default) */,
            0x0001, 0xea00 /* 0x100c b 0x1018 (case 0) */,
            0x0002, 0xea00 /* 0x1010 b 0x1020 (case 1) */,
            0xff1e, 0xe12f /* 0x1014 bx lr */,
            0x0000, 0xef00 /* 0x1018 svc 0 */,
            0xff1e, 0xe12f /* 0x101c bx lr */,
            0x0f70, 0xee1d /* 0x1020 mrc p15, 0, r0, c13, c0, 3 */,
            0xff1e, 0xe12f /* 0x1024 bx lr */ };
        struct code_function f[] = { { 0x1000, 0x1028, MODE_ARM } };
        struct code_map_result r = build_thumb(code, 20, f, 1);
        check(r.refused == 0, "map q: ARM switch not refused");
        check(state_at(&r, 0x1018) == CODE_STATE_ARM &&
            state_at(&r, 0x1020) == CODE_STATE_ARM, "map q: cases are ARM");
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
                const uint16_t *code = (const uint16_t *)(bytes + first -
                    le32(sh + 12));
                size_t halfwords = (run_end - first) / 2u;
                size_t done = 0;
                /* thumb_scan in chunks; a chunk ends on a whole instruction. */
                while (done < halfwords) {
                    size_t chunk = halfwords - done > 4096 ? 4096 :
                        halfwords - done;
                    size_t n, k;
                    if (done + chunk < halfwords &&
                        thumb_is_32bit(code[done + chunk - 1])) {
                        size_t i = done;
                        while (i < done + chunk)
                            i += thumb_is_32bit(code[i]) ? 2 : 1;
                        chunk = i - done;
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

/* Patch sites of stripped (decoded) against full (its mapping symbols). */
static void compare_pair(const char *full_path, const char *stripped_path)
{
    size_t full_size, stripped_size;
    uint8_t *full = read_file(full_path, &full_size);
    uint8_t *stripped = read_file(stripped_path, &stripped_size);
    static struct mapping_symbol truth[1u << 20];
    static uint32_t want[1u << 16], got[1u << 16];
    size_t truth_count, want_count, got_count, k;
    struct code_map_result r;

    check(full != 0 && stripped != 0, "pair: readable");
    if (full == 0 || stripped == 0) return;
    truth_count = symtab_mapping(full, truth, 1u << 20);
    want_count = patch_sites(full, truth, truth_count, want, 1u << 16);
    printf("  mapping symbols %zu, patch sites %zu\n", truth_count, want_count);
    check(code_map_from_elf(stripped, stripped_size, &r) == 0,
        "pair: decoded");
    if (r.refused)
        printf("  refused at %#x\n", r.refused_address);
    check(!r.refused, "pair: not refused");
    got_count = patch_sites(stripped, r.symbols, r.symbol_count, got, 1u << 16);
    check(got_count == want_count &&
        memcmp(got, want, want_count * sizeof(want[0])) == 0,
        "pair: same patch sites");
    /* Byte by byte: code the symbols call data (a pool or table decoded
       as instructions, where a false patch could land) must not happen;
       code left undecoded is only coverage. */
    {
        unsigned count = le16(full + 48);
        unsigned i;
        size_t as_code_wrongly = 0, wrong_mode = 0, missed = 0, total = 0;
        uint32_t first_wrong = 0;
        for (i = 0; i < count; ++i) {
            const uint8_t *sh = SHDR(full, i);
            uint32_t a, end;
            if (le32(sh + 4) != 1 || (le32(sh + 8) & 4u) == 0) continue;
            end = le32(sh + 12) + le32(sh + 20);
            for (a = le32(sh + 12); a < end; ++a) {
                uint32_t e1, e2;
                enum code_state t = mapping_state_at(truth, truth_count, a,
                    end, &e1);
                enum code_state d = mapping_state_at(r.symbols,
                    r.symbol_count, a, end, &e2);
                total++;
                if (t == CODE_STATE_DATA && d != CODE_STATE_DATA) {
                    if (as_code_wrongly++ == 0) first_wrong = a;
                } else if (t != CODE_STATE_DATA && d != CODE_STATE_DATA &&
                    t != d) {
                    if (wrong_mode++ == 0 && first_wrong == 0) first_wrong = a;
                } else if (t != CODE_STATE_DATA && d == CODE_STATE_DATA)
                    missed++;
            }
        }
        printf("  code bytes %zu: data decoded as code %zu, wrong mode %zu, "
            "code left undecoded %zu (first wrong %#x)\n", total,
            as_code_wrongly, wrong_mode, missed, first_wrong);
        check(wrong_mode == 0, "pair: no code decoded in the wrong mode");
        check(as_code_wrongly == 0, "pair: no data decoded as code");
    }
    for (k = 0; k < want_count || k < got_count; ++k)
        if (k >= want_count || k >= got_count || want[k] != got[k]) {
            printf("  first difference at site %zu: want %#x/%u got %#x/%u\n",
                k, k < want_count ? want[k] / 4u : 0,
                k < want_count ? want[k] % 4u : 0,
                k < got_count ? got[k] / 4u : 0,
                k < got_count ? got[k] % 4u : 0);
            break;
        }
    code_map_result_free(&r);
    free(full);
    free(stripped);
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

int main(int argc, char **argv)
{
    /* code-map-test UNSTRIPPED STRIPPED: compare one pair only. */
    if (argc == 3) {
        compare_pair(argv[1], argv[2]);
        printf("code_map_test_failures=%d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    test_insn_info();
    test_map_builder();
    test_fixture_files();
    printf("code_map_test_failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
