/* Phase 4 part 3: dlopen an unstripped library and its stripped copy,
   check Linuxemu patched both copies' code identically, then use the
   stripped copy.
     stripped-compare UNSTRIPPED STRIPPED mixed|sqlite
     stripped-compare --refused LIBRARY
   1 dlopen/dlsym (or, with --refused, 0 when dlopen fails and 4 when it
   loads), 2 the executable segments differ (first offset
   printed), 3 a function of the stripped copy gave a wrong result. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct segment {
    const char *name;
    const unsigned char *start;
    size_t size;
    size_t skip; /* the ELF header, which strip changes (e_shoff...) */
};

static int find_segment(struct dl_phdr_info *info, size_t size, void *data)
{
    struct segment *wanted = data;
    int i;
    (void)size;
    if (info->dlpi_name == 0 || strcmp(info->dlpi_name, wanted->name) != 0)
        return 0;
    for (i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) != 0) {
            wanted->start = (const unsigned char *)(info->dlpi_addr + ph->p_vaddr);
            wanted->size = ph->p_memsz;
            wanted->skip = ph->p_offset == 0 ? sizeof(ElfW(Ehdr)) : 0;
            return 1;
        }
    }
    return 0;
}

static void *open_library(const char *path)
{
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library == 0) fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
    return library;
}

static void *symbol(void *library, const char *name)
{
    void *address = dlsym(library, name);
    if (address == 0) fprintf(stderr, "dlsym %s: %s\n", name, dlerror());
    return address;
}

#define WANT(condition, what) do { if (!(condition)) { \
    fprintf(stderr, "wrong result: %s\n", what); return 3; } } while (0)

static int use_mixed(void *library)
{
    const char *names[] = { "mixed_getpid", "mixed_tp", "mixed_call_helpers",
        "mixed_call_table", "mixed_switch_small", "mixed_switch_large",
        "mixed_pool_forward", "mixed_pool_backward", "mixed_pool_double",
        "mixed_odd_char", "mixed_call_arm", "mixed_arm_getpid",
        "mixed_arm_pool", "mixed_sum", "mixed_hash" };
    void *f[sizeof(names) / sizeof(names[0])];
    unsigned i;
    void *ours;
    int pid = getpid();

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if ((f[i] = symbol(library, names[i])) == 0) return 1;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(ours));
    WANT(((int (*)(void))f[0])() == pid, "mixed_getpid");
    WANT(((void *(*)(void))f[1])() == ours, "mixed_tp");
    WANT(((int (*)(void))f[2])() == pid, "mixed_call_helpers");
    WANT(((int (*)(int, int))f[3])(0, 5) == pid + 5, "table getpid");
    WANT(((int (*)(int, int))f[3])(1, 5) == 10, "table double");
    WANT(((int (*)(int, int))f[3])(2, 5) == 25, "table square");
    WANT(((int (*)(int, int))f[3])(3, 5) == 6, "table tls");
    WANT(((int (*)(int))f[4])(0) == 11, "switch small 0");
    WANT(((int (*)(int))f[4])(3) == 77, "switch small 3");
    WANT(((int (*)(int))f[4])(7) == (7 * 31 + 1) * 31 + 2, "switch small 7");
    WANT(((int (*)(int))f[4])(8) == -1, "switch small default");
    {
        int x = 17, n = 17;
        int a = x * 31 + n, b = a * 31 + n * 3, c = b * 31 + a;
        WANT(((int (*)(int))f[5])(17) == ((c * 31 + b) ^ (a * 31 + n)),
            "switch large 17");
        WANT(((int (*)(int))f[5])(30) == -1, "switch large default");
    }
    WANT(((unsigned (*)(void))f[6])() == (0xdf00df00u ^ 0xef000000u),
        "pool forward");
    WANT(((unsigned (*)(void))f[7])() == 0xdf00df00u, "pool backward");
    WANT(((double (*)(void))f[8])() == 0x1.f00dfef000000p+14, "pool double");
    WANT(((int (*)(void))f[9])() == 'i', "odd char");
    WANT(((int (*)(void))f[10])() == pid * 2, "call arm");
    WANT(((int (*)(void))f[11])() == pid, "arm getpid");
    WANT(((unsigned (*)(void))f[12])() == 0xef000000u, "arm pool");
    WANT(((int (*)(int, int))f[13])(2, 3) == 5, "sum");
    WANT(((unsigned (*)(const char *))f[14])("") == 5381, "hash");
    return 0;
}

static int sqlite_row(void *out, int count, char **values, char **names)
{
    (void)names;
    if (count == 2) snprintf(out, 64, "%s|%s", values[0], values[1]);
    return 0;
}

static int use_sqlite(void *library)
{
    int (*open)(const char *, void **) = symbol(library, "sqlite3_open");
    int (*exec)(void *, const char *, int (*)(void *, int, char **, char **),
        void *, char **) = symbol(library, "sqlite3_exec");
    int (*close)(void *) = symbol(library, "sqlite3_close");
    void *db;
    char row[64] = "";

    if (open == 0 || exec == 0 || close == 0) return 1;
    WANT(open(":memory:", &db) == 0, "sqlite3_open");
    WANT(exec(db, "create table t(a, b); insert into t values (42, 'linuxemu');"
        "insert into t values (7, 'qnx');", 0, 0, 0) == 0, "create/insert");
    WANT(exec(db, "select sum(a), group_concat(b, '+') from t;", sqlite_row,
        row, 0) == 0, "select");
    WANT(strcmp(row, "49|linuxemu+qnx") == 0, "row");
    close(db);
    printf("sqlite row %s\n", row);
    return 0;
}

int main(int argc, char **argv)
{
    void *unstripped, *stripped;
    struct segment a, b;
    size_t i;

    if (argc == 3 && strcmp(argv[1], "--refused") == 0) {
        if (dlopen(argv[2], RTLD_NOW | RTLD_LOCAL) != 0) {
            fprintf(stderr, "%s loaded; expected a refusal\n", argv[2]);
            return 4;
        }
        printf("refused: %s\n", dlerror());
        return 0;
    }
    if (argc != 4) return 1;
    if ((unstripped = open_library(argv[1])) == 0 ||
        (stripped = open_library(argv[2])) == 0) return 1;
    a.name = argv[1]; a.start = 0;
    b.name = argv[2]; b.start = 0;
    dl_iterate_phdr(find_segment, &a);
    dl_iterate_phdr(find_segment, &b);
    if (a.start == 0 || b.start == 0) {
        fprintf(stderr, "no executable segment found\n");
        return 1;
    }
    if (a.size != b.size) {
        fprintf(stderr, "segment sizes differ: %zu %zu\n", a.size, b.size);
        return 2;
    }
    for (i = a.skip; i < a.size; ++i) {
        if (a.start[i] != b.start[i]) {
            fprintf(stderr, "first difference at offset 0x%zx: %02x %02x\n",
                i, a.start[i], b.start[i]);
            return 2;
        }
    }
    printf("identical %zu bytes\n", a.size);
    if (strcmp(argv[3], "sqlite") == 0) {
        if (use_sqlite(stripped) != 0) return 3;
    } else if (use_mixed(stripped) != 0) return 3;
    puts("stripped-compare ok");
    return 0;
}
