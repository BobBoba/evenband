#include "eb_ui_core.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "eb_presets.h"
#include "eb_spawn.h"

enum {
    WIDTH = 960,
    HEIGHT = 350,
    ROW_H = 24,
    LIST_ROWS = 5,
    MAX_WIDGETS = 80,
    BAND_COUNT = 5,
    HIST_N = 240
};

enum { W_KNOB, W_TOGGLE, W_SEG, W_BUTTON, W_LIST };
enum { A_NONE, A_NEW, A_SAVE, A_SAVE_AS, A_DELETE, A_IMPORT, A_RESET, A_SETTINGS };
enum { IMPORT_NONE, IMPORT_KDIALOG, IMPORT_ZENITY, IMPORT_CONVERT };

static const char *const BAND_NAME[BAND_COUNT] = {"Deep Bass", "Warmth", "Mids", "Presence", "Brilliance"};
static const char *const BUTTON_CAPTION[] = {"", "New", "Save", "Save as...", "Delete", "Import...", "Reset integrated", "Settings"};
static const char *const SUFFIX = " (BreakawayOne)";

typedef struct {
    int type;
    int port;                 /* control port, -1 for buttons and the list */
    int action;
    double x, y, w, h;
    int cell;                 /* knob inside the band table: smaller, no caption */
} widget;

struct eb_ui {
    eb_ui_write_fn write;
    void *ctx;

    int count;                /* presets, entry 0 is the built-in default */
    eb_preset preset[EB_PRESETS_MAX + 1];
    float cur[P_COUNT];
    int selected;

    widget w[MAX_WIDGETS];
    int nw;
    int hover, drag, drag_list;
    int show_settings;        /* knobs and band table visible; off by default */
    double tx, tw;            /* telemetry area: left edge and width */
    double drag_y0, drag_f0;  /* knob drag: pointer height and knob position when it started */
    int list_first;
    int last_w;
    double last_click;

    char last_label[EB_PRESET_LABEL_MAX];   /* the preset most recently applied or saved */
    int last_user;

    int dlg;
    int dlg_fresh;            /* the proposed name is selected: the first key replaces it */
    char dlg_text[EB_USER_LABEL_MAX * 4];
    int dlg_len;

    char toast[128];
    double toast_until, delete_until;

    float hist_in[HIST_N], hist_out[HIST_N];   /* momentary loudness, one point per 0.5 s */
    int hist_n, hist_pos;
    double hist_t;

    eb_spawn sp;
    int stage;
    char convert_label[EB_PRESET_LABEL_MAX];
};

/* ---- helpers ------------------------------------------------------------ */

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void say(eb_ui *ui, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->toast, sizeof ui->toast, fmt, ap);
    va_end(ap);
    ui->toast_until = now() + 5.0;
}

static int is_control(int port) { return port >= P_POWER && !eb_port_is_output[port]; }
static int in_preset(int port) { return is_control(port) && eb_meta[port].in_preset; }

static double frac_of(int port, double v)
{
    double lo = (double)eb_port_min[port], hi = (double)eb_port_max[port];
    if (hi <= lo) return 0;
    double f = eb_meta[port].log && lo > 0 ? (log(fmax(v, lo)) - log(lo)) / (log(hi) - log(lo)) : (v - lo) / (hi - lo);
    return fmax(0.0, fmin(1.0, f));
}

static double value_of(int port, double f)
{
    double lo = (double)eb_port_min[port], hi = (double)eb_port_max[port];
    f = fmax(0.0, fmin(1.0, f));
    return eb_meta[port].log && lo > 0 ? exp(log(lo) + f * (log(hi) - log(lo))) : lo + f * (hi - lo);
}

static void set_port(eb_ui *ui, int port, double v)
{
    if (!is_control(port)) return;
    v = fmax((double)eb_port_min[port], fmin((double)eb_port_max[port], v));
    if (eb_meta[port].integer) v = floor(v + 0.5);
    float f = (float)v;
    if (f == ui->cur[port]) return;
    ui->cur[port] = f;
    if (ui->write) ui->write(ui->ctx, (uint32_t)port, f);
}

static int preset_matches(const eb_ui *ui, int b)
{
    const eb_preset *p = &ui->preset[b];
    int any = 0;
    for (int i = 0; i < P_COUNT; i++) {
        if (!p->has[i] || !in_preset(i)) continue;
        any = 1;
        float tol = 1e-3f * fmaxf(1.0f, fabsf(p->value[i]));
        if (fabsf(ui->cur[i] - p->value[i]) > tol) return 0;
    }
    return any;
}

/* Several presets can hold the same values; the one being worked with wins. */
static void update_selected(eb_ui *ui)
{
    ui->selected = -1;
    for (int b = 0; b < ui->count && ui->last_label[0]; b++) {
        if (strcmp(ui->preset[b].label, ui->last_label) == 0 && preset_matches(ui, b)) {
            ui->selected = b;
            return;
        }
    }
    for (int b = 0; b < ui->count; b++) {
        if (preset_matches(ui, b)) {
            ui->selected = b;
            return;
        }
    }
}

static void build_default(eb_preset *p)
{
    memset(p, 0, sizeof *p);
    snprintf(p->label, sizeof p->label, "Balance (default)");
    for (int i = 0; i < P_COUNT; i++) {
        if (in_preset(i)) {
            p->value[i] = eb_port_default[i];
            p->has[i] = 1;
        }
    }
}

static void ensure_visible(eb_ui *ui, int index)
{
    if (index < 0) return;
    if (index < ui->list_first) ui->list_first = index;
    if (index >= ui->list_first + LIST_ROWS) ui->list_first = index - LIST_ROWS + 1;
    int max_first = ui->count > LIST_ROWS ? ui->count - LIST_ROWS : 0;
    if (ui->list_first > max_first) ui->list_first = max_first;
    if (ui->list_first < 0) ui->list_first = 0;
}

static void rescan(eb_ui *ui, const char *select)
{
    char keep[EB_PRESET_LABEL_MAX] = "";
    if (select) snprintf(keep, sizeof keep, "%s", select);
    else if (ui->selected >= 0) snprintf(keep, sizeof keep, "%s", ui->preset[ui->selected].label);
    ui->count = 1 + eb_presets_scan(&ui->preset[1], EB_PRESETS_MAX);
    update_selected(ui);
    for (int i = 0; i < ui->count && keep[0]; i++) {
        if (strcmp(ui->preset[i].label, keep) == 0) ensure_visible(ui, i);
    }
    ensure_visible(ui, ui->list_first);
}

