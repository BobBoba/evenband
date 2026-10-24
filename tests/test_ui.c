/* Tests for the preset parser and the user interface core (no display needed). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "eb_presets.h"
#include "eb_ui_core.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL: ");                           \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static float written[P_COUNT];
static int write_count;

static void fake_write(void *ctx, uint32_t port, float value)
{
    (void)ctx;
    written[port] = value;
    write_count++;
}

static const char *preset_text(const char *label, double power, double mode, const char *extra)
{
    static char buf[4][2048];
    static int n;
    char *b = buf[n++ % 4];
    snprintf(b, 2048,
             "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
             "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
             "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n\n"
             "<%s/preset/test>\n    a pset:Preset ;\n    lv2:appliesTo <%s> ;\n"
             "    rdfs:label \"%s\" ;\n    lv2:port\n"
             "        [ lv2:symbol \"power\" ; pset:value %g ] ,\n"
             "        [ lv2:symbol \"mode\" ; pset:value %g ] ,\n"
             "        [ lv2:symbol \"deep_bass\" ; pset:value 2.5 ]%s .\n",
             EB_PLUGIN_URI, EB_PLUGIN_URI, label, power, mode, extra);
    return b;
}

static void put(const char *dir, const char *name, const char *text)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static void test_parser(void)
{
    printf("[preset parser]\n");
    eb_preset p;
    CHECK(eb_preset_parse(preset_text("Foo (BreakawayOne)", 75, 1, ""), &p) == 1, "valid preset rejected");
    CHECK(strcmp(p.label, "Foo (BreakawayOne)") == 0, "label '%s'", p.label);
    CHECK(p.has[P_POWER] && fabsf(p.value[P_POWER] - 75.0f) < 1e-4f, "power not parsed");
    CHECK(p.has[P_MODE] && p.value[P_MODE] == 1.0f, "mode not parsed");
    CHECK(p.has[P_DEEP_BASS] && fabsf(p.value[P_DEEP_BASS] - 2.5f) < 1e-4f, "offset not parsed");
    CHECK(!p.has[P_TILT], "unset port marked as set");

    eb_preset q;
    eb_preset_parse(preset_text("Clamp", 500, 1, ""), &q);
    CHECK(q.value[P_POWER] == 100.0f, "out-of-range value not clamped: %g", (double)q.value[P_POWER]);

    eb_preset r;
    eb_preset_parse(preset_text("Extra", 50, 0,
        " ,\n        [ lv2:symbol \"meter_warmth\" ; pset:value 9 ] ,\n        [ lv2:symbol \"nonsense\" ; pset:value 1 ]"), &r);
    CHECK(!r.has[P_METER_WARMTH], "an output port must never be set by a preset");

    eb_preset w;
    eb_preset_parse(preset_text("Wide", 100, 1,
        " ,\n        [ lv2:symbol \"range_boost\" ; pset:value 24 ] ,\n        [ lv2:symbol \"range_cut\" ; pset:value 40 ]"), &w);
    CHECK(w.value[P_RANGE_BOOST] == 24.0f && w.value[P_RANGE_CUT] == 40.0f,
          "AGC deviation limits must fit the port ranges: boost %g cut %g", (double)w.value[P_RANGE_BOOST], (double)w.value[P_RANGE_CUT]);

    CHECK(eb_preset_parse("<http://example.org/other> a pset:Preset ; lv2:appliesTo <http://example.org/x> ; rdfs:label \"x\" .", &p) == 0,
          "preset of another plugin accepted");
    CHECK(eb_preset_parse("", &p) == 0, "empty text accepted");
}

static void test_ui(void)
{
    printf("[ui core]\n");
    char dir[] = "/tmp/eb_ui_test_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    char sub[600];
    snprintf(sub, sizeof sub, "%s/x.lv2", dir); mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/x.lv2/presets", dir); mkdir(sub, 0755);
    put(sub, "zeta.ttl", preset_text("Zeta (BreakawayOne)", 80, 1, ""));
    put(sub, "alpha.ttl", preset_text("Alpha (BreakawayOne)", 40, 1, ""));
    put(sub, "dup.ttl", preset_text("Alpha (BreakawayOne)", 99, 1, ""));
    put(sub, "other.ttl", "<http://example.org/p> lv2:appliesTo <http://example.org/x> ; rdfs:label \"Other\" .");
    setenv("LV2_PATH", dir, 1);

    eb_ui *ui = eb_ui_new(fake_write, NULL);
    CHECK(eb_ui_button_count(ui) == 3, "expected 3 buttons (default + 2), got %d", eb_ui_button_count(ui));
    CHECK(strcmp(eb_ui_button_label(ui, 0), "Balance (default)") == 0, "first button is '%s'", eb_ui_button_label(ui, 0));
    CHECK(strcmp(eb_ui_button_label(ui, 1), "Alpha (BreakawayOne)") == 0, "presets not sorted: '%s'", eb_ui_button_label(ui, 1));
    CHECK(strcmp(eb_ui_button_label(ui, 2), "Zeta (BreakawayOne)") == 0, "second preset '%s'", eb_ui_button_label(ui, 2));
    CHECK(eb_ui_selected(ui) == 0, "fresh UI at default values must show the default selected, got %d", eb_ui_selected(ui));

    double x, y;
    eb_ui_button_centre(ui, 2, &x, &y);
    write_count = 0;
    CHECK(eb_ui_click(ui, x, y) == 1, "click on a preset did not request a redraw");
    CHECK(write_count == 3, "preset sets 3 ports, wrote %d", write_count);
    CHECK(written[P_POWER] == 80.0f && written[P_MODE] == 1.0f && fabsf(written[P_DEEP_BASS] - 2.5f) < 1e-4f,
          "wrong values written: power %g mode %g", (double)written[P_POWER], (double)written[P_MODE]);
    CHECK(eb_ui_selected(ui) == 2, "clicked preset not selected (%d)", eb_ui_selected(ui));

    eb_ui_port(ui, P_POWER, 55.0f);
    CHECK(eb_ui_selected(ui) == -1, "changing a parameter must clear the selection");
    eb_ui_port(ui, P_POWER, 80.0f);
    CHECK(eb_ui_selected(ui) == 2, "restoring the value must select the preset again");

    eb_ui_button_centre(ui, 0, &x, &y);
    write_count = 0;
    eb_ui_click(ui, x, y);
    CHECK(write_count > 20, "default button must write every control input, wrote %d", write_count);
    CHECK(written[P_MODE] == 0.0f && written[P_POWER] == 60.0f, "default values wrong");
    CHECK(eb_ui_selected(ui) == 0, "default not selected after click");
    CHECK(eb_ui_click(ui, -5, -5) == 0, "click outside any button changed something");

    CHECK(eb_ui_motion(ui, x, y) == 1 && eb_ui_motion(ui, x, y) == 0, "hover must redraw once");

    /* drawing smoke test: a non-trivial picture of the promised size */
    int w, h;
    eb_ui_size(ui, &w, &h);
    CHECK(w > 300 && h > 150, "odd size %dx%d", w, h);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    eb_ui_port(ui, P_METER_WARMTH, 6.0f);
    eb_ui_port(ui, P_METER_MIDS, -9.0f);
    eb_ui_draw(ui, cr);
    cairo_surface_flush(s);
    const unsigned char *d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    unsigned seen[64] = {0};
    int distinct = 0;
    for (int yy = 0; yy < h; yy += 3) {
        for (int xx = 0; xx < w; xx += 3) {
            unsigned c = *(const unsigned *)(d + yy * stride + xx * 4) & 0xffffff;
            int known = 0;
            for (int i = 0; i < distinct; i++) if (seen[i] == c) known = 1;
            if (!known && distinct < 64) seen[distinct++] = c;
        }
    }
    CHECK(distinct >= 6, "picture looks blank: %d distinct colours", distinct);
    CHECK((*(const unsigned *)d & 0xffffff) == 0x1f2329, "background colour wrong: %06x", *(const unsigned *)d & 0xffffff);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    eb_ui_free(ui);

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0, "cleanup");
}

int main(void)
{
    test_parser();
    test_ui();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
