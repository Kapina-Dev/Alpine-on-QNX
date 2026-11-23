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

/* Per-address marks a decoding round leaves (struct builder.reached). */
#define REACHED_CALL_ARM 1u
#define REACHED_CALL_THUMB 2u
#define REACHED_BRANCH_ARM 4u
#define REACHED_BRANCH_THUMB 8u

/* A tbb/tbh is bounded by a cmp of its index register within this many
   preceding instructions (GCC emits cmp; bhi; tbb). */
#define TABLE_CMP_WINDOW 4u
/* Rounds before giving up on a fixpoint. */
#define MAX_ROUNDS 64u

struct span {
    uint32_t start;
    uint32_t limit;  /* end before clipping at the next start */
    enum fn_mode mode;
};

/*
 * One round decodes every function whose mode is known, using the literal
 * data (data) and the call/branch targets (reached) the previous round
 * found, and records what it finds into next_data and next_reached. The
 * function list is the caller's plus every call target (a merged unwind
 * entry can cover functions of both modes). Rounds repeat until nothing
 * changes, so nothing a discarded decoding found survives.
 */
struct builder {
    const uint8_t *bytes;
    uint32_t base;
    size_t size;
    const struct code_function *initial;
    size_t initial_count;
    struct span *spans;
    size_t span_count;
    uint8_t *klass;
    uint8_t *data;
    uint8_t *next_data;
    uint8_t *reached;
    uint8_t *next_reached;
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

static int in_section(const struct builder *b, uint32_t address)
{
    return address >= b->base && address - b->base < b->size;
}

static int any_data(const struct builder *b, size_t offset, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i)
        if (b->data[offset + i]) return 1;
    return 0;
}