static void apply_preset(eb_ui *ui, int index)
{
    const eb_preset *p = &ui->preset[index];
    for (int i = 0; i < P_COUNT; i++) {
        if (p->has[i] && in_preset(i)) set_port(ui, i, p->value[i]);
    }
    snprintf(ui->last_label, sizeof ui->last_label, "%s", p->label);
    ui->last_user = p->user;
    update_selected(ui);
}

/* ---- layout ------------------------------------------------------------- */

static widget *add(eb_ui *ui, int type, int port, int action, double x, double y, double w, double h)
{
    widget *a = &ui->w[ui->nw++];
    memset(a, 0, sizeof *a);
    a->type = type;
    a->port = port;
    a->action = action;
    a->x = x;
    a->y = y;
    a->w = w;
    a->h = h;
    return a;
}

enum { CELL_W = 54, KNOB_CELL_H = 60, MATRIX_CELL_H = 42, KNOB_X = 262 };

static void build_layout(eb_ui *ui)
{
    const int full = ui->show_settings;
    ui->nw = 0;
    ui->hover = ui->drag = ui->last_w = -1;
    ui->tx = full ? 652 : KNOB_X;                         /* telemetry area */
    ui->tw = WIDTH - 10 - ui->tx;

    add(ui, W_LIST, -1, A_NONE, 10, 38, 240, ROW_H * LIST_ROWS + 4);
    const double bw3 = (240 - 12) / 3.0;
    add(ui, W_BUTTON, -1, A_NEW, 10, 168, bw3, 24);
    add(ui, W_BUTTON, -1, A_SAVE, 10 + bw3 + 6, 168, bw3, 24);
    add(ui, W_BUTTON, -1, A_SAVE_AS, 10 + 2 * (bw3 + 6), 168, bw3, 24);
    add(ui, W_BUTTON, -1, A_DELETE, 10, 198, 117, 24);
    add(ui, W_BUTTON, -1, A_IMPORT, 133, 198, 117, 24);
    add(ui, W_TOGGLE, P_BYPASS, A_NONE, WIDTH - 10 - 96, 6, 96, 22);
    add(ui, W_BUTTON, -1, A_SETTINGS, WIDTH - 10 - 96 - 6 - 96, 6, 96, 22);
    add(ui, W_BUTTON, -1, A_RESET, ui->tx, full ? 124 : 156, ui->tw, 20);
    if (!full) return;

    add(ui, W_SEG, P_MODE, A_NONE, 10, 230, 240, 24);
    add(ui, W_SEG, P_XOVER_TYPE, A_NONE, 10, 260, 240, 24);
    add(ui, W_TOGGLE, P_KEEP_LEVEL, A_NONE, 10, 290, 117, 24);
    add(ui, W_TOGGLE, P_LIMITER, A_NONE, 133, 290, 117, 24);

    /* general knobs, two rows of up to seven */
    static const int row1[7] = {P_POWER, P_SPEED, P_RANGE_BOOST, P_RANGE_CUT, P_TILT, P_CEILING, P_TRIM};
    static const int row2[6] = {P_TARGET, P_GATE, P_XOVER1, P_XOVER2, P_XOVER3, P_XOVER4};
    for (int c = 0; c < 7; c++) add(ui, W_KNOB, row1[c], A_NONE, KNOB_X + c * CELL_W, 38, CELL_W, KNOB_CELL_H);
    for (int c = 0; c < 6; c++) add(ui, W_KNOB, row2[c], A_NONE, KNOB_X + c * CELL_W, 38 + KNOB_CELL_H, CELL_W, KNOB_CELL_H);

    /* band table: one column per band, one row per parameter */
    static const int kind[4] = {P_DEEP_BASS, P_MIX0, P_ATK0, P_REL0};
    for (int r = 0; r < 4; r++) {
        for (int b = 0; b < BAND_COUNT; b++) {
            add(ui, W_KNOB, kind[r] + b, A_NONE, KNOB_X + CELL_W + b * CELL_W, 168 + r * MATRIX_CELL_H, CELL_W, MATRIX_CELL_H)->cell = 1;
        }
    }
}

static int find_widget(const eb_ui *ui, double x, double y)
{
    for (int i = 0; i < ui->nw; i++) {
        const widget *a = &ui->w[i];
        if (x >= a->x && x < a->x + a->w && y >= a->y && y < a->y + a->h) return i;
    }
    return -1;
}

static int find_port_widget(const eb_ui *ui, int port)
{
    for (int i = 0; i < ui->nw; i++) {
        if (ui->w[i].port == port) return i;
    }
    return -1;
}

/* ---- construction ------------------------------------------------------- */

eb_ui *eb_ui_new(eb_ui_write_fn write, void *ctx)
{
    eb_ui *ui = calloc(1, sizeof *ui);
    if (!ui) return NULL;
    ui->write = write;
    ui->ctx = ctx;
    ui->hover = ui->drag = ui->last_w = -1;
    ui->drag_list = 0;
    for (int i = 0; i < P_COUNT; i++) ui->cur[i] = eb_port_default[i];
    build_default(&ui->preset[0]);
    ui->sp.fd = -1;
    build_layout(ui);
    rescan(ui, NULL);
    if (ui->selected >= 0) {
        snprintf(ui->last_label, sizeof ui->last_label, "%s", ui->preset[ui->selected].label);
        ui->last_user = ui->preset[ui->selected].user;
        ensure_visible(ui, ui->selected);
    }
    return ui;
}

void eb_ui_free(eb_ui *ui)
{
    if (!ui) return;
    eb_spawn_abort(&ui->sp);
    free(ui);
}

void eb_ui_size(const eb_ui *ui, int *width, int *height)
{
    (void)ui;
    *width = WIDTH;
    *height = HEIGHT;
}

void eb_ui_port(eb_ui *ui, uint32_t port, float value)
{
    if (port >= P_COUNT) return;
    ui->cur[port] = value;
    int before = ui->selected;
    update_selected(ui);
    /* The host restored a saved session (or another control changed the values): show the matching preset */
    if (ui->selected != before && ui->selected >= 0 && ui->drag < 0) ensure_visible(ui, ui->selected);
}

/* ---- actions ------------------------------------------------------------ */

static void open_dialog(eb_ui *ui, const char *initial)
{
    ui->dlg = 1;
    snprintf(ui->dlg_text, sizeof ui->dlg_text, "%s", initial);
    ui->dlg_len = (int)strlen(ui->dlg_text);
    ui->dlg_fresh = ui->dlg_len > 0;
}

