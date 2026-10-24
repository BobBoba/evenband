/* Evenband: DSP core. See eb_dsp.h and SPEC.md. */
#include "eb_dsp.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EB_PI 3.14159265358979323846
#define EB_Q (0.70710678118654752440) /* Butterworth: two cascaded sections give LR4 */

#define LIMITER_LOOKAHEAD_S 0.005
#define LIMITER_RELEASE_S 0.080
#define CONTROL_RATE_HZ 1500.0
#define GATE_POWER 1e-7        /* -70 dB: below this the programme is treated as silence */
#define BAND_FLOOR 1e-20
#define BASE_DETECTOR_S 0.40
#define AGC_DETECTOR_S 0.040
#define BASE_ATTACK_S 0.10     /* gain reduction */
#define BASE_RELEASE_S 0.40    /* gain increase */
#define BASE_MAKEUP_S 1.00
#define MAKEUP_LIMIT_DB 6.0
#define BOOST_FADE_START_DB 40.0 /* bands this far under the total stop being boosted... */
#define BOOST_FADE_END_DB 50.0   /* ...completely at this depth */
#define MIN_XOVER_RATIO 1.25

typedef struct {
    double b0, b1, b2, a1, a2, z1, z2;
} biquad;

typedef struct {
    biquad lp[EB_SPLITS][2];
    biquad hp[EB_SPLITS][2];
    biquad ap[EB_BANDS][EB_SPLITS]; /* ap[b][k] used for k > b */
} xover_ch;

struct eb_state {
    double sr;
    xover_ch ch[2];

    /* settings the cached coefficients were computed for */
    double cur_xover[EB_SPLITS];
    int cur_type;
    int xover_valid;
    double cur_tilt;
    double cur_offset[EB_BANDS];
    int ref_valid;
    double cur_speed;
    int cur_mode;
    int speed_valid;

    double ref[EB_BANDS]; /* reference fraction of total power per band */

    /* analysis */
    double p[EB_BANDS]; /* slow power per band, input side, stereo-linked */
    double det_a;

    /* gains */
    double gain_db[EB_BANDS];
    double makeup_db;
    double g[EB_BANDS];
    double g_step[EB_BANDS];
    uint32_t ctl_n;
    uint32_t ctl_count;
    double meter_db[EB_BANDS];

    /* limiter */
    uint32_t w; /* look-ahead window in samples */
    uint32_t pos;
    uint64_t n;
    float *dly_l, *dly_r;
    float *dry_l, *dry_r;       /* the input, delayed like the processed path (for bypass) */
    double bypass_mix;          /* 0 = processed, 1 = dry, moves with a 10 ms time constant */
    double bypass_a;
    double *gm;
    double gm_sum;
    double gr_prev;
    double rel_a;
    uint64_t *dq_idx;
    double *dq_val;
    uint32_t dq_head, dq_tail, dq_count, dq_cap;
};

/* ---- helpers ---------------------------------------------------------- */

