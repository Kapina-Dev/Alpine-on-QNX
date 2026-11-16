/*
 * QNX-native probe for RIM's Android 4.3 binder resmgr and for W^X policy.
 *
 * Run it twice: as the ordinary user (expected: EACCES), and from the
 * Android player context via the core wrapper hook (expected: VERSION 7).
 * Both checks are in one binary because, in the player context, fsecd
 * trusts executables per inode: only one binary has to be placed there.
 *
 * Protocol facts come from docs/FINDINGS.md (2026-11-16 desk study). They
 * have not been reproduced by Linuxemu yet; this probe is that test.
 * Output is key=value lines; the last line is a summary.
 */
#define _QNX_SOURCE 1
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RIM_BINDER_VERSION        0xC0046209
#define RIM_BINDER_WRITE_READ     0xC0186201
#define RIM_BINDER_CFG            0xC108620C
#define RIM_SET_MAX_THREADS_RIM   0x80046205  /* RIM direction-swapped */
#define RIM_SET_MAX_THREADS_AOSP  0x40046205  /* expected ENOSYS */

#define CFG_SIZE        0x108
#define CFG_ADDR_OFF    0x100
#define CFG_VMSIZE_OFF  0x104
#define BINDER_VM_SIZE  ((1u * 1024u * 1024u) - (4096u * 2u))

/* Command words: AOSP value with the _IOC direction bits swapped. */
#define SWAP_DIR(x)     (((x) & 0x3fffffffu) | \
                         ((((x) >> 30) == 1u) ? 0x80000000u : \
                          (((x) >> 30) == 2u) ? 0x40000000u : \
                          ((x) & 0xc0000000u)))
#define AOSP_BC_TRANSACTION   0x40286300u
#define AOSP_BC_FREE_BUFFER   0x40046303u
#define PING_TRANSACTION      0x5f504e47u  /* '_PNG' */

struct bwr32 {
    uint32_t write_size;
    uint32_t write_consumed;
    uint32_t write_buffer;   /* offset into the devctl message */
    uint32_t read_size;
    uint32_t read_consumed;
    uint32_t read_buffer;    /* offset into the devctl message */
};

struct btd32 {
    uint32_t target;
    uint32_t cookie;
    uint32_t code;
    uint32_t flags;
    int32_t sender_pid;
    uint32_t sender_euid;
    uint32_t data_size;
    uint32_t offsets_size;
    uint32_t buffer;         /* offset into the devctl message on write */
    uint32_t offsets;
};

static const char *step_name = "start";

static void on_alarm(int signo)
{
    char line[96];
    int length;

    (void)signo;
    length = snprintf(line, sizeof(line), "timeout step=%s\n", step_name);
    write(STDOUT_FILENO, line, (size_t)length);
    _exit(3);
}

static void report_identity(void)
{
    gid_t groups[64];
    int count;
    int i;

    printf("pid=%d uid=%d euid=%d gid=%d egid=%d\n", (int)getpid(),
        (int)getuid(), (int)geteuid(), (int)getgid(), (int)getegid());
    count = getgroups(64, groups);
    printf("groups=");
    for (i = 0; i < count; i++) {
        printf("%s%d", i ? "," : "", (int)groups[i]);
    }
    printf("\n");
}

static void report_node(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        printf("node=%s stat_errno=%d %s\n", path, errno, strerror(errno));
        return;
    }
    printf("node=%s uid=%d gid=%d mode=%04o\n", path, (int)st.st_uid,
        (int)st.st_gid, (unsigned)(st.st_mode & 07777));
}

static int call_devctl(int fd, const char *name, unsigned dcmd, void *data,
    size_t size)
{
    int info = 0;
    int result;

    step_name = name;
    result = devctl(fd, (int)dcmd, data, size, &info);
    printf("devctl=%s dcmd=0x%08x size=%u result=%d info=%d %s\n", name,
        dcmd, (unsigned)size, result, info, result ? strerror(result) : "EOK");
    fflush(stdout);
    return result;
}

