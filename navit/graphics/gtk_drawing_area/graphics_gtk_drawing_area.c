/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2008 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "callback.h"
#include "color.h"
#include "config.h"
#include "debug.h"
#include "event.h"
#include "graphics.h"
#include "item.h"
#include "keys.h"
#include "navit.h"
#include "navit/font/freetype/font_freetype.h"
#include "plugin.h"
#include "point.h"
#include "window.h"
#include <cairo.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <locale.h> /* For WIN32 */
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>
#if !defined(GDK_KEY_Book) || !defined(GDK_KEY_Calendar)
#    include <X11/XF86keysym.h>
#endif
#ifdef HAVE_IMLIB2
#    include <Imlib2.h>
#endif
#ifndef _WIN32
#    include <gdk/x11/gdkx.h>
#endif
#ifndef GDK_KEY_Book
#    define GDK_KEY_Book XF86XK_Book
#endif
#ifndef GDK_KEY_Calendar
#    define GDK_KEY_Calendar XF86XK_Calendar
#endif

struct graphics_priv {
    int button_timeout;
    GtkWidget *widget;
    GtkWidget *win;
    struct window window;
    cairo_t *cairo;
    struct point p;
    int width;
    int height;
    int win_w;
    int win_h;
    int visible;
    int overlay_disabled;
    int overlay_autodisabled;
    int wraparound;
    struct graphics_priv *parent;
    struct graphics_priv *overlays;
    struct graphics_priv *next;
    struct graphics_gc_priv *background_gc;
    struct callback_list *cbl;
    struct font_freetype_methods freetype_methods;
    struct navit *nav;
    int pid;
    struct timeval button_press[8];
    struct timeval button_release[8];
    int timeout;
    int delay;
    char *window_title;
    guint tick_callback_id;
    int needs_redraw;
};

struct graphics_gc_priv {
    struct graphics_priv *gr;
    struct color c;
    double linewidth;
    double *dashes;
    int ndashes;
    double offset;
    cairo_surface_t *texture;
};

struct graphics_image_priv {
    GdkPixbuf *pixbuf;
    int w;
    int h;
#ifdef HAVE_IMLIB2
    void *image;
#endif
};

static void set_drawing_color(cairo_t *cairo, struct color c) {
    double col_max = 1 << COLOR_BITDEPTH;
    cairo_set_source_rgba(cairo, c.r / col_max, c.g / col_max, c.b / col_max, c.a / col_max);
}

