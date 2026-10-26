/*
 * test_keydisp.c -- keydisp.c's shifted key, in the notation rbp used.
 */
#include <stdio.h>
#include <string.h>

#include "keydisp.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;           \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);      \
        printf("\n"); } } while (0)

void klog(const char *fmt, ...) { (void)fmt; }

static int is(const char *key, int semis, const unsigned short *want)
{
    unsigned short out[16];
    int n = keydisp_shifted(key, semis, out, 16);
    for (int i = 0; i <= n; i++)
        if (out[i] != want[i])
            return 0;
    return n > 0;
}

int main(void)
{
    unsigned short out[16];

    CHECK(is("8A", 1, (const unsigned short[]){ '3', 'A', 0 }), "8A +1 -> 3A (a semitone is 7 numbers)");
    CHECK(is("8A", -2, (const unsigned short[]){ '6', 'A', 0 }), "8A -2 -> 6A");
    CHECK(is("12B", 2, (const unsigned short[]){ '2', 'B', 0 }), "12B +2 -> 2B");
    CHECK(is("3A", -1, (const unsigned short[]){ '8', 'A', 0 }), "3A (B-flat m) -1 -> 8A (Am)");
    CHECK(is("5A", 1, (const unsigned short[]){ '1', '2', 'A', 0 }), "5A +1 -> 12A, two digits");
    CHECK(is("Am", 2, (const unsigned short[]){ 'B', 'm', 0 }), "Am +2 -> Bm");
    CHECK(is("Am", 1, (const unsigned short[]){ 'B', 0x266D, 'm', 0 }), "Am +1 -> B-flat m, rbp's flat");
    CHECK(is("F", 1, (const unsigned short[]){ 'F', 0x266F, 0 }), "F +1 -> F-sharp, rbp's sharp");
    CHECK(is("Abm", -1, (const unsigned short[]){ 'G', 'm', 0 }), "A-flat m -1 -> Gm");
    CHECK(keydisp_shifted("", 1, out, 16) == 0 && keydisp_shifted("XY", 1, out, 16) == 0,
          "not a key: nothing");
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
