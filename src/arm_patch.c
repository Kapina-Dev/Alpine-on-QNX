#include "linuxemu.h"

#include <errno.h>

#define ARM_LINUX_SVC_0 0xef000000u
#define ARM_TPIDRURO_MASK 0xffff0fffu
#define ARM_TPIDRURO_READ 0xee1d0f70u
#define ARM_UDF_TRAP 0xe7f000f0u

struct patch_record {
    uintptr_t address;
    uint32_t original;
};

static struct patch_record patches[MAX_PATCHES];
static size_t patch_count;

int arm_patch_range(uintptr_t start, size_t length)
{
    uintptr_t address;
    uintptr_t end;

    if ((start & 3u) != 0 || (length & 3u) != 0 ||
        length > UINTPTR_MAX - start) {
        errno = ENOEXEC;
        return -1;
    }
    end = start + length;
    for (address = start; address < end; address += 4) {
        uint32_t *instruction = (uint32_t *)address;
        uint32_t word = *instruction;

        if (word != ARM_LINUX_SVC_0 &&
            (word & ARM_TPIDRURO_MASK) != ARM_TPIDRURO_READ) {
            continue;
        }
        if (patch_count == ARRAY_COUNT(patches)) {
            errno = E2BIG;
            return -1;
        }
        patches[patch_count].address = address;
        patches[patch_count].original = word;
        patch_count++;
        *instruction = ARM_UDF_TRAP;
    }
    return 0;
}

const uint32_t *arm_patch_original(uintptr_t address)
{
    size_t i;

    for (i = 0; i < patch_count; ++i) {
        if (patches[i].address == address) {
            return &patches[i].original;
        }
    }
    return 0;
}

size_t arm_patch_count(void)
{
    return patch_count;
}
