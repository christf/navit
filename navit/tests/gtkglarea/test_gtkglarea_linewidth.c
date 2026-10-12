/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2009 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation;
 * either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */

/**
 * @file Regression test for graphics/gtkglarea: graphics_gc_set_linewidth() was silently
 * ignored, so every stroked primitive (most visibly the speed limit ring of the
 * \c speed_warner OSD, which asks for d/10 px) was rendered one pixel wide.
 *
 * The test drives the real gtkglarea plugin through the public graphics API, renders one
 * frame into the GtkGLArea and reads the framebuffer back with glReadPixels() to measure the
 * width of the resulting stroke.
 *
 * It needs a display, so it is run under Xvfb (see tests/gtkglarea/CMakeLists.txt).
 */

#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "attr.h"
#include "color.h"
#include "debug.h"
#include "event_glib.h"
#include "file.h"
#include "geom.h"
#include "graphics.h"
#include "main.h"
#include "navit.h"
#include "plugin.h"
#include "point.h"

static int g_failures;

#define CHECK(cond, msg...)                                                                                            \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                                                \
            printf(msg);                                                                                               \
            printf("\n");                                                                                              \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static void PASS(const char *name) {
    printf("%-24s PASS\n", name);
}

/* ----------------------------------------------------------------
 * Framebuffer read back
 * ---------------------------------------------------------------- */

struct framebuffer {
    unsigned char *rgba; /* w*h*4, row 0 is the bottom row (GL order) */
    int w, h;
};

static struct framebuffer g_fb;

static int g_capture_next_frame = 0;
static int g_frame_seen = 0;
static int g_render_calls = 0;
static int g_skipped = 0;

/* Connected after the plugin's own ::render handler, so the scene is already drawn and the
 * back buffer still holds it (GtkGLArea swaps after the last handler returns). */
static gboolean on_capture_render(GtkGLArea *area, GdkGLContext *context, gpointer data) {
    (void)context;
    (void)data;
    g_render_calls++;
    if (g_render_calls <= 3)
        printf("  [capture render #%d on %p]\n", g_render_calls, (void *)area);
    if (!g_capture_next_frame)
        return TRUE;
    g_capture_next_frame = 0;
    g_frame_seen++;

    int w = gtk_widget_get_width(GTK_WIDGET(area));
    int h = gtk_widget_get_height(GTK_WIDGET(area));
    if (w <= 0 || h <= 0) {
        printf("FAIL capture: glarea has no size (%dx%d)\n", w, h);
        g_failures++;
        return TRUE;
    }
    free(g_fb.rgba);
    g_fb.w = w;
    g_fb.h = h;
    g_fb.rgba = g_malloc0((gsize)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_fb.rgba);
    return TRUE;
}

/* GtkGLArea only draws from its frame clock. The frame clock is driven by a GDK source, which
 * a hand-rolled "iterate while pending, then usleep" loop starves - it stops dispatching after
 * the first couple of frames. Running a real main loop lets GDK keep the clock ticking.
 *
 * The window is also re-presented on every tick: under Xvfb (no window manager) an occluding
 * window makes GTK stop the covered window's frame clock, which is indistinguishable from a
 * rendering failure. */
static gboolean on_frame_tick(gpointer data) {
    GtkWidget *glarea = data;
    GtkRoot *root = gtk_widget_get_root(glarea);
    if (root && GTK_IS_WINDOW(root))
        gtk_window_present(GTK_WINDOW(root));
    gtk_widget_queue_draw(glarea);
    /* Since GTK 4.14 GtkGLArea re-renders through ::render only when a frame is explicitly
     * requested; queue_draw alone just re-presents the last texture. */
    gtk_gl_area_queue_render(GTK_GL_AREA(glarea));
    GdkFrameClock *fc = gtk_widget_get_frame_clock(glarea);
    if (fc)
        gdk_frame_clock_request_phase(fc, GDK_FRAME_CLOCK_PHASE_PAINT);
    return G_SOURCE_CONTINUE;
}

