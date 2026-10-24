/* Evenband: LV2 wrapper around eb_dsp. */
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#include "eb_dsp.h"

#define EB_URI "https://github.com/BobBoba/evenband"

enum port {
    P_IN_L, P_IN_R, P_OUT_L, P_OUT_R,
    P_POWER, P_SPEED, P_RANGE_BOOST, P_RANGE_CUT, P_TILT,
    P_OFF0, P_OFF1, P_OFF2, P_OFF3, P_OFF4,
    P_XO0, P_XO1, P_XO2, P_XO3,
    P_KEEP_LEVEL, P_LIMITER, P_CEILING, P_XOVER_TYPE,
    P_MODE, P_TARGET, P_TRIM, P_GATE,
    P_ATK0, P_ATK1, P_ATK2, P_ATK3, P_ATK4,
    P_REL0, P_REL1, P_REL2, P_REL3, P_REL4,
    P_MIX0, P_MIX1, P_MIX2, P_MIX3, P_MIX4,
    P_LATENCY,
    P_METER0, P_METER1, P_METER2, P_METER3, P_METER4,
    P_COUNT
};

typedef struct {
    eb_state *dsp;
    const float *port[P_COUNT];
    float *out_ctl[P_COUNT];
} plugin;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                              const char *bundle, const LV2_Feature *const *f)
{
    (void)d; (void)bundle; (void)f;
    plugin *p = (plugin *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->dsp = eb_create(rate);
    if (!p->dsp) {
        free(p);
        return NULL;
    }
    return p;
}

static void connect_port(LV2_Handle h, uint32_t index, void *data)
{
    plugin *p = (plugin *)h;
    if (index >= P_COUNT) return;
    p->port[index] = (const float *)data;
    p->out_ctl[index] = (float *)data;
}

static void activate(LV2_Handle h) { eb_reset(((plugin *)h)->dsp); }

static float val(const plugin *p, int idx, float def)
{
    return p->port[idx] ? *p->port[idx] : def;
}

static void run(LV2_Handle h, uint32_t n)
{
    plugin *p = (plugin *)h;
    if (!p->port[P_IN_L] || !p->port[P_IN_R] || !p->out_ctl[P_OUT_L] || !p->out_ctl[P_OUT_R]) {
        return;
    }

    eb_params q;
    eb_defaults(&q);
    q.power = val(p, P_POWER, q.power);
    q.speed = val(p, P_SPEED, q.speed);
    q.range_boost = val(p, P_RANGE_BOOST, q.range_boost);
    q.range_cut = val(p, P_RANGE_CUT, q.range_cut);
    q.tilt = val(p, P_TILT, q.tilt);
    for (int i = 0; i < EB_BANDS; i++) q.offset[i] = val(p, P_OFF0 + i, 0.0f);
    for (int k = 0; k < EB_SPLITS; k++) q.xover[k] = val(p, P_XO0 + k, q.xover[k]);
    q.keep_level = val(p, P_KEEP_LEVEL, 1.0f) >= 0.5f;
    q.limiter_on = val(p, P_LIMITER, 1.0f) >= 0.5f;
    q.ceiling = val(p, P_CEILING, q.ceiling);
    q.xover_type = (int)(val(p, P_XOVER_TYPE, (float)q.xover_type) + 0.5f);
    q.mode = (int)(val(p, P_MODE, (float)q.mode) + 0.5f);
    q.target = val(p, P_TARGET, q.target);
    q.trim = val(p, P_TRIM, q.trim);
    q.gate = val(p, P_GATE, q.gate);
    for (int i = 0; i < EB_BANDS; i++) {
        q.atk[i] = val(p, P_ATK0 + i, q.atk[i]);
        q.rel[i] = val(p, P_REL0 + i, q.rel[i]);
        q.mix[i] = val(p, P_MIX0 + i, 0.0f);
    }

    eb_process(p->dsp, &q, p->port[P_IN_L], p->port[P_IN_R],
               p->out_ctl[P_OUT_L], p->out_ctl[P_OUT_R], n);

    if (p->out_ctl[P_LATENCY]) *p->out_ctl[P_LATENCY] = (float)eb_latency(p->dsp);
    for (int b = 0; b < EB_BANDS; b++) {
        if (p->out_ctl[P_METER0 + b]) *p->out_ctl[P_METER0 + b] = eb_band_gain_db(p->dsp, b);
    }
}

static void cleanup(LV2_Handle h)
{
    plugin *p = (plugin *)h;
    eb_destroy(p->dsp);
    free(p);
}

static const void *extension_data(const char *uri)
{
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    EB_URI, instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
