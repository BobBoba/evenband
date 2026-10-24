/* Loads the built plugin binary like a host, runs a sine through it and checks the
 * loudness ports and the reset counter. Usage: test_lv2 build/evenband.lv2/evenband.so */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#include "eb_ports.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { BLOCK = 512 };

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const LV2_Descriptor *(*get)(uint32_t);
    *(void **)(&get) = dlsym(lib, "lv2_descriptor");
    const LV2_Descriptor *d = get ? get(0) : NULL;
    if (!d) return 2;
    printf("[lv2 wrapper: loudness ports]\n");

    const double sr = 48000.0;
    float port[P_COUNT];
    float in[BLOCK], out_l[BLOCK], out_r[BLOCK];
    for (int i = 0; i < P_COUNT; i++) port[i] = eb_port_default[i];
    LV2_Handle h = d->instantiate(d, sr, ".", NULL);
    CHECK(h != NULL, "instantiate failed");
    if (!h) return 1;
    for (uint32_t i = 0; i < P_COUNT; i++) d->connect_port(h, i, &port[i]);
    d->connect_port(h, P_IN_L, in);
    d->connect_port(h, P_IN_R, in);
    d->connect_port(h, P_OUT_L, out_l);
    d->connect_port(h, P_OUT_R, out_r);
    port[P_BYPASS] = 1.0f;                       /* output = input: both meters must agree */
    d->activate(h);

    double phase = 0.0;
    for (long b = 0; b < (long)(5.0 * sr / BLOCK); b++) {
        for (int i = 0; i < BLOCK; i++) {
            in[i] = (float)(0.1 * sin(phase));   /* -20 dBFS peak in both channels */
            phase += 2.0 * M_PI * 1000.0 / sr;
        }
        d->run(h, BLOCK);
    }
    /* two channels at 0.1 peak: mean square 0.005 each, sum 0.01 = -20 dB; K-weighting gains +0.691 dB at 1 kHz,
     * which the -0.691 offset of BS.1770 cancels, so the reading is -20.0 LUFS */
    CHECK(fabsf(port[P_LUFS_IN_M] + 20.0f) < 0.15f, "input momentary %g, expected -20.0", (double)port[P_LUFS_IN_M]);
    CHECK(fabsf(port[P_LUFS_IN_S] + 20.0f) < 0.15f, "input short-term %g, expected -20.0", (double)port[P_LUFS_IN_S]);
    CHECK(fabsf(port[P_LUFS_IN_I] + 20.0f) < 0.15f, "input integrated %g, expected -20.0", (double)port[P_LUFS_IN_I]);
    CHECK(fabsf(port[P_LUFS_OUT_M] - port[P_LUFS_IN_M]) < 0.3f, "bypassed output %g differs from input %g", (double)port[P_LUFS_OUT_M], (double)port[P_LUFS_IN_M]);

    /* a counter change restarts integrated loudness (and only that) */
    port[P_METER_RESET] += 1.0f;
    memset(in, 0, sizeof in);
    d->run(h, BLOCK);
    CHECK(port[P_LUFS_IN_I] <= -119.0f, "integrated loudness not cleared by reset: %g", (double)port[P_LUFS_IN_I]);
    d->cleanup(h);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
