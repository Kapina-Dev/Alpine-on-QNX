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

int main(void)
{
    test_insn_info();
    printf("code_map_test_failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
