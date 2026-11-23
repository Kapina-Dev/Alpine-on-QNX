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

/* ---- map builder ---- */

#include <stdlib.h>

enum byte_class { BYTE_UNDECODED = 0, BYTE_DATA, BYTE_ARM, BYTE_THUMB };

/* A tbb/tbh is bounded by a cmp of its index register within this many
   preceding instructions (GCC emits cmp; bhi; tbb). */
#define TABLE_CMP_WINDOW 4u
/* Rounds the literal-pool set may keep changing without a mode change. */
#define DATA_ROUNDS 8u

struct builder {
    const uint8_t *bytes;
    uint32_t base;
    size_t size;
    struct code_function *functions;
    size_t count;
    uint8_t *klass;      /* enum byte_class per byte, this round */
    uint8_t *data;       /* literal data known going into this round */
    uint8_t *next_data;  /* literal data found this round */
    int mode_changed;
    int conflict;
    uint8_t *poisoned;   /* per function: reached in both modes */
};

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static void mark(uint8_t *array, struct builder *b, uint32_t address,
    uint32_t length, uint8_t value)
{
    uint32_t i;
    for (i = 0; i < length; ++i) {
        uint32_t offset = address + i - b->base;
        if (address + i >= b->base && offset < b->size) array[offset] = value;
    }
}

static int any_data(const struct builder *b, size_t offset, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i)
        if (b->data[offset + i]) return 1;
    return 0;
}

static size_t find_function_start(const struct builder *b, uint32_t address)
{
    size_t low = 0;
    size_t high = b->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (b->functions[middle].start < address) low = middle + 1;
        else high = middle;
    }
    if (low < b->count && b->functions[low].start == address) return low;
    return b->count;
}

static void propagate(struct builder *b, const struct insn_info *info)
{
    size_t index;
    enum fn_mode mode;
    if (!info->target_valid) return;
    index = find_function_start(b, info->target);
    if (index == b->count || b->poisoned[index]) return;
    mode = info->target_thumb ? MODE_THUMB : MODE_ARM;
    if (b->functions[index].mode == MODE_UNKNOWN) {
        b->functions[index].mode = mode;
        b->mode_changed = 1;
    } else if (b->functions[index].mode != mode) {
        b->poisoned[index] = 1;
        b->conflict = 1;
    }
}

static void note_literal(struct builder *b, const struct insn_info *info)
{
    if (info->literal_valid)
        mark(b->next_data, b, info->literal, info->literal_size, 1);
}

static void decode_thumb(struct builder *b, uint32_t start, uint32_t end)
{
    size_t offset = start - b->base;
    size_t stop = end - b->base;
    struct { int valid; unsigned reg; uint32_t imm; } recent[TABLE_CMP_WINDOW];
    unsigned i;

    memset(recent, 0, sizeof(recent));
    if ((start & 1u) != 0) return;
    while (offset + 2u <= stop) {
        uint16_t code[2];
        struct insn_info info;
        size_t available;

        if (any_data(b, offset, 2)) { offset += 2u; continue; }
        code[0] = read16(b->bytes + offset);
        available = 1;
        if (offset + 4u <= stop && !any_data(b, offset + 2u, 2)) {
            code[1] = read16(b->bytes + offset + 2u);
            available = 2;
        }
        if (thumb_insn_info(code, available, b->base + (uint32_t)offset,
                &info) != 0) {
            /* A 32-bit prefix cut by data or the end: padding, leave it. */
            offset += 2u;
            continue;
        }
        memset(b->klass + offset, BYTE_THUMB, info.width);
        note_literal(b, &info);
        propagate(b, &info);
        if (info.flow == FLOW_TABLE) {
            uint32_t length = 0;
            int bounded = 0;
            for (i = 0; i < TABLE_CMP_WINDOW; ++i) {
                if (recent[i].valid && recent[i].reg == info.table_reg) {
                    length = (recent[i].imm + 1u) * info.table_entry_size;
                    length = (length + 1u) & ~1u;
                    bounded = recent[i].imm < 0x10000u;
                    break;
                }
            }
            if (!bounded) return; /* rest of the function: undecoded */
            offset += info.width;
            if (length > stop - offset) length = (uint32_t)(stop - offset);
            memset(b->klass + offset, BYTE_DATA, length);
            offset += length;
            memset(recent, 0, sizeof(recent));
            continue;
        }
        memmove(&recent[1], &recent[0], sizeof(recent[0]) *
            (TABLE_CMP_WINDOW - 1u));
        recent[0].valid = thumb_cmp_imm(code, available, &recent[0].reg,
            &recent[0].imm);
        offset += info.width;
    }
}

static void decode_arm(struct builder *b, uint32_t start, uint32_t end)
{
    size_t offset = start - b->base;
    size_t stop = end - b->base;

    if ((start & 3u) != 0) return;
    for (; offset + 4u <= stop; offset += 4u) {
        struct insn_info info;
        if (any_data(b, offset, 4)) continue;
        arm_insn_info(read32(b->bytes + offset), b->base + (uint32_t)offset,
            &info);
        memset(b->klass + offset, BYTE_ARM, 4);
        note_literal(b, &info);
        propagate(b, &info);
    }
}

