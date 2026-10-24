/* Evenband: DSP core (no LV2 dependencies).
 *
 * Five-band spectral-balance compressor: an LR4 crossover splits the signal,
 * each band's slow level is compared with a reference spectrum (relative to
 * the programme's overall level), and a per-band gain pulls the balance
 * toward that reference. A look-ahead peak limiter protects the output.
 *
 * All time constants are in seconds and all frequencies in Hz, so behaviour
 * does not depend on the sample rate (tested at 48 kHz and 192 kHz).
 */
#ifndef EB_DSP_H
#define EB_DSP_H

#include <stdint.h>

#define EB_BANDS 5
#define EB_SPLITS (EB_BANDS - 1)

/* Band-split shapes. The gentle ones are complementary (each band is what is
 * left after the lower bands are subtracted), so the bands always sum back to
 * the input exactly; they overlap widely, which keeps tonal changes smooth.
 * LR4 is steep (24 dB/oct) and needs all-pass compensation to sum flat. */
/* Two ways to drive the band gains. */
enum {
    EB_MODE_BALANCE = 0, /* relative: pulls the spectrum toward a reference shape, loudness-independent */
    EB_MODE_AGC = 1      /* absolute: every band is levelled toward its own target power (broadcast-style) */
};

enum {
    EB_XOVER_LR4 = 0,
    EB_XOVER_ONE_POLE = 1,
    EB_XOVER_BUTTERWORTH2 = 2
};

typedef struct {
    float power;               /* 0..100 %: how far the balance is corrected */
    float speed;               /* 0.25..4: scales every time constant (higher = faster) */
    float range_boost;         /* balance: 0..18 dB max gain; AGC: 0..36 dB max deviation up */
    float range_cut;           /* balance: 0..18 dB max attenuation; AGC: 0..60 dB max deviation down */
    float tilt;                /* -3..3 dB/octave: tilt of the reference spectrum */
    float offset[EB_BANDS];    /* -18..18 dB: per-band shift of the reference */
    float xover[EB_SPLITS];    /* Hz: crossover frequencies */
    int keep_level;            /* non-zero: hold the overall level steady */
    int limiter_on;            /* non-zero: enable the output limiter */
    float ceiling;             /* -6..0 dBFS: limiter ceiling */
    int xover_type;            /* EB_XOVER_*: shape of the band splits */

    /* EB_MODE_AGC only. gain = trim + power * clamp(target - level, -range_cut,
     * range_boost): the deviation of a band from its target is limited by
     * range_boost / range_cut (0..36 / 0..60 dB) and then scaled by power.
     * offset[] shifts each band's target. */
    int mode;                  /* EB_MODE_* */
    float target;              /* -40..0 dBFS: band power each band is levelled toward */
    float trim;                /* -12..12 dB: gain when a band sits exactly on its target */
    float atk[EB_BANDS];       /* 0.005..60 s: time constant of gain reduction */
    float rel[EB_BANDS];       /* 0.005..60 s: time constant of gain increase */
    float gate;                /* -90..-20 dBFS: below this total level all band gains are held */

    /* both modes */
    float mix[EB_BANDS];       /* -24..24 dB: static gain per band, applied after the gain stage */

    /* Non-zero: output the input, delayed by the plugin latency so that switching is
     * time-aligned. Processing keeps running, so switching back is instant. */
    int bypass;
} eb_params;

typedef struct eb_state eb_state;

void eb_defaults(eb_params *p);

/* Returns NULL on allocation failure or unusable sample rate. */
eb_state *eb_create(double sample_rate);
void eb_destroy(eb_state *s);
void eb_reset(eb_state *s);

/* Real-time safe: no allocation, no locks. in/out may alias. */
void eb_process(eb_state *s, const eb_params *p,
                const float *in_l, const float *in_r,
                float *out_l, float *out_r, uint32_t n);

uint32_t eb_latency(const eb_state *s);
/* Current gain of a band in dB, including make-up gain. */
float eb_band_gain_db(const eb_state *s, int band);

#endif
