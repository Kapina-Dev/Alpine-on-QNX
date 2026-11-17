#include "linux_diagnostics.h"
#include "linux_syscall_names.h"

#include <string.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

struct diag_entry {
    uint32_t number;
    int32_t error;
    const char *detail;
    uint32_t count;
    uint32_t args[6];
};

static struct diag_entry entries[LINUX_DIAG_MAX_ENTRIES];
static volatile uint32_t entry_count;
static volatile uint32_t dropped;
static volatile int table_lock;
static volatile int enabled;

static const struct {
    int32_t number;
    const char *name;
} errno_names[] = {
    { 1, "EPERM" }, { 2, "ENOENT" }, { 3, "ESRCH" }, { 4, "EINTR" },
    { 5, "EIO" }, { 9, "EBADF" }, { 11, "EAGAIN" }, { 12, "ENOMEM" },
    { 13, "EACCES" }, { 14, "EFAULT" }, { 17, "EEXIST" },
    { 20, "ENOTDIR" }, { 22, "EINVAL" }, { 25, "ENOTTY" },
    { 28, "ENOSPC" }, { 29, "ESPIPE" }, { 34, "ERANGE" },
    { 38, "ENOSYS" }, { 95, "EOPNOTSUPP" }, { 97, "EAFNOSUPPORT" },
};

const char *linux_syscall_name(uint32_t number)
{
    size_t low = 0;
    size_t high = ARRAY_LENGTH(linux_syscall_names);

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (linux_syscall_names[middle].number == number)
            return linux_syscall_names[middle].name;
        if (linux_syscall_names[middle].number < number) low = middle + 1;
        else high = middle;
    }
    return "unknown";
}

const char *linux_errno_name(int32_t linux_error)
{
    size_t i;

    for (i = 0; i < ARRAY_LENGTH(errno_names); ++i)
        if (errno_names[i].number == linux_error) return errno_names[i].name;
    return 0;
}

static void lock_table(void)
{
    while (__sync_lock_test_and_set(&table_lock, 1) != 0) {
    }
}

static void unlock_table(void)
{
    __sync_lock_release(&table_lock);
}

static int same_detail(const char *left, const char *right)
{
    if (left == 0 || right == 0) return left == right;
    return strcmp(left, right) == 0;
}

void linux_diag_record(uint32_t number, int32_t linux_error,
    const char *detail, const uint32_t args[6])
{
    uint32_t i;

    if (!enabled) return;
    lock_table();
    for (i = 0; i < entry_count; ++i) {
        if (entries[i].number == number && entries[i].error == linux_error &&
            same_detail(entries[i].detail, detail)) {
            entries[i].count++;
            unlock_table();
            return;
        }
    }
    if (entry_count == LINUX_DIAG_MAX_ENTRIES) {
        dropped++;
        unlock_table();
        return;
    }
    entries[entry_count].number = number;
    entries[entry_count].error = linux_error;
    entries[entry_count].detail = detail;
    entries[entry_count].count = 1;
    memcpy(entries[entry_count].args, args, sizeof(entries[0].args));
    __sync_synchronize();
    entry_count++;
    unlock_table();
}

void linux_diag_reset(void)
{
    lock_table();
    entry_count = 0;
    dropped = 0;
    unlock_table();
}

void linux_diag_set_enabled(int value)
{
    enabled = value != 0;
}

void linux_diag_after_fork(void)
{
    entry_count = 0;
    dropped = 0;
    __sync_lock_release(&table_lock);
}

/* A bounded line builder: appends fail once the line would not fit. */
struct line {
    char *text;
    size_t length;
    size_t capacity;
    int full;
};

static void put_text(struct line *line, const char *text)
{
    size_t length = strlen(text);

    if (line->full || length > line->capacity - line->length) {
        line->full = 1;
        return;
    }
    memcpy(line->text + line->length, text, length);
    line->length += length;
}

static void put_char(struct line *line, char character)
{
    char text[2];

    text[0] = character;
    text[1] = '\0';
    put_text(line, text);
}

static void put_decimal(struct line *line, uint32_t value)
{
    char digits[11];
    size_t position = sizeof(digits) - 1;

    digits[position] = '\0';
    do {
        digits[--position] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    put_text(line, digits + position);
}

static void put_signed(struct line *line, int32_t value)
{
    if (value < 0) {
        put_char(line, '-');
        put_decimal(line, (uint32_t)0 - (uint32_t)value);
    } else {
        put_decimal(line, (uint32_t)value);
    }
}

static void put_hex32(struct line *line, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    char text[11];
    int shift;
    size_t position = 2;

    text[0] = '0';
    text[1] = 'x';
    for (shift = 28; shift >= 0; shift -= 4)
        text[position++] = hex[(value >> shift) & 15u];
    text[position] = '\0';
    put_text(line, text);
}

/* Percent-encode bytes that would break one-token-per-field parsing. */
static void put_encoded(struct line *line, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *byte;

    for (byte = (const unsigned char *)text; *byte != '\0'; ++byte) {
        if (*byte <= 0x20u || *byte == '%' || *byte >= 0x7fu) {
            put_char(line, '%');
            put_char(line, hex[*byte >> 4]);
            put_char(line, hex[*byte & 15u]);
        } else {
            put_char(line, (char)*byte);
        }
    }
}

static void put_prefix(struct line *line, const char *tag,
    const char *executable, int32_t pid)
{
    put_text(line, tag);
    put_text(line, " exe=");
    put_encoded(line, executable);
    put_text(line, " pid=");
    put_signed(line, pid);
}

size_t linux_diag_format(char *buffer, size_t capacity,
    const char *executable, int32_t pid, uint32_t *cursor)
{
    size_t used = 0;
    uint32_t count = entry_count;

    __sync_synchronize();
    while (*cursor <= count) {
        uint32_t i = *cursor;
        struct line line;

        line.text = buffer + used;
        line.length = 0;
        line.capacity = capacity - used;
        line.full = 0;
        if (i < count) {
            const struct diag_entry *entry = &entries[i];
            const char *error_name = linux_errno_name(entry->error);
            int arg;

            put_prefix(&line, "linuxemu-unsupported", executable, pid);
            put_text(&line, " nr=");
            put_decimal(&line, entry->number);
            put_text(&line, " name=");
            put_text(&line, linux_syscall_name(entry->number));
            put_text(&line, " error=");
            if (error_name != 0) {
                put_text(&line, error_name);
            } else {
                put_char(&line, 'E');
                put_signed(&line, entry->error);
            }
            put_text(&line, " count=");
            put_decimal(&line, entry->count);
            put_text(&line, " detail=");
            put_text(&line, entry->detail != 0 ? entry->detail : "-");
            put_text(&line, " args=");
            for (arg = 0; arg < 6; ++arg) {
                if (arg != 0) put_char(&line, ',');
                put_hex32(&line, entry->args[arg]);
            }
        } else {
            if (dropped == 0) {
                *cursor = count + 1;
                break;
            }
            put_prefix(&line, "linuxemu-unsupported-overflow", executable,
                pid);
            put_text(&line, " dropped=");
            put_decimal(&line, dropped);
        }
        put_char(&line, '\n');
        if (line.full) {
            /* Leave it for the next call, unless it cannot fit even in an
               empty buffer: then skip it rather than stall forever. */
            if (used != 0) break;
            (*cursor)++;
            continue;
        }
        used += line.length;
        (*cursor)++;
    }
    return used;
}
