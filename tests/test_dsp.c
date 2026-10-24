/* Measurement tests for the Evenband DSP core.
 * Signals are synthesised and analysed independently of the plugin
 * (own FFT, own band edges), at 48 kHz and 192 kHz. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/eb_dsp.h"

#define PI 3.14159265358979323846

static int failures = 0, checks = 0;
static int g_xtype = 0; /* crossover type under test */

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL: ");                           \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* ---- signal helpers ---------------------------------------------------- */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static double urand(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (double)((rng_state * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}

static void fft(double *re, double *im, size_t n, int inverse)
{
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * PI / (double)len * (inverse ? 1.0 : -1.0);
        double wr = cos(ang), wi = sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; k++) {
                size_t a = i + k, b = i + k + len / 2;
                double tr = re[b] * cr - im[b] * ci;
                double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static size_t pow2_at_least(double v)
{
    size_t n = 1;
    while ((double)n < v) n <<= 1;
    return n;
}

/* Noise with power spectral density proportional to f^-alpha between 20 Hz
 * and 20 kHz (alpha 1 = pink), scaled to the given RMS. Periodic in n. */
static float *make_noise(size_t n, double sr, double alpha, double rms)
{
    double *re = calloc(n, sizeof *re), *im = calloc(n, sizeof *im);
    float *out = malloc(n * sizeof *out);
    for (size_t k = 1; k < n / 2; k++) {
        double f = (double)k * sr / (double)n;
        if (f < 20.0 || f > 20000.0) continue;
        double mag = pow(f, -alpha / 2.0);
        double ph = 2.0 * PI * urand();
        re[k] = mag * cos(ph); im[k] = mag * sin(ph);
        re[n - k] = re[k]; im[n - k] = -im[k];
    }
    fft(re, im, n, 1);
    double e = 0.0;
    for (size_t i = 0; i < n; i++) e += re[i] * re[i];
    double scale = rms / sqrt(e / (double)n);
    for (size_t i = 0; i < n; i++) out[i] = (float)(re[i] * scale);
    free(re); free(im);
    return out;
}

/* Power of x[start..start+m) in each of five bands, as dB relative to the
 * total inside 20 Hz..20 kHz. Bands are defined by edges[0..5]. */
static void band_fractions(const float *x, size_t start, size_t m, double sr,
                           const double edges[6], double out_db[5])
{
    double *re = malloc(m * sizeof *re), *im = calloc(m, sizeof *im);
    for (size_t i = 0; i < m; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * PI * (double)i / (double)m);
        re[i] = (double)x[start + i] * w;
    }
    fft(re, im, m, 0);
    double p[5] = {0}, total = 0.0;
    for (size_t k = 1; k < m / 2; k++) {
        double f = (double)k * sr / (double)m;
        double pw = re[k] * re[k] + im[k] * im[k];
        for (int b = 0; b < 5; b++) {
            if (f >= edges[b] && f < edges[b + 1]) { p[b] += pw; total += pw; break; }
        }
    }
    for (int b = 0; b < 5; b++) out_db[b] = 10.0 * log10(p[b] / total);
    free(re); free(im);
}

static double rms_db(const float *x, size_t start, size_t m)
{
    double e = 0.0;
    for (size_t i = 0; i < m; i++) e += (double)x[start + i] * (double)x[start + i];
    return 10.0 * log10(e / (double)m + 1e-30);
}

static void run_all(eb_state *s, const eb_params *p, const float *in, float *out, size_t n)
{
    /* Same signal on both channels. */
    const size_t block = 512;
    for (size_t i = 0; i < n; i += block) {
        uint32_t c = (uint32_t)(n - i < block ? n - i : block);
        eb_process(s, p, in + i, in + i, out + i, out + i, c);
    }
}

static const double EDGES[6] = {20.0, 120.0, 500.0, 2000.0, 6000.0, 20000.0};

/* ---- tests -------------------------------------------------------------- */

static void test_flat_sum(double sr)
{
    printf("[flat sum] sr=%.0f\n", sr);
    const double freqs[] = {30, 60, 100, 120, 150, 300, 500, 800, 2000, 4000, 6000, 8000, 15000};
    eb_params p;
    eb_defaults(&p);
    p.xover_type = g_xtype;
    p.power = 0.0f; p.keep_level = 0; p.limiter_on = 0;
    size_t n = (size_t)(sr * 2.5);
    float *in = malloc(n * sizeof *in), *out = malloc(n * sizeof *out);
    for (size_t t = 0; t < sizeof freqs / sizeof *freqs; t++) {
        eb_state *s = eb_create(sr);
        for (size_t i = 0; i < n; i++) in[i] = (float)(0.25 * sin(2.0 * PI * freqs[t] * (double)i / sr));
        run_all(s, &p, in, out, n);
        size_t m = (size_t)(sr * 1.0);
        double d = rms_db(out, n - m, m) - rms_db(in, n - m, m);
        CHECK(fabs(d) < 0.05, "%.0f Hz: level error %.3f dB", freqs[t], d);
        eb_destroy(s);
    }
    free(in); free(out);
}