static double clampd(double v, double lo, double hi, double def)
{
    if (!(v == v)) { /* NaN */
        return def;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline double bq_run(biquad *f, double x)
{
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static void bq_flush(biquad *f)
{
    if (fabs(f->z1) < 1e-30) f->z1 = 0.0;
    if (fabs(f->z2) < 1e-30) f->z2 = 0.0;
}

enum { LOW, HIGH, ALL, ONEPOLE };

static void bq_design(biquad *f, int kind, double fc, double sr)
{
    if (kind == ONEPOLE) {
        double a = exp(-2.0 * EB_PI * fc / sr);
        f->b0 = 1.0 - a;
        f->b1 = 0.0;
        f->b2 = 0.0;
        f->a1 = -a;
        f->a2 = 0.0;
        return;
    }
    double w0 = 2.0 * EB_PI * fc / sr;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * EB_Q);
    double a0 = 1.0 + alpha;

    switch (kind) {
    case LOW:
        f->b0 = (1.0 - cw) / 2.0 / a0;
        f->b1 = (1.0 - cw) / a0;
        f->b2 = f->b0;
        break;
    case HIGH:
        f->b0 = (1.0 + cw) / 2.0 / a0;
        f->b1 = -(1.0 + cw) / a0;
        f->b2 = f->b0;
        break;
    default: /* all-pass: the sum of an LR4 low-pass and high-pass */
        f->b0 = (1.0 - alpha) / a0;
        f->b1 = -2.0 * cw / a0;
        f->b2 = (1.0 + alpha) / a0;
        break;
    }
    f->a1 = -2.0 * cw / a0;
    f->a2 = (1.0 - alpha) / a0;
}

static void design_split(eb_state *s, int k, double fc, int type)
{
    for (int c = 0; c < 2; c++) {
        if (type != EB_XOVER_LR4) {
            bq_design(&s->ch[c].lp[k][0], type == EB_XOVER_ONE_POLE ? ONEPOLE : LOW, fc, s->sr);
            continue;
        }
        for (int j = 0; j < 2; j++) {
            bq_design(&s->ch[c].lp[k][j], LOW, fc, s->sr);
            bq_design(&s->ch[c].hp[k][j], HIGH, fc, s->sr);
        }
        for (int b = 0; b < k; b++) {
            bq_design(&s->ch[c].ap[b][k], ALL, fc, s->sr);
        }
    }
}

/* Splits one sample into EB_BANDS phase-aligned bands. Every band that skips
 * a later split is passed through that split's all-pass, so the bands sum to
 * a flat magnitude response. */
static inline void split_bands(xover_ch *x, double in, double band[EB_BANDS], int type)
{
    double r = in;
    if (type != EB_XOVER_LR4) {
        for (int k = 0; k < EB_SPLITS; k++) {
            double lo = bq_run(&x->lp[k][0], r);
            band[k] = lo;
            r -= lo;
        }
        band[EB_BANDS - 1] = r;
        return;
    }
    for (int k = 0; k < EB_SPLITS; k++) {
        band[k] = bq_run(&x->lp[k][1], bq_run(&x->lp[k][0], r));
        r = bq_run(&x->hp[k][1], bq_run(&x->hp[k][0], r));
    }
    band[EB_BANDS - 1] = r;
    for (int b = 0; b < EB_BANDS - 2; b++) {
        for (int k = b + 1; k < EB_SPLITS; k++) {
            band[b] = bq_run(&x->ap[b][k], band[b]);
        }
    }
}

/* ---- parameters ------------------------------------------------------- */

void eb_defaults(eb_params *p)
{
    p->power = 60.0f;
    p->speed = 1.0f;
    p->range_boost = 9.0f;
    p->range_cut = 9.0f;
    p->tilt = 0.0f;
    for (int i = 0; i < EB_BANDS; i++) p->offset[i] = 0.0f;
    p->xover[0] = 120.0f;
    p->xover[1] = 500.0f;
    p->xover[2] = 2000.0f;
    p->xover[3] = 6000.0f;
    p->keep_level = 1;
    p->limiter_on = 1;
    p->ceiling = -1.0f;
    p->xover_type = EB_XOVER_LR4;
    p->mode = EB_MODE_BALANCE;
    p->target = -12.0f;
    p->trim = -6.0f;
    for (int i = 0; i < EB_BANDS; i++) {
        p->atk[i] = 0.2f;
        p->rel[i] = 1.0f;
    }
    p->gate = -60.0f;
    for (int i = 0; i < EB_BANDS; i++) p->mix[i] = 0.0f;
    p->bypass = 0;
}

/* Returns a copy with every value forced into its valid range. */
static void sanitize(const eb_state *s, const eb_params *in, eb_params *q)
{
    eb_params d;
    eb_defaults(&d);

    q->bypass = in->bypass != 0;
    q->mode = in->mode == EB_MODE_AGC ? EB_MODE_AGC : EB_MODE_BALANCE;
    q->power = (float)clampd(in->power, 0.0, 100.0, d.power);
    q->speed = (float)clampd(in->speed, 0.25, 4.0, d.speed);
    if (q->mode == EB_MODE_AGC) {
        q->range_boost = (float)clampd(in->range_boost, 0.0, 36.0, d.range_boost);
        q->range_cut = (float)clampd(in->range_cut, 0.0, 60.0, d.range_cut);
    } else {
        q->range_boost = (float)clampd(in->range_boost, 0.0, 18.0, d.range_boost);
        q->range_cut = (float)clampd(in->range_cut, 0.0, 18.0, d.range_cut);
    }
    q->target = (float)clampd(in->target, -40.0, 0.0, d.target);
    q->trim = (float)clampd(in->trim, -12.0, 12.0, d.trim);
    q->gate = (float)clampd(in->gate, -90.0, -20.0, d.gate);
    for (int i = 0; i < EB_BANDS; i++) {
        q->mix[i] = (float)clampd(in->mix[i], -24.0, 24.0, 0.0);
        q->atk[i] = (float)clampd(in->atk[i], 0.005, 60.0, d.atk[i]);
        q->rel[i] = (float)clampd(in->rel[i], 0.005, 60.0, d.rel[i]);
    }
    q->tilt = (float)clampd(in->tilt, -3.0, 3.0, d.tilt);
    for (int i = 0; i < EB_BANDS; i++) {
        q->offset[i] = (float)clampd(in->offset[i], -18.0, 18.0, 0.0);
    }
    q->keep_level = in->keep_level != 0;
    q->limiter_on = in->limiter_on != 0;
    q->ceiling = (float)clampd(in->ceiling, -6.0, 0.0, d.ceiling);
    q->xover_type = (in->xover_type >= EB_XOVER_LR4 && in->xover_type <= EB_XOVER_BUTTERWORTH2)
                        ? in->xover_type : d.xover_type;

    double fmax_hz = fmin(18000.0, 0.45 * s->sr);
    double f[EB_SPLITS];
    for (int k = 0; k < EB_SPLITS; k++) {
        f[k] = clampd(in->xover[k], 30.0, fmax_hz, d.xover[k]);
        if (k > 0 && f[k] < f[k - 1] * MIN_XOVER_RATIO) f[k] = f[k - 1] * MIN_XOVER_RATIO;
    }
    for (int k = EB_SPLITS - 1; k >= 0; k--) {
        double top = (k == EB_SPLITS - 1) ? fmax_hz : f[k + 1] / MIN_XOVER_RATIO;
        if (f[k] > top) f[k] = top;
    }
    for (int k = 0; k < EB_SPLITS; k++) q->xover[k] = (float)f[k];
}

/* Complex response of one section at angular frequency w (radians/sample). */
static double complex bq_response(const biquad *f, double w)
{
    double complex z1 = cexp(-I * w);
    double complex z2 = z1 * z1;
    return (f->b0 + f->b1 * z1 + f->b2 * z2) / (1.0 + f->a1 * z1 + f->a2 * z2);
}

/* Magnitude-squared response of band `b` of the current crossover. */
static double band_power_response(const eb_state *s, int type, int b, double w)
{
    const xover_ch *x = &s->ch[0];
    double complex h = 1.0;

    if (type == EB_XOVER_LR4) {
        for (int k = 0; k < b; k++) {
            h *= bq_response(&x->hp[k][0], w) * bq_response(&x->hp[k][1], w);
        }
        if (b < EB_SPLITS) h *= bq_response(&x->lp[b][0], w) * bq_response(&x->lp[b][1], w);
    } else {
        for (int k = 0; k < b; k++) h *= 1.0 - bq_response(&x->lp[k][0], w);
        if (b < EB_SPLITS) h *= bq_response(&x->lp[b][0], w);
    }
    return creal(h) * creal(h) + cimag(h) * cimag(h);
}

/* Reference share of the total band power for each band: the power a
 * tilted pink spectrum (equal power per octave) puts into the band, taken
 * through the real crossover, then shifted by the per-band offsets. Using
 * the actual band responses keeps pink noise neutral for every crossover
 * shape, including the widely overlapping ones. */
static void update_reference(eb_state *s, const eb_params *q)
{
    enum { GRID = 720 };
    const double lo = log(20.0);
    const double hi = log(fmin(20000.0, 0.45 * s->sr));
    double w[EB_BANDS] = {0}, sum = 0.0;

    for (int g = 0; g < GRID; g++) {
        double lnf = lo + (hi - lo) * ((double)g + 0.5) / (double)GRID;
        double f = exp(lnf);
        double weight = pow(10.0, 0.1 * q->tilt * log2(f / 1000.0));
        double omega = 2.0 * EB_PI * f / s->sr;
        for (int i = 0; i < EB_BANDS; i++) {
            w[i] += weight * band_power_response(s, q->xover_type, i, omega);
        }
    }
    for (int i = 0; i < EB_BANDS; i++) {
        w[i] *= pow(10.0, 0.1 * q->offset[i]);
        sum += w[i];
    }
    for (int i = 0; i < EB_BANDS; i++) s->ref[i] = w[i] / sum;
}

/* ---- lifecycle -------------------------------------------------------- */

eb_state *eb_create(double sample_rate)
{
    if (!(sample_rate >= 22050.0 && sample_rate <= 768000.0)) return NULL;

    eb_state *s = (eb_state *)calloc(1, sizeof *s);
    if (!s) return NULL;

    s->sr = sample_rate;
    s->w = (uint32_t)ceil(LIMITER_LOOKAHEAD_S * sample_rate);
    if (s->w < 2) s->w = 2;
    s->dq_cap = s->w + 1;
    s->ctl_n = (uint32_t)(sample_rate / CONTROL_RATE_HZ);
    if (s->ctl_n < 1) s->ctl_n = 1;
    s->rel_a = 1.0 - exp(-1.0 / (LIMITER_RELEASE_S * sample_rate));

    s->dly_l = (float *)calloc(s->w, sizeof(float));
    s->dly_r = (float *)calloc(s->w, sizeof(float));
    s->dry_l = (float *)calloc(s->w, sizeof(float));
    s->dry_r = (float *)calloc(s->w, sizeof(float));
    s->bypass_a = 1.0 - exp(-1.0 / (0.010 * sample_rate));
    s->gm = (double *)calloc(s->w, sizeof(double));
    s->dq_idx = (uint64_t *)calloc(s->dq_cap, sizeof(uint64_t));
    s->dq_val = (double *)calloc(s->dq_cap, sizeof(double));
    if (!s->dly_l || !s->dly_r || !s->dry_l || !s->dry_r || !s->gm || !s->dq_idx || !s->dq_val) {
        eb_destroy(s);
        return NULL;
    }
    eb_reset(s);
    return s;
}

void eb_destroy(eb_state *s)
{
    if (!s) return;
    free(s->dly_l);
    free(s->dly_r);
    free(s->dry_l);
    free(s->dry_r);
    free(s->gm);
    free(s->dq_idx);
    free(s->dq_val);
    free(s);
}

static void eb_reset_filters(eb_state *s)
{
    memset(s->ch, 0, sizeof s->ch);
}

void eb_reset(eb_state *s)
{
    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < EB_SPLITS; k++) {
            for (int j = 0; j < 2; j++) {
                s->ch[c].lp[k][j].z1 = s->ch[c].lp[k][j].z2 = 0.0;
                s->ch[c].hp[k][j].z1 = s->ch[c].hp[k][j].z2 = 0.0;
            }
        }
        for (int b = 0; b < EB_BANDS; b++) {
            for (int k = 0; k < EB_SPLITS; k++) {
                s->ch[c].ap[b][k].z1 = s->ch[c].ap[b][k].z2 = 0.0;
            }
        }
    }
    for (int i = 0; i < EB_BANDS; i++) {
        s->p[i] = BAND_FLOOR;
        s->gain_db[i] = 0.0;
        s->g[i] = 1.0;
        s->g_step[i] = 0.0;
        s->meter_db[i] = 0.0;
    }
    s->makeup_db = 0.0;
    s->ctl_count = 0;
    memset(s->dly_l, 0, s->w * sizeof(float));
    memset(s->dly_r, 0, s->w * sizeof(float));
    memset(s->dry_l, 0, s->w * sizeof(float));
    memset(s->dry_r, 0, s->w * sizeof(float));
    s->bypass_mix = 0.0;
    for (uint32_t i = 0; i < s->w; i++) s->gm[i] = 1.0;
    s->gm_sum = (double)s->w;
    s->gr_prev = 1.0;
    s->pos = 0;
    s->n = 0;
    s->dq_head = s->dq_tail = s->dq_count = 0;
}