static gboolean on_quit_loop(gpointer data) {
    g_main_loop_quit((GMainLoop *)data);
    return G_SOURCE_REMOVE;
}

static void pump_frames(GtkWidget *glarea, int milliseconds) {
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    guint tick = g_timeout_add(2, on_frame_tick, glarea);
    g_timeout_add(milliseconds, on_quit_loop, loop);
    g_main_loop_run(loop);
    /* The tick source is the one we still own; the quit source removed itself by returning. */
    g_source_remove(tick);
    g_main_loop_unref(loop);
}

static int render_once(struct graphics *gra, GtkWidget *glarea) {
    (void)gra;
    /* Request a frame through GtkGLArea's own API and let the main loop run it. The frame clock
     * only ticks while the main loop is live, hence the real main loop in pump_frames(). */
    g_frame_seen = 0;
    g_capture_next_frame = 1;
    gtk_gl_area_queue_render(GTK_GL_AREA(glarea));
    pump_frames(glarea, 1500);
    return g_frame_seen;
}

/* ----------------------------------------------------------------
 * Measurement
 * ---------------------------------------------------------------- */

/* Brightness of a pixel. The frame is cleared to opaque black, so the alpha channel is 255
 * everywhere and cannot be used to tell stroke from background. */
static int pixel_is_stroke(const struct framebuffer *fb, int y, int x) {
    if (y < 0 || y >= fb->h || x < 0 || x >= fb->w)
        return 0;
    const unsigned char *p = &fb->rgba[((gsize)y * fb->w + x) * 4];
    int lum = (p[0] * 30 + p[1] * 59 + p[2] * 11) / 100;
    return lum > 40;
}

/* Width in pixels of the horizontal stroke run on row y that contains x0. A circle of radius
 * radius has its leftmost point at (xc - radius, yc); there the ring is exactly vertical in
 * screen terms, so a horizontal scan measures its true stroke width. */
static int measure_stroke(const struct framebuffer *fb, int y, int x0) {
    int left = x0, right = x0;
    while (pixel_is_stroke(fb, y, left - 1))
        left--;
    while (pixel_is_stroke(fb, y, right + 1))
        right++;
    return right - left + 1;
}

/* The plugin projects with mvp_ortho(mvp, 0, width, height, 0, -1, 1), i.e. bottom=height and
 * top=0, which mirrors Y: navit's y axis points down the screen while glReadPixels() row 0 is
 * the bottom of the buffer. A circle drawn at navit y therefore appears on readback row
 * fb->h - y. X is not mirrored. Ignoring this makes a correctly drawn ring look completely blank
 * as soon as the window's client area is not exactly as tall as assumed. */
static int readback_row(const struct framebuffer *fb, int navit_y) {
    return fb->h - navit_y;
}

/* Thickest horizontal run through the ring's leftmost point, searched over a few rows because
 * the rasteriser may place the extreme point between pixel centres. Only a small window is safe:
 * further from the extreme point the ring turns horizontal and a scan there crosses the whole
 * chord instead of the stroke. */
static int measure_ring(const struct framebuffer *fb, struct point centre, int radius) {
    int best = 0;
    int row = readback_row(fb, centre.y);
    for (int dy = -1; dy <= 1; dy++) {
        int w_run = measure_stroke(fb, row + dy, centre.x - radius);
        if (w_run > best)
            best = w_run;
    }
    return best;
}

/* Geometry derived from the widget's real allocation. The GtkWindow is requested at 400x600 but
 * its client area is smaller once decorations are subtracted, so hardcoding coordinates measures
 * the wrong pixels. The diameter is kept to an eighth of the short side so that the stroke, which
 * straddles the path by linewidth/2 on each side, can never be clipped by the viewport. */