static void test_pink_unchanged(double sr)
{
    printf("[pink stays pink] sr=%.0f\n", sr);
    size_t n = pow2_at_least(sr * 14.0);
    float *in = make_noise(n, sr, 1.0, 0.1), *out = malloc(n * sizeof *out);
    eb_params p;
    eb_defaults(&p);
    p.xover_type = g_xtype;
    p.power = 100.0f;
    eb_state *s = eb_create(sr);
    run_all(s, &p, in, out, n);
    size_t m = pow2_at_least(sr * 2.0) / 2;
    double fi[5], fo[5];
    band_fractions(in, n - m, m, sr, EDGES, fi);
    band_fractions(out, n - m, m, sr, EDGES, fo);
    for (int b = 0; b < 5; b++) {
        CHECK(fabs(fo[b] - fi[b]) < 1.0, "band %d: in %.2f dB out %.2f dB", b, fi[b], fo[b]);
    }
    double dl = rms_db(out, n - m, m) - rms_db(in, n - m, m);
    CHECK(fabs(dl) < 1.0, "overall level changed by %.2f dB", dl);
    eb_destroy(s);
    free(in); free(out);
}

static double spread_db(const double v[5])
{
    double mean = 0.0, var = 0.0;
    for (int b = 0; b < 5; b++) mean += v[b] / 5.0;
    for (int b = 0; b < 5; b++) var += (v[b] - mean) * (v[b] - mean) / 5.0;
    return sqrt(var);
}

static void test_corrects_balance(double sr, double alpha, const char *label)
{
    printf("[corrects %s] sr=%.0f\n", label, sr);
    size_t n = pow2_at_least(sr * 14.0);
    float *in = make_noise(n, sr, alpha, 0.1), *out = malloc(n * sizeof *out);
    size_t m = pow2_at_least(sr * 2.0) / 2;

    /* The reference is pink; measure its fractions from pink noise. */
    float *pink = make_noise(n, sr, 1.0, 0.1);
    double fp[5], fi[5], fo[5];
    band_fractions(pink, n - m, m, sr, EDGES, fp);
    band_fractions(in, n - m, m, sr, EDGES, fi);
    double dev_in[5], dev_out[5];
    for (int b = 0; b < 5; b++) dev_in[b] = fi[b] - fp[b];

    const float powers[] = {0.0f, 50.0f, 100.0f};
    double spread_prev = 1e9;
    for (int t = 0; t < 3; t++) {
        eb_params p;
        eb_defaults(&p);
    p.xover_type = g_xtype;
        p.power = powers[t];
        p.range_boost = 18.0f; p.range_cut = 18.0f;
        eb_state *s = eb_create(sr);
        run_all(s, &p, in, out, n);
        band_fractions(out, n - m, m, sr, EDGES, fo);
        for (int b = 0; b < 5; b++) dev_out[b] = fo[b] - fp[b];
        double si = spread_db(dev_in), so = spread_db(dev_out);
        printf("  power %3.0f%%: deviation spread %.2f dB -> %.2f dB\n", (double)powers[t], si, so);
        if (t == 0) CHECK(fabs(so - si) < 0.5, "power 0 must not change the balance (%.2f vs %.2f)", si, so);
        if (t == 1) CHECK(so > 0.3 * si && so < 0.7 * si, "power 50: spread %.2f not about half of %.2f", so, si);
        /* Widely overlapping bands leak a steep tilt into their neighbours, so
         * full correction is only approached, not reached. */
        if (t == 2) CHECK(so < (g_xtype == EB_XOVER_LR4 ? 1.2 : 2.5), "power 100: spread %.2f dB left", so);
        CHECK(so <= spread_prev + 0.3, "more power must not worsen the balance");
        spread_prev = so;
        double dl = rms_db(out, n - m, m) - rms_db(in, n - m, m);
        CHECK(fabs(dl) < 1.5, "power %.0f: overall level changed by %.2f dB", (double)powers[t], dl);
        eb_destroy(s);
    }
    free(in); free(out); free(pink);
}

