/* Tests for the preset store and the user interface core (no display needed). Everything
 * happens in a temporary directory; the real ~/.lv2 is never touched. */
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

static char root[256], user_dir[300], inst_dir[300], bin_dir[300], orig_path[2048];
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
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static void mkdirs(const char *path)
{
    char cmd[700];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
    if (system(cmd) != 0) exit(2);
}

static int exists(const char *path) { return access(path, F_OK) == 0; }

static int count_presets(eb_ui *ui, int user_only)
{
    int n = 0;
    for (int i = 1; i < eb_ui_preset_count(ui); i++) n += !user_only || eb_ui_preset_is_user(ui, i);
    return n;
}

static int find_label(eb_ui *ui, const char *label)
{
    for (int i = 0; i < eb_ui_preset_count(ui); i++) if (strcmp(eb_ui_preset_label(ui, i), label) == 0) return i;
    return -1;
}

static void click(eb_ui *ui, double x, double y)
{
    eb_ui_press(ui, x, y, 1);
    eb_ui_release(ui, x, y, 1);
}

static void click_port(eb_ui *ui, uint32_t port, int part)
{
    double x, y;
    if (eb_ui_port_centre(ui, port, part, &x, &y)) click(ui, x, y);
}

static void click_button(eb_ui *ui, const char *caption)
{
    double x, y;
    CHECK(eb_ui_button_centre(ui, caption, &x, &y), "no button '%s'", caption);
    click(ui, x, y);
}

static void type_text(eb_ui *ui, const char *text)
{
    for (const char *c = text; *c; c++) eb_ui_key(ui, (unsigned char)*c, EB_KEY_NONE);
}

static void wait_poll(eb_ui *ui, const char *expect)
{
    for (int i = 0; i < 400; i++) {
        eb_ui_poll(ui);
        if (!expect || strstr(eb_ui_toast(ui), expect)) return;
        usleep(10000);
    }
}

/* ---- tests -------------------------------------------------------------- */