static void graphics_destroy(struct graphics_priv *gr) {
    struct graphics_priv **pp;
    dbg(lvl_debug, "enter parent %p", gr->parent);
    if (gr->parent) {
        pp = &gr->parent->overlays;
        while (*pp) {
            if (*pp == gr) {
                *pp = gr->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    gr->freetype_methods.destroy();
    if (gr->cairo)
        cairo_destroy(gr->cairo);
    if (!gr->parent) {
        dbg(lvl_debug, "widget %p win %p", gr->widget, gr->win);
        if (gr->tick_callback_id)
            gtk_widget_remove_tick_callback(gr->widget, gr->tick_callback_id);
        if (gr->win)
            gtk_window_destroy(GTK_WINDOW(gr->win));
        g_free(gr->window_title);
        while (gr->overlays) {
            struct graphics_priv *overlay = gr->overlays;
            gr->overlays = overlay->next;
            if (overlay->cairo)
                cairo_destroy(overlay->cairo);
            overlay->freetype_methods.destroy();
            g_free(overlay);
        }
    }
    g_free(gr);
}

static void gc_destroy(struct graphics_gc_priv *gc) {
    if (gc->texture != NULL)
        cairo_surface_destroy(gc->texture);
    g_free(gc->dashes);
    g_free(gc);
}

static void gc_set_linewidth(struct graphics_gc_priv *gc, int w) {
    gc->linewidth = w;
}

static void gc_set_dashes(struct graphics_gc_priv *gc, int w, int offset, unsigned char *dash_list, int n) {
    int i;
    g_free(gc->dashes);
    gc->ndashes = n;
    gc->offset = offset;
    if (n) {
        gc->dashes = g_malloc_n(n, sizeof(double));
        for (i = 0; i < n; i++) {
            gc->dashes[i] = dash_list[i];
        }
    } else {
        gc->dashes = NULL;
    }
}

static void gc_set_foreground(struct graphics_gc_priv *gc, struct color *c) {
    gc->c = *c;
}

static void gc_set_background(struct graphics_gc_priv *gc, struct color *c) {
}

static void gc_set_texture(struct graphics_gc_priv *gc, struct graphics_image_priv *img) {
    cairo_surface_t *surface;
    cairo_t *cr;

    // If called twice, clean up
    if (gc->texture != NULL)
        cairo_surface_destroy(gc->texture);
    gc->texture = NULL;

    // build fill pattern
    if ((img != NULL) && (img->pixbuf != NULL)) {

        // create a new surface same size as the image
        surface =
            cairo_image_surface_create(gdk_pixbuf_get_has_alpha(img->pixbuf) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
                                       gdk_pixbuf_get_height(img->pixbuf), gdk_pixbuf_get_width(img->pixbuf));
        // run cairo on it
        cr = cairo_create(surface);
        // paint background
        set_drawing_color(cr, gc->c);
        cairo_rectangle(cr, 0, 0, gdk_pixbuf_get_height(img->pixbuf), gdk_pixbuf_get_width(img->pixbuf));
        cairo_fill(cr);
        // paint image on top
        gdk_cairo_set_source_pixbuf(cr, img->pixbuf, 0, 0);
        cairo_paint(cr);
        // destroy the cairo context, but keep the surface.
        cairo_destroy(cr);
        gc->texture = surface;
    }
}

static struct graphics_gc_methods gc_methods = {
    .gc_destroy = gc_destroy,
    .gc_set_linewidth = gc_set_linewidth,
    .gc_set_dashes = gc_set_dashes,
    .gc_set_foreground = gc_set_foreground,
    .gc_set_background = gc_set_background,
    .gc_set_texture = gc_set_texture,
};

static struct graphics_gc_priv *gc_new(struct graphics_priv *gr, struct graphics_gc_methods *meth) {
    struct graphics_gc_priv *gc = g_new(struct graphics_gc_priv, 1);

    *meth = gc_methods;
    gc->gr = gr;

    gc->linewidth = 1;
    gc->c.r = 0;
    gc->c.g = 0;
    gc->c.b = 0;
    gc->c.a = 0;
    gc->dashes = NULL;
    gc->ndashes = 0;
    gc->offset = 0;
    gc->texture = NULL;

    return gc;
}

static struct graphics_image_priv *image_new(struct graphics_priv *gr, struct graphics_image_methods *meth, char *name,
                                             int *w, int *h, struct point *hot, int rotation) {
    GdkPixbuf *pixbuf;
    struct graphics_image_priv *ret;
    const char *option;

    if (*w == IMAGE_W_H_UNSET && *h == IMAGE_W_H_UNSET)
        pixbuf = gdk_pixbuf_new_from_file(name, NULL);
    else
        pixbuf = gdk_pixbuf_new_from_file_at_size(name, *w, *h, NULL);

    if (!pixbuf)
        return NULL;

    if (rotation) {
        GdkPixbuf *tmp;
        switch (rotation) {
        case 90:
            rotation = 270;
            break;
        case 180:
            break;
        case 270:
            rotation = 90;
            break;
        default:
            return NULL;
        }

        tmp = gdk_pixbuf_rotate_simple(pixbuf, rotation);

        if (!tmp) {
            g_object_unref(pixbuf);
            return NULL;
        }

        g_object_unref(pixbuf);
        pixbuf = tmp;
    }

    ret = g_new0(struct graphics_image_priv, 1);
    ret->pixbuf = pixbuf;
    ret->w = gdk_pixbuf_get_width(pixbuf);
    ret->h = gdk_pixbuf_get_height(pixbuf);
    *w = ret->w;
    *h = ret->h;
    if (hot) {
        option = gdk_pixbuf_get_option(pixbuf, "x_hot");
        if (option)
            hot->x = atoi(option);
        else
            hot->x = ret->w / 2 - 1;
        option = gdk_pixbuf_get_option(pixbuf, "y_hot");
        if (option)
            hot->y = atoi(option);
        else
            hot->y = ret->h / 2 - 1;
    }
    return ret;
}

static void image_free(struct graphics_priv *gr, struct graphics_image_priv *priv) {
    g_object_unref(priv->pixbuf);
    g_free(priv);
}

static void set_stroke_params_from_gc(cairo_t *cairo, struct graphics_gc_priv *gc) {
    set_drawing_color(cairo, gc->c);
    cairo_set_dash(cairo, gc->dashes, gc->ndashes, gc->offset);
    cairo_set_line_width(cairo, gc->linewidth);
}

static void draw_lines(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count) {
    int i;
    if (!count)
        return;
    cairo_move_to(gr->cairo, p[0].x, p[0].y);
    for (i = 1; i < count; i++) {
        cairo_line_to(gr->cairo, p[i].x, p[i].y);
    }
    set_stroke_params_from_gc(gr->cairo, gc);
    cairo_stroke(gr->cairo);
}

static void draw_polygon(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count) {
    int i;
    if (count < 1)
        return;
    set_drawing_color(gr->cairo, gc->c);
    if (gc->texture != NULL) {
        cairo_set_source_surface(gr->cairo, gc->texture, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(gr->cairo), CAIRO_EXTEND_REPEAT);
    }
    cairo_move_to(gr->cairo, p[0].x, p[0].y);
    for (i = 1; i < count; i++) {
        cairo_line_to(gr->cairo, p[i].x, p[i].y);
    }
    cairo_fill(gr->cairo);
}

static void draw_polygon_with_holes(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count,
                                    int hole_count, int *ccount, struct point **holes) {
    int i;
    int j;
    cairo_fill_rule_t old_rule;
    set_drawing_color(gr->cairo, gc->c);
    if (gc->texture != NULL) {
        cairo_set_source_surface(gr->cairo, gc->texture, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(gr->cairo), CAIRO_EXTEND_REPEAT);
    }
    /* remember current fill rule */
    old_rule = cairo_get_fill_rule(gr->cairo);
    /* set fill rule */
    cairo_set_fill_rule(gr->cairo, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_move_to(gr->cairo, p[0].x, p[0].y);
    for (i = 1; i < count; i++) {
        cairo_line_to(gr->cairo, p[i].x, p[i].y);
    }
    for (j = 0; j < hole_count; j++) {
        if (hole_count > 0) {
            cairo_move_to(gr->cairo, holes[j][0].x, holes[j][0].y);
            for (i = 0; i < ccount[j]; i++) {
                cairo_line_to(gr->cairo, holes[j][i].x, holes[j][i].y);
            }
        }
    }
    cairo_fill(gr->cairo);
    /* restore fill rule */
    cairo_set_fill_rule(gr->cairo, old_rule);
}

static void draw_rectangle(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int w, int h) {
    cairo_save(gr->cairo);
    // Use OPERATOR_SOURCE to overwrite old contents even when drawing with transparency.
    // Necessary for OSD drawing.
    cairo_set_operator(gr->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_rectangle(gr->cairo, p->x, p->y, w, h);
    set_drawing_color(gr->cairo, gc->c);
    cairo_fill(gr->cairo);
    cairo_restore(gr->cairo);
}

static void draw_circle(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int r) {
    cairo_arc(gr->cairo, p->x, p->y, r / 2, 0.0, 2 * G_PI);
    set_stroke_params_from_gc(gr->cairo, gc);
    cairo_stroke(gr->cairo);
}

static void draw_rgb_image_buffer(cairo_t *cairo, int buffer_width, int buffer_height, int draw_pos_x, int draw_pos_y,
                                  int stride, unsigned char *buffer) {
    cairo_surface_t *buffer_surface =
        cairo_image_surface_create_for_data(buffer, CAIRO_FORMAT_ARGB32, buffer_width, buffer_height, stride);
    cairo_set_source_surface(cairo, buffer_surface, draw_pos_x, draw_pos_y);
    cairo_paint(cairo);
    cairo_surface_destroy(buffer_surface);
}

static void display_text_draw(struct font_freetype_text *text, struct graphics_priv *gr, struct graphics_gc_priv *fg,
                              struct graphics_gc_priv *bg, struct point *p) {
    int i, x, y, stride;
    struct font_freetype_glyph *g, **gp;
    struct color transparent = {0x0, 0x0, 0x0, 0x0};

    gp = text->glyph;
    i = text->glyph_count;
    x = p->x << 6;
    y = p->y << 6;
    while (i-- > 0) {
        g = *gp++;
        if (g->w && g->h && bg) {
            unsigned char *shadow;
            stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, g->w + 2);
            shadow = g_malloc(stride * (g->h + 2));
            gr->freetype_methods.get_shadow(g, shadow, stride, &bg->c, &transparent);
            draw_rgb_image_buffer(gr->cairo, g->w + 2, g->h + 2, ((x + g->x) >> 6) - 1, ((y + g->y) >> 6) - 1, stride,
                                  shadow);
            g_free(shadow);
        }
        x += g->dx;
        y += g->dy;
    }
    x = p->x << 6;
    y = p->y << 6;
    gp = text->glyph;
    i = text->glyph_count;
    while (i-- > 0) {
        g = *gp++;
        if (g->w && g->h) {
            unsigned char *glyph;
            stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, g->w);
            glyph = g_malloc(stride * g->h);
            gr->freetype_methods.get_glyph(g, glyph, stride, &fg->c, bg ? &bg->c : &transparent, &transparent);
            draw_rgb_image_buffer(gr->cairo, g->w, g->h, (x + g->x) >> 6, (y + g->y) >> 6, stride, glyph);
            g_free(glyph);
        }
        x += g->dx;
        y += g->dy;
    }
}

static void draw_text(struct graphics_priv *gr, struct graphics_gc_priv *fg, struct graphics_gc_priv *bg,
                      struct graphics_font_priv *font, char *text, struct point *p, int dx, int dy) {
    struct font_freetype_text *t;

    if (!font) {
        dbg(lvl_error, "no font, returning");
        return;
    }
#if 0 /* Temporarily disabled because it destroys text rendering of overlays and in gui internal in some places */
    /*
     This needs an improvement, no one checks if the strings are visible
    */
    if (p->x > gr->width-50 || p->y > gr->height-50) {
        return;
    }
    if (p->x < -50 || p->y < -50) {
        return;
    }
#endif
    if (bg && !bg->c.a)
        bg = NULL;
    t = gr->freetype_methods.text_new(text, (struct font_freetype_font *)font, dx, dy);
    display_text_draw(t, gr, fg, bg, p);
    gr->freetype_methods.text_destroy(t);
}

static void draw_image(struct graphics_priv *gr, struct graphics_gc_priv *fg, struct point *p,
                       struct graphics_image_priv *img) {
    gdk_cairo_set_source_pixbuf(gr->cairo, img->pixbuf, p->x, p->y);
    cairo_paint(gr->cairo);
}

#ifdef HAVE_IMLIB2
static unsigned char *create_buffer_with_stride_if_required(unsigned char *input_buffer, int w, int h,
                                                            size_t bytes_per_pixel, size_t output_stride) {
    int line;
    size_t input_offset, output_offset;
    unsigned char *out_buf;
    size_t input_stride = w * bytes_per_pixel;
    if (input_stride == output_stride) {
        return NULL;
    }

    out_buf = g_malloc(h * output_stride);
    for (line = 0; line < h; line++) {
        input_offset = line * input_stride;
        output_offset = line * output_stride;
        memcpy(out_buf + output_offset, input_buffer + input_offset, input_stride);
    }
    return out_buf;
}

static void draw_image_warp(struct graphics_priv *gr, struct graphics_gc_priv *fg, struct point *p, int count,
                            struct graphics_image_priv *img) {
    int w, h;
    DATA32 *intermediate_buffer;
    unsigned char *intermediate_buffer_aligned;
    Imlib_Image intermediate_image;
    size_t stride;
    dbg(lvl_debug, "draw_image_warp data=%p", img);
    w = img->w;
    h = img->h;
    if (!img->image) {
        int x, y;
        img->image = imlib_create_image(w, h);
        imlib_context_set_image(img->image);
        if (gdk_pixbuf_get_colorspace(img->pixbuf) != GDK_COLORSPACE_RGB
            || gdk_pixbuf_get_bits_per_sample(img->pixbuf) != 8) {
            dbg(lvl_error, "implement me");
        } else if (gdk_pixbuf_get_has_alpha(img->pixbuf) && gdk_pixbuf_get_n_channels(img->pixbuf) == 4) {
            for (y = 0; y < h; y++) {
                unsigned int *dst = imlib_image_get_data() + y * w;
                unsigned char *src = gdk_pixbuf_get_pixels(img->pixbuf) + y * gdk_pixbuf_get_rowstride(img->pixbuf);
                for (x = 0; x < w; x++) {
                    *dst++ = 0xff000000 | (src[0] << 16) | (src[1] << 8) | src[2];
                    src += 4;
                }
            }
        } else if (!gdk_pixbuf_get_has_alpha(img->pixbuf) && gdk_pixbuf_get_n_channels(img->pixbuf) == 3) {
            for (y = 0; y < h; y++) {
                unsigned int *dst = imlib_image_get_data() + y * w;
                unsigned char *src = gdk_pixbuf_get_pixels(img->pixbuf) + y * gdk_pixbuf_get_rowstride(img->pixbuf);
                for (x = 0; x < w; x++) {
                    *dst++ = 0xff000000 | (src[0] << 16) | (src[1] << 8) | src[2];
                    src += 3;
                }
            }
        } else {
            dbg(lvl_error, "implement me");
        }
    }

    intermediate_buffer = g_malloc0(gr->width * gr->height * 4);
    intermediate_image = imlib_create_image_using_data(gr->width, gr->height, intermediate_buffer);
    imlib_context_set_image(intermediate_image);
    imlib_image_set_has_alpha(1);

    if (count == 3) {
        /* 0 1
                   2   */
        imlib_blend_image_onto_image_skewed(img->image, 1, 0, 0, w, h, p[0].x, p[0].y, p[1].x - p[0].x, p[1].y - p[0].y,
                                            p[2].x - p[0].x, p[2].y - p[0].y);
    }
    if (count == 2) {
        /* 0
                     1 */
        imlib_blend_image_onto_image_skewed(img->image, 1, 0, 0, w, h, p[0].x, p[0].y, p[1].x - p[0].x, 0, 0,
                                            p[1].y - p[0].y);
    }
    if (count == 1) {
        /*
                   0
                     */
        imlib_blend_image_onto_image_skewed(img->image, 1, 0, 0, w, h, p[0].x - w / 2, p[0].y - h / 2, w, 0, 0, h);
    }

    stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, gr->width);
    intermediate_buffer_aligned = create_buffer_with_stride_if_required((unsigned char *)intermediate_buffer, gr->width,
                                                                        gr->height, sizeof(DATA32), stride);
    cairo_surface_t *buffer_surface = cairo_image_surface_create_for_data(
        intermediate_buffer_aligned ? intermediate_buffer_aligned : (unsigned char *)intermediate_buffer,
        CAIRO_FORMAT_ARGB32, gr->width, gr->height, stride);
    cairo_set_source_surface(gr->cairo, buffer_surface, 0, 0);
    cairo_paint(gr->cairo);

    cairo_surface_destroy(buffer_surface);
    imlib_free_image();
    g_free(intermediate_buffer);
    g_free(intermediate_buffer_aligned);
}
#endif

static void overlay_rect(struct graphics_priv *parent, struct graphics_priv *overlay, GdkRectangle *r) {
    r->x = overlay->p.x;
    r->y = overlay->p.y;
    r->width = overlay->width;
    r->height = overlay->height;
    if (!overlay->wraparound)
        return;
    if (r->x < 0)
        r->x += parent->width;
    if (r->y < 0)
        r->y += parent->height;
    if (r->width < 0)
        r->width += parent->width;
    if (r->height < 0)
        r->height += parent->height;
}

static void overlay_draw(struct graphics_priv *parent, struct graphics_priv *overlay, GdkRectangle *re,
                         cairo_t *cairo) {
    GdkRectangle or, ir;
    if (parent->overlay_disabled || overlay->overlay_disabled || overlay->overlay_autodisabled)
        return;
    overlay_rect(parent, overlay, &or);
    if (!gdk_rectangle_intersect(re, &or, &ir))
        return;
    or.x -= re->x;
    or.y -= re->y;
    cairo_surface_t *overlay_surface = cairo_get_target(overlay->cairo);
    cairo_set_source_surface(cairo, overlay_surface, or.x, or.y);
    cairo_paint(cairo);
}

static void draw_drag(struct graphics_priv *gr, struct point *p) {
    if (p)
        gr->p = *p;
    else {
        gr->p.x = 0;
        gr->p.y = 0;
    }
}

static void background_gc(struct graphics_priv *gr, struct graphics_gc_priv *gc) {
    gr->background_gc = gc;
}

static void draw_mode(struct graphics_priv *gr, enum draw_mode_num mode) {
    if (mode == draw_mode_end) {
        gr->needs_redraw = 1;
        gtk_widget_queue_draw(gr->widget);
    }
}

/* Drawing — GTK4 draw function replaces expose + configure */

static void draw_func(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    struct graphics_priv *gra = user_data;
    struct graphics_gc_priv *background_gc = gra->background_gc;
    struct graphics_priv *overlay;

    gra->visible = 1;

    /* Handle resize inline (replaces configure handler) */
    if (gra->width != width || gra->height != height) {
#ifndef _WIN32
        dbg(lvl_debug, "resize %dx%d", width, height);
#endif
        gra->width = width;
        gra->height = height;
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gra->width, gra->height);
        if (gra->cairo)
            cairo_destroy(gra->cairo);
        gra->cairo = cairo_create(surface);
        cairo_surface_destroy(surface);
        cairo_set_antialias(gra->cairo, CAIRO_ANTIALIAS_GOOD);
        callback_list_call_attr_2(gra->cbl, attr_resize, GINT_TO_POINTER(gra->width), GINT_TO_POINTER(gra->height));
    }

    if (!gra->cairo)
        return;

    if (gra->p.x || gra->p.y) {
        if (background_gc) {
            set_drawing_color(cairo, background_gc->c);
            cairo_paint(cairo);
        }
    }
    cairo_set_source_surface(cr, cairo_get_target(gra->cairo), gra->p.x, gra->p.y);
    cairo_paint(cr);

    GdkRectangle area_rect = {0, 0, width, height};

    overlay = gra->overlays;
    while (overlay) {
        overlay_draw(gra, overlay, &area_rect, cr);
        overlay = overlay->next;
    }
}

static gboolean on_frame_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    struct graphics_priv *gr = user_data;
    if (gr->needs_redraw) {
        gr->needs_redraw = 0;
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

/* Events — GTK4 event controllers */

static int tv_delta(struct timeval *old, struct timeval *new) {
    if (new->tv_sec - old->tv_sec >= INT_MAX / 1000)
        return INT_MAX;
    return (new->tv_sec - old->tv_sec) * 1000 + (new->tv_usec - old->tv_usec) / 1000;
}

static void on_gesture_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct point p;
    struct timeval tv;
    struct timezone tz;
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    gettimeofday(&tv, &tz);

    if (button < 8) {
        if (tv_delta(&this->button_press[button], &tv) < this->timeout)
            return;
        this->button_press[button] = tv;
        this->button_release[button].tv_sec = 0;
        this->button_release[button].tv_usec = 0;
    }
    p.x = (int)x;
    p.y = (int)y;
    callback_list_call_attr_3(this->cbl, attr_button, GINT_TO_POINTER(1), GINT_TO_POINTER(button), (void *)&p);
}

static void on_gesture_click_released(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct point p;
    struct timeval tv;
    struct timezone tz;
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    gettimeofday(&tv, &tz);

    if (button < 8) {
        if (tv_delta(&this->button_release[button], &tv) < this->timeout)
            return;
        this->button_release[button] = tv;
        this->button_press[button].tv_sec = 0;
        this->button_press[button].tv_usec = 0;
    }
    p.x = (int)x;
    p.y = (int)y;
    callback_list_call_attr_3(this->cbl, attr_button, GINT_TO_POINTER(0), GINT_TO_POINTER(button), (void *)&p);
}

static void on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct point p;
    int button;
    double x, y;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    gdk_event_get_position(event, &x, &y);

    p.x = (int)x;
    p.y = (int)y;
    if (dy < 0)
        button = 4;
    else if (dy > 0)
        button = 5;
    else
        button = -1;
    if (button != -1) {
        callback_list_call_attr_3(this->cbl, attr_button, GINT_TO_POINTER(1), GINT_TO_POINTER(button), (void *)&p);
        callback_list_call_attr_3(this->cbl, attr_button, GINT_TO_POINTER(0), GINT_TO_POINTER(button), (void *)&p);
    }
}

