/* Evenband presets as the UI sees them: LV2 preset files that apply to this plugin.
 * Pure C, no toolkit; used by the user interfaces and tested on its own. */
#ifndef EB_PRESETS_H
#define EB_PRESETS_H

#include "eb_ports.h"

#define EB_PRESET_LABEL_MAX 96
#define EB_PRESETS_MAX 256

typedef struct {
    char label[EB_PRESET_LABEL_MAX];
    float value[P_COUNT];
    unsigned char has[P_COUNT];     /* 1 where the preset sets this port */
} eb_preset;

/* Parses the text of one preset file. Returns 1 and fills *out when the text is a
 * preset of this plugin (it must name the plugin URI), otherwise 0. */
int eb_preset_parse(const char *text, eb_preset *out);

/* Scans the preset bundles of the LV2 path (LV2_PATH, else the usual directories)
 * and returns the number of presets found, sorted by label. */
int eb_presets_scan(eb_preset *list, int max);

/* Scans one directory that holds *.lv2 bundles. Returns how many were appended. */
int eb_presets_scan_dir(const char *dir, eb_preset *list, int count, int max);

#endif
