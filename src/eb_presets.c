#include "eb_presets.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_MAX (128 * 1024)
#define PLUGIN_URI EB_PLUGIN_URI

static int port_index(const char *symbol, size_t len)
{
    for (int i = 0; i < P_COUNT; i++) {
        if (strlen(eb_port_symbol[i]) == len && strncmp(eb_port_symbol[i], symbol, len) == 0) return i;
    }
    return -1;
}

static int preset_port(int i)
{
    return i >= P_POWER && !eb_port_is_output[i] && eb_meta[i].in_preset;
}

int eb_preset_parse(const char *text, eb_preset *out)
{
    memset(out, 0, sizeof *out);
    if (!strstr(text, "appliesTo") || !strstr(text, PLUGIN_URI)) return 0;

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

int eb_user_bundle_dir(char *out, size_t cap)
{
    const char *over = getenv("EVENBAND_USER_DIR");
    if (over && *over) {
        snprintf(out, cap, "%s", over);
        return 0;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(out, cap, "%s/.lv2/evenband-user.lv2", home);
    return 0;
}

int eb_presets_scan_dir(const char *dir, eb_preset *list, int count, int max)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    char user_bundle[900] = "";
    eb_user_bundle_dir(user_bundle, sizeof user_bundle);
    char *buf = malloc(FILE_MAX);
    int added = 0;
    struct dirent *ent;
    while (buf && (ent = readdir(d)) != NULL && count + added < max) {
        if (!has_suffix(ent->d_name, ".lv2")) continue;
        char pdir[1024];
        snprintf(pdir, sizeof pdir, "%s/%s/presets", dir, ent->d_name);
        DIR *pd = opendir(pdir);
        if (!pd) continue;
        char bundle[1024];
        snprintf(bundle, sizeof bundle, "%s/%s", dir, ent->d_name);
        int user = strcmp(bundle, user_bundle) == 0;
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
                if (!dup && strlen(path) < sizeof p.file) {
                    snprintf(p.file, sizeof p.file, "%s", path);
                    p.user = (unsigned char)user;
                    list[count + added++] = p;
                }
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
    /* the user's bundle is always shown, even when its folder is not on the LV2 path */
    char ub[1024];
    if (eb_user_bundle_dir(ub, sizeof ub) == 0) {
        char *slash = strrchr(ub, '/');
        if (slash && slash != ub) {
            *slash = '\0';
            count += eb_presets_scan_dir(ub, list, count, max);
        }
    }
    qsort(list, (size_t)count, sizeof *list, by_label);
    return count;
}

/* ---- the user's bundle ---------------------------------------------------- */

void eb_preset_clean_label(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    int space = 1;                                   /* trims leading blanks */
    for (const unsigned char *c = (const unsigned char *)in; *c && n + 1 < cap && n < EB_USER_LABEL_MAX; c++) {
        if (*c < 32 || *c == 127) continue;
        if (*c == '"' || *c == '\\') {
            out[n++] = '\'';
            space = 0;
        } else if (*c == ' ') {
            if (!space) out[n++] = ' ';
            space = 1;
        } else {
            out[n++] = (char)*c;
            space = 0;
        }
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = '\0';
    if (!n) snprintf(out, cap, "Untitled");
}

void eb_preset_slug(const char *label, char *out, size_t cap)
{
    size_t n = 0;
    int dash = 1;
    for (const unsigned char *c = (const unsigned char *)label; *c && n + 1 < cap; c++) {
        if (isalnum(*c)) {
            out[n++] = (char)tolower(*c);
            dash = 0;
        } else if (!dash) {
            out[n++] = '-';
            dash = 1;
        }
    }
    while (n && out[n - 1] == '-') n--;
    out[n] = '\0';
    if (!n) snprintf(out, cap, "preset");
}

static int mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

static int presets_dir(char *out, size_t cap)
{
    char b[900];
    if (eb_user_bundle_dir(b, sizeof b) != 0) return -1;
    snprintf(out, cap, "%s/presets", b);
    return 0;
}

int eb_user_manifest_rebuild(void)
{
    char pd[1024], bd[900], mf[1100];
    if (presets_dir(pd, sizeof pd) != 0 || eb_user_bundle_dir(bd, sizeof bd) != 0) return -1;
    snprintf(mf, sizeof mf, "%s/manifest.ttl", bd);
    FILE *out = fopen(mf, "w");
    if (!out) return -1;
    DIR *d = opendir(pd);
    struct dirent *ent;
    char head[2048];
    while (d && (ent = readdir(d)) != NULL) {
        if (!has_suffix(ent->d_name, ".ttl")) continue;
        char path[1536];
        snprintf(path, sizeof path, "%s/%s", pd, ent->d_name);
        if (!read_file(path, head, sizeof head)) continue;
        const char *u = strstr(head, "\n<");
        const char *e = u ? strchr(u + 2, '>') : NULL;
        if (!e) continue;
        fprintf(out,
                "<%.*s>\n    a <http://lv2plug.in/ns/ext/presets#Preset> ;\n"
                "    <http://lv2plug.in/ns/lv2core#appliesTo> <" PLUGIN_URI "> ;\n"
                "    <http://www.w3.org/2000/01/rdf-schema#seeAlso> <presets/%s> .\n\n",
                (int)(e - u - 2), u + 2, ent->d_name);
    }
    if (d) closedir(d);
    return fclose(out) == 0 ? 0 : -1;
}

int eb_preset_save(const char *label, const float *values, char *file_out, size_t cap)
{
    char clean[EB_PRESET_LABEL_MAX], slug[96], pd[1024];
    eb_preset_clean_label(label, clean, sizeof clean);
    eb_preset_slug(clean, slug, sizeof slug);
    if (presets_dir(pd, sizeof pd) != 0 || mkdir_p(pd) != 0) return -1;
    char path[1400], tmp[1420];
    snprintf(path, sizeof path, "%s/%s.ttl", pd, slug);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f,
            "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
            "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
            "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n\n"
            "<" PLUGIN_URI "/user/%s>\n    a pset:Preset ;\n    lv2:appliesTo <" PLUGIN_URI "> ;\n"
            "    rdfs:label \"%s\" ;\n    lv2:port\n",
            slug, clean);
    int first = 1;
    for (int i = 0; i < P_COUNT; i++) {
        if (!preset_port(i)) continue;
        float v = values[i];
        if (v < eb_port_min[i]) v = eb_port_min[i];
        if (v > eb_port_max[i]) v = eb_port_max[i];
        fprintf(f, "%s        [ lv2:symbol \"%s\" ; pset:value %.9g ]", first ? "" : " ,\n", eb_port_symbol[i], (double)v);
        first = 0;
    }
    fprintf(f, " .\n");
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    if (file_out) snprintf(file_out, cap, "%s", path);
    return eb_user_manifest_rebuild();
}

int eb_preset_delete(const char *file)
{
    char pd[1024];
    if (presets_dir(pd, sizeof pd) != 0) return -1;
    size_t n = strlen(pd);
    if (strncmp(file, pd, n) != 0 || file[n] != '/' || strstr(file, "..") || strchr(file + n + 1, '/') ||
        !has_suffix(file, ".ttl")) {
        return -1;                                   /* never touch anything outside the user's bundle */
    }
    if (remove(file) != 0) return -1;
    return eb_user_manifest_rebuild();
}

int eb_preset_import_ttl(const char *path, char *label_out, size_t cap)
{
    char *buf = malloc(FILE_MAX);
    if (!buf) return -1;
    eb_preset p;
    int ok = read_file(path, buf, FILE_MAX) && eb_preset_parse(buf, &p);
    free(buf);
    if (!ok) return -1;
    float values[P_COUNT];
    for (int i = 0; i < P_COUNT; i++) values[i] = p.has[i] ? p.value[i] : eb_port_default[i];
    char clean[EB_PRESET_LABEL_MAX];
    eb_preset_clean_label(p.label, clean, sizeof clean);
    if (eb_preset_save(clean, values, NULL, 0) != 0) return -1;
    if (label_out) snprintf(label_out, cap, "%s", clean);
    return 0;
}