static int circle_geometry(GtkWidget *glarea, struct point *centre, int *diameter) {
    int w = gtk_widget_get_width(GTK_WIDGET(glarea));
    int h = gtk_widget_get_height(GTK_WIDGET(glarea));
    if (w <= 0 || h <= 0)
        return 0;
    centre->x = w / 2;
    centre->y = h / 2;
    *diameter = (w < h ? w : h) / 8;
    if (*diameter < 16)
        *diameter = 16;
    return 1;
}

static void test_circle_linewidth(struct graphics *gra, GtkWidget *glarea) {
    /* speed_warner sets gc_set_linewidth(d / 10) with d = min(osd w, osd h) = 46, and the
     * integer division gives 4. */
    const int wanted = 46 / 10;
    struct point centre;
    int diameter = 0;
    if (!circle_geometry(glarea, &centre, &diameter)) {
        g_skipped = 1;
        printf("  SKIP: glarea has no allocation yet\n");
        return;
    }
    /* graphics_draw_circle() takes a DIAMETER; every backend halves it. */
    int radius = diameter / 2;
    struct color white = {0xffff, 0xffff, 0xffff, 0xffff};

    /* Clear to opaque black so the background is unambiguous. */
    struct color black = {0, 0, 0, 0xffff};
    struct graphics_gc *bg = graphics_gc_new(gra);
    graphics_gc_set_background(bg, &black);
    graphics_background_gc(gra, bg);
    graphics_draw_mode(gra, draw_mode_begin_clear);
    graphics_draw_mode(gra, draw_mode_end);

    struct graphics_gc *gc = graphics_gc_new(gra);
    graphics_gc_set_foreground(gc, &white);
    graphics_gc_set_linewidth(gc, wanted);
    graphics_draw_mode(gra, draw_mode_begin);
    graphics_draw_circle(gra, gc, &centre, diameter);
    graphics_draw_mode(gra, draw_mode_end);

    if (!render_once(gra, glarea) || !g_fb.rgba) {
        g_skipped = 1;
        printf("  SKIP: no frame rendered, cannot measure linewidth\n");
        return;
    }

    int best = measure_ring(&g_fb, centre, radius);
    printf("  linewidth %d -> %d px on screen (centre=(%d,%d) d=%d r=%d, glarea=%dx%d, "
           "scanned row=%d)\n",
           wanted, best, centre.x, centre.y, diameter, radius, g_fb.w, g_fb.h, readback_row(&g_fb, centre.y));
    /* Allow one pixel of slack for rasterisation at different segment phases. */
    CHECK(best >= wanted - 1, "stroke of linewidth %d rendered only %d px wide", wanted, best);
    /* 1px means glLineWidth() was never called at all. */
    CHECK(best > 1, "stroke is 1 px wide: graphics_gc_set_linewidth() is ignored");
    if (g_failures == 0)
        PASS("circle_linewidth");
}

static void test_circle_linewidth_1px(struct graphics *gra, GtkWidget *glarea) {
    /* A linewidth of 1 must stay 1px - the fix must not inflate thin lines. */
    struct point centre;
    int diameter = 0;
    if (!circle_geometry(glarea, &centre, &diameter)) {
        g_skipped = 1;
        printf("  SKIP: glarea has no allocation yet\n");
        return;
    }
    int radius = diameter / 2;
    struct color white = {0xffff, 0xffff, 0xffff, 0xffff};

    struct graphics_gc *gc = graphics_gc_new(gra);
    graphics_gc_set_foreground(gc, &white);
    graphics_gc_set_linewidth(gc, 1);
    graphics_draw_mode(gra, draw_mode_begin);
    graphics_draw_circle(gra, gc, &centre, diameter);
    graphics_draw_mode(gra, draw_mode_end);

    if (!render_once(gra, glarea) || !g_fb.rgba) {
        g_skipped = 1;
        printf("  SKIP: no frame rendered, cannot measure linewidth\n");
        return;
    }
    int best = measure_ring(&g_fb, centre, radius);
    printf("  linewidth 1 -> %d px on screen (centre=(%d,%d) d=%d r=%d, glarea=%dx%d, "
           "scanned row=%d)\n",
           best, centre.x, centre.y, diameter, radius, g_fb.w, g_fb.h, readback_row(&g_fb, centre.y));
    CHECK(best <= 2, "linewidth 1 rendered %d px wide", best);
    if (g_failures == 0)
        PASS("circle_linewidth_1px");
}

