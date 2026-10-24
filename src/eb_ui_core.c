#include "eb_ui_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eb_presets.h"

enum {
    WIDTH = 660,
    MARGIN = 16,
    GAP = 8,
    COLUMNS = 3,
    CELL_H = 30,
    HEADER_H = 58,
    METER_ROW_H = 24,
    METER_RANGE_DB = 24
};

static const char *const BAND_NAME[5] = {"Deep Bass", "Warmth", "Mids", "Presence", "Brilliance"};
static const char *const SUFFIX = " (BreakawayOne)";

struct eb_ui {
    eb_ui_write_fn write;
    void *ctx;
    int count;                       /* button 0 is the built-in default */
    eb_preset preset[EB_PRESETS_MAX + 1];
    float cur[P_COUNT];
    int hover;
    int selected;
};

static int is_control_input(int port)
{
    return port >= P_POWER && port < P_COUNT && !eb_port_is_output[port];
}

static void build_default(eb_preset *p)
{
    memset(p, 0, sizeof *p);
    snprintf(p->label, sizeof p->label, "Balance (default)");
    for (int i = 0; i < P_COUNT; i++) {
        if (is_control_input(i)) {
            p->value[i] = eb_port_default[i];
            p->has[i] = 1;
        }
    }
}

static void update_selected(eb_ui *ui)
{
    ui->selected = -1;
    for (int b = 0; b < ui->count && ui->selected < 0; b++) {
        const eb_preset *p = &ui->preset[b];
        int match = 1;
        for (int i = 0; i < P_COUNT && match; i++) {
            if (!p->has[i]) continue;
            float tol = 1e-3f * fmaxf(1.0f, fabsf(p->value[i]));
            if (fabsf(ui->cur[i] - p->value[i]) > tol) match = 0;
        }
        if (match) ui->selected = b;
    }
}

eb_ui *eb_ui_new(eb_ui_write_fn write, void *ctx)
{
    eb_ui *ui = calloc(1, sizeof *ui);
    if (!ui) return NULL;
    ui->write = write;
    ui->ctx = ctx;
    ui->hover = -1;
    for (int i = 0; i < P_COUNT; i++) ui->cur[i] = eb_port_default[i];
    build_default(&ui->preset[0]);
    ui->count = 1 + eb_presets_scan(&ui->preset[1], EB_PRESETS_MAX);
    update_selected(ui);
    return ui;
}

void eb_ui_free(eb_ui *ui) { free(ui); }

static int rows(const eb_ui *ui) { return (ui->count + COLUMNS - 1) / COLUMNS; }

static double cell_w(void) { return (WIDTH - 2.0 * MARGIN - (COLUMNS - 1) * GAP) / COLUMNS; }

static void cell_origin(int index, double *x, double *y)
{
    *x = MARGIN + (index % COLUMNS) * (cell_w() + GAP);
    *y = HEADER_H + (index / COLUMNS) * (CELL_H + GAP);
}

static double meter_top(const eb_ui *ui)
{
    return HEADER_H + rows(ui) * (CELL_H + GAP) + 18;
}

void eb_ui_size(const eb_ui *ui, int *width, int *height)
{
    *width = WIDTH;
    *height = (int)(meter_top(ui) + 5 * METER_ROW_H + MARGIN);
}

void eb_ui_port(eb_ui *ui, uint32_t port, float value)
{
    if (port >= P_COUNT) return;
    ui->cur[port] = value;
    update_selected(ui);
}

static int hit(const eb_ui *ui, double x, double y)
{
    for (int i = 0; i < ui->count; i++) {
        double ox, oy;
        cell_origin(i, &ox, &oy);
        if (x >= ox && x < ox + cell_w() && y >= oy && y < oy + CELL_H) return i;
    }
    return -1;
}

int eb_ui_motion(eb_ui *ui, double x, double y)
{
    int h = hit(ui, x, y);
    if (h == ui->hover) return 0;
    ui->hover = h;
    return 1;
}

int eb_ui_click(eb_ui *ui, double x, double y)
{
    int b = hit(ui, x, y);
    if (b < 0) return 0;
    const eb_preset *p = &ui->preset[b];
    for (int i = 0; i < P_COUNT; i++) {
        if (!p->has[i] || !is_control_input(i)) continue;
        ui->cur[i] = p->value[i];
        if (ui->write) ui->write(ui->ctx, (uint32_t)i, p->value[i]);
    }
    update_selected(ui);
    return 1;
}

int eb_ui_button_count(const eb_ui *ui) { return ui->count; }

const char *eb_ui_button_label(const eb_ui *ui, int index)
{
    return index >= 0 && index < ui->count ? ui->preset[index].label : "";
}

