/*
 * test_masteratt.c -- host checks for masteratt.c: rbp's MASTER ATT. setting as
 * the gain the master output gets: 0 = 0 dB, 1 = -6 dB, 2 = -12 dB (by ear on
 * the unit; rbp's UTILITY row lists its labels the other way round).
 *
 *   make test_masteratt && ./test_masteratt
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

volatile float g_master_att = 1.0f;
void *rbp_hook(unsigned long t, const uint8_t g[8], void *r) { (void)t; (void)g; (void)r; return NULL; }
int is_rbp_process(void) { return 0; }
void klog(const char *fmt, ...) { (void)fmt; }

float masteratt_gain(int att);

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
     printf(__VA_ARGS__); printf("\n"); } } while (0)

static double db(float g) { return 20.0 * log10((double)g); }

int main(void)
{
     CHECK(masteratt_gain(0) == 1.0f, "0 is 0 dB, untouched");
     CHECK(fabs(db(masteratt_gain(1)) + 6.0) < 0.01, "1 is -6 dB (%.3f)", db(masteratt_gain(1)));
     CHECK(fabs(db(masteratt_gain(2)) + 12.0) < 0.01, "2 is -12 dB (%.3f)", db(masteratt_gain(2)));
     CHECK(masteratt_gain(7) == 1.0f && masteratt_gain(-1) == 1.0f,
           "a value rbp never labels: untouched, never silence");
     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
