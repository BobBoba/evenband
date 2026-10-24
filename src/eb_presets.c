#include "eb_presets.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FILE_MAX (128 * 1024)

static int port_index(const char *symbol, size_t len)
{
    for (int i = 0; i < P_COUNT; i++) {
        if (strlen(eb_port_symbol[i]) == len && strncmp(eb_port_symbol[i], symbol, len) == 0) return i;
    }
    return -1;
}

int eb_preset_parse(const char *text, eb_preset *out)
{
    memset(out, 0, sizeof *out);
    if (!strstr(text, "appliesTo") || !strstr(text, EB_PLUGIN_URI)) return 0;

    const char *l = strstr(text, "rdfs:label");
    if (!l) return 0;
    const char *q = strchr(l, '"');
    const char *e = q ? strchr(q + 1, '"') : NULL;
    if (!e || e == q + 1) return 0;
    size_t n = (size_t)(e - q - 1);
    if (n >= sizeof out->label) n = sizeof out->label - 1;
    memcpy(out->label, q + 1, n);
    out->label[n] = '\0';

    const char *p = text;
    while ((p = strstr(p, "lv2:symbol")) != NULL) {
        const char *s0 = strchr(p, '"');
        const char *s1 = s0 ? strchr(s0 + 1, '"') : NULL;
        if (!s1) break;
        const char *next = strstr(s1, "lv2:symbol");
        const char *v = strstr(s1, "pset:value");
        int idx = port_index(s0 + 1, (size_t)(s1 - s0 - 1));
        if (v && (!next || v < next) && idx >= 0 && !eb_port_is_output[idx]) {
            char *end = NULL;
            float f = strtof(v + 10, &end);
            if (end != v + 10) {
                if (f < eb_port_min[idx]) f = eb_port_min[idx];
                if (f > eb_port_max[idx]) f = eb_port_max[idx];
                out->value[idx] = f;
                out->has[idx] = 1;
            }
        }
        p = s1 + 1;
    }
    return 1;
}

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 1;
}

static int has_suffix(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

int eb_presets_scan_dir(const char *dir, eb_preset *list, int count, int max)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    char *buf = malloc(FILE_MAX);
    int added = 0;
    struct dirent *ent;
    while (buf && (ent = readdir(d)) != NULL && count + added < max) {
        if (!has_suffix(ent->d_name, ".lv2")) continue;
        char pdir[1024];
        snprintf(pdir, sizeof pdir, "%s/%s/presets", dir, ent->d_name);
        DIR *pd = opendir(pdir);
        if (!pd) continue;
        struct dirent *pe;
        while ((pe = readdir(pd)) != NULL && count + added < max) {
            if (!has_suffix(pe->d_name, ".ttl")) continue;
            char path[1536];
            snprintf(path, sizeof path, "%s/%s", pdir, pe->d_name);
            eb_preset p;
            if (read_file(path, buf, FILE_MAX) && eb_preset_parse(buf, &p)) {
                int dup = 0;
                for (int i = 0; i < count + added; i++) {
                    if (strcmp(list[i].label, p.label) == 0) dup = 1;
                }
                if (!dup) list[count + added++] = p;
            }
        }
        closedir(pd);
    }
    free(buf);
    closedir(d);
    return added;
}

static int by_label(const void *a, const void *b)
{
    return strcasecmp(((const eb_preset *)a)->label, ((const eb_preset *)b)->label);
}

int eb_presets_scan(eb_preset *list, int max)
{
    int count = 0;
    const char *path = getenv("LV2_PATH");
    char buf[4096];
    if (path && *path) {
        snprintf(buf, sizeof buf, "%s", path);
    } else {
        const char *home = getenv("HOME");
        snprintf(buf, sizeof buf, "%s/.lv2:/usr/local/lib/lv2:/usr/lib/lv2:/usr/lib64/lv2", home ? home : "");
    }
    for (char *save = NULL, *dir = strtok_r(buf, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        count += eb_presets_scan_dir(dir, list, count, max);
    }
    qsort(list, (size_t)count, sizeof *list, by_label);
    return count;
}