uint32_t eb_latency(const eb_state *s) { return s->w - 1; }

float eb_band_gain_db(const eb_state *s, int band)
{
    if (band < 0 || band >= EB_BANDS) return 0.0f;
    return (float)s->meter_db[band];
}

/* ---- control-rate update ---------------------------------------------- */

/* AGC mode: every band is driven toward its own absolute target power. */
static void agc_tick(eb_state *s, const eb_params *q)
{
    const double dt = (double)s->ctl_n / s->sr;
    const double strength = q->power / 100.0;

    /* Below the gate level the whole processor holds its gains, so quiet
     * passages and room noise are not pulled up toward the targets. */
    double total = 0.0;
    for (int i = 0; i < EB_BANDS; i++) total += s->p[i];
    const int active = 10.0 * log10(total > BAND_FLOOR ? total : BAND_FLOOR) >= q->gate;

    for (int i = 0; active && i < EB_BANDS; i++) {
        double level = 10.0 * log10(s->p[i] > BAND_FLOOR ? s->p[i] : BAND_FLOOR);
        double target = q->target + q->offset[i];
        /* the deviation from the target is limited first, then scaled */
        double dev = target - level;
        if (dev > q->range_boost) dev = q->range_boost;
        if (dev < -q->range_cut) dev = -q->range_cut;
        double want = q->trim + strength * dev;
        double tau = want < s->gain_db[i] ? q->atk[i] : q->rel[i];
        s->gain_db[i] += (1.0 - exp(-dt / tau)) * (want - s->gain_db[i]);
    }
    s->makeup_db = 0.0;
    for (int i = 0; i < EB_BANDS; i++) {
        double db = s->gain_db[i] + q->mix[i];
        s->meter_db[i] = db;
        double target = pow(10.0, db / 20.0);
        s->g_step[i] = (target - s->g[i]) / (double)s->ctl_n;
    }
}

