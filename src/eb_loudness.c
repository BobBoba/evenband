#include "eb_loudness.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SUB_SECONDS 0.05              /* energy is collected in 50 ms pieces */
#define MOMENTARY_SUBS 8              /* 400 ms */
#define SHORT_SUBS 60                 /* 3 s */
#define HIST_BINS 1000
#define HIST_LO (-70.0)               /* absolute gate */
#define HIST_HI 10.0
#define ABS_GATE (-70.0)
#define REL_GATE_LU 10.0
#define OFFSET (-0.691)

typedef struct {
    double b0, b1, b2, a1, a2, z1, z2;
} biquad;

struct eb_loud {
    uint32_t sub_len, sub_pos;
    double acc;
    biquad k[2][2];                   /* [channel][shelf, high-pass] */
    double ring[SHORT_SUBS];
    int ring_pos, ring_n;
    uint64_t subs;
    double hist_e[HIST_BINS];
    uint32_t hist_n[HIST_BINS];
};

static inline double run(biquad *f, double x)
{
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static double to_lufs(double energy)
{
    if (energy <= 1e-12) return (double)EB_LOUD_NONE;
    double l = OFFSET + 10.0 * log10(energy);
    return l < (double)EB_LOUD_NONE ? (double)EB_LOUD_NONE : l;
}

/* The two K-weighting stages of BS.1770, for any sample rate (same constants as libebur128). */
static void design(biquad out[2], double sr)
{
    const double pi = 3.14159265358979323846;
    {
        double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        double k = tan(pi * f0 / sr), vh = pow(10.0, g / 20.0), vb = pow(vh, 0.4996667741545416);
        double a0 = 1.0 + k / q + k * k;
        out[0] = (biquad){(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                          2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0, 0, 0};
    }
    {
        double f0 = 38.13547087602444, q = 0.5003270373238773;
        double k = tan(pi * f0 / sr);
        double a0 = 1.0 + k / q + k * k;
        out[1] = (biquad){1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0, 0, 0};
    }
}

eb_loud *eb_loud_create(double sr)
{
    eb_loud *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->sub_len = (uint32_t)(SUB_SECONDS * sr + 0.5);
    if (m->sub_len < 1) m->sub_len = 1;
    for (int c = 0; c < 2; c++) design(m->k[c], sr);
    return m;
}

void eb_loud_free(eb_loud *m) { free(m); }

void eb_loud_reset(eb_loud *m)
{
    m->acc = 0.0;
    m->sub_pos = 0;
    m->ring_pos = m->ring_n = 0;
    m->subs = 0;
    memset(m->ring, 0, sizeof m->ring);
    memset(m->hist_e, 0, sizeof m->hist_e);
    memset(m->hist_n, 0, sizeof m->hist_n);
}

static double mean_last(const eb_loud *m, int want)
{
    int n = m->ring_n < want ? m->ring_n : want;
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += m->ring[(m->ring_pos - 1 - i + SHORT_SUBS) % SHORT_SUBS];
    return sum / n;
}

void eb_loud_push(eb_loud *m, double left, double right)
{
    if (!(fabs(left) < 1e6)) left = 0.0;
    if (!(fabs(right) < 1e6)) right = 0.0;
    double l = run(&m->k[0][1], run(&m->k[0][0], left));
    double r = run(&m->k[1][1], run(&m->k[1][0], right));
    m->acc += l * l + r * r;
    if (++m->sub_pos < m->sub_len) return;

    m->ring[m->ring_pos] = m->acc / (double)m->sub_len;
    m->ring_pos = (m->ring_pos + 1) % SHORT_SUBS;
    if (m->ring_n < SHORT_SUBS) m->ring_n++;
    m->acc = 0.0;
    m->sub_pos = 0;
    m->subs++;

    /* 400 ms gating blocks every 100 ms (75 % overlap) */
    if ((m->subs & 1) == 0 && m->ring_n >= MOMENTARY_SUBS) {
        double e = mean_last(m, MOMENTARY_SUBS), lu = to_lufs(e);
        if (lu > ABS_GATE) {
            int bin = (int)((lu - HIST_LO) / (HIST_HI - HIST_LO) * HIST_BINS);
            if (bin < 0) bin = 0;
            if (bin >= HIST_BINS) bin = HIST_BINS - 1;
            m->hist_e[bin] += e;
            m->hist_n[bin]++;
        }
    }
}

void eb_loud_get(const eb_loud *m, float *momentary, float *shortterm, float *integrated)
{
    if (momentary) *momentary = (float)to_lufs(mean_last(m, MOMENTARY_SUBS));
    if (shortterm) *shortterm = (float)to_lufs(mean_last(m, SHORT_SUBS));
    if (!integrated) return;

    double sum_e = 0.0;
    uint64_t sum_n = 0;
    for (int b = 0; b < HIST_BINS; b++) {
        sum_e += m->hist_e[b];
        sum_n += m->hist_n[b];
    }
    if (sum_n == 0) {
        *integrated = EB_LOUD_NONE;
        return;
    }
    double gate = to_lufs(sum_e / (double)sum_n) - REL_GATE_LU;
    double e = 0.0;
    uint64_t n = 0;
    for (int b = 0; b < HIST_BINS; b++) {
        if (!m->hist_n[b]) continue;
        if (to_lufs(m->hist_e[b] / m->hist_n[b]) > gate) {
            e += m->hist_e[b];
            n += m->hist_n[b];
        }
    }
    *integrated = n ? (float)to_lufs(e / (double)n) : EB_LOUD_NONE;
}
