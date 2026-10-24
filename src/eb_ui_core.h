/* Evenband user interface core: preset buttons and band-gain meters.
 * Toolkit independent (it only needs cairo); the X11 and GTK3 front ends feed it
 * events and ask it to draw. */
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

/* Return non-zero when the picture changed and must be redrawn. */
int eb_ui_motion(eb_ui *ui, double x, double y);
int eb_ui_click(eb_ui *ui, double x, double y);

/* For tests: number of buttons (the built-in Balance default plus the presets),
 * label of a button, index of the button matching the current port values (-1 if none),
 * and the centre of a button. */
int eb_ui_button_count(const eb_ui *ui);
const char *eb_ui_button_label(const eb_ui *ui, int index);
int eb_ui_selected(const eb_ui *ui);
void eb_ui_button_centre(const eb_ui *ui, int index, double *x, double *y);

#endif