static void on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct point p;

    p.x = (int)x;
    p.y = (int)y;
    callback_list_call_attr_1(this->cbl, attr_motion, (void *)&p);
}

static gboolean close_request(GtkWindow *win, gpointer user_data) {
    struct graphics_priv *this = user_data;
    dbg(lvl_debug, "enter this->win=%p", this->win);
    if (this->delay & 2) {
        if (this->win)
            this->win = NULL;
    } else {
        callback_list_call_attr_0(this->cbl, attr_window_closed);
    }
    return TRUE;
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state,
                            gpointer user_data) {
    struct graphics_priv *this = user_data;
    int len, ucode;
    char key[8];
    ucode = gdk_keyval_to_unicode(keyval);
    len = g_unichar_to_utf8(ucode, key);
    key[len] = '\0';

    switch (keyval) {
    case GDK_KEY_Up:
        key[0] = NAVIT_KEY_UP;
        key[1] = '\0';
        break;
    case GDK_KEY_Down:
        key[0] = NAVIT_KEY_DOWN;
        key[1] = '\0';
        break;
    case GDK_KEY_Left:
        key[0] = NAVIT_KEY_LEFT;
        key[1] = '\0';
        break;
    case GDK_KEY_Right:
        key[0] = NAVIT_KEY_RIGHT;
        key[1] = '\0';
        break;
    case GDK_KEY_BackSpace:
        key[0] = NAVIT_KEY_BACKSPACE;
        key[1] = '\0';
        break;
    case GDK_KEY_Tab:
        key[0] = '\t';
        key[1] = '\0';
        break;
    case GDK_KEY_Delete:
        key[0] = NAVIT_KEY_DELETE;
        key[1] = '\0';
        break;
    case GDK_KEY_Escape:
        key[0] = NAVIT_KEY_BACK;
        key[1] = '\0';
        break;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        key[0] = NAVIT_KEY_RETURN;
        key[1] = '\0';
        break;
    case GDK_KEY_Book:
#ifdef USE_HILDON
    case GDK_KEY_F7:
#endif
        key[0] = NAVIT_KEY_ZOOM_IN;
        key[1] = '\0';
        break;
    case GDK_KEY_Calendar:
#ifdef USE_HILDON
    case GDK_KEY_F8:
#endif
        key[0] = NAVIT_KEY_ZOOM_OUT;
        key[1] = '\0';
        break;
    case GDK_KEY_Page_Up:
        key[0] = NAVIT_KEY_PAGE_UP;
        key[1] = '\0';
        break;
    case GDK_KEY_Page_Down:
        key[0] = NAVIT_KEY_PAGE_DOWN;
        key[1] = '\0';
        break;
    }
    if (key[0])
        callback_list_call_attr_1(this->cbl, attr_keypress, (void *)key);
    else
        dbg(lvl_debug, "keyval 0x%x", keyval);
    return FALSE;
}

