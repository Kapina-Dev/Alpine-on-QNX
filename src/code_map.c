#include "code_map.h"

#include <string.h>

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1u);
    return (int32_t)((value ^ sign) - sign);
}

static uint32_t thumb_literal_base(uint32_t address)
{
    return (address + 4u) & ~3u; /* Align(PC, 4) */
}

static void set_literal(struct insn_info *out, uint32_t base, int add,
    uint32_t offset, unsigned size)
{
    out->literal_valid = 1;
    out->literal = add ? base + offset : base - offset;
    out->literal_size = size;
}

static void set_target(struct insn_info *out, enum insn_flow flow,
    uint32_t target, int thumb)
{
    out->flow = flow;
    out->target_valid = 1;
    out->target = target;
    out->target_thumb = thumb;
}

static void thumb16_info(uint16_t hw, uint32_t address, struct insn_info *out)
{
    if ((hw & 0xf000u) == 0xd000u && (hw & 0x0e00u) != 0x0e00u) {
        /* b<cond>.n (T1); 0xde is udf, 0xdf is svc */
        set_target(out, FLOW_BRANCH,
            address + 4u + (uint32_t)sign_extend((hw & 0xffu) << 1, 9), 1);
    } else if ((hw & 0xf800u) == 0xe000u) {
        /* b.n (T2) */
        set_target(out, FLOW_BRANCH,
            address + 4u + (uint32_t)sign_extend((hw & 0x7ffu) << 1, 12), 1);
    } else if ((hw & 0xf800u) == 0x4800u) {
        /* ldr Rt, [pc, #imm8*4] (T1) */
        set_literal(out, thumb_literal_base(address), 1, (hw & 0xffu) * 4u, 4);
    }
}

static void thumb32_branch(uint16_t hw0, uint16_t hw1, uint32_t address,
    struct insn_info *out)
{
    uint32_t s = (hw0 >> 10) & 1u;
    uint32_t j1 = (hw1 >> 13) & 1u;
    uint32_t j2 = (hw1 >> 11) & 1u;
    uint32_t imm11 = hw1 & 0x7ffu;
    uint32_t pc = address + 4u;

    if ((hw1 & 0x5000u) == 0x0000u) {
        /* b<cond>.w (T3); cond 111x is another instruction class */
        uint32_t cond = (hw0 >> 6) & 0xfu;
        uint32_t imm;
        if ((cond & 0xeu) == 0xeu) return;
        imm = (s << 20) | (j2 << 19) | (j1 << 18) |
            ((uint32_t)(hw0 & 0x3fu) << 12) | (imm11 << 1);
        set_target(out, FLOW_BRANCH, pc + (uint32_t)sign_extend(imm, 21), 1);
    } else {
        uint32_t i1 = !(j1 ^ s);
        uint32_t i2 = !(j2 ^ s);
        uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) |
            ((uint32_t)(hw0 & 0x3ffu) << 12) | (imm11 << 1);
        int32_t offset = sign_extend(imm, 25);
        switch (hw1 & 0x5000u) {
        case 0x1000u: /* b.w (T4) */
            set_target(out, FLOW_BRANCH, pc + (uint32_t)offset, 1);
            break;
        case 0x5000u: /* bl */
            set_target(out, FLOW_CALL, pc + (uint32_t)offset, 1);
            break;
        default:      /* 0x4000: blx imm, to ARM */
            if ((hw1 & 1u) != 0) return;
            set_target(out, FLOW_CALL, (pc & ~3u) + (uint32_t)offset, 0);
            break;
        }
    }
}

static void thumb32_info(uint16_t hw0, uint16_t hw1, uint32_t address,
    struct insn_info *out)
{
    uint32_t base = thumb_literal_base(address);
    int add = (hw0 & 0x0080u) != 0;
    unsigned rt = hw1 >> 12;

    if ((hw0 & 0xf800u) == 0xf000u && (hw1 & 0x8000u) != 0) {
        thumb32_branch(hw0, hw1, address, out);
        return;
    }
    switch (hw0 & 0xff7fu) {
    case 0xf85fu: /* ldr.w Rt, [pc, #+/-imm12] */
        set_literal(out, base, add, hw1 & 0xfffu, 4);
        return;
    case 0xf81fu: /* ldrb; Rt == pc is pld */
    case 0xf91fu: /* ldrsb; Rt == pc is pli */
        if (rt != 15u) set_literal(out, base, add, hw1 & 0xfffu, 1);
        return;
    case 0xf83fu: /* ldrh; Rt == pc is a hint */
    case 0xf93fu: /* ldrsh */
        if (rt != 15u) set_literal(out, base, add, hw1 & 0xfffu, 2);
        return;
    case 0xe95fu: /* ldrd Rt, Rt2, [pc, #+/-imm8*4] */
        set_literal(out, base, add, (hw1 & 0xffu) * 4u, 8);
        return;
    default:
        break;
    }
    if ((hw0 & 0xff3fu) == 0xed1fu && (hw1 & 0x0e00u) == 0x0a00u) {
        /* vldr Sd/Dd, [pc, #+/-imm8*4] */
        set_literal(out, base, add, (hw1 & 0xffu) * 4u,
            (hw1 & 0x0100u) ? 8u : 4u);
        return;
    }
    if (hw0 == 0xe8dfu && (hw1 & 0xffe0u) == 0xf000u) {
        /* tbb/tbh [pc, Rm{, lsl #1}] */
        out->flow = FLOW_TABLE;
        out->table_reg = hw1 & 0xfu;
        out->table_entry_size = (hw1 & 0x10u) ? 2u : 1u;
    }
}