/* An input far outside the correctable range must saturate at the configured
 * range instead of overshooting it. */
static void test_range_clamp(double sr)
{
    printf("[range clamp] sr=%.0f\n", sr);
    size_t n = pow2_at_least(sr * 14.0);
    float *in = make_noise(n, sr, -0.5, 0.1), *out = malloc(n * sizeof *out);
    eb_params p;
    eb_defaults(&p);
    p.xover_type = g_xtype;
    p.power = 100.0f; p.range_boost = 6.0f; p.range_cut = 6.0f; p.keep_level = 0;
    eb_state *s = eb_create(sr);
    run_all(s, &p, in, out, n);
    float lo = 0.0f, hi = 0.0f;
    for (int b = 0; b < 5; b++) {
        float g = eb_band_gain_db(s, b);
        if (g < lo) lo = g;
        if (g > hi) hi = g;
    }
    printf("  band gains span %.2f .. %.2f dB\n", (double)lo, (double)hi);
    CHECK(lo >= -6.05f && hi <= 6.05f, "gain left the +-6 dB range");
    CHECK(lo <= -5.5f, "expected the strongest cut to saturate at -6 dB, got %.2f", (double)lo);
    eb_destroy(s);
    free(in); free(out);
}

static void test_sample_rate_independence(void)
{
    printf("[sample-rate independence]\n");
    /* Several noise realisations per rate: the spread between realisations
     * of one rate is the noise floor of this comparison. */
    enum { REAL = 4 };
    float g[2][REAL][5];
    const double rates[2] = {48000.0, 192000.0};
    for (int r = 0; r < 2; r++) {
        double sr = rates[r];
        size_t n = pow2_at_least(sr * 14.0);
        for (int k = 0; k < REAL; k++) {
            float *in = make_noise(n, sr, 2.0, 0.1), *out = malloc(n * sizeof *out);
            eb_params p;
            eb_defaults(&p);
    p.xover_type = g_xtype;
            p.power = 100.0f; p.range_boost = 18.0f; p.range_cut = 18.0f;
            eb_state *s = eb_create(sr);
            run_all(s, &p, in, out, n);
            for (int b = 0; b < 5; b++) g[r][k][b] = eb_band_gain_db(s, b);
            eb_destroy(s);
            free(in); free(out);
        }
    }
    for (int b = 0; b < 5; b++) {
        double mean[2] = {0, 0}, lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
        for (int r = 0; r < 2; r++) {
            for (int k = 0; k < REAL; k++) {
                double v = g[r][k][b];
                mean[r] += v / REAL;
                if (v < lo[r]) lo[r] = v;
                if (v > hi[r]) hi[r] = v;
            }
        }
        printf("  band %d: 48k %.2f dB [%.2f..%.2f]  192k %.2f dB [%.2f..%.2f]\n",
               b, mean[0], lo[0], hi[0], mean[1], lo[1], hi[1]);
        CHECK(fabs(mean[0] - mean[1]) < 1.0, "band %d mean gain differs between rates by %.2f dB", b, fabs(mean[0] - mean[1]));
    }
}

static void test_limiter(double sr)
{
    printf("[limiter] sr=%.0f\n", sr);
    size_t n = (size_t)(sr * 8.0);
    float *in = malloc(n * sizeof *in), *out = malloc(n * sizeof *out);
    float *noise = make_noise(pow2_at_least((double)n), sr, 1.0, 0.5);
    for (size_t i = 0; i < n; i++) {
        double x = noise[i];
        if ((i / (size_t)(sr * 0.5)) % 2) x += 0.9 * sin(2.0 * PI * 80.0 * (double)i / sr);
        if (i % (size_t)(sr * 0.37) == 0) x = 1.0;     /* single-sample impulses */
        if (i % (size_t)(sr * 0.91) < 3) x = -1.0;
        in[i] = (float)x;
    }
    eb_params p;
    eb_defaults(&p);
    p.xover_type = g_xtype;
    p.power = 100.0f; p.range_boost = 18.0f; p.ceiling = -3.0f;
    eb_state *s = eb_create(sr);
    run_all(s, &p, in, out, n);
    double ceil_lin = pow(10.0, -3.0 / 20.0), peak = 0.0;
    for (size_t i = 0; i < n; i++) if (fabs((double)out[i]) > peak) peak = fabs((double)out[i]);
    CHECK(peak <= ceil_lin + 1e-6, "peak %.6f exceeds ceiling %.6f", peak, ceil_lin);
    CHECK(peak > ceil_lin * 0.8, "limiter should reach near the ceiling, got %.4f", peak);
    eb_destroy(s);
    free(in); free(out); free(noise);
}