static struct graphics_priv *graphics_gtk_drawing_area_new_helper(struct graphics_methods *meth);

static void overlay_disable(struct graphics_priv *gr, int disabled) {
    if (!gr->overlay_disabled != !disabled) {
        gr->overlay_disabled = disabled;
        if (gr->parent) {
            GdkRectangle r;
            overlay_rect(gr->parent, gr, &r);
            gtk_widget_queue_draw(gr->parent->widget);
        }
    }
}

static void overlay_resize(struct graphics_priv *this, struct point *p, int w, int h, int wraparound) {
    // do not dereference parent for non overlay osds
    if (!this->parent) {
        return;
    }

    int changed = 0;
    int w2, h2;

    if (w == 0) {
        w2 = 1;
    } else {
        w2 = w;
    }

    if (h == 0) {
        h2 = 1;
    } else {
        h2 = h;
    }

    this->p = *p;
    if (this->width != w2) {
        this->width = w2;
        changed = 1;
    }

    if (this->height != h2) {
        this->height = h2;
        changed = 1;
    }

    this->wraparound = wraparound;

    if (changed) {
        cairo_destroy(this->cairo);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w2, h2);
        this->cairo = cairo_create(surface);
        cairo_surface_destroy(surface);

        if ((w == 0) || (h == 0)) {
            this->overlay_autodisabled = 1;
        } else {
            this->overlay_autodisabled = 0;
        }

        callback_list_call_attr_2(this->cbl, attr_resize, GINT_TO_POINTER(this->width), GINT_TO_POINTER(this->height));
    }
}

