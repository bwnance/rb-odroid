/*
 * test_searchfold.c -- searchfold.c's fold: Latin-1 letters to plain upper-case ASCII.
 */
#include <stdint.h>
#include <stdio.h>

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;           \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);      \
        printf("\n"); } } while (0)

void searchfold_fold(uint16_t *text, unsigned n);
int is_rbp_process(void) { return 0; }
void klog(const char *fmt, ...) { (void)fmt; }
void *rbp_hook(unsigned long t, const uint8_t g[8], void *r) { (void)t; (void)g; (void)r; return 0; }

int main(void)
{
    uint16_t t[] = { 'N', 'I', 0xD1, 'O', ' ', 0xE9, 0xDF, 0xD7, 0x178, 'x', 0 };
    uint16_t u[] = { 0xE9, 0xE9, 0xE9 };

    searchfold_fold(t, 10);
    CHECK(t[2] == 'N' && t[5] == 'E' && t[6] == 'S' && t[7] == 0xD7 && t[8] == 'Y' &&
          t[0] == 'N' && t[9] == 'x', "NIÑO e-acute sharp-s times Y-diaeresis folded");
    searchfold_fold(u, 2);
    CHECK(u[0] == 'E' && u[1] == 'E' && u[2] == 0xE9, "only the first n characters");
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