int eb_ui_selected(const eb_ui *ui) { return ui->selected; }

void eb_ui_button_centre(const eb_ui *ui, int index, double *x, double *y)
{
    (void)ui;
    cell_origin(index, x, y);
    *x += cell_w() / 2;
    *y += CELL_H / 2.0;
}

/* ---- drawing ----------------------------------------------------------- */

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
    char buf[EB_PRESET_LABEL_MAX + 4];
    snprintf(buf, sizeof buf, "%s", text);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    size_t len = strlen(buf);
    while (ext.x_advance > max_w && len > 1) {
        len--;
        while (len > 1 && (buf[len] & 0xC0) == 0x80) len--;      /* stay on a UTF-8 boundary */
        memcpy(buf + len, "\xE2\x80\xA6", 4);
        cairo_text_extents(cr, buf, &ext);
    }
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, buf);
}

static void display_label(const char *label, char *out, size_t cap)
{
    snprintf(out, cap, "%s", label);
    size_t n = strlen(out), s = strlen(SUFFIX);
    if (n > s && strcmp(out + n - s, SUFFIX) == 0) out[n - s] = '\0';
}

void eb_ui_draw(eb_ui *ui, cairo_t *cr)
{
    int w, h;
    eb_ui_size(ui, &w, &h);
    colour(cr, 0x1f2329, 1);
    cairo_paint(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 22);
    colour(cr, 0xe8ecf1, 1);
    cairo_move_to(cr, MARGIN, 34);
    cairo_show_text(cr, "Evenband");

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    char head[160], name[EB_PRESET_LABEL_MAX];
    if (ui->selected >= 0) {
        display_label(ui->preset[ui->selected].label, name, sizeof name);
    } else {
        snprintf(name, sizeof name, "custom settings");
    }
    snprintf(head, sizeof head, "%s mode  \xC2\xB7  %s", ui->cur[P_MODE] >= 0.5f ? "AGC" : "Balance", name);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, head, &ext);
    colour(cr, 0x9aa5b4, 1);
    cairo_move_to(cr, w - MARGIN - ext.x_advance, 32);
    cairo_show_text(cr, head);

    cairo_set_font_size(cr, 13);
    for (int i = 0; i < ui->count; i++) {
        double x, y;
        cell_origin(i, &x, &y);
        int sel = i == ui->selected, hov = i == ui->hover;
        round_rect(cr, x, y, cell_w(), CELL_H, 6);
        if (sel) colour(cr, 0x2f7d5b, 1);
        else if (hov) colour(cr, 0x3d4655, 1);
        else colour(cr, 0x2a303a, 1);
        cairo_fill(cr);
        colour(cr, sel ? 0xffffff : 0xd6dce5, 1);
        char shown[EB_PRESET_LABEL_MAX];
        display_label(ui->preset[i].label, shown, sizeof shown);
        fit_text(cr, shown, x + 10, y + CELL_H / 2.0 + 4.5, cell_w() - 20);
    }

    double top = meter_top(ui);
    cairo_set_font_size(cr, 11);
    colour(cr, 0x9aa5b4, 1);
    cairo_move_to(cr, MARGIN, top - 6);
    cairo_show_text(cr, "Gain per band, dB");
    const double bar_x = 110, bar_w = w - bar_x - 70;
    for (int b = 0; b < 5; b++) {
        double y = top + b * METER_ROW_H;
        float g = ui->cur[P_METER_DEEP_BASS + b];
        double clamped = fmax(-METER_RANGE_DB, fmin(METER_RANGE_DB, (double)g));
        colour(cr, 0xd6dce5, 1);
        cairo_set_font_size(cr, 12);
        cairo_move_to(cr, MARGIN, y + 14);
        cairo_show_text(cr, BAND_NAME[b]);
        round_rect(cr, bar_x, y + 3, bar_w, 14, 4);
        colour(cr, 0x2a303a, 1);
        cairo_fill(cr);
        double mid = bar_x + bar_w / 2, len = clamped / METER_RANGE_DB * (bar_w / 2);
        colour(cr, g >= 0 ? 0x3aa675 : 0xd9822b, 1);
        cairo_rectangle(cr, len >= 0 ? mid : mid + len, y + 3, fabs(len), 14);
        cairo_fill(cr);
        colour(cr, 0x5b6676, 1);
        cairo_rectangle(cr, mid - 0.5, y + 1, 1, 18);
        cairo_fill(cr);
        char val[24];
        snprintf(val, sizeof val, "%+.1f", (double)g);
        colour(cr, 0xd6dce5, 1);
        cairo_move_to(cr, bar_x + bar_w + 10, y + 14);
        cairo_show_text(cr, val);
    }
}
