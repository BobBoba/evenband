/* Evenband user interface, GTK3 front end (ui:Gtk3UI). Lets a GTK3-capable host such
 * as Carla show the interface as a native Wayland window. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include "eb_ports.h"
#include "eb_ui_core.h"

#define UI_URI EB_PLUGIN_URI "#ui-gtk3"

typedef struct {
    eb_ui *core;
    GtkWidget *area;
    guint timer;
    LV2UI_Write_Function write;
    LV2UI_Controller controller;
} gtk_ui;

static void write_port(void *ctx, uint32_t port, float value)
{
    gtk_ui *u = (gtk_ui *)ctx;
    u->write(u->controller, port, sizeof(float), 0, &value);
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)w;
    eb_ui_draw(((gtk_ui *)data)->core, cr);
    return FALSE;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    gtk_ui *u = (gtk_ui *)data;
    if (e->type != GDK_BUTTON_PRESS || (e->button != 1 && e->button != 3)) return TRUE;
    gtk_widget_grab_focus(w);
    if (eb_ui_press(u->core, e->x, e->y, (int)e->button)) gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    if (eb_ui_release(((gtk_ui *)data)->core, e->x, e->y, (int)e->button)) gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    if (eb_ui_motion(((gtk_ui *)data)->core, e->x, e->y, (e->state & GDK_BUTTON1_MASK) != 0)) gtk_widget_queue_draw(w);
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *e, gpointer data)
{
    if (eb_ui_motion(((gtk_ui *)data)->core, -1, -1, (e->state & GDK_BUTTON1_MASK) != 0)) gtk_widget_queue_draw(w);
    return FALSE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data)
{
    int dir = 0;
    if (e->direction == GDK_SCROLL_UP) dir = 1;
    else if (e->direction == GDK_SCROLL_DOWN) dir = -1;
    else if (e->direction == GDK_SCROLL_SMOOTH) dir = e->delta_y < 0 ? 1 : (e->delta_y > 0 ? -1 : 0);
    if (dir && eb_ui_scroll(((gtk_ui *)data)->core, e->x, e->y, dir)) gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    gtk_ui *u = (gtk_ui *)data;
    if (!eb_ui_wants_keys(u->core)) return FALSE;     /* leave shortcuts to the host */
    int dirty;
    if (e->keyval == GDK_KEY_BackSpace) dirty = eb_ui_key(u->core, 0, EB_KEY_BACKSPACE);
    else if (e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_KP_Enter) dirty = eb_ui_key(u->core, 0, EB_KEY_ENTER);
    else if (e->keyval == GDK_KEY_Escape) dirty = eb_ui_key(u->core, 0, EB_KEY_ESCAPE);
    else dirty = eb_ui_key(u->core, gdk_keyval_to_unicode(e->keyval), EB_KEY_NONE);
    if (dirty) gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean on_tick(gpointer data)
{
    gtk_ui *u = (gtk_ui *)data;
    if (eb_ui_poll(u->core)) gtk_widget_queue_draw(u->area);
    return G_SOURCE_CONTINUE;
}

static LV2UI_Handle instantiate(const LV2UI_Descriptor *d, const char *uri, const char *bundle,
                                LV2UI_Write_Function write, LV2UI_Controller controller,
                                LV2UI_Widget *widget, const LV2_Feature *const *features)
{
    (void)d; (void)uri; (void)bundle; (void)features;
    gtk_ui *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->write = write;
    u->controller = controller;
    u->core = eb_ui_new(write_port, u);
    if (!u->core) {
        free(u);
        return NULL;
    }
    int w, h;
    eb_ui_size(u->core, &w, &h);
    u->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(u->area, w, h);
    gtk_widget_set_can_focus(u->area, TRUE);
    gtk_widget_add_events(u->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                       GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(u->area, "draw", G_CALLBACK(on_draw), u);
    g_signal_connect(u->area, "button-press-event", G_CALLBACK(on_press), u);
    g_signal_connect(u->area, "button-release-event", G_CALLBACK(on_release), u);
    g_signal_connect(u->area, "motion-notify-event", G_CALLBACK(on_motion), u);
    g_signal_connect(u->area, "leave-notify-event", G_CALLBACK(on_leave), u);
    g_signal_connect(u->area, "scroll-event", G_CALLBACK(on_scroll), u);
    g_signal_connect(u->area, "key-press-event", G_CALLBACK(on_key), u);
    u->timer = g_timeout_add(50, on_tick, u);
    *widget = (LV2UI_Widget)u->area;
    return u;
}

static void cleanup(LV2UI_Handle h)
{
    gtk_ui *u = (gtk_ui *)h;
    if (u->timer) g_source_remove(u->timer);
    eb_ui_free(u->core);
    free(u);
}

static void port_event(LV2UI_Handle h, uint32_t port, uint32_t size, uint32_t format, const void *buffer)
{
    gtk_ui *u = (gtk_ui *)h;
    if (format == 0 && size == sizeof(float)) {
        eb_ui_port(u->core, port, *(const float *)buffer);
        gtk_widget_queue_draw(u->area);
    }
}

static const void *extension_data(const char *uri)
{
    (void)uri;
    return NULL;
}

static const LV2UI_Descriptor descriptor = {UI_URI, instantiate, cleanup, port_event, extension_data};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
