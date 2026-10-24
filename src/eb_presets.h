/* Evenband presets as the UI sees them: LV2 preset files that apply to this plugin, plus
 * the user's own bundle where new presets are saved. Pure C, no toolkit; tested on its own. */
#ifndef EB_PRESETS_H
#define EB_PRESETS_H

#include <stddef.h>

#include "eb_ports.h"

#define EB_PRESET_LABEL_MAX 96
#define EB_PRESETS_MAX 256
#define EB_USER_LABEL_MAX 60

typedef struct {
    char label[EB_PRESET_LABEL_MAX];
    float value[P_COUNT];
    unsigned char has[P_COUNT];     /* 1 where the preset sets this port */
    char file[512];                 /* path of the preset file, empty for the built-in default */
    unsigned char user;             /* lives in the user's bundle: can be overwritten and deleted */
} eb_preset;

/* Parses the text of one preset file. Returns 1 and fills *out when the text is a
 * preset of this plugin (it must name the plugin URI), otherwise 0. */
int eb_preset_parse(const char *text, eb_preset *out);

/* Scans the preset bundles of the LV2 path (LV2_PATH, else the usual directories) and the
 * user's bundle; returns the number of presets found, sorted by label. */
int eb_presets_scan(eb_preset *list, int max);

/* Scans one directory that holds *.lv2 bundles. Returns how many were appended. */
int eb_presets_scan_dir(const char *dir, eb_preset *list, int count, int max);

/* The user's bundle: $EVENBAND_USER_DIR, else ~/.lv2/evenband-user.lv2. */
int eb_user_bundle_dir(char *out, size_t cap);

/* File-name form of a label ([a-z0-9-] only) and a label made safe for a preset file. */
void eb_preset_slug(const char *label, char *out, size_t cap);
void eb_preset_clean_label(const char *in, char *out, size_t cap);

/* Writes the preset (every port that presets carry) into the user's bundle, replacing a
 * preset of the same name, and rebuilds the bundle manifest. file_out receives the path.
 * Returns 0 on success, -1 on failure. */
int eb_preset_save(const char *label, const float *values, char *file_out, size_t cap);

/* Deletes a preset file, but only inside the user's bundle. Returns 0 on success. */
int eb_preset_delete(const char *file);

/* Imports an LV2 preset file of this plugin into the user's bundle. Returns 0 on success
 * and the stored label in label_out. */
int eb_preset_import_ttl(const char *path, char *label_out, size_t cap);

/* Rewrites manifest.ttl of the user's bundle from the preset files it contains. */
int eb_user_manifest_rebuild(void);

#endif
