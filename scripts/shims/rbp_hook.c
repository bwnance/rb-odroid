/* SPDX-License-Identifier: MPL-2.0
 *
 * rbp_hook.c -- a detour on one of rbp's functions (rbp_hook.h).
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f,
 * MPL-2.0), mod/modules/core/services/rx3_hooks.c's install_raw_hook: the
 * target's first two instructions go into a trampoline that jumps on to
 * target+8, and the target becomes `ldr pc,[pc,#-4]; .word replacement`. Used by
 * keyshift.c, searchfold.c, masteratt.c, blitfast.c and browserows.c.
 *
 * One addition to the toolkit's: a taken instruction that loads a literal
 * (`ldr rd,[pc,#imm]`) is moved, with its literal, into the trampoline -- rbp's
 * ui_ListBrowse_SetTextStartScrollTimer @0x2995c8 starts with one.
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

uint32_t rbp_hook_relocate(uint32_t insn, unsigned long at, uint32_t *literal)
{
    unsigned rd = (insn >> 12) & 15u, imm = insn & 0xfffu;
    unsigned long from;

    /* ldr<cond> rd,[pc,#+-imm]: P=1 W=0 B=0 L=1, rn=pc; rd=pc would be a jump */
    if ((insn & 0x0f7f0000u) != 0x051f0000u || rd == 15u || (insn >> 28) == 15u)
        return insn;
    from = at + 8u;
    from = (insn & (1u << 23)) ? from + imm : from - imm;
    *literal = *(const uint32_t *)from;
    /* the same load, from the trampoline's slot 8 bytes past the pc it runs at */
    return (insn & 0xf000f000u) | 0x059f0008u;
}

void *rbp_hook(unsigned long target, const uint8_t guard[8], void *replacement)
{
    uint32_t *tramp, patch[2], insn[2];

    if (memcmp((const void *)target, guard, 8))
        return NULL;
    tramp = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED)
        return NULL;
    memcpy(insn, (const void *)target, 8);
    /* slot i runs at tramp+4i, pc reads tramp+4i+8: its literal is tramp[4+i] */
    tramp[0] = rbp_hook_relocate(insn[0], target, &tramp[4]);
    tramp[1] = rbp_hook_relocate(insn[1], target + 4, &tramp[5]);
    tramp[2] = 0xe51ff004u;                          /* ldr pc,[pc,#-4] */
    tramp[3] = (uint32_t)(target + 8);
    __builtin___clear_cache((char *)tramp, (char *)tramp + 24);
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