static void save_as(eb_ui *ui, const char *name)
{
    char label[EB_PRESET_LABEL_MAX];
    eb_preset_clean_label(name, label, sizeof label);
    for (int i = 1; i < ui->count; i++) {                /* never hide an installed preset behind a user one */
        if (!ui->preset[i].user && strcmp(ui->preset[i].label, label) == 0) {
            size_t n = strlen(label);
            if (n + 8 < sizeof label) snprintf(label + n, sizeof label - n, " (mine)");
            break;
        }
    }
    char file[512];
    if (eb_preset_save(label, ui->cur, file, sizeof file) != 0) {
        say(ui, "Could not save the preset");
        return;
    }
    snprintf(ui->last_label, sizeof ui->last_label, "%s", label);
    ui->last_user = 1;
    rescan(ui, label);
    say(ui, "Saved \"%s\"", label);
}

static void do_save(eb_ui *ui)
{
    if (ui->last_user && ui->last_label[0]) save_as(ui, ui->last_label);
    else open_dialog(ui, "My preset");
}

static void do_delete(eb_ui *ui)
{
    if (ui->selected < 0 || !ui->preset[ui->selected].user) return;
    const eb_preset *p = &ui->preset[ui->selected];
    double t = now();
    if (ui->delete_until < t) {
        ui->delete_until = t + 3.0;
        say(ui, "Click Delete again to remove \"%s\"", p->label);
        return;
    }
    char label[EB_PRESET_LABEL_MAX];
    snprintf(label, sizeof label, "%s", p->label);
    ui->delete_until = 0;
    if (eb_preset_delete(p->file) == 0) {
        ui->last_label[0] = '\0';
        ui->last_user = 0;
        rescan(ui, "");
        say(ui, "Deleted \"%s\"", label);
    } else {
        say(ui, "Could not delete \"%s\"", label);
    }
}

static int has_ext(const char *path, const char *ext)
{
    size_t a = strlen(path), b = strlen(ext);
    return a >= b && strcasecmp(path + a - b, ext) == 0;
}

void eb_ui_import_file(eb_ui *ui, const char *path)
{
    if (has_ext(path, ".ttl")) {
        char label[EB_PRESET_LABEL_MAX];
        if (eb_preset_import_ttl(path, label, sizeof label) == 0) {
            rescan(ui, label);
            say(ui, "Imported \"%s\"", label);
        } else {
            say(ui, "Not an Evenband preset file");
        }
    } else if (has_ext(path, ".amx")) {
        char bundle[900];
        if (eb_user_bundle_dir(bundle, sizeof bundle) != 0) return;
        char *argv[] = {"evenband-amx2lv2", "--add", (char *)path, bundle, NULL};
        if (eb_spawn_start(&ui->sp, argv) == 0) {
            ui->stage = IMPORT_CONVERT;
            say(ui, "Converting...");
        } else {
            say(ui, "Could not start the converter");
        }
    } else {
        say(ui, "Choose a .amx (BreakawayOne) or .ttl (LV2 preset) file");
    }
}

static void start_chooser(eb_ui *ui, int stage)
{
    const char *home = getenv("HOME");
    char *kd[] = {"kdialog", "--getopenfilename", (char *)(home ? home : "/"), "*.amx *.ttl|Presets (*.amx *.ttl)", NULL};
    char *zn[] = {"zenity", "--file-selection", "--title=Import preset", "--file-filter=Presets | *.amx *.ttl", NULL};
    if (eb_spawn_start(&ui->sp, stage == IMPORT_KDIALOG ? kd : zn) == 0) ui->stage = stage;
    else say(ui, "Could not open a file chooser");
}

static void do_import(eb_ui *ui)
{
    if (ui->stage != IMPORT_NONE) return;
    start_chooser(ui, IMPORT_KDIALOG);
}

static void run_action(eb_ui *ui, int action)
{
    switch (action) {
    case A_NEW:
        apply_preset(ui, 0);
        ui->last_label[0] = '\0';
        ui->last_user = 0;
        say(ui, "New settings: defaults");
        break;
    case A_SAVE: do_save(ui); break;
    case A_SAVE_AS: open_dialog(ui, ui->last_label[0] ? ui->last_label : "My preset"); break;
    case A_DELETE: do_delete(ui); break;
    case A_IMPORT: do_import(ui); break;
    case A_SETTINGS:
        ui->show_settings = !ui->show_settings;
        build_layout(ui);
        break;
    case A_RESET:
        set_port(ui, P_METER_RESET, fmod((double)ui->cur[P_METER_RESET] + 1.0, (double)eb_port_max[P_METER_RESET]));
        ui->hist_n = ui->hist_pos = 0;
        say(ui, "Loudness measurement restarted");
        break;
    default: break;
    }
}

int eb_ui_poll(eb_ui *ui)
{
    int redraw = 0;
    double t = now();
    if (ui->toast[0] && t > ui->toast_until) {
        ui->toast[0] = '\0';
        redraw = 1;
    }
    if (ui->delete_until && t > ui->delete_until) {
        ui->delete_until = 0;
        redraw = 1;
    }
    if (t - ui->hist_t >= 0.5) {                          /* one point of loudness history every 0.5 s */
        ui->hist_t = t;
        ui->hist_in[ui->hist_pos] = ui->cur[P_LUFS_IN_M];
        ui->hist_out[ui->hist_pos] = ui->cur[P_LUFS_OUT_M];
        ui->hist_pos = (ui->hist_pos + 1) % HIST_N;
        if (ui->hist_n < HIST_N) ui->hist_n++;
        redraw = 1;
    }
    if (ui->stage != IMPORT_NONE) {
        int st = 0;
        char out[1024];
        if (eb_spawn_poll(&ui->sp, &st, out, sizeof out)) {
            int stage = ui->stage;
            ui->stage = IMPORT_NONE;
            redraw = 1;
            if (stage == IMPORT_KDIALOG && st == 127) {
                start_chooser(ui, IMPORT_ZENITY);
            } else if (stage == IMPORT_ZENITY && st == 127) {
                say(ui, "No file chooser found (install kdialog or zenity)");
            } else if (stage == IMPORT_CONVERT) {
                if (st == 0) {
                    rescan(ui, NULL);
                    say(ui, "Imported");
                } else if (st == 127) {
                    say(ui, "Install evenband-amx2lv2 to import .amx files");
                } else {
                    say(ui, "The preset file could not be converted");
                }
            } else if (st == 0 && out[0]) {
                eb_ui_import_file(ui, out);
            }
        }
    }
    return redraw;
}

/* ---- events ------------------------------------------------------------- */

static void list_geometry(const eb_ui *ui, const widget **list)
{
    for (int i = 0; i < ui->nw; i++) {
        if (ui->w[i].type == W_LIST) {
            *list = &ui->w[i];
            return;
        }
    }
    *list = NULL;
}