static void control_tick(eb_state *s, const eb_params *q)
{
    if (q->mode == EB_MODE_AGC) {
        agc_tick(s, q);
        return;
    }
    const double dt = (double)s->ctl_n / s->sr;
    const double speed = q->speed;
    const double att = 1.0 - exp(-dt / (BASE_ATTACK_S / speed));
    const double rel = 1.0 - exp(-dt / (BASE_RELEASE_S / speed));
    const double mka = 1.0 - exp(-dt / (BASE_MAKEUP_S / speed));
    const double strength = q->power / 100.0;

    double total = 0.0;
    for (int i = 0; i < EB_BANDS; i++) total += s->p[i];
    const int silent = total < GATE_POWER;

    for (int i = 0; i < EB_BANDS; i++) {
        double target = 0.0;
        if (!silent) {
            double pi = s->p[i] > BAND_FLOOR ? s->p[i] : BAND_FLOOR;
            double err = 10.0 * log10(total * s->ref[i] / pi);
            if (err > 0.0) {
                /* do not amplify a band that holds almost nothing */
                double depth = 10.0 * log10(total / pi);
                double f = (BOOST_FADE_END_DB - depth) / (BOOST_FADE_END_DB - BOOST_FADE_START_DB);
                err *= f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
            }
            target = strength * err;
            if (target > q->range_boost) target = q->range_boost;
            if (target < -q->range_cut) target = -q->range_cut;
        }
        double a = target < s->gain_db[i] ? att : rel;
        s->gain_db[i] += a * (target - s->gain_db[i]);
    }

    double makeup_target = 0.0;
    if (q->keep_level && !silent) {
        double out = 0.0;
        for (int i = 0; i < EB_BANDS; i++) {
            out += pow(10.0, s->gain_db[i] / 10.0) * s->p[i];
        }
        if (out < BAND_FLOOR) out = BAND_FLOOR;
        makeup_target = 10.0 * log10(total / out);
        if (makeup_target > MAKEUP_LIMIT_DB) makeup_target = MAKEUP_LIMIT_DB;
        if (makeup_target < -MAKEUP_LIMIT_DB) makeup_target = -MAKEUP_LIMIT_DB;
    }
    s->makeup_db += mka * (makeup_target - s->makeup_db);

    for (int i = 0; i < EB_BANDS; i++) {
        double db = s->gain_db[i] + s->makeup_db + q->mix[i];
        s->meter_db[i] = db;
        double target = pow(10.0, db / 20.0);
        s->g_step[i] = (target - s->g[i]) / (double)s->ctl_n;
    }
}