static void test_robustness(double sr)
{
    printf("[robustness] sr=%.0f\n", sr);
    size_t n = (size_t)(sr * 4.0);
    float *in = malloc(n * sizeof *in), *out = malloc(n * sizeof *out);
    eb_state *s = eb_create(sr);
    eb_params p;
    eb_defaults(&p);
    p.xover_type = g_xtype;
    int bad = 0;

    for (size_t i = 0; i < n; i++) {
        float x = (float)(urand() * 2.0 - 1.0);
        size_t k = i % 5000;
        if (k == 10) x = NAN;
        if (k == 20) x = INFINITY;
        if (k == 30) x = -INFINITY;
        if (k == 40) x = 1e30f;
        in[i] = x;
    }
    run_all(s, &p, in, out, n);
    for (size_t i = 0; i < n; i++) if (!isfinite(out[i]) || fabsf(out[i]) > 1.0f) bad++;
    CHECK(bad == 0, "%d non-finite or over-range outputs with hostile input", bad);

    for (size_t i = 0; i < n; i++) in[i] = 0.0f;
    run_all(s, &p, in, out, n);
    bad = 0;
    for (size_t i = 0; i < n; i++) if (!isfinite(out[i])) bad++;
    CHECK(bad == 0, "%d non-finite outputs in silence", bad);

    for (size_t i = 0; i < n; i++) in[i] = 0.5f;
    run_all(s, &p, in, out, n);
    bad = 0;
    for (size_t i = 0; i < n; i++) if (!isfinite(out[i])) bad++;
    CHECK(bad == 0, "%d non-finite outputs with DC", bad);

    /* Random and invalid parameters, including NaN. */
    for (int round = 0; round < 200; round++) {
        eb_params r;
        r.power = (float)(urand() * 400.0 - 150.0);
        r.speed = (float)(urand() * 20.0 - 5.0);
        r.range_boost = (float)(urand() * 80.0 - 20.0);
        r.range_cut = (float)(urand() * 80.0 - 20.0);
        r.tilt = (float)(urand() * 20.0 - 10.0);
        for (int b = 0; b < EB_BANDS; b++) r.offset[b] = (float)(urand() * 60.0 - 30.0);
        for (int k = 0; k < EB_SPLITS; k++) r.xover[k] = (float)(urand() * 40000.0 - 5000.0);
        r.keep_level = (int)(urand() * 3.0);
        r.limiter_on = (int)(urand() * 3.0);
        r.ceiling = (float)(urand() * 40.0 - 30.0);
        r.xover_type = (int)(urand() * 5.0) - 1;
        if (round % 10 == 0) r.power = NAN;
        if (round % 13 == 0) r.xover[1] = NAN;
        for (size_t i = 0; i < 4096; i++) in[i] = (float)(urand() * 2.0 - 1.0);
        eb_process(s, &r, in, in, out, out, 4096);
        bad = 0;
        for (size_t i = 0; i < 4096; i++) if (!isfinite(out[i])) bad++;
        if (bad) { CHECK(0, "round %d: %d non-finite outputs", round, bad); break; }
    }
    CHECK(1, "random parameter sweep");
    eb_destroy(s);
    free(in); free(out);
}

static void test_block_invariance(double sr)
{
    printf("[block-size invariance] sr=%.0f\n", sr);
    size_t n = (size_t)(sr * 3.0);
    float *in = make_noise(pow2_at_least((double)n), sr, 1.5, 0.2);
    float *a = malloc(n * sizeof *a), *b = malloc(n * sizeof *b);
    eb_params p;
    eb_defaults(&p);
    p.xover_type = g_xtype;
    p.power = 90.0f;

    eb_state *s1 = eb_create(sr), *s2 = eb_create(sr);
    eb_process(s1, &p, in, in, a, a, (uint32_t)n);
    size_t i = 0;
    while (i < n) {
        size_t c = 1 + (size_t)(urand() * 777.0);
        if (c > n - i) c = n - i;
        eb_process(s2, &p, in + i, in + i, b + i, b + i, (uint32_t)c);
        i += c;
    }
    int diff = memcmp(a, b, n * sizeof *a) != 0;
    CHECK(!diff, "output depends on the block size");
    eb_destroy(s1); eb_destroy(s2);
    free(in); free(a); free(b);
}