static int list_row_at(const eb_ui *ui, const widget *l, double x, double y)
{
    if (x < l->x || x >= l->x + l->w - (ui->count > LIST_ROWS ? 10 : 0) || y < l->y + 2 || y >= l->y + 2 + ROW_H * LIST_ROWS) return -1;
    int row = ui->list_first + (int)((y - l->y - 2) / ROW_H);
    return row < ui->count ? row : -1;
}

static void scroll_to_fraction(eb_ui *ui, const widget *l, double y)
{
    int max_first = ui->count > LIST_ROWS ? ui->count - LIST_ROWS : 0;
    double f = (y - l->y - 2) / (ROW_H * LIST_ROWS);
    ui->list_first = (int)floor(f * (max_first + 1));
    if (ui->list_first < 0) ui->list_first = 0;
    if (ui->list_first > max_first) ui->list_first = max_first;
}

static int dialog_geometry(double *x, double *y, double *w, double *h)
{
    *w = 380;
    *h = 124;
    *x = (WIDTH - *w) / 2;
    *y = (HEIGHT - *h) / 2;
    return 1;
}

static void dialog_buttons(double *okx, double *cx, double *by, double *bw)
{
    double x, y, w, h;
    dialog_geometry(&x, &y, &w, &h);
    *bw = 80;
    *by = y + h - 36;
    *okx = x + w - 2 * 80 - 22;
    *cx = x + w - 80 - 12;
}

static void finish_dialog(eb_ui *ui, int accept)
{
    ui->dlg = 0;
    if (accept && ui->dlg_len > 0) save_as(ui, ui->dlg_text);
}

int eb_ui_press(eb_ui *ui, double x, double y, int button)
{
    if (ui->dlg) {
        double okx, cx, by, bw;
        dialog_buttons(&okx, &cx, &by, &bw);
        if (button == 1 && y >= by && y < by + 26) {
            if (x >= okx && x < okx + bw) finish_dialog(ui, 1);
            else if (x >= cx && x < cx + bw) finish_dialog(ui, 0);
        }
        return 1;
    }
    double t = now();
    int i = find_widget(ui, x, y);
    if (i < 0) return 0;
    widget *a = &ui->w[i];
    int redraw = 1;
    switch (a->type) {
    case W_KNOB:
        if (button == 3 || (button == 1 && ui->last_w == i && t - ui->last_click < 0.35)) {
            set_port(ui, a->port, (double)eb_port_default[a->port]);
            ui->drag = -1;
        } else if (button == 1) {
            ui->drag = i;                                 /* drag up or down to turn the knob */
            ui->drag_y0 = y;
            ui->drag_f0 = frac_of(a->port, (double)ui->cur[a->port]);
        }
        break;
    case W_TOGGLE:
        if (button == 1) set_port(ui, a->port, ui->cur[a->port] >= 0.5f ? 0.0 : 1.0);
        break;
    case W_SEG:
        if (button == 1) {
            int n = eb_meta[a->port].n_labels;
            int seg = n > 0 ? (int)((x - a->x) / (a->w / n)) : 0;
            if (seg >= n) seg = n - 1;
            set_port(ui, a->port, seg);
        }
        break;
    case W_BUTTON:
        if (button == 1) run_action(ui, a->action);
        break;
    case W_LIST: {
        int r = list_row_at(ui, a, x, y);
        if (r >= 0 && button == 1) {
            apply_preset(ui, r);
            ui->delete_until = 0;
        } else if (r < 0 && ui->count > LIST_ROWS && x >= a->x + a->w - 10 && button == 1) {
            ui->drag_list = 1;
            scroll_to_fraction(ui, a, y);
        }
        break;
    }
    default:
        redraw = 0;
        break;
    }
    ui->last_w = i;
    ui->last_click = t;
    return redraw;
}

int eb_ui_motion(eb_ui *ui, double x, double y, int left_down)
{
    if (ui->dlg) return 0;
    if (!left_down) {
        ui->drag = -1;
        ui->drag_list = 0;
    }
    if (ui->drag >= 0) {
        const widget *a = &ui->w[ui->drag];
        set_port(ui, a->port, value_of(a->port, ui->drag_f0 + (ui->drag_y0 - y) * 0.005));
        return 1;
    }
    if (ui->drag_list) {
        const widget *l;
        list_geometry(ui, &l);
        if (l) scroll_to_fraction(ui, l, y);
        return 1;
    }
    int h = find_widget(ui, x, y);
    if (h == ui->hover) return 0;
    ui->hover = h;
    return 1;
}

int eb_ui_release(eb_ui *ui, double x, double y, int button)
{
    (void)x; (void)y;
    if (button == 1) {
        ui->drag = -1;
        ui->drag_list = 0;
    }
    return 0;
}

int eb_ui_scroll(eb_ui *ui, double x, double y, int direction)
{
    if (ui->dlg) return 0;
    int i = find_widget(ui, x, y);
    if (i < 0) return 0;
    const widget *a = &ui->w[i];
    if (a->type == W_LIST) {
        int max_first = ui->count > LIST_ROWS ? ui->count - LIST_ROWS : 0;
        ui->list_first -= direction;
        if (ui->list_first < 0) ui->list_first = 0;
        if (ui->list_first > max_first) ui->list_first = max_first;
        return 1;
    }
    if (a->type == W_KNOB) {
        set_port(ui, a->port, value_of(a->port, frac_of(a->port, ui->cur[a->port]) + 0.02 * direction));
        return 1;
    }
    return 0;
}

int eb_ui_wants_keys(const eb_ui *ui) { return ui->dlg; }