static void hexdump(const char *label, const unsigned char *data, size_t size)
{
    size_t i;

    printf("%s=", label);
    for (i = 0; i < size; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

static void decode_read_stream(const unsigned char *stream, uint32_t size,
    uint32_t cfg_addr)
{
    uint32_t pos = 0;

    while (pos + 4 <= size) {
        uint32_t cmd;
        uint32_t payload = 0;

        memcpy(&cmd, stream + pos, 4);
        pos += 4;
        payload = (cmd >> 16) & 0x3fffu;
        printf("br cmd=0x%08x nr=%u type=%c payload=%u\n", cmd, cmd & 0xffu,
            (int)((cmd >> 8) & 0xffu), payload);
        if ((cmd & 0xffffu) == 0x7203u && payload == sizeof(struct btd32)
            && pos + payload <= size) {  /* BR_REPLY in either direction */
            struct btd32 reply;

            memcpy(&reply, stream + pos, sizeof(reply));
            printf("br_reply code=0x%08x flags=0x%x sender_pid=%d "
                "sender_euid=%u data_size=%u offsets_size=%u buffer=0x%08x "
                "in_cfg_mapping=%d\n", reply.code, reply.flags,
                reply.sender_pid, reply.sender_euid, reply.data_size,
                reply.offsets_size, reply.buffer, cfg_addr != 0
                && reply.buffer >= cfg_addr
                && reply.buffer < cfg_addr + BINDER_VM_SIZE);
        }
        if (pos + payload > size) {
            printf("br truncated\n");
            return;
        }
        pos += payload;
    }
}

static const char *binder_probe(void)
{
    unsigned char cfg[CFG_SIZE];
    unsigned char message[512];
    struct bwr32 bwr;
    struct btd32 txn;
    uint32_t version = 0;
    uint32_t threads = 0;
    uint32_t cfg_addr = 0;
    uint32_t cmd;
    uint32_t payload = 0;
    const uint32_t write_off = sizeof(bwr);
    const uint32_t payload_off = write_off + 4 + sizeof(txn);
    const uint32_t read_off = payload_off + 4;
    const uint32_t read_size = sizeof(message) - read_off;
    int fd;

    step_name = "open";
    fd = open("/dev/binder", O_RDWR);
    if (fd < 0) {
        printf("open=/dev/binder errno=%d %s\n", errno, strerror(errno));
        return errno == EACCES ? "open_eacces" : "open_failed";
    }
    printf("open=/dev/binder fd=%d\n", fd);

    if (call_devctl(fd, "VERSION", RIM_BINDER_VERSION, &version,
            sizeof(version)) != 0) {
        close(fd);
        return "version_failed";
    }
    printf("binder_version=%u\n", version);

    memset(cfg, 0, sizeof(cfg));
    {
        uint32_t vm_size = BINDER_VM_SIZE;
        memcpy(cfg + CFG_VMSIZE_OFF, &vm_size, 4);
    }
    if (call_devctl(fd, "CFG", RIM_BINDER_CFG, cfg, sizeof(cfg)) != 0) {
        close(fd);
        return "cfg_failed";
    }
    memcpy(&cfg_addr, cfg + CFG_ADDR_OFF, 4);
    hexdump("cfg_reply", cfg, sizeof(cfg));
    printf("cfg_client_addr=0x%08x\n", cfg_addr);

    call_devctl(fd, "SET_MAX_THREADS_AOSP", RIM_SET_MAX_THREADS_AOSP,
        &threads, sizeof(threads));
    call_devctl(fd, "SET_MAX_THREADS_RIM", RIM_SET_MAX_THREADS_RIM,
        &threads, sizeof(threads));

    if (getenv("PROBE_SKIP_PING") != NULL) {
        close(fd);
        return "no_ping";
    }

    /* [bwr][BC_TRANSACTION][btd][4-byte zero payload][read area] */
    memset(message, 0, sizeof(message));
    memset(&txn, 0, sizeof(txn));
    txn.target = 0;              /* handle 0: context manager */
    txn.code = PING_TRANSACTION;
    txn.data_size = 4;
    txn.buffer = payload_off;
    cmd = SWAP_DIR(AOSP_BC_TRANSACTION);
    memcpy(message + write_off, &cmd, 4);
    memcpy(message + write_off + 4, &txn, sizeof(txn));
    memcpy(message + payload_off, &payload, 4);
    memset(&bwr, 0, sizeof(bwr));
    bwr.write_size = 4 + sizeof(txn);
    bwr.write_buffer = write_off;
    bwr.read_size = read_size;
    bwr.read_buffer = read_off;
    memcpy(message, &bwr, sizeof(bwr));
    printf("ping bc_transaction=0x%08x write_off=%u payload_off=%u "
        "read_off=%u read_size=%u\n", cmd, write_off, payload_off, read_off,
        read_size);

    if (call_devctl(fd, "WRITE_READ_PING", RIM_BINDER_WRITE_READ, message,
            read_off + read_size) != 0) {
        close(fd);
        return "write_read_failed";
    }
    memcpy(&bwr, message, sizeof(bwr));
    printf("bwr write_consumed=%u read_consumed=%u\n", bwr.write_consumed,
        bwr.read_consumed);
    if (bwr.read_consumed > read_size) {
        bwr.read_consumed = read_size;
    }
    hexdump("read_stream", message + read_off, bwr.read_consumed);
    decode_read_stream(message + read_off, bwr.read_consumed, cfg_addr);

    /* A reply may arrive in a second read when the first only completes. */
    if (bwr.read_consumed <= 8) {
        memset(message, 0, sizeof(message));
        memset(&bwr, 0, sizeof(bwr));
        bwr.read_size = read_size;
        bwr.read_buffer = read_off;
        memcpy(message, &bwr, sizeof(bwr));
        if (call_devctl(fd, "WRITE_READ_REPLY", RIM_BINDER_WRITE_READ,
                message, read_off + read_size) == 0) {
            memcpy(&bwr, message, sizeof(bwr));
            printf("bwr2 read_consumed=%u\n", bwr.read_consumed);
            if (bwr.read_consumed > read_size) {
                bwr.read_consumed = read_size;
            }
            hexdump("read_stream2", message + read_off, bwr.read_consumed);
            decode_read_stream(message + read_off, bwr.read_consumed,
                cfg_addr);
        }
    }
    close(fd);
    return "ping_sent";
}

/* Same sequence as executable-memory.c, ARM mode: RW -> RX -> call. */
static const char *wx_probe(void)
{
    long page = sysconf(_SC_PAGESIZE);
    uint32_t *code;
    union { uintptr_t address; int (*function)(void); } call;
    int value;

    step_name = "wx_mmap";
    code = mmap(0, (size_t)page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) {
        printf("wx mmap errno=%d %s\n", errno, strerror(errno));
        return "mmap_failed";
    }
    code[0] = 0xe3a0002au;  /* mov r0, #42 */
    code[1] = 0xe12fff1eu;  /* bx lr */
    step_name = "wx_mprotect";
    if (mprotect(code, (size_t)page, PROT_READ | PROT_EXEC) != 0) {
        printf("wx mprotect_rx errno=%d %s\n", errno, strerror(errno));
        munmap(code, (size_t)page);
        return "mprotect_denied";
    }
    if (msync(code, 8, MS_INVALIDATE_ICACHE) != 0) {
        printf("wx msync errno=%d %s\n", errno, strerror(errno));
    }
    step_name = "wx_call";
    call.address = (uintptr_t)code;
    value = call.function();
    printf("wx call_value=%d\n", value);
    munmap(code, (size_t)page);
    return value == 42 ? "PASS" : "wrong_value";
}

int main(void)
{
    const char *binder;
    const char *wx;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(15);
    printf("probe=rim-binder-probe\n");
    report_identity();
    report_node("/dev/binder");
    report_node("/dev/android/epoll");
    binder = binder_probe();
    wx = wx_probe();
    printf("summary binder=%s wx=%s\n", binder, wx);
    return 0;
}