static void test_latency(double sr)
{
    printf("[latency] sr=%.0f\n", sr);
    eb_state *s = eb_create(sr);
    double ms = 1000.0 * (double)eb_latency(s) / sr;
    CHECK(ms > 4.0 && ms < 6.0, "latency %.2f ms", ms);
    eb_destroy(s);
}

static void test_speed(double sr)
{
    size_t n = (size_t)(sr * 10.0);
    float *in = make_noise(pow2_at_least((double)n), sr, 1.2, 0.1), *out = malloc(n * sizeof *out);
    eb_params p;
    eb_defaults(&p);
    p.xover_type = g_xtype;
    eb_state *s = eb_create(sr);
    clock_t t0 = clock();
    run_all(s, &p, in, out, n);
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("[speed] sr=%.0f: 10 s of stereo audio in %.3f s (%.0fx real time)\n", sr, sec, 10.0 / sec);
    CHECK(10.0 / sec > 20.0, "too slow for comfortable real-time use");
    eb_destroy(s);
    free(in); free(out);
}


/* ---- AGC mode -------------------------------------------------------- */

static eb_params agc_params(double ratio_pct, double range_boost)
{
    eb_params p;
    eb_defaults(&p);
    p.mode = EB_MODE_AGC;
    p.power = (float)ratio_pct;
    p.range_boost = (float)range_boost;
    p.range_cut = 40.0f;
    p.trim = -6.0f;
    p.target = -12.0f;
    p.gate = -90.0f;
    p.keep_level = 0;
    p.limiter_on = 0;
    for (int i = 0; i < EB_BANDS; i++) { p.atk[i] = 0.1f; p.rel[i] = 0.3f; }
    return p;
}

static double overall_gain(double sr, const eb_params *p, double level_dbfs, double seconds)
{
    size_t n = pow2_at_least(sr * seconds);
    float *in = make_noise(n, sr, 1.0, pow(10.0, level_dbfs / 20.0));
    float *out = malloc(n * sizeof *out);
    eb_state *s = eb_create(sr);
    run_all(s, p, in, out, n);
    size_t m = n / 2;
    double g = rms_db(out, n - m, m) - rms_db(in, n - m, m);
    eb_destroy(s);
    free(in); free(out);
    return g;
}

static void test_agc_levelling(double sr)
{
    printf("[agc levelling] sr=%.0f\n", sr);
    /* infinite ratio, 24 dB deviation limit: gain = -6 + clamp(target - level) */
    eb_params p = agc_params(100.0, 24.0);
    double g_quiet = overall_gain(sr, &p, -50.0, 10.0);
    double g_mid = overall_gain(sr, &p, -24.0, 10.0);
    double g_loud = overall_gain(sr, &p, -4.0, 10.0);
    printf("  gain at -50/-24/-4 dBFS: %.1f / %.1f / %.1f dB\n", g_quiet, g_mid, g_loud);
    CHECK(fabs(g_quiet - 18.0) < 1.0, "quiet input must hit the limit (+18 dB), got %.1f", g_quiet);
    CHECK(fabs((g_mid - g_loud) - 20.0) < 2.0, "20 dB more level must cost ~20 dB of gain, cost %.1f", g_mid - g_loud);
    CHECK(g_mid < g_quiet - 2.0, "gain must fall once the target is reached");

    eb_params off = agc_params(0.0, 24.0);
    double g0a = overall_gain(sr, &off, -40.0, 6.0), g0b = overall_gain(sr, &off, -10.0, 6.0);
    CHECK(fabs(g0a + 6.0) < 0.3 && fabs(g0b + 6.0) < 0.3, "ratio 0 must give the trim gain (-6 dB), got %.2f / %.2f", g0a, g0b);

    /* the limit applies to the deviation, then ratio scales it: 50 %% of 13.9 dB - 6 dB = +1 dB */
    eb_params half = agc_params(50.0, 13.92);
    double gh = overall_gain(sr, &half, -50.0, 10.0);
    CHECK(fabs(gh - 0.96) < 0.5, "ratio 50, limit 13.9: expected +1.0 dB, got %.2f", gh);
}