static int utf8_encode(uint32_t c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

int eb_ui_key(eb_ui *ui, uint32_t codepoint, int special)
{
    if (!ui->dlg) return 0;
    if (special == EB_KEY_ESCAPE) {
        finish_dialog(ui, 0);
    } else if (special == EB_KEY_ENTER) {
        finish_dialog(ui, 1);
    } else if (special == EB_KEY_BACKSPACE) {
        if (ui->dlg_fresh) {
            ui->dlg_fresh = 0;
            ui->dlg_len = 0;
            ui->dlg_text[0] = '\0';
            return 1;
        }
        while (ui->dlg_len > 0 && (ui->dlg_text[ui->dlg_len - 1] & 0xC0) == 0x80) ui->dlg_len--;
        if (ui->dlg_len > 0) ui->dlg_len--;
        ui->dlg_text[ui->dlg_len] = '\0';
    } else if (codepoint >= 32 && codepoint != 127) {
        char enc[4];
        if (ui->dlg_fresh) {
            ui->dlg_fresh = 0;
            ui->dlg_len = 0;
            ui->dlg_text[0] = '\0';
        }
        int n = utf8_encode(codepoint, enc);
        if (ui->dlg_len + n <= EB_USER_LABEL_MAX) {
            memcpy(ui->dlg_text + ui->dlg_len, enc, (size_t)n);
            ui->dlg_len += n;
            ui->dlg_text[ui->dlg_len] = '\0';
        }
    }
    return 1;
}

/* ---- test access -------------------------------------------------------- */

int eb_ui_preset_count(const eb_ui *ui) { return ui->count; }
const char *eb_ui_preset_label(const eb_ui *ui, int i) { return i >= 0 && i < ui->count ? ui->preset[i].label : ""; }
int eb_ui_preset_is_user(const eb_ui *ui, int i) { return i >= 0 && i < ui->count ? ui->preset[i].user : 0; }
int eb_ui_selected(const eb_ui *ui) { return ui->selected; }
int eb_ui_history_len(const eb_ui *ui) { return ui->hist_n; }
float eb_ui_value(const eb_ui *ui, uint32_t port) { return port < P_COUNT ? ui->cur[port] : 0.0f; }
int eb_ui_list_rows(const eb_ui *ui) { (void)ui; return LIST_ROWS; }
int eb_ui_list_first(const eb_ui *ui) { return ui->list_first; }
const char *eb_ui_toast(const eb_ui *ui) { return ui->toast; }
const char *eb_ui_dialog_text(const eb_ui *ui) { return ui->dlg ? ui->dlg_text : NULL; }

int eb_ui_port_centre(const eb_ui *ui, uint32_t port, int part, double *x, double *y)
{
    int i = find_port_widget(ui, (int)port);
    if (i < 0) return 0;
    const widget *a = &ui->w[i];
    double w = a->w;
    double off = 0;
    if (a->type == W_SEG && eb_meta[port].n_labels > 0) {
        w = a->w / eb_meta[port].n_labels;
        off = part * w;
    }
    *x = a->x + off + w / 2;
    *y = a->y + a->h / 2;
    return 1;
}

int eb_ui_row_centre(const eb_ui *ui, int index, double *x, double *y)
{
    const widget *l;
    list_geometry(ui, &l);
    if (!l || index < ui->list_first || index >= ui->list_first + LIST_ROWS || index >= ui->count) return 0;
    *x = l->x + 40;
    *y = l->y + 2 + (index - ui->list_first) * ROW_H + ROW_H / 2.0;
    return 1;
}

int eb_ui_button_centre(const eb_ui *ui, const char *caption, double *x, double *y)
{
    for (int i = 0; i < ui->nw; i++) {
        if (ui->w[i].type == W_BUTTON && strcmp(BUTTON_CAPTION[ui->w[i].action], caption) == 0) {
            *x = ui->w[i].x + ui->w[i].w / 2;
            *y = ui->w[i].y + ui->w[i].h / 2;
            return 1;
        }
    }
    return 0;
}

/* ---- drawing ------------------------------------------------------------ */

static void colour(cairo_t *cr, unsigned rgb, double alpha)
{
    cairo_set_source_rgba(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0, alpha);
}

static void round_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* Shows the text, shortened with an ellipsis if it does not fit in max_w. */
static void fit_text(cairo_t *cr, const char *text, double x, double y, double max_w)
{
    char buf[EB_PRESET_LABEL_MAX + 8];
    snprintf(buf, sizeof buf, "%s", text);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    size_t len = strlen(buf);
    while (ext.x_advance > max_w && len > 1) {
        len--;
        while (len > 1 && (buf[len] & 0xC0) == 0x80) len--;
        memcpy(buf + len, "\xE2\x80\xA6", 4);
        cairo_text_extents(cr, buf, &ext);
    }
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, buf);
}

static void centred(cairo_t *cr, const char *text, double cx, double y, double max_w)
{
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    double w = fmin(ext.x_advance, max_w);
    fit_text(cr, text, cx - w / 2, y, max_w);
}

static void display_label(const char *label, char *out, size_t cap)
{
    snprintf(out, cap, "%s", label);
    size_t n = strlen(out), s = strlen(SUFFIX);
    if (n > s && strcmp(out + n - s, SUFFIX) == 0) out[n - s] = '\0';
}

static void format_value(int port, double v, int with_unit, char *out, size_t cap)
{
    const char *u = eb_meta[port].unit;
    int bipolar = eb_port_min[port] < 0.0f;
    if (!strcmp(u, "pc")) snprintf(out, cap, with_unit ? "%.0f %%" : "%.0f", v);
    else if (!strcmp(u, "hz")) {
        if (v >= 1000.0) snprintf(out, cap, with_unit ? "%.2f kHz" : "%.2fk", v / 1000.0);
        else snprintf(out, cap, with_unit ? "%.0f Hz" : "%.0f", v);
    } else if (!strcmp(u, "s")) {
        if (v < 1.0) snprintf(out, cap, with_unit ? "%.0f ms" : "%.0fms", v * 1000.0);
        else snprintf(out, cap, with_unit ? "%.2f s" : "%.2fs", v);
    } else if (!strcmp(u, "db")) {
        snprintf(out, cap, bipolar ? (with_unit ? "%+.1f dB" : "%+.1f") : (with_unit ? "%.1f dB" : "%.1f"), v);
    } else {
        snprintf(out, cap, "%.2f", v);
    }
}

static int widget_enabled(const eb_ui *ui, const widget *a)
{
    if (a->type == W_BUTTON && a->action == A_DELETE) return ui->selected >= 0 && ui->preset[ui->selected].user;
    return 1;
}

static const char *short_name(int port)
{
    switch (port) {
    case P_POWER: return "Power";
    case P_SPEED: return "Speed";
    case P_RANGE_BOOST: return "Boost";
    case P_RANGE_CUT: return "Cut";
    case P_TILT: return "Tilt";
    case P_CEILING: return "Ceiling";
    case P_TRIM: return "Trim";
    case P_TARGET: return "Target";
    case P_GATE: return "Gate";
    case P_XOVER1: return "X-over 1";
    case P_XOVER2: return "X-over 2";
    case P_XOVER3: return "X-over 3";
    case P_XOVER4: return "X-over 4";
    default: return eb_meta[port].name;
    }
}

