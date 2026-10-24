/* Evenband user interface, X11 front end (ui:X11UI). Works in every LV2 host; on a
 * Wayland desktop the host shows it through XWayland. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <cairo-xlib.h>
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include "eb_ports.h"
#include "eb_ui_core.h"

#define UI_URI EB_PLUGIN_URI "#ui"

typedef struct {
    eb_ui *core;
    Display *dpy;
    Window win;
    int width, height;
    int dirty;
    LV2UI_Write_Function write;
    LV2UI_Controller controller;
} x11_ui;

static void write_port(void *ctx, uint32_t port, float value)
{
    x11_ui *u = (x11_ui *)ctx;
    u->write(u->controller, port, sizeof(float), 0, &value);
}

static LV2UI_Handle instantiate(const LV2UI_Descriptor *d, const char *uri, const char *bundle,
                                LV2UI_Write_Function write, LV2UI_Controller controller,
                                LV2UI_Widget *widget, const LV2_Feature *const *features)
{
    (void)d; (void)uri; (void)bundle;
    Window parent = 0;
    LV2UI_Resize *resize = NULL;
    for (int i = 0; features && features[i]; i++) {
        if (!strcmp(features[i]->URI, LV2_UI__parent)) parent = (Window)(uintptr_t)features[i]->data;
        if (!strcmp(features[i]->URI, LV2_UI__resize)) resize = (LV2UI_Resize *)features[i]->data;
    }
    if (!parent) return NULL;                       /* X11UI is only usable embedded in a host window */

    x11_ui *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->write = write;
    u->controller = controller;
    u->core = eb_ui_new(write_port, u);
    u->dpy = XOpenDisplay(NULL);
    if (!u->core || !u->dpy) {
        if (u->core) eb_ui_free(u->core);
        free(u);
        return NULL;
    }
    eb_ui_size(u->core, &u->width, &u->height);
    u->win = XCreateSimpleWindow(u->dpy, parent, 0, 0, (unsigned)u->width, (unsigned)u->height, 0, 0, 0x1f2329);
    XSelectInput(u->dpy, u->win, ExposureMask | ButtonPressMask | PointerMotionMask | LeaveWindowMask | StructureNotifyMask);
    XMapWindow(u->dpy, u->win);
    XFlush(u->dpy);
    if (resize && resize->ui_resize) resize->ui_resize(resize->handle, u->width, u->height);
    *widget = (LV2UI_Widget)(uintptr_t)u->win;
    u->dirty = 1;
    return u;
}

static void cleanup(LV2UI_Handle h)
{
    x11_ui *u = (x11_ui *)h;
    XDestroyWindow(u->dpy, u->win);
    XCloseDisplay(u->dpy);
    eb_ui_free(u->core);
    free(u);
}

static void port_event(LV2UI_Handle h, uint32_t port, uint32_t size, uint32_t format, const void *buffer)
{
    x11_ui *u = (x11_ui *)h;
    if (format == 0 && size == sizeof(float)) {
        eb_ui_port(u->core, port, *(const float *)buffer);
        u->dirty = 1;
    }
}

static void redraw(x11_ui *u)
{
    XWindowAttributes attr;
    if (!XGetWindowAttributes(u->dpy, u->win, &attr)) return;
    cairo_surface_t *s = cairo_xlib_surface_create(u->dpy, u->win, attr.visual, u->width, u->height);
    cairo_t *cr = cairo_create(s);
    cairo_push_group(cr);
    eb_ui_draw(u->core, cr);
    cairo_pop_group_to_source(cr);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
    XFlush(u->dpy);
}

static int idle(LV2UI_Handle h)
{
    x11_ui *u = (x11_ui *)h;
    while (XPending(u->dpy)) {
        XEvent ev;
        XNextEvent(u->dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0) u->dirty = 1;
            break;
        case ConfigureNotify:
            u->width = ev.xconfigure.width;
            u->height = ev.xconfigure.height;
            u->dirty = 1;
            break;
        case MotionNotify:
            if (eb_ui_motion(u->core, ev.xmotion.x, ev.xmotion.y)) u->dirty = 1;
            break;
        case LeaveNotify:
            if (eb_ui_motion(u->core, -1, -1)) u->dirty = 1;
            break;
        case ButtonPress:
            if (ev.xbutton.button == Button1 && eb_ui_click(u->core, ev.xbutton.x, ev.xbutton.y)) u->dirty = 1;
            break;
        default:
            break;
        }
    }
    if (u->dirty) {
        u->dirty = 0;
        redraw(u);
    }
    return 0;
}

static const LV2UI_Idle_Interface idle_iface = {idle};

static const void *extension_data(const char *uri)
{
    return !strcmp(uri, LV2_UI__idleInterface) ? &idle_iface : NULL;
}

static const LV2UI_Descriptor descriptor = {UI_URI, instantiate, cleanup, port_event, extension_data};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