/* ---- processing -------------------------------------------------------- */

void eb_process(eb_state *s, const eb_params *params,
                const float *in_l, const float *in_r,
                float *out_l, float *out_r, uint32_t count)
{
    eb_params q;
    sanitize(s, params, &q);

    int xover_changed = !s->xover_valid || s->cur_type != q.xover_type;
    for (int k = 0; k < EB_SPLITS; k++) {
        if (s->cur_xover[k] != q.xover[k]) xover_changed = 1;
    }
    if (xover_changed) {
        if (s->cur_type != q.xover_type) eb_reset_filters(s);
        s->cur_type = q.xover_type;
        for (int k = 0; k < EB_SPLITS; k++) {
            s->cur_xover[k] = q.xover[k];
            design_split(s, k, q.xover[k], q.xover_type);
        }
        s->xover_valid = 1;
    }

    int ref_changed = xover_changed || !s->ref_valid || s->cur_tilt != q.tilt;
    for (int i = 0; i < EB_BANDS; i++) {
        if (s->cur_offset[i] != q.offset[i]) ref_changed = 1;
    }
    if (ref_changed) {
        s->cur_tilt = q.tilt;
        for (int i = 0; i < EB_BANDS; i++) s->cur_offset[i] = q.offset[i];
        update_reference(s, &q);
        s->ref_valid = 1;
    }

    if (!s->speed_valid || s->cur_speed != q.speed || s->cur_mode != q.mode) {
        s->cur_speed = q.speed;
        s->cur_mode = q.mode;
        double tau = q.mode == EB_MODE_AGC ? AGC_DETECTOR_S : BASE_DETECTOR_S / q.speed;
        s->det_a = 1.0 - exp(-1.0 / (s->sr * tau));
        s->speed_valid = 1;
    }

    const double ceiling = pow(10.0, q.ceiling / 20.0);
    const uint32_t w = s->w;

    for (uint32_t i = 0; i < count; i++) {
        double xl = in_l[i], xr = in_r[i];
        if (!isfinite(xl)) xl = 0.0;
        if (!isfinite(xr)) xr = 0.0;
        if (xl > 1e4) xl = 1e4; else if (xl < -1e4) xl = -1e4;
        if (xr > 1e4) xr = 1e4; else if (xr < -1e4) xr = -1e4;

        double bl[EB_BANDS], br[EB_BANDS];
        split_bands(&s->ch[0], xl, bl, q.xover_type);
        split_bands(&s->ch[1], xr, br, q.xover_type);

        if (s->ctl_count == 0) {
            control_tick(s, &q);
            s->ctl_count = s->ctl_n;
        }
        s->ctl_count--;

        double yl = 0.0, yr = 0.0;
        for (int b = 0; b < EB_BANDS; b++) {
            double pw = 0.5 * (bl[b] * bl[b] + br[b] * br[b]);
            s->p[b] += s->det_a * (pw - s->p[b]);
            if (s->p[b] < BAND_FLOOR) s->p[b] = BAND_FLOOR;
            s->g[b] += s->g_step[b];
            yl += bl[b] * s->g[b];
            yr += br[b] * s->g[b];
        }

        /* look-ahead limiter: sliding minimum of the required gain, release
         * limited, then a box average of the same length. The output sample
         * is delayed by w-1, so the gain has fully arrived when it plays. */
        double peak = fabs(yl) > fabs(yr) ? fabs(yl) : fabs(yr);
        double req = (q.limiter_on && peak > ceiling) ? ceiling / peak : 1.0;

        while (s->dq_count &&
               s->dq_val[(s->dq_tail + s->dq_cap - 1) % s->dq_cap] >= req) {
            s->dq_tail = (s->dq_tail + s->dq_cap - 1) % s->dq_cap;
            s->dq_count--;
        }
        s->dq_idx[s->dq_tail] = s->n;
        s->dq_val[s->dq_tail] = req;
        s->dq_tail = (s->dq_tail + 1) % s->dq_cap;
        s->dq_count++;
        while (s->n - s->dq_idx[s->dq_head] >= w) {
            s->dq_head = (s->dq_head + 1) % s->dq_cap;
            s->dq_count--;
        }
        double gmin = s->dq_val[s->dq_head];

        double gr = s->gr_prev + (1.0 - s->gr_prev) * s->rel_a;
        if (gr > 0.999999) gr = 1.0;
        if (gmin < gr) gr = gmin;
        s->gr_prev = gr;

        s->gm_sum += gr - s->gm[s->pos];
        s->gm[s->pos] = gr;
        s->dly_l[s->pos] = (float)yl;
        s->dly_r[s->pos] = (float)yr;
        s->dry_l[s->pos] = (float)xl;
        s->dry_r[s->pos] = (float)xr;
        uint32_t rd = s->pos + 1 == w ? 0 : s->pos + 1;
        double gain = s->gm_sum / (double)w;
        if (gain > 1.0) gain = 1.0;

        double ol = (double)s->dly_l[rd] * gain;
        double or_ = (double)s->dly_r[rd] * gain;
        if (q.limiter_on) {
            if (ol > ceiling) ol = ceiling; else if (ol < -ceiling) ol = -ceiling;
            if (or_ > ceiling) or_ = ceiling; else if (or_ < -ceiling) or_ = -ceiling;
        }
        double target = q.bypass ? 1.0 : 0.0;
        s->bypass_mix += s->bypass_a * (target - s->bypass_mix);
        if (fabs(target - s->bypass_mix) < 1e-6) s->bypass_mix = target;
        if (s->bypass_mix > 0.0) {
            ol += s->bypass_mix * ((double)s->dry_l[rd] - ol);
            or_ += s->bypass_mix * ((double)s->dry_r[rd] - or_);
        }
        out_l[i] = (float)ol;
        out_r[i] = (float)or_;

        s->pos = rd;
        s->n++;
        if (s->pos == 0) { /* keep the running sum from drifting */
            double sum = 0.0;
            for (uint32_t j = 0; j < w; j++) sum += s->gm[j];
            s->gm_sum = sum;
        }
    }

    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < EB_SPLITS; k++) {
            for (int j = 0; j < 2; j++) {
                bq_flush(&s->ch[c].lp[k][j]);
                bq_flush(&s->ch[c].hp[k][j]);
            }
        }
        for (int b = 0; b < EB_BANDS; b++) {
            for (int k = 0; k < EB_SPLITS; k++) bq_flush(&s->ch[c].ap[b][k]);
        }
    }
}
