/* SPDX-License-Identifier: MPL-2.0
 *
 * rbp_images.c -- recoloured copies of rbp's own images (rbp_images.h).
 *
 * Adapted from rx3-toolkit (github.com/Tratosca/rx3-toolkit at cc17a1f,
 * MPL-2.0): mod/modules/core/services/rx3_images.c (recolour, register_recolour,
 * rx3_image_resolve) and the image-info hook of mod/modules/core/rx3_core_hook.c.
 * The toolkit's bitmaps, light theme and native-record replacements are not
 * here; nothing registered is ever taken back. This file stays under the MPL.
 *
 * rbp 1.20 (sha1 3513d34b...), read 2026-10-27: NS_GetImageInfoByID @0x1d192c
 * takes an image ID, answers NULL above 0x15cc, else the 44-byte record at
 * *0x05a14f60 + 44*id, after writing into it the absolute pixel pointer (+0x0c),
 * the stride (+0x10) and the palette pointer (+0x14). Every image rbp draws --
 * NS_PALRender_DrawImage, NS_GlyphImage_SetImageID, 30-odd more -- asks it, so
 * an ID above 0x15cc that it answers is drawable everywhere. Private IDs start at
 * 0x1700, clear of the stock table and of the toolkit's tab-strip IDs.
 */
#include "rbp_images.h"

#include <string.h>

#include "rbp_hook.h"

#define IMAGE_INFO 0x001d192cul
/* movw r3,#0x15cc ; cmp r0,r3 -- nothing pc-relative */
static const uint8_t image_info_guard[8] = { 0xcc, 0x35, 0x01, 0xe3, 0x03, 0x00, 0x50, 0xe1 };
#define IMAGE_TABLE_POINTER 0x05a14f60ul
#define IMAGE_PIXELS 1024u

struct variant {
    unsigned int used, source, ready;
    const void *source_base;
    uint32_t source_offset;
    uint16_t source_width, source_height;
    uint16_t from, to, pixels[IMAGE_PIXELS];
    uint32_t record[11];
};
static struct variant variants[RBP_IMAGES_LIMIT];

typedef void *(*image_info_fn)(unsigned int);
static image_info_fn original_image_info;

/* Preserve antialiasing shades along the source colour's RGB565 ray. */
uint16_t rbp_images_shade(uint16_t pixel, uint16_t from, uint16_t to)
{
    unsigned r = pixel >> 11, g = (pixel >> 5) & 63, b = pixel & 31;
    unsigned fr = from >> 11, fg = (from >> 5) & 63, fb = from & 31;
    if (r * fg != g * fr || r * fb != b * fr || g * fb != b * fg)
        return pixel;
    unsigned scale = fr ? fr : (fg ? fg : fb), amount = fr ? r : (fg ? g : b);
    if (!scale || amount > scale)
        return pixel == from ? to : pixel;
    return (uint16_t)((((to >> 11) * amount + scale / 2) / scale) << 11 |
                      (((((to >> 5) & 63) * amount + scale / 2) / scale) << 5) |
                      (((to & 31) * amount + scale / 2) / scale));
}

unsigned int rbp_images_recolour(unsigned int source, uint16_t from, uint16_t to)
{
    if (source >= RBP_IMAGES_FIRST || from == to)
        return 0;
    for (unsigned int i = 0; i < RBP_IMAGES_LIMIT; i++) {
        struct variant *v = &variants[i];
        if (v->used)
            continue;
        v->source = source;
        v->from = from;
        v->to = to;
        v->ready = 0;
        v->used = 1;
        return RBP_IMAGES_FIRST + i;
    }
    return 0;
}

/* On rbp's render thread, which draws one image at a time. Offsets always use the
 * current table base. */
void *rbp_images_resolve(unsigned int id, void *(*lookup)(unsigned int), const void *base)
{
    if (id < RBP_IMAGES_FIRST || id >= RBP_IMAGES_FIRST + RBP_IMAGES_LIMIT || !lookup || !base)
        return NULL;
    struct variant *v = &variants[id - RBP_IMAGES_FIRST];
    if (!v->used)
        return NULL;
    const uint8_t *source = lookup(v->source);
    if (!source)
        return NULL;
    uint16_t width, height;
    uint32_t offset;
    memcpy(&width, source + 4, 2);
    memcpy(&height, source + 6, 2);
    unsigned int count = (unsigned int)width * height;
    /* RGB565, keyed or not, unpaletted -- the list icons are */
    if (!width || !height || count > IMAGE_PIXELS || source[0x19] ||
        (source[0x18] != 1 && source[0x18] != 2))
        return NULL;
    memcpy(v->record, source, 44);
    memcpy(&offset, source + 0x20, 4);
    if (!v->ready || v->source_base != base || v->source_offset != offset ||
        v->source_width != width || v->source_height != height) {
        const uint16_t *pixels = (const void *)((unsigned long)base + offset);
        for (unsigned int i = 0; i < count; i++)
            v->pixels[i] = rbp_images_shade(pixels[i], v->from, v->to);
        v->source_base = base;
        v->source_offset = offset;
        v->source_width = width;
        v->source_height = height;
        v->ready = 1;
    }
    offset = (uint32_t)((unsigned long)v->pixels - (unsigned long)base);
    memcpy((uint8_t *)v->record + 0x20, &offset, 4);
    /* lookup() already resolved the stock record's absolute pixel pointer; ours
     * bypasses that fixup, so resolve it too. */
    uint32_t pixels = (uint32_t)(unsigned long)v->pixels;
    memcpy((uint8_t *)v->record + 0x0c, &pixels, 4);
    return v->record;
}

static void *hooked_image_info(unsigned int id)
{
    if (id >= RBP_IMAGES_FIRST) {
        void *record = rbp_images_resolve(id, original_image_info,
                                          *(void **)IMAGE_TABLE_POINTER);
        if (record)
            return record;
        /* a source the recolour cannot take is drawn as it is, not dropped */
        if (id < RBP_IMAGES_FIRST + RBP_IMAGES_LIMIT && variants[id - RBP_IMAGES_FIRST].used)
            return original_image_info(variants[id - RBP_IMAGES_FIRST].source);
    }
    return original_image_info(id);
}

int rbp_images_install(void)
{
    if (original_image_info)
        return 0;
    original_image_info = (image_info_fn)rbp_hook(IMAGE_INFO, image_info_guard,
                                                  (void *)hooked_image_info);
    return original_image_info ? 0 : -1;
}