static void decode_round(struct builder *b)
{
    size_t i;
    memset(b->klass, BYTE_UNDECODED, b->size);
    memset(b->next_data, 0, b->size);
    for (i = 0; i < b->count; ++i) {
        const struct code_function *f = &b->functions[i];
        if (b->poisoned[i]) continue;
        if (f->mode == MODE_THUMB) decode_thumb(b, f->start, f->end);
        else if (f->mode == MODE_ARM) decode_arm(b, f->start, f->end);
    }
    for (i = 0; i < b->size; ++i)
        if (b->data[i]) b->klass[i] = BYTE_DATA;
}

/* First possible patch site starting in an undecoded byte, or -1. */
static long first_undecoded_site(const struct builder *b)
{
    size_t i;
    for (i = 0; i < b->size; ++i) {
        uint32_t address = b->base + (uint32_t)i;
        if (b->klass[i] != BYTE_UNDECODED) continue;
        if ((address & 3u) == 0 && i + 4u <= b->size) {
            uint32_t word = read32(b->bytes + i);
            if (word == 0xef000000u || (word & 0xffff0fffu) == 0xee1d0f70u)
                return (long)i;
        }
        if ((address & 1u) == 0 && i + 2u <= b->size) {
            uint16_t hw = read16(b->bytes + i);
            if (hw == 0xdf00u) return (long)i;
            if (hw == 0xee1du && i + 4u <= b->size &&
                (read16(b->bytes + i + 2u) & 0x0fffu) == 0x0f70u)
                return (long)i;
        }
    }
    return -1;
}

static int emit_symbols(const struct builder *b, struct code_map_result *out)
{
    size_t i;
    size_t count = 0;
    enum code_state previous = CODE_STATE_DATA;

    out->symbols = malloc(sizeof(*out->symbols) * (b->size ? b->size : 1));
    if (out->symbols == 0) return -1;
    for (i = 0; i < b->size; ++i) {
        enum code_state state = b->klass[i] == BYTE_ARM ? CODE_STATE_ARM :
            b->klass[i] == BYTE_THUMB ? CODE_STATE_THUMB : CODE_STATE_DATA;
        if (i == 0 || state != previous) {
            out->symbols[count].address = b->base + (uint32_t)i;
            out->symbols[count].state = state;
            count++;
            previous = state;
        }
    }
    out->symbol_count = count;
    return 0;
}

int code_map_build(const uint8_t *bytes, uint32_t base, size_t size,
    struct code_function *functions, size_t function_count,
    struct code_map_result *out)
{
    struct builder b;
    enum fn_mode *initial;
    unsigned data_rounds = 0;
    size_t i;
    long site;
    int result = -1;

    memset(out, 0, sizeof(*out));
    memset(&b, 0, sizeof(b));
    b.bytes = bytes;
    b.base = base;
    b.size = size;
    b.functions = functions;
    b.count = function_count;
    /* Clip function extents to the section. */
    for (i = 0; i < function_count; ++i) {
        if (functions[i].start < base) functions[i].start = base;
        if (functions[i].end > base + size) functions[i].end = base + (uint32_t)size;
        if (functions[i].end < functions[i].start)
            functions[i].end = functions[i].start;
    }
    b.klass = malloc(size ? size : 1);
    b.data = calloc(size ? size : 1, 1);
    b.next_data = malloc(size ? size : 1);
    b.poisoned = calloc(function_count ? function_count : 1, 1);
    initial = malloc(sizeof(*initial) * (function_count ? function_count : 1));
    if (b.klass == 0 || b.data == 0 || b.next_data == 0 || b.poisoned == 0 ||
        initial == 0) goto done;
    for (i = 0; i < function_count; ++i) initial[i] = functions[i].mode;

    for (;;) {
        b.mode_changed = 0;
        b.conflict = 0;
        decode_round(&b);
        if (b.conflict) {
            /* Start over from the evidence, without the poisoned ones:
               modes they handed on came from a decoding that can't be
               trusted. */
            for (i = 0; i < function_count; ++i)
                functions[i].mode = b.poisoned[i] ? MODE_UNKNOWN : initial[i];
            memset(b.data, 0, size);
            data_rounds = 0;
            continue;
        }
        if (memcmp(b.data, b.next_data, size) == 0 && !b.mode_changed) break;
        if (b.mode_changed) data_rounds = 0;
        else if (++data_rounds >= DATA_ROUNDS) {
            /* Pools that keep moving: no safe boundaries anywhere. */
            memset(b.klass, BYTE_UNDECODED, size);
            break;
        }
        memcpy(b.data, b.next_data, size);
    }
    site = first_undecoded_site(&b);
    if (site >= 0) {
        out->refused = 1;
        out->refused_address = base + (uint32_t)site;
    }
    result = emit_symbols(&b, out);
done:
    free(b.klass);
    free(b.data);
    free(b.next_data);
    free(b.poisoned);
    free(initial);
    return result;
}

void code_map_result_free(struct code_map_result *result)
{
    free(result->symbols);
    result->symbols = 0;
    result->symbol_count = 0;
}