static void note(struct builder *b, const struct insn_info *info)
{
    uint32_t i;
    if (info->literal_valid) {
        for (i = 0; i < info->literal_size; ++i)
            if (in_section(b, info->literal + i))
                b->next_data[info->literal + i - b->base] = 1;
    }
    if (info->target_valid && in_section(b, info->target)) {
        uint8_t mark;
        if (info->flow == FLOW_CALL)
            mark = info->target_thumb ? REACHED_CALL_THUMB : REACHED_CALL_ARM;
        else
            mark = info->target_thumb ? REACHED_BRANCH_THUMB :
                REACHED_BRANCH_ARM;
        b->next_reached[info->target - b->base] |= mark;
    }
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
        note(b, &info);
        if (info.flow == FLOW_TABLE) {
            uint32_t length = 0;
            int bounded = 0;
            for (i = 0; i < TABLE_CMP_WINDOW; ++i) {
                if (recent[i].valid && recent[i].reg == info.table_reg) {
                    bounded = recent[i].imm < 0x10000u;
                    length = (recent[i].imm + 1u) * info.table_entry_size;
                    length = (length + 1u) & ~1u;
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
        memmove(&recent[1], &recent[0],
            sizeof(recent[0]) * (TABLE_CMP_WINDOW - 1u));
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
        note(b, &info);
    }
}

static int compare_spans(const void *left, const void *right)
{
    const struct span *a = left;
    const struct span *b = right;
    return a->start < b->start ? -1 : a->start > b->start;
}

static enum fn_mode combine(enum fn_mode mode, uint8_t marks)
{
    int arm = mode == MODE_ARM || (marks & (REACHED_CALL_ARM |
        REACHED_BRANCH_ARM)) != 0;
    int thumb = mode == MODE_THUMB || (marks & (REACHED_CALL_THUMB |
        REACHED_BRANCH_THUMB)) != 0;
    if (arm && thumb) return MODE_UNKNOWN; /* reached in both modes */
    return arm ? MODE_ARM : thumb ? MODE_THUMB : MODE_UNKNOWN;
}

/* The caller's functions plus every call target, sorted, each clipped at
   the next start, with modes from the evidence and from reached. */
static void build_spans(struct builder *b)
{
    size_t i;
    size_t count = 0;
    size_t unique = 0;

    for (i = 0; i < b->initial_count; ++i) {
        b->spans[count].start = b->initial[i].start;
        b->spans[count].limit = b->initial[i].end;
        b->spans[count].mode = b->initial[i].mode;
        count++;
    }
    for (i = 0; i < b->size; ++i) {
        if ((b->reached[i] & (REACHED_CALL_ARM | REACHED_CALL_THUMB)) == 0)
            continue;
        b->spans[count].start = b->base + (uint32_t)i;
        b->spans[count].limit = b->base + (uint32_t)b->size;
        b->spans[count].mode = MODE_UNKNOWN;
        count++;
    }
    qsort(b->spans, count, sizeof(*b->spans), compare_spans);
    for (i = 0; i < count; ++i) {
        if (unique > 0 && b->spans[unique - 1].start == b->spans[i].start) {
            /* A call target that is also one of the caller's functions:
               keep the caller's extent and evidence. */
            if (b->spans[i].mode != MODE_UNKNOWN ||
                b->spans[unique - 1].mode == MODE_UNKNOWN)
                b->spans[unique - 1] = b->spans[i];
            continue;
        }
        /* A call target inside a caller's function ends where that one
           did. */
        if (unique > 0 && b->spans[i].limit > b->spans[unique - 1].limit &&
            b->spans[i].start < b->spans[unique - 1].limit)
            b->spans[i].limit = b->spans[unique - 1].limit;
        b->spans[unique++] = b->spans[i];
    }
    for (i = 0; i < unique; ++i) {
        uint32_t start = b->spans[i].start;
        if (i + 1 < unique && b->spans[i + 1].start < b->spans[i].limit)
            b->spans[i].limit = b->spans[i + 1].start;
        b->spans[i].mode = combine(b->spans[i].mode,
            b->reached[start - b->base]);
    }
    b->span_count = unique;
}

static void decode_round(struct builder *b)
{
    size_t i;
    build_spans(b);
    memset(b->klass, BYTE_UNDECODED, b->size);
    memset(b->next_data, 0, b->size);
    memset(b->next_reached, 0, b->size);
    for (i = 0; i < b->span_count; ++i) {
        const struct span *f = &b->spans[i];
        if (f->mode == MODE_THUMB) decode_thumb(b, f->start, f->limit);
        else if (f->mode == MODE_ARM) decode_arm(b, f->start, f->limit);
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

    for (i = 0; i < b->size; ++i)
        if (i == 0 || b->klass[i] != b->klass[i - 1]) count++;
    out->symbols = malloc(sizeof(*out->symbols) * (count ? count : 1));
    if (out->symbols == 0) return -1;
    count = 0;
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
    size_t allocation = size ? size : 1;
    unsigned round;
    size_t i;
    long site;
    int result = -1;

    memset(out, 0, sizeof(*out));
    memset(&b, 0, sizeof(b));
    b.bytes = bytes;
    b.base = base;
    b.size = size;
    /* Clip the caller's functions to the section. */
    for (i = 0; i < function_count; ++i) {
        if (functions[i].start < base) functions[i].start = base;
        if (functions[i].end > base + size)
            functions[i].end = base + (uint32_t)size;
        if (functions[i].end < functions[i].start)
            functions[i].end = functions[i].start;
    }
    b.initial = functions;
    b.initial_count = function_count;
    /* Call targets are halfword-aligned: at most size / 2 of them. */
    b.spans = malloc(sizeof(*b.spans) * (function_count + size / 2u + 1u));
    b.klass = malloc(allocation);
    b.data = calloc(allocation, 1);
    b.next_data = malloc(allocation);
    b.reached = calloc(allocation, 1);
    b.next_reached = malloc(allocation);
    if (b.spans == 0 || b.klass == 0 || b.data == 0 || b.next_data == 0 ||
        b.reached == 0 || b.next_reached == 0) goto done;

    for (round = 0;; ++round) {
        decode_round(&b);
        if (memcmp(b.data, b.next_data, size) == 0 &&
            memcmp(b.reached, b.next_reached, size) == 0) break;
        if (round + 1u == MAX_ROUNDS) {
            /* No fixpoint: no instruction boundary can be trusted. */
            memset(b.klass, BYTE_UNDECODED, size);
            break;
        }
        memcpy(b.data, b.next_data, size);
        memcpy(b.reached, b.next_reached, size);
    }
    /* Report the modes the caller's functions ended with. */
    for (i = 0; i < function_count; ++i) {
        size_t j;
        for (j = 0; j < b.span_count; ++j)
            if (b.spans[j].start == functions[i].start)
                functions[i].mode = b.spans[j].mode;
    }
    site = first_undecoded_site(&b);
    if (site >= 0) {
        out->refused = 1;
        out->refused_address = base + (uint32_t)site;
    }
    result = emit_symbols(&b, out);
done:
    free(b.spans);
    free(b.klass);
    free(b.data);
    free(b.next_data);
    free(b.reached);
    free(b.next_reached);
    return result;
}

void code_map_result_free(struct code_map_result *result)
{
    free(result->symbols);
    result->symbols = 0;
    result->symbol_count = 0;
}