static void get_data_window(struct graphics_priv *this, unsigned int xid) {
    GtkEventController *key_controller;

    if (!this->win) {
        this->win = gtk_window_new();
        gtk_window_set_default_size(GTK_WINDOW(this->win), this->win_w, this->win_h);
        dbg(lvl_debug, "h= %i, w= %i", this->win_h, this->win_w);
        gtk_window_set_title(GTK_WINDOW(this->win), this->window_title);

        /* Close request */
        g_signal_connect(this->win, "close-request", G_CALLBACK(close_request), this);

        /* Key controller — created once with the window */
        key_controller = gtk_event_controller_key_new();
        g_signal_connect(key_controller, "key-pressed", G_CALLBACK(key_pressed), this);
        gtk_widget_add_controller(this->widget, key_controller);
    }

    gtk_window_set_child(GTK_WINDOW(this->win), this->widget);

    gtk_widget_set_visible(this->win, TRUE);

    gtk_widget_set_focusable(this->widget, TRUE);
    gtk_widget_set_sensitive(this->widget, TRUE);
    gtk_widget_grab_focus(this->widget);
}

static int set_attr(struct graphics_priv *gr, struct attr *attr) {
    dbg(lvl_debug, "enter");
    switch (attr->type) {
    case attr_windowid:
        get_data_window(gr, attr->u.num);
        return 1;
    default:
        return 0;
    }
}