static void test_parser_and_store(void)
{
    printf("[preset parser and store]\n");
    eb_preset p;
    CHECK(eb_preset_parse(preset_text("Foo (BreakawayOne)", 75, 1, ""), &p) == 1, "valid preset rejected");
    CHECK(strcmp(p.label, "Foo (BreakawayOne)") == 0, "label '%s'", p.label);
    CHECK(p.has[P_POWER] && fabsf(p.value[P_POWER] - 75.0f) < 1e-4f, "power not parsed");
    CHECK(p.has[P_MODE] && p.value[P_MODE] == 1.0f && p.has[P_DEEP_BASS], "mode or offset not parsed");
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
    CHECK(w.value[P_RANGE_BOOST] == 24.0f && w.value[P_RANGE_CUT] == 40.0f, "AGC limits must fit the port ranges");

    CHECK(eb_preset_parse("<http://example.org/other> a pset:Preset ; lv2:appliesTo <http://example.org/x> ; rdfs:label \"x\" .", &p) == 0,
          "preset of another plugin accepted");
    CHECK(eb_preset_parse("", &p) == 0, "empty text accepted");

    char slug[96], clean[96];
    eb_preset_slug("../../etc/Pass wd!!", slug, sizeof slug);
    CHECK(strcmp(slug, "etc-pass-wd") == 0, "slug '%s'", slug);
    eb_preset_slug("***", slug, sizeof slug);
    CHECK(strcmp(slug, "preset") == 0, "empty slug '%s'", slug);
    eb_preset_clean_label("  My \"loud\" \\ mix  \n", clean, sizeof clean);
    CHECK(strcmp(clean, "My 'loud' ' mix") == 0, "label '%s'", clean);

    /* save, scan back, overwrite, delete, and refuse to delete elsewhere */
    float v[P_COUNT];
    for (int i = 0; i < P_COUNT; i++) v[i] = eb_port_default[i];
    v[P_POWER] = 33.0f;
    v[P_BYPASS] = 1.0f;
    char file[512];
    CHECK(eb_preset_save("Round Trip", v, file, sizeof file) == 0, "save failed");
    CHECK(exists(file) && strstr(file, "round-trip.ttl"), "file '%s'", file);
    eb_preset list[8];
    int n = eb_presets_scan(list, 8);
    int found = -1;
    for (int i = 0; i < n; i++) if (strcmp(list[i].label, "Round Trip") == 0) found = i;
    CHECK(found >= 0 && list[found].user && fabsf(list[found].value[P_POWER] - 33.0f) < 1e-4f, "saved preset not found again");
    CHECK(found >= 0 && !list[found].has[P_BYPASS], "bypass must not be stored in presets");
    v[P_POWER] = 44.0f;
    CHECK(eb_preset_save("Round Trip", v, file, sizeof file) == 0, "overwrite failed");
    n = eb_presets_scan(list, 8);
    int same = 0;
    for (int i = 0; i < n; i++) same += strcmp(list[i].label, "Round Trip") == 0;
    CHECK(same == 1, "overwrite made %d copies", same);
    char manifest[600];
    snprintf(manifest, sizeof manifest, "%s/manifest.ttl", user_dir);
    FILE *mf = fopen(manifest, "r");
    char mbuf[2048] = "";
    if (mf) { size_t got = fread(mbuf, 1, sizeof mbuf - 1, mf); mbuf[got] = '\0'; fclose(mf); }
    CHECK(strstr(mbuf, "round-trip.ttl") && strstr(mbuf, "evenband/user/round-trip"), "manifest does not list the preset");

    CHECK(eb_preset_delete("/etc/passwd") != 0, "deleted a file outside the bundle");
    char evil[700];
    snprintf(evil, sizeof evil, "%s/presets/../manifest.ttl", user_dir);
    CHECK(eb_preset_delete(evil) != 0 && exists(manifest), "deleted through a parent path");
    CHECK(eb_preset_delete(file) == 0 && !exists(file), "delete failed");
    mf = fopen(manifest, "r");
    mbuf[0] = '\0';
    if (mf) { size_t got = fread(mbuf, 1, sizeof mbuf - 1, mf); mbuf[got] = '\0'; fclose(mf); }
    CHECK(!strstr(mbuf, "round-trip"), "manifest still lists a deleted preset");
}

