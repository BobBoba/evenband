/* Evenband user interface core: every control of the plugin, a preset list with
 * New / Save / Save as / Delete / Import, a bypass switch and live band meters.
 * Toolkit independent (it only needs cairo); the X11 and GTK3 front ends feed it events,
 * call eb_ui_poll from a timer and ask it to draw. */
#ifndef EB_UI_CORE_H
#define EB_UI_CORE_H

#include <stdint.h>

#include <cairo.h>

typedef void (*eb_ui_write_fn)(void *ctx, uint32_t port, float value);

typedef struct eb_ui eb_ui;

/* Scans the installed presets (see eb_presets_scan) and builds the interface. */
eb_ui *eb_ui_new(eb_ui_write_fn write, void *ctx);
void eb_ui_free(eb_ui *ui);

void eb_ui_size(const eb_ui *ui, int *width, int *height);

/* A port value reported by the host. */
void eb_ui_port(eb_ui *ui, uint32_t port, float value);

void eb_ui_draw(eb_ui *ui, cairo_t *cr);

/* Pointer events. button: 1 = left, 3 = right. Each returns non-zero when the picture changed
 * and must be redrawn. */
int eb_ui_motion(eb_ui *ui, double x, double y, int left_down);
int eb_ui_press(eb_ui *ui, double x, double y, int button);
int eb_ui_release(eb_ui *ui, double x, double y, int button);
int eb_ui_scroll(eb_ui *ui, double x, double y, int direction);   /* +1 up, -1 down */

/* Keyboard, only used while a name is being typed (eb_ui_wants_keys). `codepoint` is a
 * Unicode character, or 0 with `special` set to EB_KEY_*. */
enum { EB_KEY_NONE = 0, EB_KEY_BACKSPACE = 1, EB_KEY_ENTER = 2, EB_KEY_ESCAPE = 3 };
int eb_ui_key(eb_ui *ui, uint32_t codepoint, int special);
int eb_ui_wants_keys(const eb_ui *ui);

/* Call about every 50 ms: finishes background work (file chooser, converter) and expires
 * messages. Returns non-zero when a redraw is needed. */
int eb_ui_poll(eb_ui *ui);

/* Imports a preset file (.ttl of this plugin, or a BreakawayOne .amx through the helper
 * program `evenband-amx2lv2`). The Import button calls this with the chosen file. */
void eb_ui_import_file(eb_ui *ui, const char *path);

/* ---- for tests ---------------------------------------------------------- */

/* Presets in the list: button 0 is the built-in default. */
int eb_ui_preset_count(const eb_ui *ui);
const char *eb_ui_preset_label(const eb_ui *ui, int index);
int eb_ui_preset_is_user(const eb_ui *ui, int index);
/* Index of the preset matching the current port values, or -1. */
int eb_ui_selected(const eb_ui *ui);
/* Current value of a port as the interface sees it. */
float eb_ui_value(const eb_ui *ui, uint32_t port);
/* Centre of the control for a port (slider, toggle, meter), or the centre of one segment of
 * an enumeration (segment index in `part`). Returns 0 when the port has no control. */
int eb_ui_port_centre(const eb_ui *ui, uint32_t port, int part, double *x, double *y);
/* Centre of the row of a preset in the list, and whether that row is currently visible. */
int eb_ui_row_centre(const eb_ui *ui, int index, double *x, double *y);
/* Centre of a toolbar button by caption ("New", "Save", "Save as...", "Delete", "Import..."). */
int eb_ui_button_centre(const eb_ui *ui, const char *caption, double *x, double *y);
/* Rows of the list shown at once, first visible row, and the pending message. */
int eb_ui_list_rows(const eb_ui *ui);
int eb_ui_list_first(const eb_ui *ui);
const char *eb_ui_toast(const eb_ui *ui);
/* Name being typed in the Save as dialog, or NULL when no dialog is open. */
const char *eb_ui_dialog_text(const eb_ui *ui);

#endif