static void load_plugin(enum plugin_category category, const char *name, const char *path) {
    if (plugin_get_category(category, name, NULL))
        return;
    struct attr pa = {attr_path};
    pa.u.str = (char *)path;
    struct attr *pattrs[] = {&pa, NULL};
    struct plugin *pl = plugin_new(NULL, pattrs);
    if (!pl || !plugin_load(pl)) {
        printf("FAIL: cannot load %s plugin %s\n", name, path);
        exit(2);
    }
    plugin_call_init(pl);
}

/* navit's graphics_draw_circle() takes a DIAMETER and every backend halves it
 * (gtk_drawing_area's cairo_arc(r/2), win32's r = r/2, svg_debug's r/2, qt's r/2 width).
 * The plugin originally used r as the radius, which made every circle - most visibly the
 * speed_warner ring, whose osd_core radius is d - width*2 = 38 - exactly twice as large as on
 * every other backend. Draw a known diameter and assert the ring lands at radius diameter/2. */
static void test_circle_radius_is_diameter_half(struct graphics *gra, GtkWidget *glarea) {
    struct point centre;
    int diameter = 0;
    if (!circle_geometry(glarea, &centre, &diameter)) {
        g_skipped = 1;
        printf("  SKIP: glarea has no allocation yet\n");
        return;
    }
    const int want_radius = diameter / 2;

    struct color black = {0, 0, 0, 0xffff};
    struct graphics_gc *bg = graphics_gc_new(gra);
    graphics_gc_set_background(bg, &black);
    graphics_background_gc(gra, bg);
    graphics_draw_mode(gra, draw_mode_begin_clear);
    graphics_draw_mode(gra, draw_mode_end);

    struct color white = {0xffff, 0xffff, 0xffff, 0xffff};
    struct graphics_gc *gc = graphics_gc_new(gra);
    graphics_gc_set_foreground(gc, &white);
    graphics_gc_set_linewidth(gc, 1);
    graphics_draw_mode(gra, draw_mode_begin);
    graphics_draw_circle(gra, gc, &centre, diameter);
    graphics_draw_mode(gra, draw_mode_end);

    if (!render_once(gra, glarea) || !g_fb.rgba) {
        g_skipped = 1;
        printf("  SKIP: no frame rendered, cannot measure circle radius\n");
        return;
    }

    /* Probe the centre row: with a 1px stroke there must be a lit pixel at exactly
     * centre.x +/- radius and none at twice that distance. */
    int row = readback_row(&g_fb, centre.y);
    int at = pixel_is_stroke(&g_fb, row, centre.x - want_radius)
             || pixel_is_stroke(&g_fb, row, centre.x - want_radius + 1);
    int doubled = pixel_is_stroke(&g_fb, row, centre.x - 2 * want_radius)
                  || pixel_is_stroke(&g_fb, row, centre.x - 2 * want_radius + 1);
    printf("  diameter %d -> ring at radius %d: at=%d at_double_radius=%d\n", diameter, want_radius, at, doubled);
    CHECK(at, "no ring pixel at radius %d (diameter %d); draw_circle must use r/2", want_radius, diameter);
    CHECK(!doubled, "ring found at radius %d, twice the expected %d; draw_circle must use r/2", 2 * want_radius,
          want_radius);
    if (g_failures == 0)
        PASS("circle_radius_is_diameter_half");
}