/* A rotary knob: 270 degree sweep, bipolar ranges fill from the zero position. */
static void draw_knob(eb_ui *ui, cairo_t *cr, const widget *a, int hot)
{
    const double a0 = 0.75 * M_PI, a1 = 2.25 * M_PI;
    double v = (double)ui->cur[a->port], f = frac_of(a->port, v);
    int bipolar = eb_port_min[a->port] < 0.0f && eb_port_max[a->port] > 0.0f;
    double r = a->cell ? 12.5 : 15.0;
    double cx = a->x + a->w / 2, cy = a->y + (a->cell ? 2.0 : 13.0) + r;
    char buf[40];

    cairo_set_font_size(cr, 10);
    if (!a->cell) {
        colour(cr, 0x9aa5b4, 1);
        centred(cr, short_name(a->port), cx, a->y + 9, a->w - 2);
    }
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    colour(cr, 0x39414f, 1);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, a0, a1);
    cairo_stroke(cr);
    double av = a0 + f * (a1 - a0), az = bipolar ? a0 + frac_of(a->port, 0.0) * (a1 - a0) : a0;
    if (fabs(av - az) > 0.01) {
        colour(cr, hot ? 0x78b8f5 : 0x4f95e0, 1);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, r, fmin(av, az), fmax(av, az));
        cairo_stroke(cr);
    }
    cairo_arc(cr, cx, cy, r - 5.0, 0, 2 * M_PI);
    colour(cr, hot ? 0x3a4350 : 0x2d333d, 1);
    cairo_fill(cr);
    cairo_set_line_width(cr, 2.0);
    colour(cr, 0xf2f5f9, 1);
    cairo_move_to(cr, cx + cos(av) * (r - 10.0), cy + sin(av) * (r - 10.0));
    cairo_line_to(cr, cx + cos(av) * (r - 4.0), cy + sin(av) * (r - 4.0));
    cairo_stroke(cr);

    format_value(a->port, v, 0, buf, sizeof buf);
    cairo_set_font_size(cr, 10.5);
    colour(cr, 0xe6ebf2, 1);
    centred(cr, buf, cx, cy + r + 12.0, a->w - 2);
}