static void test_agc_time(double sr)
{
    printf("[agc time constants] sr=%.0f\n", sr);
    eb_params p = agc_params(100.0, 24.0);
    for (int i = 0; i < EB_BANDS; i++) { p.atk[i] = 0.5f; p.rel[i] = 2.0f; }
    size_t seg = (size_t)(sr * 12.0), n = 3 * seg;
    float *noise = make_noise(pow2_at_least((double)n), sr, 1.0, 1.0);
    float *in = malloc(n * sizeof *in), *out = malloc(n * sizeof *out);
    for (size_t i = 0; i < n; i++) {
        double lvl = (i / seg == 1) ? -14.0 : -34.0;
        in[i] = (float)(noise[i] * pow(10.0, lvl / 20.0));
    }
    eb_state *s = eb_create(sr);
    run_all(s, &p, in, out, n);
    /* gain per 100 ms window */
    const size_t w = (size_t)(sr * 0.1);
    double g_before = 0, g_after = 0, g63a = -1, g63r = -1;
    double gl[400] = {0};
    int nw = (int)(n / w) < 400 ? (int)(n / w) : 400;
    for (int k = 0; k < nw; k++) gl[k] = rms_db(out, (size_t)k * w, w) - rms_db(in, (size_t)k * w, w);
    int k1 = (int)(seg / w), k2 = (int)(2 * seg / w);
    g_before = gl[k1 - 2];
    g_after = gl[k2 - 2];
    double tgt = g_before + 0.632 * (g_after - g_before);
    /* the window holding the step also holds the limiter delay: skip it */
    for (int k = k1 + 1; k < k2; k++) if (gl[k] <= tgt) { g63a = (double)(k - k1) * 0.1; break; }
    double g_end = gl[nw - 2];
    double tgr = g_after + 0.632 * (g_end - g_after);
    for (int k = k2 + 1; k < nw; k++) if (gl[k] >= tgr) { g63r = (double)(k - k2) * 0.1; break; }
    printf("  gain %.1f -> %.1f -> %.1f dB; attack 63%% at %.1f s (set 0.5), release at %.1f s (set 2.0)\n",
           g_before, g_after, g_end, g63a, g63r);
    CHECK(g63a > 0.25 && g63a < 0.9, "attack time off: %.2f s", g63a);
    CHECK(g63r > 1.0 && g63r < 3.5, "release time off: %.2f s", g63r);
    eb_destroy(s);
    free(noise); free(in); free(out);
}

static void test_mix(double sr)
{
    printf("[static mix] sr=%.0f\n", sr);
    eb_params p = agc_params(0.0, 24.0);        /* ratio 0: gain is the constant trim */
    size_t n = (size_t)(sr * 3.0);
    float *in = malloc(n * sizeof *in), *out = malloc(n * sizeof *out);
    for (size_t i = 0; i < n; i++) in[i] = (float)(0.1 * sin(2.0 * PI * 1000.0 * (double)i / sr));
    eb_state *s0 = eb_create(sr);
    run_all(s0, &p, in, out, n);
    double base = rms_db(out, n / 2, n / 2);
    p.mix[2] = 6.0f;                             /* 1 kHz sits in band 2 (500..2000 Hz) */
    eb_state *s1 = eb_create(sr);
    run_all(s1, &p, in, out, n);
    double boosted = rms_db(out, n / 2, n / 2);
    p.mix[2] = 0.0f; p.mix[0] = 12.0f;           /* a band far away must not change a 1 kHz tone */
    eb_state *s2 = eb_create(sr);
    run_all(s2, &p, in, out, n);
    double far = rms_db(out, n / 2, n / 2);
    CHECK(fabs((boosted - base) - 6.0) < 0.8, "mix +6 dB changed a 1 kHz tone by %.2f dB", boosted - base);
    CHECK(fabs(far - base) < 0.3, "an unrelated band's mix moved the tone by %.2f dB", far - base);
    eb_destroy(s0); eb_destroy(s1); eb_destroy(s2);
    free(in); free(out);
}

static void test_agc_gate(double sr)
{
    printf("[agc gate] sr=%.0f\n", sr);
    size_t seg = (size_t)(sr * 8.0), n = 2 * seg;
    float *noise = make_noise(pow2_at_least((double)n), sr, 1.0, 1.0);
    float *in = malloc(n * sizeof *in), *out = malloc(n * sizeof *out);
    for (size_t i = 0; i < n; i++) in[i] = (float)(noise[i] * pow(10.0, (i < seg ? -24.0 : -64.0) / 20.0));
    double held[2], loud = 0.0;
    for (int g = 0; g < 2; g++) {
        eb_params p = agc_params(100.0, 24.0);
        p.gate = g == 0 ? -90.0f : -50.0f;
        eb_state *s = eb_create(sr);
        run_all(s, &p, in, out, n);
        held[g] = rms_db(out, n - (size_t)(sr * 2.0), (size_t)(sr * 2.0)) - rms_db(in, n - (size_t)(sr * 2.0), (size_t)(sr * 2.0));
        if (g == 1) loud = rms_db(out, seg - (size_t)(sr * 2.0), (size_t)(sr * 1.5)) - rms_db(in, seg - (size_t)(sr * 2.0), (size_t)(sr * 1.5));
        eb_destroy(s);
    }
    printf("  gain in the quiet part: %.1f dB without gate, %.1f dB with gate -50 dBFS (loud part: %.1f dB)\n", held[0], held[1], loud);
    CHECK(held[0] > 16.5, "without a gate the quiet part must reach the +18 dB limit, got %.1f", held[0]);
    CHECK(fabs(held[1] - loud) < 3.5, "gate must hold the gain of the loud part (%.1f dB), got %.1f", loud, held[1]);
    CHECK(held[1] < held[0] - 1.0, "the gate had no effect");
    free(noise); free(in); free(out);
}

