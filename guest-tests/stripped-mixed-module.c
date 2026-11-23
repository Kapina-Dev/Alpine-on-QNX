/* The Thumb half of the Phase 4 part 3 stripped-decoding fixture
   (stripped-mixed-armhf.so), built -mthumb -funwind-tables. The ARM half
   is stripped-mixed-arm.c. The library is kept unstripped (ground truth:
   mapping symbols) and stripped (decoded); stripped-compare checks both
   copies are patched identically.

   It holds what the decoder must get right: Thumb svc and TLS reads in
   exported functions and in static ones reached only by bl or only
   through a function-pointer table; switch statements (tbb/tbh tables);
   literal pools holding words that look like svc (forward and
   backward-referenced, and a double); an odd pointer into a string in
   .text; and calls into ARM code. Every svc sets ip to an invalid QNX call
   first, in case it ever runs unpatched. */

#define THUMB_GETPID(out) do {                                       \
        register int r0_ __asm__("r0");                                 \
        __asm__ volatile(                                               \
            "push {r7}\n\t"                                             \
            "movw ip, #0x7fff\n\t"                                      \
            "movs r7, #20\n\t"                                          \
            "svc #0\n\t"                                                \
            "pop {r7}"                                                  \
            : "=r"(r0_) : : "ip", "memory");                            \
        (out) = r0_;                                                    \
    } while (0)

#define HIDDEN __attribute__((visibility("hidden")))
#define NOINLINE __attribute__((noinline))

HIDDEN int arm_getpid_inner(void);
HIDDEN void *arm_tp_inner(void);

int mixed_getpid(void)
{
    int result;
    THUMB_GETPID(result);
    return result;
}

void *mixed_tp(void)
{
    void *value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

/* Reached only by bl. */
static NOINLINE int helper_getpid(int bias)
{
    int result;
    THUMB_GETPID(result);
    return result + bias;
}

static NOINLINE void *helper_tp(int bias)
{
    char *value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value + bias;
}

int mixed_call_helpers(void)
{
    return helper_getpid(3) - 3 + (helper_tp(5) == (char *)mixed_tp() + 5 ? 0 : 1000);
}

/* Reached only through the table below (R_ARM_RELATIVE, Thumb bit set). */
static int table_getpid(int x) { int r; THUMB_GETPID(r); return r + x; }
static int table_double(int x) { return x * 2; }
static int table_square(int x) { return x * x; }
static int table_tls(int x)
{
    int value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return (value != 0) + x;
}

static int (*const table[])(int) = {
    table_getpid, table_double, table_square, table_tls
};

int mixed_call_table(int index, int x)
{
    if (index < 0 || index > 3) return -1;
    return table[index](x);
}

static NOINLINE int step(int a, int b) { return a * 31 + b; }

/* Dense, uneven cases: a tbb. */
int mixed_switch_small(int x)
{
    switch (x) {
    case 0: return step(x, 11);
    case 1: return step(x, 5) ^ 3;
    case 2: return step(x + 9, 1);
    case 3: return mixed_getpid() - mixed_getpid() + 77;
    case 4: return step(x, x) - 1;
    case 5: return step(2, x) + 40;
    case 6: return -step(x, 6);
    case 7: return step(step(x, 1), 2);
    default: return -1;
    }
}

/* Many large cases: a tbh. */
#define BIG_CASE(n)                                                     \
    case n: {                                                           \
        int a = step(x, n), b = step(a, n * 3), c = step(b, a);         \
        return step(c, b) ^ step(a, n);                                 \
    }

int mixed_switch_large(int x)
{
    switch (x) {
    BIG_CASE(0) BIG_CASE(1) BIG_CASE(2) BIG_CASE(3) BIG_CASE(4)
    BIG_CASE(5) BIG_CASE(6) BIG_CASE(7) BIG_CASE(8) BIG_CASE(9)
    BIG_CASE(10) BIG_CASE(11) BIG_CASE(12) BIG_CASE(13) BIG_CASE(14)
    BIG_CASE(15) BIG_CASE(16) BIG_CASE(17) BIG_CASE(18) BIG_CASE(19)
    BIG_CASE(20) BIG_CASE(21) BIG_CASE(22) BIG_CASE(23) BIG_CASE(24)
    BIG_CASE(25) BIG_CASE(26) BIG_CASE(27) BIG_CASE(28) BIG_CASE(29)
    default: return -1;
    }
}

/* Literal pools with words that look like svc, read by ldr. */
unsigned mixed_pool_forward(void)
{
    unsigned a, b;
    __asm__ volatile(
        "ldr %0, =0xdf00df00\n\t"
        "ldr %1, =0xef000000\n\t"
        "b 1f\n\t"
        ".ltorg\n"
        "1:"
        : "=r"(a), "=r"(b));
    return a ^ b;
}

unsigned mixed_pool_backward(void)
{
    unsigned a;
    __asm__ volatile(
        "b 2f\n\t"
        ".balign 4\n"
        "1: .word 0xdf00df00\n"
        "2: ldr.w %0, 1b"
        : "=r"(a));
    return a;
}

/* Bits 0x40df00dfef000000: vldr from a pool. */
double mixed_pool_double(void)
{
    return 0x1.f00dfef000000p+14;
}

/* A string in .text and an odd pointer to it, stored in data. */
__asm__(
    ".text\n"
    ".balign 4\n"
    ".type mixed_text_string, %object\n"
    "mixed_text_string:\n"
    ".ascii \"linuxemu-odd-pointer\\0\"\n"
    ".balign 4\n"
    ".size mixed_text_string, 24\n");
extern const char mixed_text_string[] HIDDEN;
static const char *const odd_pointer = &mixed_text_string[1];

int mixed_odd_char(void)
{
    return odd_pointer[0];
}

int mixed_call_arm(void)
{
    char *ours;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(ours));
    return arm_getpid_inner() * 2 + (arm_tp_inner() == ours ? 0 : 1);
}

int mixed_sum(int a, int b) { return a + b; }
int mixed_mul(int a, int b) { return a * b; }
int mixed_max(int a, int b) { return a > b ? a : b; }
int mixed_min(int a, int b) { return a < b ? a : b; }
int mixed_abs(int a) { return a < 0 ? -a : a; }
unsigned mixed_hash(const char *s)
{
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}