static void draw_widget(eb_ui *ui, cairo_t *cr, int idx)
{
    const widget *a = &ui->w[idx];
    int hot = idx == ui->hover || idx == ui->drag;
    char buf[64];
    switch (a->type) {
    case W_KNOB:
        draw_knob(ui, cr, a, hot);
        break;
    case W_TOGGLE: {
        int on = ui->cur[a->port] >= 0.5f;
        round_rect(cr, a->x, a->y, a->w, a->h, 5);
        if (on) colour(cr, a->port == P_BYPASS ? 0xc9742a : 0x2f7d5b, 1);
        else colour(cr, hot ? 0x3d4655 : 0x2a303a, 1);
        cairo_fill(cr);
        cairo_set_font_size(cr, 12);
        colour(cr, on ? 0xffffff : 0xc2cad6, 1);
        centred(cr, eb_meta[a->port].name, a->x + a->w / 2, a->y + a->h / 2 + 4.3, a->w - 8);
        break;
    }
    case W_SEG: {
        int n = eb_meta[a->port].n_labels, sel = (int)(ui->cur[a->port] + 0.5f);
        double sw = a->w / (n > 0 ? n : 1);
        cairo_set_font_size(cr, 12);
        for (int s = 0; s < n; s++) {
            round_rect(cr, a->x + s * sw + 1, a->y, sw - 2, a->h, 5);
            colour(cr, s == sel ? 0x2f7d5b : (hot ? 0x323946 : 0x2a303a), 1);
            cairo_fill(cr);
            colour(cr, s == sel ? 0xffffff : 0xc2cad6, 1);
            centred(cr, eb_meta[a->port].labels[s], a->x + s * sw + sw / 2, a->y + a->h / 2 + 4.3, sw - 10);
        }
        break;
    }
    case W_BUTTON: {
        int enabled = widget_enabled(ui, a);
        round_rect(cr, a->x, a->y, a->w, a->h, 5);
        colour(cr, a->action == A_SETTINGS && ui->show_settings ? 0x2f6fb5 : hot && enabled ? 0x3d4655 : 0x2a303a, 1);
        cairo_fill(cr);
        cairo_set_font_size(cr, 12);
        colour(cr, enabled ? 0xe6ebf2 : 0x6b7483, 1);
        const char *cap = BUTTON_CAPTION[a->action];
        if (a->action == A_DELETE && ui->delete_until > 0) cap = "Sure?";
        centred(cr, cap, a->x + a->w / 2, a->y + a->h / 2 + 4.3, a->w - 8);
        break;
    }
    case W_LIST: {
        round_rect(cr, a->x, a->y, a->w, a->h, 5);
        colour(cr, 0x242a32, 1);
        cairo_fill(cr);
        cairo_set_font_size(cr, 12.5);
        for (int r = 0; r < LIST_ROWS; r++) {
            int p = ui->list_first + r;
            if (p >= ui->count) break;
            double ry = a->y + 2 + r * ROW_H;
            double rw = a->w - 4 - (ui->count > LIST_ROWS ? 10 : 0);
            if (p == ui->selected || (p == ui->hover)) {
                round_rect(cr, a->x + 2, ry, rw, ROW_H - 2, 4);
                colour(cr, p == ui->selected ? 0x2f7d5b : 0x323946, 1);
                cairo_fill(cr);
            }
            colour(cr, p == ui->selected ? 0xffffff : 0xd6dce5, 1);
            display_label(ui->preset[p].label, buf, sizeof buf);
            fit_text(cr, buf, a->x + 10, ry + ROW_H / 2.0 + 3.8, rw - 26);
            if (ui->preset[p].user) {
                colour(cr, 0xe0a050, 1);
                cairo_arc(cr, a->x + 2 + rw - 11, ry + (ROW_H - 2) / 2.0, 3, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        }
        if (ui->count > LIST_ROWS) {
            double th = a->h - 6, max_first = ui->count - LIST_ROWS;
            double len = fmax(18.0, th * LIST_ROWS / ui->count);
            double ty = a->y + 3 + (th - len) * (ui->list_first / max_first);
            round_rect(cr, a->x + a->w - 9, ty, 5, len, 2.5);
            colour(cr, 0x5b6676, 1);
            cairo_fill(cr);
        }
        break;
    }
    default:
        break;
    }
}

static void draw_dialog(eb_ui *ui, cairo_t *cr)
{
    colour(cr, 0x000000, 0.55);
    cairo_paint(cr);
    double x, y, w, h;
    dialog_geometry(&x, &y, &w, &h);
    round_rect(cr, x, y, w, h, 8);
    colour(cr, 0x2b313b, 1);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    colour(cr, 0xe6ebf2, 1);
    cairo_move_to(cr, x + 16, y + 26);
    cairo_show_text(cr, "Save preset as");
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    round_rect(cr, x + 16, y + 38, w - 32, 28, 5);
    colour(cr, 0x1f2329, 1);
    cairo_fill(cr);
    cairo_set_font_size(cr, 13);
    colour(cr, 0xffffff, 1);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, ui->dlg_text, &ext);
    if (ui->dlg_fresh) {
        colour(cr, 0x2f5d8a, 1);
        cairo_rectangle(cr, x + 22, y + 43, ext.x_advance + 4, 18);
        cairo_fill(cr);
        colour(cr, 0xffffff, 1);
    }
    cairo_move_to(cr, x + 24, y + 57);
    cairo_show_text(cr, ui->dlg_text);
    if (!ui->dlg_fresh) {
        colour(cr, 0x7fd0a8, 1);
        cairo_rectangle(cr, x + 25 + ext.x_advance, y + 44, 1.5, 16);
        cairo_fill(cr);
    }
    double okx, cx, by, bw;
    dialog_buttons(&okx, &cx, &by, &bw);
    for (int b = 0; b < 2; b++) {
        round_rect(cr, b ? cx : okx, by, bw, 26, 5);
        colour(cr, b ? 0x3a414d : 0x2f7d5b, 1);
        cairo_fill(cr);
        colour(cr, 0xffffff, 1);
        cairo_set_font_size(cr, 12);
        centred(cr, b ? "Cancel" : "Save", (b ? cx : okx) + bw / 2, by + 17.5, bw - 8);
    }
    cairo_set_font_size(cr, 11);
    colour(cr, 0x9aa5b4, 1);
    cairo_move_to(cr, x + 16, y + 84);
    cairo_show_text(cr, "Enter saves, Esc cancels");
}

static void lufs_text(double v, char *out, size_t cap)
{
    if (v <= -119.0) snprintf(out, cap, "-");
    else snprintf(out, cap, "%.1f", v);
}

/* Momentary loudness in large figures with a bar, short-term and integrated below. */
static void draw_lufs(eb_ui *ui, cairo_t *cr, double x, double y, double w, double h,
                      const char *title, int pm, int ps, int pi)
{
    const double fs = h > 100 ? 52 : 30;
    char buf[32];
    cairo_text_extents_t ext;
    round_rect(cr, x, y, w, h, 6);
    colour(cr, 0x242a32, 1);
    cairo_fill(cr);
    cairo_set_font_size(cr, 10);
    colour(cr, 0x9aa5b4, 1);
    cairo_move_to(cr, x + 10, y + 14);
    cairo_show_text(cr, title);
    const char *unit = w > 200 ? "LUFS momentary" : "LUFS";
    cairo_text_extents(cr, unit, &ext);
    cairo_move_to(cr, x + w - 10 - ext.x_advance, y + 14);
    cairo_show_text(cr, unit);

    double m = (double)ui->cur[pm];
    lufs_text(m, buf, sizeof buf);
    const double base = y + 16 + fs * 0.95;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    colour(cr, m > -9.0 ? 0xf0c987 : 0xffffff, 1);
    cairo_move_to(cr, x + 10, base);
    cairo_show_text(cr, buf);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    const double bh = h > 100 ? 8 : 6, bx = x + 10, by = base + 8, bw = w - 20;
    round_rect(cr, bx, by, bw, bh, bh / 2);
    colour(cr, 0x2f3641, 1);
    cairo_fill(cr);
    double f = m <= -119.0 ? 0.0 : fmax(0.0, fmin(1.0, (m + 40.0) / 40.0));
    if (f > 0) {
        round_rect(cr, bx, by, bw * f, bh, bh / 2);
        colour(cr, m > -9.0 ? 0xd9822b : 0x4f95e0, 1);
        cairo_fill(cr);
    }
    colour(cr, 0xffffff, 0.8);
    cairo_rectangle(cr, bx + bw * (40.0 - 23.0) / 40.0 - 0.5, by - 2, 1, bh + 4);   /* -23 LUFS, the EBU R128 target */
    cairo_fill(cr);

    const double ty = y + h - 7, fs2 = h > 100 ? 14 : 12;
    for (int k = 0; k < 2; k++) {
        double tx = x + 10 + k * (w / 2);
        cairo_set_font_size(cr, 10);
        colour(cr, 0x9aa5b4, 1);
        const char *cap = k ? "integrated" : "short";
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, cap);
        cairo_text_extents(cr, cap, &ext);
        cairo_set_font_size(cr, fs2);
        colour(cr, 0xe6ebf2, 1);
        lufs_text((double)ui->cur[k ? pi : ps], buf, sizeof buf);
        cairo_move_to(cr, tx + ext.x_advance + 6, ty);
        cairo_show_text(cr, buf);
    }
}

static void draw_band_meters(eb_ui *ui, cairo_t *cr, double x, double y, double w, double h)
{
    round_rect(cr, x, y, w, h, 6);
    colour(cr, 0x242a32, 1);
    cairo_fill(cr);
    cairo_set_font_size(cr, 10);
    colour(cr, 0x9aa5b4, 1);
    cairo_move_to(cr, x + 10, y + 12);
    cairo_show_text(cr, "Gain per band, dB");
    const double top = y + 20, bottom = y + h - 28, half = (bottom - top) / 2, zero = top + half;
    const double col = w / BAND_COUNT, bar = fmin(40.0, col - 8.0);
    char buf[24];
    for (int b = 0; b < BAND_COUNT; b++) {
        double cx = x + (b + 0.5) * col, g = (double)ui->cur[P_METER_DEEP_BASS + b];
        double len = fmax(-24.0, fmin(24.0, g)) / 24.0 * half;
        round_rect(cr, cx - bar / 2, top, bar, bottom - top, 4);
        colour(cr, 0x2a303a, 1);
        cairo_fill(cr);
        colour(cr, g >= 0 ? 0x3aa675 : 0xd9822b, 1);
        cairo_rectangle(cr, cx - bar / 2, len >= 0 ? zero - len : zero, bar, fabs(len));
        cairo_fill(cr);
        colour(cr, 0x7b8696, 1);
        cairo_rectangle(cr, cx - bar / 2, zero - 0.5, bar, 1);
        cairo_fill(cr);
        snprintf(buf, sizeof buf, "%+.1f", g);
        cairo_set_font_size(cr, 11);
        colour(cr, 0xe6ebf2, 1);
        centred(cr, buf, cx, y + h - 15, col - 2);
        cairo_set_font_size(cr, 9.5);
        colour(cr, 0x9aa5b4, 1);
        centred(cr, BAND_NAME[b], cx, y + h - 4, col - 2);
    }
}

