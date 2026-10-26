/* SPDX-License-Identifier: MPL-2.0
 *
 * rbp_hook.c -- a detour on one of rbp's functions (rbp_hook.h).
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f,
 * MPL-2.0), mod/modules/core/services/rx3_hooks.c's install_raw_hook: the
 * target's first two instructions go into a trampoline that jumps on to
 * target+8, and the target becomes `ldr pc,[pc,#-4]; .word replacement`. Used by
 * keyshift.c and searchfold.c.
 */
#include "rbp_hook.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int write_code(unsigned long addr, const void *src, size_t n)
{
    long pg = sysconf(_SC_PAGESIZE);
    unsigned long page = addr & ~((unsigned long)pg - 1);
    size_t span = (addr + n) - page;

    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC))
        return -1;
    memcpy((void *)addr, src, n);
    __builtin___clear_cache((char *)addr, (char *)addr + n);
    mprotect((void *)page, span, PROT_READ | PROT_EXEC);
    return 0;
}

void *rbp_hook(unsigned long target, const uint8_t guard[8], void *replacement)
{
    uint32_t *tramp, patch[2];

    if (memcmp((const void *)target, guard, 8))
        return NULL;
    tramp = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED)
        return NULL;
    memcpy(tramp, (const void *)target, 8);
    tramp[2] = 0xe51ff004u;                          /* ldr pc,[pc,#-4] */
    tramp[3] = (uint32_t)(target + 8);
    __builtin___clear_cache((char *)tramp, (char *)tramp + 16);
    if (mprotect(tramp, 4096, PROT_READ | PROT_EXEC)) {
        munmap(tramp, 4096);
        return NULL;
    }
    patch[0] = 0xe51ff004u;
    patch[1] = (uint32_t)(uintptr_t)replacement;
    if (write_code(target, patch, sizeof patch)) {
        munmap(tramp, 4096);
        return NULL;
    }
    return tramp;
}