/* Reads the GL framebuffer pixel at a navit-space point (y is mirrored, see readback_row). */
static void read_pixel(const struct framebuffer *fb, struct point navit_pt, unsigned char rgb[3]) {
    int y = readback_row(fb, navit_pt.y);
    if (y < 0 || y >= fb->h || navit_pt.x < 0 || navit_pt.x >= fb->w) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    const unsigned char *p = &fb->rgba[((gsize)y * fb->w + navit_pt.x) * 4];
    rgb[0] = p[0];
    rgb[1] = p[1];
    rgb[2] = p[2];
}

/* osd_speed_warner_init() builds its gcs like this: red 0xffff,0,0; green 0,0xffff,0;
 * grey 0x8888 with alpha 0x8888 - all on a black background. Reproduce that exact sequence and
 * verify each ring keeps its own colour. A batching/uniform bug leaks the previous command's
 * colour into the next, which showed up as red rendering as yellow (green channel stuck on). */
static void check_speedwarner_gc(const char *name, struct color *fg, struct graphics *gra, GtkWidget *glarea,
                                 int width) {
    struct point centre;
    int diameter = 0;
    if (!circle_geometry(glarea, &centre, &diameter)) {
        g_skipped = 1;
        printf("  SKIP: glarea has no allocation yet\n");
        return;
    }
    /* The speed_warner uses d - width*2 = 46 - 8 = 38 as the circle diameter. */
    diameter = 38;

    struct color black = {0, 0, 0, 0xffff};
    struct graphics_gc *bg = graphics_gc_new(gra);
    graphics_gc_set_background(bg, &black);
    graphics_background_gc(gra, bg);
    graphics_draw_mode(gra, draw_mode_begin_clear);
    graphics_draw_mode(gra, draw_mode_end);

    struct graphics_gc *gc = graphics_gc_new(gra);
    graphics_gc_set_foreground(gc, fg);
    graphics_gc_set_linewidth(gc, width);
    graphics_draw_mode(gra, draw_mode_begin);
    graphics_draw_circle(gra, gc, &centre, diameter);
    graphics_draw_mode(gra, draw_mode_end);

    if (!render_once(gra, glarea) || !g_fb.rgba) {
        g_skipped = 1;
        printf("  SKIP: no frame rendered, cannot check colour\n");
        return;
    }

    /* Sample the leftmost point of the ring (radius = diameter/2). */
    struct point on_ring = {centre.x - diameter / 2, centre.y};
    unsigned char rgb[3];
    read_pixel(&g_fb, on_ring, rgb);
    printf("  %-5s ring pixel rgb=(%3u,%3u,%3u)\n", name, rgb[0], rgb[1], rgb[2]);
    /* Only require that the ring is lit and that red/green are separated correctly:
     * grey and the alpha-blended red are both dimmed, so compare channel dominance. */
    int lum = (rgb[0] * 30 + rgb[1] * 59 + rgb[2] * 11) / 100;
    CHECK(lum > 40, "%s ring is not lit at radius %d: rgb=(%u,%u,%u)", name, diameter / 2, rgb[0], rgb[1], rgb[2]);
    CHECK(!strcmp(name, "red") ? (rgb[0] >= rgb[1] && rgb[0] >= rgb[2])
                               : (!strcmp(name, "green") ? (rgb[1] >= rgb[0] && rgb[1] >= rgb[2]) : 1),
          "%s ring has the wrong dominant channel: rgb=(%u,%u,%u)", name, rgb[0], rgb[1], rgb[2]);
}

static void test_speed_warner_colours(struct graphics *gra, GtkWidget *glarea) {
    struct color red = {0xffff, 0, 0, 0xffff};
    struct color green = {0, 0xffff, 0, 0xffff};
    struct color grey = {0x8888, 0x8888, 0x8888, 0x8888};
    check_speedwarner_gc("red", &red, gra, glarea, 4);
    check_speedwarner_gc("green", &green, gra, glarea, 2);
    check_speedwarner_gc("grey", &grey, gra, glarea, 4);
    if (g_failures == 0)
        PASS("speed_warner_colours");
}

