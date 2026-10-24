/* Loudness meter after ITU-R BS.1770-4 / EBU R128: K-weighting, momentary (400 ms),
 * short-term (3 s) and gated integrated loudness, in LUFS. Stereo, any sample rate,
 * no allocation after creation, safe to call from the audio thread. */
#ifndef EB_LOUDNESS_H
#define EB_LOUDNESS_H

typedef struct eb_loud eb_loud;

#define EB_LOUD_NONE (-120.0f)       /* reported when there is nothing to measure yet */

eb_loud *eb_loud_create(double sample_rate);
void eb_loud_free(eb_loud *m);

/* Forgets the history (the integrated value restarts); filter states are kept. */
void eb_loud_reset(eb_loud *m);

void eb_loud_push(eb_loud *m, double left, double right);

void eb_loud_get(const eb_loud *m, float *momentary, float *shortterm, float *integrated);

#endif
