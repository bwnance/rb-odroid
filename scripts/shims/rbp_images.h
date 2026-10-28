/* SPDX-License-Identifier: MPL-2.0
 *
 * rbp_images.h -- recoloured copies of rbp's own images, under private IDs
 * (rbp_images.c).
 */
#ifndef RBPI4B_RBP_IMAGES_H
#define RBPI4B_RBP_IMAGES_H

#include <stdint.h>

/* The first private ID. rbp's NS_GetImageInfoByID answers 0..0x15cc only. */
#define RBP_IMAGES_FIRST 0x1700u
#define RBP_IMAGES_LIMIT 24u

/* rbp's image `source` with every pixel on `from`'s RGB565 ray (its antialiased
 * shades included) moved onto `to`'s. Answers the private ID, or 0. Register
 * before rbp_images_install(); nothing is ever unregistered. */
unsigned int rbp_images_recolour(unsigned int source, uint16_t from, uint16_t to);

/* Detour NS_GetImageInfoByID @0x1d192c so the private IDs draw. 0 = in. */
int rbp_images_install(void);

/* One RGB565 pixel recoloured as above. Pure, for the test. */
uint16_t rbp_images_shade(uint16_t pixel, uint16_t from, uint16_t to);

/* The record for private `id`, built from the stock record `lookup` answers for its
 * source with the table at `base`; NULL for anything else. The hook's body. For the
 * test. */
void *rbp_images_resolve(unsigned int id, void *(*lookup)(unsigned int), const void *base);

#endif /* RBPI4B_RBP_IMAGES_H */