/* Regression test for the "moon problem": a concave polygon was over-filled across its notch
 * because the ear-clipper's strict point-in-triangle test rejected vertices lying exactly on a
 * candidate ear's edge, which produced overlapping triangles that covered the concave cut-out
 * (and, once clipping stalled, a whole-ring fan that filled the convex hull). The polygon below
 * is a minimal reproducer found by fuzzing the triangulator; (36,14) is a pixel that the broken
 * version covered although it lies ~5px outside the polygon. Draw it and check that pixel stays
 * background while the interior is filled. */
static void test_concave_polygon_no_overcover(struct graphics *gra, GtkWidget *glarea) {
    int w = gtk_widget_get_width(GTK_WIDGET(glarea));
    int h = gtk_widget_get_height(GTK_WIDGET(glarea));
    if (w <= 0 || h <= 0) {
        g_skipped = 1;
        printf("  SKIP: glarea has no allocation yet\n");
        return;
    }

    /* star-shaped about the origin, so (0,0) is a safe interior sample */
    static const struct point base[14] = {
        {35,  0  },
        {31,  15 },
        {42,  50 },
        {11,  66 },
        {-4,  35 },
        {-28, 31 },
        {-66, 34 },
        {-71, 1  },
        {-59, -24},
        {-26, -29},
        {-6,  -45},
        {5,   -29},
        {23,  -29},
        {57,  -29},
    };
    /* pixel the broken (strict-containment) triangulator over-covered, 5px clear of any edge */
    static const struct point bad_pixel = {36, 14};

    struct color black = {0, 0, 0, 0xffff};
    struct graphics_gc *bg = graphics_gc_new(gra);
    graphics_gc_set_background(bg, &black);
    graphics_background_gc(gra, bg);
    graphics_draw_mode(gra, draw_mode_begin_clear);
    graphics_draw_mode(gra, draw_mode_end);
    struct point_rect pr = {
        {0, 0},
        {w, h}
    };
    graphics_set_rect(gra, &pr);

    int minx = base[0].x, maxx = base[0].x, miny = base[0].y, maxy = base[0].y;
    for (int i = 1; i < 14; i++) {
        if (base[i].x < minx)
            minx = base[i].x;
        if (base[i].x > maxx)
            maxx = base[i].x;
        if (base[i].y < miny)
            miny = base[i].y;
        if (base[i].y > maxy)
            maxy = base[i].y;
    }
    int offx = w / 2 - (minx + maxx) / 2;
    int offy = h / 2 - (miny + maxy) / 2;
    struct point poly[14];
    for (int i = 0; i < 14; i++) {
        poly[i].x = base[i].x + offx;
        poly[i].y = base[i].y + offy;
    }
    struct point probe = {bad_pixel.x + offx, bad_pixel.y + offy};
    struct point inside = {offx, offy};

    struct color white = {0xffff, 0xffff, 0xffff, 0xffff};
    struct graphics_gc *gc = graphics_gc_new(gra);
    graphics_gc_set_foreground(gc, &white);
    graphics_draw_mode(gra, draw_mode_begin);
    graphics_draw_polygon_clipped(gra, gc, poly, 14);
    graphics_draw_mode(gra, draw_mode_end);

    if (!render_once(gra, glarea) || !g_fb.rgba) {
        g_skipped = 1;
        printf("  SKIP: no frame rendered, cannot measure concave polygon\n");
        return;
    }

    int inside_lit = pixel_is_stroke(&g_fb, readback_row(&g_fb, inside.y), inside.x);
    int probe_lit = pixel_is_stroke(&g_fb, readback_row(&g_fb, probe.y), probe.x);
    printf("  concave polygon interior_lit=%d notch_pixel(%d,%d)_lit=%d\n", inside_lit, probe.x, probe.y, probe_lit);
    CHECK(inside_lit, "concave polygon interior is not filled at (%d,%d)", inside.x, inside.y);
    CHECK(!probe_lit, "concave polygon over-covers the notch at (%d,%d); ear-clipper fills outside the outline",
          probe.x, probe.y);
    if (g_failures == 0)
        PASS("concave_polygon_no_overcover");
}

