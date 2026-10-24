/* Developer tool: loads an Evenband user interface library the way an LV2 host does, shows
 * it in a window for a few seconds and prints every port write the interface makes.
 *
 *   build:  make build/ui_harness_x11 build/ui_harness_gtk3
 *   run:    ui_harness_x11  build/evenband.lv2/evenband_ui_x11.so  [seconds]
 *           ui_harness_gtk3 build/evenband.lv2/evenband_ui_gtk3.so [seconds]
 * Used with Xvfb, xdotool and ImageMagick `import` for an end-to-end check without a desktop. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include "eb_ports.h"

#ifdef HARNESS_GTK3
#include <gtk/gtk.h>
#else
#include <X11/Xlib.h>
#endif

static void on_write(LV2UI_Controller c, uint32_t port, uint32_t size, uint32_t format, const void *buf)
{
    (void)c; (void)format;
    if (size == sizeof(float) && port < P_COUNT) printf("write %s = %g\n", eb_port_symbol[port], (double)*(const float *)buf);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    double seconds = argc > 2 ? atof(argv[2]) : 8.0;
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    const LV2UI_Descriptor *(*get)(uint32_t) = (const LV2UI_Descriptor *(*)(uint32_t))dlsym(lib, "lv2ui_descriptor");
    const LV2UI_Descriptor *d = get ? get(0) : NULL;
    if (!d) { fprintf(stderr, "no descriptor\n"); return 1; }
    printf("ui uri: %s\n", d->URI);

    LV2UI_Widget widget = NULL;
    LV2UI_Handle h;
#ifdef HARNESS_GTK3
    gtk_init(&argc, &argv);
    h = d->instantiate(d, EB_PLUGIN_URI, ".", on_write, NULL, &widget, NULL);
    if (!h) { fprintf(stderr, "instantiate failed\n"); return 1; }
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_move(GTK_WINDOW(win), 20, 20);
    gtk_container_add(GTK_CONTAINER(win), (GtkWidget *)widget);
    gtk_widget_show_all(win);
    time_t end = time(NULL) + (time_t)seconds;
    while (time(NULL) < end) {
        while (gtk_events_pending()) gtk_main_iteration();
        struct timespec ts = {0, 20000000};
        nanosleep(&ts, NULL);
    }
#else
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
    Window top = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 20, 20, 700, 640, 0, 0, 0);
    XMapWindow(dpy, top);
    XFlush(dpy);
    LV2_Feature parent = {LV2_UI__parent, (void *)(uintptr_t)top};
    const LV2_Feature *features[] = {&parent, NULL};
    h = d->instantiate(d, EB_PLUGIN_URI, ".", on_write, NULL, &widget, features);
    if (!h) { fprintf(stderr, "instantiate failed\n"); return 1; }
    const LV2UI_Idle_Interface *idle = (const LV2UI_Idle_Interface *)d->extension_data(LV2_UI__idleInterface);
    time_t end = time(NULL) + (time_t)seconds;
    while (time(NULL) < end) {
        if (idle) idle->idle(h);
        struct timespec ts = {0, 20000000};
        nanosleep(&ts, NULL);
    }
#endif
    d->cleanup(h);
    return 0;
}