static struct graphics_priv *overlay_new(struct graphics_priv *gr, struct graphics_methods *meth, struct point *p,
                                         int w, int h, int wraparound) {
    int w2, h2;
    struct graphics_priv *this = graphics_gtk_drawing_area_new_helper(meth);
    this->widget = gr->widget;
    this->p = *p;
    this->width = w;
    this->height = h;
    this->parent = gr;

    /* If either height or width is 0, we set it to 1 to avoid warnings, and
     * disable the overlay. */
    if (h == 0) {
        h2 = 1;
    } else {
        h2 = h;
    }

    if (w == 0) {
        w2 = 1;
    } else {
        w2 = w;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w2, h2);
    this->cairo = cairo_create(surface);
    cairo_surface_destroy(surface);

    if ((w == 0) || (h == 0)) {
        this->overlay_autodisabled = 1;
    } else {
        this->overlay_autodisabled = 0;
    }

    this->next = gr->overlays;
    this->wraparound = wraparound;
    gr->overlays = this;
    return this;
}

static int graphics_gtk_drawing_area_fullscreen(struct window *w, int on) {
    struct graphics_priv *gr = w->priv;
    if (on)
        gtk_window_fullscreen(GTK_WINDOW(gr->win));
    else
        gtk_window_unfullscreen(GTK_WINDOW(gr->win));
    return 1;
}

