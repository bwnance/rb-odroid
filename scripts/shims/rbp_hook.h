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
 * taken must not be pc-relative -- every guard in this tree is checked for that
 * where it is defined. */
void *rbp_hook(unsigned long target, const uint8_t guard[8], void *replacement);

#endif /* RBPI4B_RBP_HOOK_H */