static void test_agc_block_invariance(double sr)
{
    printf("[agc block-size invariance] sr=%.0f\n", sr);
    size_t n = (size_t)(sr * 4.0);
    float *in = make_noise(pow2_at_least((double)n), sr, 1.3, 0.2);
    float *a = malloc(n * sizeof *a), *b = malloc(n * sizeof *b);
    eb_params p = agc_params(80.0, 20.0);
    p.gate = -50.0f;
    p.mix[1] = 3.0f;
    eb_state *s1 = eb_create(sr), *s2 = eb_create(sr);
    eb_process(s1, &p, in, in, a, a, (uint32_t)n);
    size_t i = 0;
    while (i < n) {
        size_t c = 1 + (size_t)(urand() * 777.0);
        if (c > n - i) c = n - i;
        eb_process(s2, &p, in + i, in + i, b + i, b + i, (uint32_t)c);
        i += c;
    }
    CHECK(memcmp(a, b, n * sizeof *a) == 0, "AGC output depends on the block size");
    eb_destroy(s1); eb_destroy(s2);
    free(in); free(a); free(b);
}

static void test_agc_robust(double sr)
{
    printf("[agc robustness] sr=%.0f\n", sr);
    size_t n = (size_t)(sr * 3.0);
    float *in = malloc(n * sizeof *in), *out = malloc(n * sizeof *out);
    eb_state *s = eb_create(sr);
    int bad = 0;
    for (int round = 0; round < 100; round++) {
        eb_params r = agc_params(urand() * 150.0 - 20.0, urand() * 60.0 - 10.0);
        r.mode = (urand() < 0.7) ? EB_MODE_AGC : (int)(urand() * 4.0) - 1;
        r.trim = (float)(urand() * 40.0 - 20.0);
        r.target = (float)(urand() * 80.0 - 60.0);
        r.gate = (float)(urand() * 100.0 - 100.0);
        for (int i = 0; i < EB_BANDS; i++) {
            r.atk[i] = (float)(urand() * 100.0 - 20.0);
            r.rel[i] = (float)(urand() * 100.0 - 20.0);
            r.mix[i] = (float)(urand() * 80.0 - 40.0);
        }
        if (round % 9 == 0) r.target = NAN;
        if (round % 11 == 0) r.atk[2] = NAN;
        r.limiter_on = 1;
        for (size_t i = 0; i < 4096; i++) in[i] = (float)((urand() * 2.0 - 1.0) * (round % 5 == 0 ? 8.0 : 0.3));
        eb_process(s, &r, in, in, out, out, 4096);
        for (size_t i = 0; i < 4096; i++) if (!isfinite(out[i])) bad++;
    }
    (void)n;
    CHECK(bad == 0, "%d non-finite outputs in the AGC parameter sweep", bad);
    eb_destroy(s);
    free(in); free(out);
}

static void test_agc_rate_independence(void)
{
    printf("[agc sample-rate independence]\n");
    eb_params p = agc_params(100.0, 24.0);
    double g48 = overall_gain(48000.0, &p, -24.0, 10.0), g192 = overall_gain(192000.0, &p, -24.0, 10.0);
    printf("  gain %.2f dB @48k, %.2f dB @192k\n", g48, g192);
    CHECK(fabs(g48 - g192) < 0.8, "AGC gain differs between rates by %.2f dB", fabs(g48 - g192));
}


/* ---- bypass ---------------------------------------------------------- */