static void graphics_gtk_drawing_area_disable_suspend(struct window *w) {
    struct graphics_priv *gr = w->priv;

#ifndef _WIN32
    if (gr->pid)
        kill(gr->pid, SIGWINCH);
#else
    dbg(lvl_warning, "failed to kill() under Windows");
#endif
}

static void *get_data(struct graphics_priv *this, char const *type) {
    FILE *f;
    if (!strcmp(type, "gtk_widget"))
        return this->widget;
#ifndef _WIN32
    if (!strcmp(type, "xwindow_id")) {
        GdkSurface *surface;
        if (this->win)
            surface = gtk_native_get_surface(GTK_NATIVE(this->win));
        else
            surface = gtk_native_get_surface(GTK_NATIVE(this->widget));
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        return (void *)gdk_x11_surface_get_xid(surface);
#    pragma GCC diagnostic pop
    }
#endif
    if (!strcmp(type, "window")) {
        char *cp = getenv("NAVIT_XID");
        unsigned xid = 0;
        if (cp)
            xid = strtol(cp, NULL, 0);
        if (!(this->delay & 1))
            get_data_window(this, xid);
        this->window.fullscreen = graphics_gtk_drawing_area_fullscreen;
        this->window.disable_suspend = graphics_gtk_drawing_area_disable_suspend;
        this->window.priv = this;
#if !defined(_WIN32) && !defined(__CEGCC__)
        f = popen("pidof /usr/bin/ipaq-sleep", "r");
        if (f) {
            int fscanf_result;
            fscanf_result = fscanf(f, "%d", &this->pid);
            if ((fscanf_result == EOF) || (fscanf_result == 0)) {
                dbg(lvl_warning, "Failed to open iPaq sleep file. Error-Code: %d", errno);
            }
            dbg(lvl_debug, "ipaq_sleep pid=%d", this->pid);
            pclose(f);
        }
#endif
        return &this->window;
    }
    return NULL;
}

/**
 * @brief Return number of dots per inch
 * @param gr self handle
 * @return dpi value
 */
