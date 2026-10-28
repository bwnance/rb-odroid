/* SPDX-License-Identifier: MPL-2.0
 *
 * rbp_hook.h -- a detour on one of rbp's functions (rbp_hook.c).
 */
#ifndef RBPI4B_RBP_HOOK_H
#define RBPI4B_RBP_HOOK_H

#include <stdint.h>

/* Replace the function at `target` with `replacement`, if its first eight bytes
 * are `guard`; answers a pointer that calls the original (its first two
 * instructions, then a jump back to target+8), or NULL. The two instructions
 * taken must not be pc-relative, except a literal load (`ldr rd,[pc,#imm]`),
 * which is moved with its literal -- every guard in this tree is checked for that
 * where it is defined. */
void *rbp_hook(unsigned long target, const uint8_t guard[8], void *replacement);

/* One taken instruction `insn`, which sat at `at`, as the trampoline runs it: a
 * literal load becomes the same load from 8 bytes on, with the value it would have
 * read put in `*literal`; anything else is answered as it is. For the test. */
uint32_t rbp_hook_relocate(uint32_t insn, unsigned long at, uint32_t *literal);

#endif /* RBPI4B_RBP_HOOK_H */