static void test_list_and_sliders(void)
{
    printf("[ui: list and controls]\n");
    put(inst_dir, "zeta.ttl", preset_text("Zeta (BreakawayOne)", 80, 1, ""));
    put(inst_dir, "alpha.ttl", preset_text("Alpha (BreakawayOne)", 40, 1, ""));
    put(inst_dir, "dup.ttl", preset_text("Alpha (BreakawayOne)", 99, 1, ""));
    put(inst_dir, "other.ttl", "<http://example.org/p> lv2:appliesTo <http://example.org/x> ; rdfs:label \"Other\" .");
    for (int i = 0; i < 6; i++) {
        char name[32], label[32];
        snprintf(name, sizeof name, "m%d.ttl", i);
        snprintf(label, sizeof label, "Mid %d", i);
        put(inst_dir, name, preset_text(label, 10 + i, 1, ""));
    }

    eb_ui *ui = eb_ui_new(fake_write, NULL);
    click_button(ui, "Settings");
    CHECK(eb_ui_preset_count(ui) == 9, "expected 9 entries (default + 8), got %d", eb_ui_preset_count(ui));
    CHECK(strcmp(eb_ui_preset_label(ui, 0), "Balance (default)") == 0, "first entry '%s'", eb_ui_preset_label(ui, 0));
    CHECK(strcmp(eb_ui_preset_label(ui, 1), "Alpha (BreakawayOne)") == 0, "not sorted: '%s'", eb_ui_preset_label(ui, 1));
    CHECK(eb_ui_selected(ui) == 0, "fresh UI shows the default selected, got %d", eb_ui_selected(ui));

    /* the list never shows more than five rows and scrolls */
    CHECK(eb_ui_list_rows(ui) == 5, "list must show 5 rows");
    double x, y;
    int visible = 0;
    for (int i = 0; i < eb_ui_preset_count(ui); i++) visible += eb_ui_row_centre(ui, i, &x, &y);
    CHECK(visible == 5, "%d rows visible at once", visible);
    CHECK(!eb_ui_row_centre(ui, 8, &x, &y), "a hidden row must not be reachable");
    eb_ui_row_centre(ui, 0, &x, &y);
    CHECK(eb_ui_scroll(ui, x, y, -1) == 1 && eb_ui_list_first(ui) == 1, "wheel did not scroll the list");
    for (int i = 0; i < 10; i++) eb_ui_scroll(ui, x, y, -1);
    CHECK(eb_ui_list_first(ui) == 4, "list scrolled past the end: first row %d", eb_ui_list_first(ui));
    CHECK(eb_ui_row_centre(ui, 8, &x, &y), "last row not reachable after scrolling");
    write_count = 0;
    click(ui, x, y);
    CHECK(strcmp(eb_ui_preset_label(ui, eb_ui_selected(ui)), "Zeta (BreakawayOne)") == 0, "clicked row not applied");
    CHECK(written[P_POWER] == 80.0f && written[P_MODE] == 1.0f && write_count == 3, "preset values wrong (%d writes)", write_count);

    /* knobs: dragging up or down turns them, 0.5% of the range per pixel */
    double sx, sy;
    eb_ui_port_centre(ui, P_POWER, 0, &sx, &sy);
    eb_ui_press(ui, sx, sy, 1);
    float p0 = eb_ui_value(ui, P_POWER);
    CHECK(p0 == 80.0f, "a plain press must not move the knob, got %g", (double)p0);
    eb_ui_motion(ui, sx + 30, sy - 20, 1);
    CHECK(fabsf(eb_ui_value(ui, P_POWER) - 90.0f) < 0.01f, "20 px up must add 10, got %g (sideways drift must not matter)", (double)eb_ui_value(ui, P_POWER));
    eb_ui_motion(ui, sx, sy + 40, 1);
    CHECK(fabsf(eb_ui_value(ui, P_POWER) - 60.0f) < 0.01f, "40 px down from the start must give 60, got %g", (double)eb_ui_value(ui, P_POWER));
    eb_ui_motion(ui, sx, sy - 500, 1);
    CHECK(eb_ui_value(ui, P_POWER) == 100.0f, "drag beyond the end must clamp, got %g", (double)eb_ui_value(ui, P_POWER));
    eb_ui_motion(ui, sx, sy + 900, 1);
    CHECK(eb_ui_value(ui, P_POWER) == 0.0f, "drag beyond the start must clamp, got %g", (double)eb_ui_value(ui, P_POWER));
    eb_ui_release(ui, sx, sy, 1);
    write_count = 0;
    eb_ui_motion(ui, sx, sy - 30, 0);
    CHECK(write_count == 0, "moving without a button changed a value");
    eb_ui_press(ui, sx, sy, 3);
    CHECK(eb_ui_value(ui, P_POWER) == 60.0f, "right click must reset to the default 60, got %g", (double)eb_ui_value(ui, P_POWER));
    eb_ui_scroll(ui, sx, sy, 1);
    CHECK(fabsf(eb_ui_value(ui, P_POWER) - 62.0f) < 0.01f, "wheel step: %g", (double)eb_ui_value(ui, P_POWER));
    eb_ui_press(ui, sx, sy, 1);
    eb_ui_motion(ui, sx, sy - 10, 1);
    eb_ui_release(ui, sx, sy - 10, 1);
    eb_ui_press(ui, sx, sy, 1);                       /* quick second press = double click */
    eb_ui_release(ui, sx, sy, 1);
    CHECK(eb_ui_value(ui, P_POWER) == 60.0f, "double click must reset, got %g", (double)eb_ui_value(ui, P_POWER));

    /* logarithmic knob: half a turn is the geometric mean, so the wheel moves it by a ratio */
    double lx, ly;
    eb_ui_port_centre(ui, P_XOVER1, 0, &lx, &ly);
    float l0 = eb_ui_value(ui, P_XOVER1);
    eb_ui_scroll(ui, lx, ly, 1);
    float l1 = eb_ui_value(ui, P_XOVER1);
    eb_ui_scroll(ui, lx, ly, 1);
    float l2 = eb_ui_value(ui, P_XOVER1);
    CHECK(l1 > l0 && l2 > l1 && fabs((double)l2 / l1 - (double)l1 / l0) < 0.02, "log knob steps %g %g %g are not geometric", (double)l0, (double)l1, (double)l2);

    /* toggles, enumerations, bypass */
    float lim = eb_ui_value(ui, P_LIMITER);
    click_port(ui, P_LIMITER, 0);
    CHECK(eb_ui_value(ui, P_LIMITER) == 1.0f - lim, "toggle did not flip");
    click_port(ui, P_MODE, 1);
    CHECK(eb_ui_value(ui, P_MODE) == 1.0f, "second mode segment not selected");
    click_port(ui, P_XOVER_TYPE, 2);
    CHECK(eb_ui_value(ui, P_XOVER_TYPE) == 2.0f, "third overlap segment not selected");
    click_port(ui, P_BYPASS, 0);
    CHECK(eb_ui_value(ui, P_BYPASS) == 1.0f && written[P_BYPASS] == 1.0f, "bypass not switched on");
    eb_ui_row_centre(ui, eb_ui_selected(ui) >= 0 ? eb_ui_selected(ui) : 0, &x, &y);
    click_button(ui, "New");
    CHECK(eb_ui_value(ui, P_BYPASS) == 1.0f, "New and presets must leave bypass alone");
    CHECK(eb_ui_selected(ui) == 0 && eb_ui_value(ui, P_MODE) == 0.0f, "New must restore the defaults");
    click_port(ui, P_BYPASS, 0);
    CHECK(eb_ui_value(ui, P_BYPASS) == 0.0f, "bypass not switched off");

    /* a host change makes the selection follow */
    eb_ui_port(ui, P_POWER, 55.0f);
    CHECK(eb_ui_selected(ui) == -1, "changing a parameter must clear the selection");
    eb_ui_port(ui, P_POWER, 60.0f);
    CHECK(eb_ui_selected(ui) == 0, "restoring the value must select the default again");

    /* a saved session (Carla restoring its project): a fresh window fed with the values of the last preset
     * must highlight it AND scroll the list so that it is visible */
    int last = eb_ui_preset_count(ui) - 1;
    eb_ui_row_centre(ui, 0, &x, &y);
    for (int i = 0; i < 20 && !eb_ui_row_centre(ui, last, &x, &y); i++) eb_ui_scroll(ui, 100, 80, -1);
    eb_ui_row_centre(ui, last, &x, &y);
    click(ui, x, y);
    CHECK(eb_ui_selected(ui) == last, "setup: last preset not selected");
    eb_ui *fresh = eb_ui_new(fake_write, NULL);
    for (uint32_t i = 0; i < P_COUNT; i++) eb_ui_port(fresh, i, eb_ui_value(ui, i));
    CHECK(eb_ui_selected(fresh) >= 0, "restored values must select a preset");
    CHECK(eb_ui_row_centre(fresh, eb_ui_selected(fresh), &x, &y), "restored preset %d is outside the visible rows (first %d)", eb_ui_selected(fresh), eb_ui_list_first(fresh));
    eb_ui_free(fresh);
    eb_ui_free(ui);
}