static navit_float get_dpi(struct graphics_priv *gr) {
    gdouble dpi = 96;
    GdkDisplay *display = gtk_widget_get_display(gr->widget);
    if (display != NULL) {
        GtkNative *native = gtk_widget_get_native(gr->widget);
        if (native != NULL) {
            GdkSurface *surface = gtk_native_get_surface(native);
            if (surface != NULL) {
                GdkMonitor *monitor = gdk_display_get_monitor_at_surface(display, surface);
                if (monitor != NULL) {
                    dpi = 96 * gdk_monitor_get_scale_factor(monitor);
                }
            }
        }
    }
    return (navit_float)dpi;
}

static struct graphics_methods graphics_methods = {
    graphics_destroy,
    draw_mode,
    draw_lines,
    draw_polygon,
    draw_rectangle,
    draw_circle,
    draw_text,
    draw_image,
#ifdef HAVE_IMLIB2
    draw_image_warp,
#else
    NULL,
#endif
    draw_drag,
    NULL, /* font_new */
    gc_new,
    background_gc,
    overlay_new,
    image_new,
    get_data,
    image_free,
    NULL, /* get_text_bbox */
    overlay_disable,
    overlay_resize,
    set_attr,
    NULL,    /* show_native_keyboard */
    NULL,    /* hide_native_keyboard */
    get_dpi, /* get dpi */
    draw_polygon_with_holes,
};

static struct graphics_priv *graphics_gtk_drawing_area_new_helper(struct graphics_methods *meth) {
    struct font_priv *(*font_freetype_new)(void *meth);
    font_freetype_new = plugin_get_category_font("freetype");
    if (!font_freetype_new)
        return NULL;
    struct graphics_priv *this = g_new0(struct graphics_priv, 1);
    font_freetype_new(&this->freetype_methods);
    *meth = graphics_methods;
    meth->font_new = (struct graphics_font_priv
                      * (*)(struct graphics_priv *, struct graphics_font_methods *, char *, int,
                            int)) this->freetype_methods.font_new;
    meth->get_text_bbox = (void (*)(struct graphics_priv *, struct graphics_font_priv *, char *, int, int,
                                    struct point *, int))this->freetype_methods.get_text_bbox;
    return this;
}

static struct graphics_priv *graphics_gtk_drawing_area_new(struct navit *nav, struct graphics_methods *meth,
                                                           struct attr **attrs, struct callback_list *cbl) {
    int i;
    GtkWidget *draw;
    struct attr *attr;
    GtkGesture *click;
    GtkEventController *motion_controller;
    GtkEventController *scroll_controller;

    if (!event_request_system("glib", "graphics_gtk_drawing_area_new"))
        return NULL;

    draw = gtk_drawing_area_new();
    struct graphics_priv *this = graphics_gtk_drawing_area_new_helper(meth);
    this->nav = nav;
    this->widget = draw;
    this->win_w = 792;
    if ((attr = attr_search(attrs, attr_w)))
        this->win_w = attr->u.num;
    this->win_h = 547;
    if ((attr = attr_search(attrs, attr_h)))
        this->win_h = attr->u.num;
    this->timeout = 100;
    if ((attr = attr_search(attrs, attr_timeout)))
        this->timeout = attr->u.num;
    this->delay = 0;
    if ((attr = attr_search(attrs, attr_delay)))
        this->delay = attr->u.num;
    if ((attr = attr_search(attrs, attr_window_title)))
        this->window_title = g_strdup(attr->u.str);
    else
        this->window_title = g_strdup("Navit");
    this->cbl = cbl;

    /* Draw function (replaces expose_event + configure_event) */
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(draw), draw_func, this, NULL);

    /* Frame clock tick callback for smooth animation */
    this->tick_callback_id = gtk_widget_add_tick_callback(draw, on_frame_tick, this, NULL);

    /* Click gesture (replaces button_press_event / button_release_event) */
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* 0 = all buttons */
    g_signal_connect(click, "pressed", G_CALLBACK(on_gesture_click_pressed), this);
    g_signal_connect(click, "released", G_CALLBACK(on_gesture_click_released), this);
    gtk_widget_add_controller(draw, GTK_EVENT_CONTROLLER(click));

    /* Motion controller (replaces motion_notify_event) */
    motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(motion_controller, "motion", G_CALLBACK(on_motion), this);
    gtk_widget_add_controller(draw, motion_controller);

    /* Scroll controller (replaces scroll_event) */
    scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_scroll), this);
    gtk_widget_add_controller(draw, scroll_controller);

    for (i = 0; i < 8; i++) {
        this->button_press[i].tv_sec = 0;
        this->button_press[i].tv_usec = 0;
        this->button_release[i].tv_sec = 0;
        this->button_release[i].tv_usec = 0;
    }

    return this;
}

void plugin_init(void) {
    gtk_init();
#ifdef HAVE_API_WIN32
    setlocale(LC_NUMERIC, "C"); /* WIN32 gtk resets LC_NUMERIC */
#endif
    plugin_register_category_graphics("gtk_drawing_area", graphics_gtk_drawing_area_new);
}