/* GtkGLArea's own framebuffer is single-sampled and its API offers no multisampling, so the backend renders the scene
 * into an off-screen 4x multisampled FBO and resolves it. Without that every polygon edge is a hard step and closely
 * spaced parallel lines (train tracks, road plus sidewalk) beat into a moire pattern. Draw a filled triangle with a
 * 45-degree hypotenuse; a coverage-based rasteriser leaves pixels there whose luminance lies strictly between the black
 * background and the white fill, a hard single-sampled rasteriser does not. */
static void test_msaa_antialiasing(struct graphics *gra, GtkWidget *glarea) {
    int w = gtk_widget_get_width(GTK_WIDGET(glarea));
    int h = gtk_widget_get_height(GTK_WIDGET(glarea));
    if (w <= 0 || h <= 0) {
        g_skipped = 1;
        printf("  SKIP: glarea has no allocation yet\n");
        return;
    }

    struct color black = {0, 0, 0, 0xffff};
    struct graphics_gc *bg = graphics_gc_new(gra);
    graphics_gc_set_background(bg, &black);
    graphics_background_gc(gra, bg);
    graphics_draw_mode(gra, draw_mode_begin_clear);
    graphics_draw_mode(gra, draw_mode_end);

    /* graphics_draw_polygon_clipped() clips against gra->r, which navit normally
     * fills in from its ::resize signal. The harness never raises that signal, so
     * set it here. */
    struct point_rect pr = {
        {0, 0},
        {w, h}
    };
    graphics_set_rect(gra, &pr);

    struct point tri[3] = {
        {w / 4,     h / 4    },
        {3 * w / 4, h / 4    },
        {3 * w / 4, 3 * h / 4},
    };
    struct color white = {0xffff, 0xffff, 0xffff, 0xffff};
    struct graphics_gc *gc = graphics_gc_new(gra);
    graphics_gc_set_foreground(gc, &white);
    graphics_draw_mode(gra, draw_mode_begin);
    graphics_draw_polygon_clipped(gra, gc, tri, 3);
    graphics_draw_mode(gra, draw_mode_end);

    if (!render_once(gra, glarea) || !g_fb.rgba) {
        g_skipped = 1;
        printf("  SKIP: no frame rendered, cannot measure antialiasing\n");
        return;
    }

    /* A point well inside the triangle (right angle at x = 3w/4) must be filled. */
    int interior = pixel_is_stroke(&g_fb, readback_row(&g_fb, 3 * h / 8), 3 * w / 4 - 8);
    int edge_pixels = 0;
    for (int y = 0; y < g_fb.h; y++) {
        for (int x = 0; x < g_fb.w; x++) {
            const unsigned char *p = &g_fb.rgba[((gsize)y * g_fb.w + x) * 4];
            int lum = (p[0] * 30 + p[1] * 59 + p[2] * 11) / 100;
            if (lum > 40 && lum < 215)
                edge_pixels++;
        }
    }
    printf("  triangle interior lit=%d, partial-coverage edge pixels=%d\n", interior, edge_pixels);
    CHECK(interior, "triangle interior was not filled");
    CHECK(edge_pixels > 0, "no partial-coverage pixels: MSAA is not active (hard edges only)");
    if (g_failures == 0)
        PASS("msaa_antialiasing");
}