static void draw_history(eb_ui *ui, cairo_t *cr, double x, double y, double w, double h)
{
    round_rect(cr, x, y, w, h, 6);
    colour(cr, 0x242a32, 1);
    cairo_fill(cr);
    cairo_set_font_size(cr, 10);
    colour(cr, 0x9aa5b4, 1);
    cairo_move_to(cr, x + 10, y + 13);
    cairo_show_text(cr, "Momentary loudness, last 2 minutes");
    colour(cr, 0x8f9bb0, 1);
    cairo_move_to(cr, x + w - 66, y + 13);
    cairo_show_text(cr, "in");
    colour(cr, 0x5fd0a0, 1);
    cairo_move_to(cr, x + w - 40, y + 13);
    cairo_show_text(cr, "out");

    const double px = x + 30, py = y + 22, pw = w - 38, ph = h - 30;
    static const double marks[3] = {0.0, -23.0, -60.0};
    for (int i = 0; i < 3; i++) {
        double gy = py + (0.0 - marks[i]) / 60.0 * ph;
        colour(cr, i == 1 ? 0x4f95e0 : 0x3a414d, i == 1 ? 0.7 : 1.0);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, px, gy + 0.5);
        cairo_line_to(cr, px + pw, gy + 0.5);
        cairo_stroke(cr);
        char lab[8];
        snprintf(lab, sizeof lab, "%.0f", marks[i]);
        colour(cr, 0x7b8696, 1);
        cairo_set_font_size(cr, 9.5);
        cairo_move_to(cr, x + 6, gy + 3.5);
        cairo_show_text(cr, lab);
    }
    const float *series[2] = {ui->hist_in, ui->hist_out};
    const unsigned colours[2] = {0x8f9bb0, 0x5fd0a0};
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int k = 0; k < 2; k++) {
        colour(cr, colours[k], 1);
        int pen_down = 0;
        for (int n = 0; n < ui->hist_n; n++) {
            int idx = (ui->hist_pos - ui->hist_n + n + HIST_N) % HIST_N;
            double v = (double)series[k][idx];
            double gx = px + (double)(HIST_N - ui->hist_n + n) / (HIST_N - 1) * pw;
            if (v <= -119.0) {
                pen_down = 0;
                continue;
            }
            double gy = py + (0.0 - fmax(-60.0, fmin(0.0, v))) / 60.0 * ph;
            if (pen_down) cairo_line_to(cr, gx, gy);
            else cairo_move_to(cr, gx, gy);
            pen_down = 1;
        }
        cairo_stroke(cr);
    }
}

static void draw_panels(eb_ui *ui, cairo_t *cr)
{
    if (ui->show_settings) {
        /* captions of the band table: parameter names on the left, band names on top */
        static const char *const rows[4] = {"Offset dB", "Mix dB", "Attack", "Release"};
        cairo_set_font_size(cr, 11);
        colour(cr, 0xd6dce5, 1);
        for (int r = 0; r < 4; r++) fit_text(cr, rows[r], KNOB_X, 168 + r * MATRIX_CELL_H + 19, CELL_W - 4);
        cairo_set_font_size(cr, 10.5);
        colour(cr, 0x9aa5b4, 1);
        for (int b = 0; b < BAND_COUNT; b++) centred(cr, BAND_NAME[b], KNOB_X + CELL_W + (b + 0.5) * CELL_W, 163, CELL_W);
        draw_lufs(ui, cr, ui->tx, 38, 144, 80, "INPUT", P_LUFS_IN_M, P_LUFS_IN_S, P_LUFS_IN_I);
        draw_lufs(ui, cr, ui->tx + 154, 38, 144, 80, "OUTPUT", P_LUFS_OUT_M, P_LUFS_OUT_S, P_LUFS_OUT_I);
        draw_band_meters(ui, cr, ui->tx, 150, ui->tw, 92);
        draw_history(ui, cr, ui->tx, 250, ui->tw, 96);
    } else {
        /* settings hidden: the telemetry takes the whole width */
        const double half = (ui->tw - 10) / 2;
        draw_lufs(ui, cr, ui->tx, 38, half, 112, "INPUT", P_LUFS_IN_M, P_LUFS_IN_S, P_LUFS_IN_I);
        draw_lufs(ui, cr, ui->tx + half + 10, 38, half, 112, "OUTPUT", P_LUFS_OUT_M, P_LUFS_OUT_S, P_LUFS_OUT_I);
        const double bw = ui->tw * 0.42;
        draw_band_meters(ui, cr, ui->tx, 184, bw, 156);
        draw_history(ui, cr, ui->tx + bw + 10, 184, ui->tw - bw - 10, 156);
    }

    char lat[48];
    snprintf(lat, sizeof lat, "Latency %.0f samples", (double)ui->cur[P_LATENCY]);
    cairo_set_font_size(cr, 10);
    colour(cr, 0x7b8696, 1);
    cairo_move_to(cr, 10, HEIGHT - 8);
    cairo_show_text(cr, lat);
}

void eb_ui_draw(eb_ui *ui, cairo_t *cr)
{
    colour(cr, 0x1f2329, 1);
    cairo_paint(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    colour(cr, 0xe8ecf1, 1);
    cairo_move_to(cr, 10, 24);
    cairo_show_text(cr, "Evenband");
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);

    char name[EB_PRESET_LABEL_MAX + 16], shown[EB_PRESET_LABEL_MAX];
    if (ui->selected >= 0) display_label(ui->preset[ui->selected].label, name, sizeof name);
    else if (ui->last_label[0]) {
        display_label(ui->last_label, shown, sizeof shown);
        snprintf(name, sizeof name, "%s (modified)", shown);
    } else {
        snprintf(name, sizeof name, "custom settings");
    }
    colour(cr, 0x9aa5b4, 1);
    cairo_move_to(cr, 100, 23);
    cairo_show_text(cr, name);

    const double right_edge = WIDTH - 10 - 96 - 6 - 96 - 14;
    const char *status = ui->toast[0] ? ui->toast : (ui->cur[P_MODE] >= 0.5f ? "AGC mode" : "Balance mode");
    cairo_text_extents_t ext;
    cairo_text_extents(cr, status, &ext);
    colour(cr, ui->toast[0] ? 0xf0c987 : 0x9aa5b4, 1);
    cairo_move_to(cr, right_edge - ext.x_advance, 23);
    cairo_show_text(cr, status);

    for (int i = 0; i < ui->nw; i++) draw_widget(ui, cr, i);

    draw_panels(ui, cr);

    if (ui->dlg) draw_dialog(ui, cr);
}