int thumb_insn_info(const uint16_t *code, size_t halfwords, uint32_t address,
    struct insn_info *out)
{
    memset(out, 0, sizeof(*out));
    out->flow = FLOW_NEXT;
    if (halfwords == 0) return -1;
    if (!thumb_is_32bit(code[0])) {
        out->width = 2;
        thumb16_info(code[0], address, out);
        return 0;
    }
    out->width = 4;
    if (halfwords < 2) return -1;
    thumb32_info(code[0], code[1], address, out);
    return 0;
}

int arm_insn_info(uint32_t word, uint32_t address, struct insn_info *out)
{
    uint32_t cond = word >> 28;
    uint32_t pc = address + 8u;
    int add = (word & (1u << 23)) != 0;

    memset(out, 0, sizeof(*out));
    out->flow = FLOW_NEXT;
    out->width = 4;
    if ((word & 0xfe000000u) == 0xfa000000u) {
        /* blx imm, to Thumb */
        uint32_t imm = ((word & 0xffffffu) << 2) | ((word >> 23) & 2u);
        set_target(out, FLOW_CALL, pc + (uint32_t)sign_extend(imm, 26), 1);
        return 0;
    }
    if (cond == 0xfu) return 0; /* unconditional space: pld and others */
    if ((word & 0x0e000000u) == 0x0a000000u) {
        set_target(out, (word & 0x01000000u) ? FLOW_CALL : FLOW_BRANCH,
            pc + (uint32_t)sign_extend((word & 0xffffffu) << 2, 26), 0);
    } else if ((word & 0x0f3f0000u) == 0x051f0000u) {
        /* ldr/ldrb Rt, [pc, #+/-imm12] */
        set_literal(out, pc, add, word & 0xfffu,
            (word & 0x00400000u) ? 1u : 4u);
    } else if ((word & 0x0f7f00f0u) == 0x014f00d0u) {
        /* ldrd Rt, Rt2, [pc, #+/-imm8] */
        set_literal(out, pc, add, ((word >> 4) & 0xf0u) | (word & 0xfu), 8);
    } else if ((word & 0x0f7f0090u) == 0x015f0090u && (word & 0x60u) != 0) {
        /* ldrh / ldrsb / ldrsh Rt, [pc, #+/-imm8] */
        set_literal(out, pc, add, ((word >> 4) & 0xf0u) | (word & 0xfu),
            (word & 0x60u) == 0x40u ? 1u : 2u);
    } else if ((word & 0x0f3f0e00u) == 0x0d1f0a00u) {
        /* vldr Sd/Dd, [pc, #+/-imm8*4] */
        set_literal(out, pc, add, (word & 0xffu) * 4u,
            (word & 0x100u) ? 8u : 4u);
    }
    return 0;
}

int thumb_cmp_imm(const uint16_t *code, size_t halfwords, unsigned *reg,
    uint32_t *imm)
{
    uint16_t hw0;
    uint16_t hw1;
    uint32_t imm12;
    uint32_t value;

    if (halfwords == 0) return 0;
    hw0 = code[0];
    if ((hw0 & 0xf800u) == 0x2800u) { /* cmp Rn, #imm8 (T1) */
        *reg = (hw0 >> 8) & 7u;
        *imm = hw0 & 0xffu;
        return 1;
    }
    if (halfwords < 2 || (hw0 & 0xfbf0u) != 0xf1b0u) return 0;
    hw1 = code[1];
    if ((hw1 & 0x8f00u) != 0x0f00u) return 0; /* cmp.w Rn, #const (T2) */
    imm12 = ((uint32_t)(hw0 & 0x0400u) << 1) | ((hw1 >> 4) & 0x700u) |
        (hw1 & 0xffu);
    /* ThumbExpandImm */
    value = imm12 & 0xffu;
    switch (imm12 >> 8) {
    case 0: break;
    case 1: value = value | (value << 16); break;
    case 2: value = (value << 8) | (value << 24); break;
    case 3: value = value | (value << 8) | (value << 16) | (value << 24); break;
    default: {
        uint32_t rotate = imm12 >> 7;
        value = 0x80u | (imm12 & 0x7fu);
        value = (value >> rotate) | (value << (32u - rotate));
        break;
    }
    }
    *reg = hw0 & 0xfu;
    *imm = value;
    return 1;
}