static void test_save_delete_import(void)
{
    printf("[ui: save, delete, import]\n");
    eb_ui *ui = eb_ui_new(fake_write, NULL);
    click_button(ui, "Settings");
    int base = eb_ui_preset_count(ui);

    /* save as: type a name, backspace, enter */
    click_port(ui, P_POWER, 0);                               /* some change away from the default */
    eb_ui_press(ui, 100, 100, 3);
    float power = eb_ui_value(ui, P_POWER);
    (void)power;
    double x, y;
    eb_ui_port_centre(ui, P_RANGE_CUT, 0, &x, &y);
    eb_ui_press(ui, x - 100, y, 1);
    eb_ui_release(ui, x, y, 1);
    float cut = eb_ui_value(ui, P_RANGE_CUT);
    click_button(ui, "Save as...");
    CHECK(eb_ui_wants_keys(ui) && eb_ui_dialog_text(ui) != NULL, "Save as must open a name dialog");
    CHECK(strlen(eb_ui_dialog_text(ui)) > 0, "the dialog must propose a name");
    eb_ui_key(ui, 'Q', EB_KEY_NONE);
    CHECK(strcmp(eb_ui_dialog_text(ui), "Q") == 0, "the first key must replace the proposed name, got '%s'", eb_ui_dialog_text(ui));
    while (eb_ui_dialog_text(ui) && eb_ui_dialog_text(ui)[0]) eb_ui_key(ui, 0, EB_KEY_BACKSPACE);
    type_text(ui, "My Test \"x\"");
    eb_ui_key(ui, 0x00e9, EB_KEY_NONE);                       /* a non-ASCII letter must survive */
    eb_ui_key(ui, 0, EB_KEY_BACKSPACE);
    CHECK(strcmp(eb_ui_dialog_text(ui), "My Test \"x\"") == 0, "typed text '%s'", eb_ui_dialog_text(ui));
    double bx, by;
    eb_ui_port_centre(ui, P_POWER, 0, &bx, &by);
    eb_ui_press(ui, bx, by, 1);                               /* clicks outside the dialog must not reach the controls */
    float before = eb_ui_value(ui, P_POWER);
    CHECK(before == eb_ui_value(ui, P_POWER), "a control moved while the dialog was open");
    eb_ui_key(ui, 0, EB_KEY_ENTER);
    CHECK(!eb_ui_wants_keys(ui), "dialog still open after Enter");
    int idx = find_label(ui, "My Test 'x'");
    CHECK(idx > 0 && eb_ui_preset_is_user(ui, idx), "saved preset missing or not marked as the user's");
    CHECK(eb_ui_selected(ui) == idx, "the saved preset must be selected");
    CHECK(strstr(eb_ui_toast(ui), "Saved") != NULL, "no confirmation: '%s'", eb_ui_toast(ui));
    char path[700];
    snprintf(path, sizeof path, "%s/presets/my-test-x.ttl", user_dir);
    CHECK(exists(path), "file %s missing", path);

    /* two presets with identical values: the one just saved stays selected */
    CHECK(eb_ui_selected(ui) == idx, "identical presets: the saved one must stay selected");

    /* save again overwrites without a dialog */
    eb_ui_port_centre(ui, P_RANGE_CUT, 0, &x, &y);
    eb_ui_press(ui, x, y, 1);
    eb_ui_motion(ui, x, y - 30, 1);
    eb_ui_release(ui, x, y - 30, 1);
    float cut2 = eb_ui_value(ui, P_RANGE_CUT);
    CHECK(cut2 != cut, "test setup: value did not change");
    click_button(ui, "Save");
    CHECK(!eb_ui_wants_keys(ui), "Save on a user preset must not ask for a name");
    eb_preset check[4];
    char buf[4096] = "";
    FILE *f = fopen(path, "r");
    if (f) { size_t got = fread(buf, 1, sizeof buf - 1, f); buf[got] = '\0'; fclose(f); }
    eb_preset_parse(buf, &check[0]);
    CHECK(check[0].has[P_RANGE_CUT] && fabsf(check[0].value[P_RANGE_CUT] - cut2) < 1e-3f, "overwrite did not store the new value");
    CHECK(count_presets(ui, 1) == 1, "overwrite made a second user preset");

    /* Escape cancels */
    click_button(ui, "Save as...");
    type_text(ui, "zzz");
    eb_ui_key(ui, 0, EB_KEY_ESCAPE);
    CHECK(!eb_ui_wants_keys(ui) && find_label(ui, "My Test 'x'zzz") < 0, "Escape must cancel");

    /* a name that clashes with an installed preset never hides it */
    click_button(ui, "Save as...");
    while (eb_ui_dialog_text(ui)[0]) eb_ui_key(ui, 0, EB_KEY_BACKSPACE);
    type_text(ui, "Zeta (BreakawayOne)");
    eb_ui_key(ui, 0, EB_KEY_ENTER);
    CHECK(find_label(ui, "Zeta (BreakawayOne)") >= 0 && find_label(ui, "Zeta (BreakawayOne) (mine)") >= 0, "clashing name handled wrongly");

    /* delete: needs two clicks, only for the user's presets */
    int zeta = find_label(ui, "Zeta (BreakawayOne)");
    double rx, ry;
    while (!eb_ui_row_centre(ui, zeta, &rx, &ry)) eb_ui_scroll(ui, 100, 80, -1);
    click(ui, rx, ry);
    int count = eb_ui_preset_count(ui);
    click_button(ui, "Delete");
    click_button(ui, "Delete");
    CHECK(eb_ui_preset_count(ui) == count, "an installed preset must not be deletable");
    int mine = find_label(ui, "My Test 'x'");
    while (!eb_ui_row_centre(ui, mine, &rx, &ry)) eb_ui_scroll(ui, 100, 80, mine > eb_ui_list_first(ui) ? -1 : 1);
    click(ui, rx, ry);
    click_button(ui, "Delete");
    CHECK(exists(path) && strstr(eb_ui_toast(ui), "again"), "the first Delete click must only ask for confirmation");
    click_button(ui, "Delete");
    CHECK(!exists(path) && find_label(ui, "My Test 'x'") < 0, "second Delete click did not delete");

    /* import: a preset file, a broken file, and the helper programs */
    char ext[700];
    snprintf(ext, sizeof ext, "%s/outside.ttl", root);
    put(root, "outside.ttl", preset_text("Imported One", 71, 1, ""));
    eb_ui_import_file(ui, ext);
    int imp = find_label(ui, "Imported One");
    CHECK(imp > 0 && eb_ui_preset_is_user(ui, imp), "imported preset missing");
    put(root, "broken.ttl", "nothing useful here");
    snprintf(ext, sizeof ext, "%s/broken.ttl", root);
    eb_ui_import_file(ui, ext);
    CHECK(strstr(eb_ui_toast(ui), "Not an Evenband") != NULL, "broken file accepted: '%s'", eb_ui_toast(ui));
    eb_ui_import_file(ui, "/tmp/notes.txt");
    CHECK(strstr(eb_ui_toast(ui), "Choose") != NULL, "wrong file type accepted");

    /* a fake file chooser and a fake converter on PATH */
    char script[800];
    snprintf(script, sizeof script, "%s/kdialog", bin_dir);
    FILE *k = fopen(script, "w");
    fprintf(k, "#!/bin/sh\necho '%s/chosen.amx'\n", root);
    fclose(k);
    chmod(script, 0755);
    snprintf(script, sizeof script, "%s/evenband-amx2lv2", bin_dir);
    FILE *c = fopen(script, "w");
    fprintf(c, "#!/bin/sh\n[ \"$1\" = \"--add\" ] || exit 3\nmkdir -p \"$3/presets\"\n"
               "cat > \"$3/presets/converted.ttl\" <<'EOT'\n%sEOT\nexit 0\n",
            preset_text("Converted Fake", 12, 1, ""));
    fclose(c);
    chmod(script, 0755);
    put(root, "chosen.amx", "[Breakaway_preset]\n");
    click_button(ui, "Import...");
    wait_poll(ui, "Converting");
    wait_poll(ui, "Imported");
    CHECK(find_label(ui, "Converted Fake") > 0, "import through the chooser and converter did not add the preset (toast '%s')", eb_ui_toast(ui));

    /* without the converter the user is told what to install */
    setenv("PATH", "/nonexistent", 1);
    eb_ui_import_file(ui, "/tmp/some.amx");
    wait_poll(ui, "Install");
    CHECK(strstr(eb_ui_toast(ui), "evenband-amx2lv2") != NULL, "missing converter not reported: '%s'", eb_ui_toast(ui));
    click_button(ui, "Import...");
    wait_poll(ui, "No file chooser");
    CHECK(strstr(eb_ui_toast(ui), "No file chooser") != NULL, "missing chooser not reported: '%s'", eb_ui_toast(ui));
    CHECK(eb_ui_preset_count(ui) > base, "presets lost");
    eb_ui_free(ui);
}