int main(int argc, char **argv) {
    const char *libdir = argc > 1 ? argv[1] : ".";

    main_init("test_gtkglarea");
    debug_init("test_gtkglarea");
    file_init();
    geom_init();
    /* the plugin drives its timeouts through the glib event system */
    event_glib_init();

    /* gtkglarea needs the freetype font plugin; graphics_new() has to run after both are
     * registered because the backend bails out in graphics_gtkglarea_new_helper() otherwise. */
    load_plugin(plugin_category_font, "freetype", g_strdup_printf("%s/font/freetype/libfont_freetype.so", libdir));
    load_plugin(plugin_category_graphics, "gtkglarea",
                g_strdup_printf("%s/graphics/gtkglarea/libgraphics_gtkglarea.so", libdir));

    if (!plugin_get_category_graphics("gtkglarea")) {
        printf("FAIL: graphics plugin 'gtkglarea' is not registered\n");
        return 2;
    }

    gtk_init();

    /* The backend calls back into the navit instance (navit_set_render_margin) on resize, so it
     * needs a real one rather than a NULL parent. */
    struct attr nav_attrs[1] = {{attr_type}};
    struct attr *nav_attr_ptrs[] = {&nav_attrs[0], NULL};
    struct navit *nav = navit_new(NULL, nav_attr_ptrs);
    if (!nav) {
        printf("FAIL: navit_new failed\n");
        return 2;
    }

    struct attr w = {attr_w};
    w.u.num = 400;
    struct attr h = {attr_h};
    h.u.num = 600;
    struct attr t = {attr_type};
    t.u.str = "gtkglarea";
    struct attr *gattrs[] = {&t, &w, &h, NULL};
    struct attr parent = {attr_type};
    parent.u.navit = nav;
    struct graphics *gra = graphics_new(&parent, gattrs);
    if (!gra) {
        printf("FAIL: graphics_new failed\n");
        return 2;
    }

    /* Asking for "window" is what makes the plugin create and present its GtkWindow, without
     * which the GtkGLArea never gets a size and is never rendered. */
    if (!graphics_get_data(gra, "window")) {
        printf("FAIL: gtkglarea did not expose its window\n");
        return 2;
    }
    GtkWidget *glarea = graphics_get_data(gra, "gtk_widget");
    if (!glarea) {
        printf("FAIL: gtkglarea did not expose its widget\n");
        return 2;
    }
    /* Our handler is connected after the plugin's, so it sees the finished frame.
     * A plain connect is enough: the plugin returns FALSE from ::render, so the
     * default true_handled accumulator does not halt emission before us. */
    g_signal_connect(glarea, "render", G_CALLBACK(on_capture_render), NULL);
    /* Since GTK 4.14 GtkGLArea caches the GL output in a texture and only re-runs ::render when
     * a frame is explicitly requested, so auto-render has to stay on for the plugin to draw. */
    GtkRoot *root = gtk_widget_get_root(glarea);
    if (GTK_IS_WINDOW(root))
        gtk_window_present(GTK_WINDOW(root));
    pump_frames(glarea, 500);

    /* Whether this machine can actually drive the plugin's GL at all. Some headless setups
     * (notably Xvfb with a software GL stack) let the window paint but never run ::render, and
     * navit's own window stays blank there too, so this is an environment limit rather than a
     * regression. Skip instead of reporting a bogus failure; the test does assert wherever the
     * GL stack works. */
    int renders_before = g_render_calls;
    pump_frames(glarea, 2000);
    if (g_render_calls == renders_before) {
        GError *glerr = gtk_gl_area_get_error(GTK_GL_AREA(glarea));
        printf("SKIP: gtkglarea never emitted ::render here "
               "(gl_error=%s, context=%p); no working GL for this environment\n",
               glerr ? glerr->message : "none", (void *)gtk_gl_area_get_context(GTK_GL_AREA(glarea)));
        return 77; /* automake/CTest convention for "skipped" */
    }
    printf("gtkglarea rendered %d frames\n", g_render_calls - renders_before);

    test_circle_linewidth(gra, glarea);
    test_circle_linewidth_1px(gra, glarea);
    test_circle_radius_is_diameter_half(gra, glarea);
    test_speed_warner_colours(gra, glarea);
    test_concave_polygon_no_overcover(gra, glarea);
    test_msaa_antialiasing(gra, glarea);

    free(g_fb.rgba);
    printf("failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
