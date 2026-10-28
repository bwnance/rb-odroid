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
float headroom_gain_of(int db);
float trim_curve_entry(const float *c, int i);

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
     CHECK(headroom_gain_of(0) == 1.0f && headroom_gain_of(3) == 1.0f, "HEADROOM 0 dB: untouched");
     CHECK(fabs(db(headroom_gain_of(-12)) + 12.0) < 0.01, "HEADROOM -12 dB (%.3f)", db(headroom_gain_of(-12)));
     CHECK(fabs(db(headroom_gain_of(-3)) + 3.0) < 0.01, "HEADROOM -3 dB");
     {    /* rbp's trim curve's measured points: 0, -30.27 dB at 32, -8.71 at 128, 0 at 254/255 */
          static float c[256];
          for (int i = 0; i < 256; i++)
               c[i] = i == 0 ? 0.0f : i >= 254 ? 1.0f : i <= 128 ? 0.36706f * (float)i / 128.0f
                    : powf(10.0f, (-8.71f + 8.71f * (float)(i - 128) / 126.0f) / 20.0f);
          c[128] = 0.36706f;
          CHECK(trim_curve_entry(c, 0) == 0.0f, "TRIM: far left stays silent");
          CHECK(fabs(db(trim_curve_entry(c, 128))) < 0.01, "TRIM: the centre is 0 dB (%.3f)",
                db(trim_curve_entry(c, 128)));
          CHECK(fabs(db(trim_curve_entry(c, 255)) - 9.0) < 0.01, "TRIM: far right is +9 dB (%.3f)",
                db(trim_curve_entry(c, 255)));
          CHECK(fabs(db(trim_curve_entry(c, 64)) - (db(c[64]) + 8.71)) < 0.02,
                "TRIM: the left half is rbp's lifted by 8.71 dB");
          int up = 1;
          for (int i = 1; i < 256; i++)
               up &= trim_curve_entry(c, i) >= trim_curve_entry(c, i - 1);
          CHECK(up, "TRIM: never falls as the knob turns right");
     }
     printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
     return failures ? 1 : 0;
}