static void test_draw(void)
{
    printf("[ui: drawing]\n");
    eb_ui *ui = eb_ui_new(fake_write, NULL);
    int w, h;
    eb_ui_size(ui, &w, &h);
    CHECK(w >= 800 && w <= 1000 && h >= 300 && h <= 420, "odd size %dx%d", w, h);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    double hx, hy;
    CHECK(!eb_ui_port_centre(ui, P_POWER, 0, &hx, &hy), "settings must be hidden by default");
    CHECK(eb_ui_button_centre(ui, "Reset integrated", &hx, &hy), "telemetry reset button missing while settings are hidden");
    eb_ui_draw(ui, cr);                                       /* wide telemetry layout */
    click_button(ui, "Settings");
    CHECK(eb_ui_port_centre(ui, P_POWER, 0, &hx, &hy), "Settings button must show the knobs");
    click_button(ui, "Settings");
    CHECK(!eb_ui_port_centre(ui, P_POWER, 0, &hx, &hy), "Settings button must hide the knobs again");
    click_button(ui, "Settings");
    eb_ui_port(ui, P_METER_WARMTH, 6.0f);
    eb_ui_port(ui, P_METER_MIDS, -9.0f);
    eb_ui_port(ui, P_LUFS_IN_M, -23.4f);
    eb_ui_port(ui, P_LUFS_OUT_M, -14.2f);
    eb_ui_port(ui, P_LUFS_IN_I, -120.0f);                    /* nothing measured yet: must draw a dash, not crash */
    eb_ui_draw(ui, cr);

    /* loudness history grows with time and the reset button restarts the measurement */
    for (int i = 0; i < 70; i++) { eb_ui_poll(ui); usleep(10000); }
    CHECK(eb_ui_history_len(ui) >= 1, "no history points after 0.7 s: %d", eb_ui_history_len(ui));
    eb_ui_draw(ui, cr);
    write_count = 0;
    float r0 = written[P_METER_RESET];
    click_button(ui, "Reset integrated");
    CHECK(write_count == 1 && written[P_METER_RESET] == r0 + 1.0f, "reset button must bump the counter once (%d writes, %g -> %g)", write_count, (double)r0, (double)written[P_METER_RESET]);
    CHECK(eb_ui_history_len(ui) == 0, "history must restart after reset, got %d", eb_ui_history_len(ui));
    click_button(ui, "Save as...");
    eb_ui_draw(ui, cr);                                       /* with the dialog */
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
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    eb_ui_free(ui);
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/eb_ui_test_XXXXXX");
    if (!mkdtemp(root)) return 2;
    snprintf(inst_dir, sizeof inst_dir, "%s/lv2/x.lv2/presets", root);
    snprintf(user_dir, sizeof user_dir, "%s/lv2/my-presets.lv2", root);
    snprintf(bin_dir, sizeof bin_dir, "%s/bin", root);
    mkdirs(inst_dir);
    mkdirs(bin_dir);
    char lv2[300], lv2x[2400];
    snprintf(lv2, sizeof lv2, "%s/lv2", root);
    setenv("LV2_PATH", lv2, 1);
    setenv("EVENBAND_USER_DIR", user_dir, 1);
    setenv("HOME", root, 1);
    snprintf(orig_path, sizeof orig_path, "%s", getenv("PATH"));
    snprintf(lv2x, sizeof lv2x, "%s:%s", bin_dir, orig_path);
    setenv("PATH", lv2x, 1);

    test_parser_and_store();
    test_list_and_sliders();
    test_save_delete_import();
    test_draw();

    setenv("PATH", orig_path, 1);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0) failures++;
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