static void test_bypass(double sr)
{
    printf("[bypass] sr=%.0f\n", sr);
    size_t n = pow2_at_least(sr * 3.0);
    float *in = make_noise(n, sr, 1.0, 0.1), *out = malloc(n * sizeof *out);
    eb_params p;
    eb_defaults(&p);
    p.power = 100.0f;                       /* strong processing, so a difference would show */

    /* 1. bypassed output is the input, delayed by exactly the reported latency */
    p.bypass = 1;
    eb_state *s = eb_create(sr);
    run_all(s, &p, in, out, n);
    size_t lat = eb_latency(s), start = (size_t)(sr * 0.2);
    double worst = 0.0;
    for (size_t i = start; i < n; i++) {
        double d = fabs((double)out[i] - (double)in[i - lat]);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-6, "bypassed output differs from the delayed input by %g", worst);
    eb_destroy(s);

    /* 2. toggling must not click: a 1 kHz sine switched on and off every 0.5 s */
    float *sine = malloc(n * sizeof *sine);
    for (size_t i = 0; i < n; i++) sine[i] = (float)(0.25 * sin(2.0 * PI * 1000.0 * (double)i / sr));
    s = eb_create(sr);
    const size_t seg = (size_t)(sr * 0.5);
    for (size_t i = 0; i + seg <= n; i += seg) {
        p.bypass = (int)((i / seg) & 1);
        eb_process(s, &p, sine + i, sine + i, out + i, out + i, (uint32_t)seg);
    }
    double jump = 0.0, slope = 2.0 * PI * 1000.0 * 0.25 / sr;
    for (size_t i = 1; i < n - seg; i++) {
        double d = fabs((double)out[i] - (double)out[i - 1]);
        if (d > jump) jump = d;
    }
    CHECK(jump < 4.0 * slope, "switching clicks: largest step %.4f vs signal slope %.4f", jump, slope);
    eb_destroy(s);

    /* 3. processing keeps adapting during bypass, so switching back is instant */
    eb_params a = agc_params(100.0, 24.0);
    a.limiter_on = 1;
    size_t half = (size_t)(sr * 4.0);
    float *buf = malloc(2 * half * sizeof *buf);
    float *noise = make_noise(pow2_at_least((double)(2 * half)), sr, 1.0, pow(10.0, -24.0 / 20.0));
    memcpy(buf, noise, 2 * half * sizeof *buf);
    float *res = malloc(2 * half * sizeof *res);
    s = eb_create(sr);
    a.bypass = 1;
    eb_process(s, &a, buf, buf, res, res, (uint32_t)half);
    a.bypass = 0;
    eb_process(s, &a, buf + half, buf + half, res + half, res + half, (uint32_t)half);
    size_t w = (size_t)(sr * 0.3), t0 = half + (size_t)(sr * 0.1);
    double g_first = rms_db(res, t0, w) - rms_db(buf, t0, w);
    double g_late = rms_db(res, 2 * half - w, w) - rms_db(buf, 2 * half - w, w);
    CHECK(fabs(g_first - g_late) < 1.5, "after un-bypass the gain must already be adapted: %.1f dB then %.1f dB", g_first, g_late);
    eb_destroy(s);
    free(in); free(out); free(sine); free(buf); free(noise); free(res);
}

int main(void)
{
    const double rates[2] = {48000.0, 192000.0};
    for (int r = 0; r < 2; r++) {
        g_xtype = EB_XOVER_LR4;
        test_flat_sum(rates[r]);
        test_pink_unchanged(rates[r]);
        test_corrects_balance(rates[r], 2.0, "bass-heavy");
        test_corrects_balance(rates[r], 0.5, "treble-heavy");
        test_range_clamp(rates[r]);
        test_limiter(rates[r]);
        test_robustness(rates[r]);
        test_block_invariance(rates[r]);
        test_latency(rates[r]);
        test_speed(rates[r]);
        for (g_xtype = EB_XOVER_ONE_POLE; g_xtype <= EB_XOVER_BUTTERWORTH2; g_xtype++) {
            printf("--- crossover type %d ---\n", g_xtype);
            test_flat_sum(rates[r]);
            test_pink_unchanged(rates[r]);
            test_corrects_balance(rates[r], 2.0, "bass-heavy");
            test_block_invariance(rates[r]);
        }
    }
    g_xtype = EB_XOVER_LR4;
    test_sample_rate_independence();
    for (int r = 0; r < 2; r++) {
        test_agc_levelling(rates[r]);
        test_agc_time(rates[r]);
        test_mix(rates[r]);
        test_agc_gate(rates[r]);
        test_agc_block_invariance(rates[r]);
        test_agc_robust(rates[r]);
    }
    test_agc_rate_independence();
    for (int r = 0; r < 2; r++) test_bypass(rates[r]);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
