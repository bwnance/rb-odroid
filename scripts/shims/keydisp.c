/*
 * keydisp.c -- rbp's deck panel shows the key a deck plays in (keydisp.h).
 */
#include "keydisp.h"

#include <string.h>

#include "shimutil.h"       /* klog() */

#define UI_KEYLINE_BASE   0x03268bf0ul   /* ui_Deck_Update's literal at 0x2909b8 */
#define UI_KEYLINE_STRIDE 0x238u
#define UI_KEYLINE_KIND   8              /* u16, 0 = no key line */
#define UI_KEYLINE_TEXT   34             /* UTF-16, NUL-terminated */
#define KEY_MAX           15

#define SHARP 0x266F
#define FLAT  0x266D

__attribute__((weak)) int keyshift_get(int deck);

/* Camelot index (num-1)*2 + (B ? 1 : 0) -> the classic name, rekordbox's spelling
 * (rx3-toolkit's rx3_camelot_classic table). */
static const char *const classic[24] = {
    "Abm", "B", "Ebm", "F#", "Bbm", "Db", "Fm", "Ab", "Cm", "Eb", "Gm", "Bb",
    "Dm", "F", "Am", "C", "Em", "G", "Bm", "D", "F#m", "A", "Dbm", "E"
};

/* Camelot number and letter of a key text, 0 when it is not one. */
static int camelot_of(const char *k, char *letter)
{
    static const int maj[12] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
    static const int min[12] = { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };
    static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A..G */
    int num, i, s;
    char c;

    if (!k || !k[0])
        return 0;
    if (k[0] >= '0' && k[0] <= '9') {
        num = k[0] - '0';
        i = 1;
        if (k[1] >= '0' && k[1] <= '9')
            num = num * 10 + (k[i++] - '0');
        if (k[i] == 'A' || k[i] == 'a')
            *letter = 'A';
        else if (k[i] == 'B' || k[i] == 'b')
            *letter = 'B';
        else
            return 0;
        return num >= 1 && num <= 12 ? num : 0;
    }
    c = k[0];
    if (c >= 'a' && c <= 'g')
        c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'G')
        return 0;
    s = semis[c - 'A'];
    i = 1;
    if (k[i] == '#') { s++; i++; }
    else if (k[i] == 'b') { s += 11; i++; }
    s %= 12;
    if (k[i] == 'm' || k[i] == 'M') {
        *letter = 'A';
        return min[s];
    }
    *letter = 'B';
    return maj[s];
}

int keydisp_shifted(const char *key, int semis, unsigned short *out, int max)
{
    char letter, buf[8];
    int num = camelot_of(key, &letter), n = 0;
    const char *t;

    if (!num || max < 6)
        return 0;
    num = ((num - 1 + 7 * semis) % 12 + 12) % 12 + 1;   /* a semitone is 7 numbers */
    if (key[0] >= '0' && key[0] <= '9') {
        buf[0] = (char)(num >= 10 ? '1' : '0' + num);
        n = num >= 10 ? 2 : 1;
        if (num >= 10)
            buf[1] = (char)('0' + num - 10);
        buf[n++] = letter;
        buf[n] = '\0';
        t = buf;
    } else {
        t = classic[(num - 1) * 2 + (letter == 'B')];
    }
    for (n = 0; t[n] && n < max - 1; n++)
        out[n] = t[n] == '#' ? SHARP : (n == 1 && t[n] == 'b') ? FLAT : (unsigned short)t[n];
    out[n] = 0;
    return n;
}

static int u16len(const unsigned short *s, int max)
{
    int n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static int u16eq(const unsigned short *a, const unsigned short *b)
{
    int i;
    for (i = 0; a[i] && a[i] == b[i]; i++)
        ;
    return a[i] == b[i];
}

static void u16put(unsigned short *dst, const unsigned short *src)
{
    int n = u16len(src, KEY_MAX);
    dst[n] = 0;                                  /* never longer than what is there */
    memcpy(dst, src, (size_t)n * 2);
}

static void ascii_of(const unsigned short *w, char *out, int max)
{
    int k;
    for (k = 0; k < max - 1 && w[k]; k++)
        out[k] = w[k] == SHARP ? '#' : w[k] == FLAT ? 'b' :
                 (w[k] >= 0x20 && w[k] < 0x7f) ? (char)w[k] : '?';
    out[k] = '\0';
}

static unsigned short orig[2][KEY_MAX + 1];     /* rbp's own text for the track */
static unsigned short ours[2][KEY_MAX + 1];     /* what we last wrote, "" = nothing */
static int last_total[2] = { -1, -1 };

void keydisp_deck(int deck, int total_ms, char *out, int max)
{
    unsigned char *line;
    unsigned short *text, want[KEY_MAX + 1];
    char key[KEY_MAX + 1];
    int shift;

    if (deck < 0 || deck > 1) {
        out[0] = '\0';
        return;
    }
    line = (unsigned char *)(UI_KEYLINE_BASE + (unsigned long)deck * UI_KEYLINE_STRIDE);
    text = (unsigned short *)(line + UI_KEYLINE_TEXT);
    if (!*(unsigned short *)(line + UI_KEYLINE_KIND) || u16len(text, KEY_MAX + 1) > KEY_MAX) {
        orig[deck][0] = ours[deck][0] = 0;
        out[0] = '\0';
        return;
    }
    /* rbp's text, unless it is the one we wrote on this same track */
    if (total_ms != last_total[deck] || !ours[deck][0] || !u16eq(text, ours[deck])) {
        u16put(orig[deck], text);
        ours[deck][0] = 0;
    }
    last_total[deck] = total_ms;
    ascii_of(orig[deck], key, sizeof key);
    shift = keyshift_get ? keyshift_get(deck) : 0;
    if (shift && keydisp_shifted(key, shift, want, KEY_MAX + 1)) {
        if (!u16eq(text, want)) {
            u16put(text, want);
            u16put(ours[deck], want);
        }
    } else if (ours[deck][0]) {
        u16put(text, orig[deck]);              /* the shift went: the track's own key */
        ours[deck][0] = 0;
    }
    ascii_of(orig[deck], out, max);
}
