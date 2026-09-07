/**
 * Navit, a modular navigation system.
 * Copyright (C) 2024 Navit Team
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

/**
 * @file graphics_gtkglarea.c
 * @brief OpenGL ES 2.0 graphics backend using GTK4's GtkGLArea.
 *
 * Replaces Cairo/pixman software rendering with GPU-accelerated drawing.
 * Reuses GLES2 shaders from the existing opengl backend.
 */

#include <epoxy/gl.h>
#include <gdk/gdk.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <sys/time.h>

#include "attr.h"
#include "callback.h"
#include "color.h"
#include "config.h"
#include "coord.h"
#include "debug.h"
#include "event.h"
#include "graphics.h"
#include "item.h"
#include "keys.h"
#include "navit.h"
#include "navit/font/freetype/font_freetype.h"
#include "plugin.h"
#include "point.h"
#include "transform.h"
#include "util.h"
#include "window.h"

/* GLES2 shaders — adapted from graphics_opengl.c, with precision qualifiers for GLES2 */
static const char vertex_src[] = "precision highp float;\n"
                                 "attribute vec2 position;\n"
                                 "attribute vec2 texture_position;\n"
                                 "uniform mat4 mvp;\n"
                                 "varying vec2 v_texture_position;\n"
                                 "void main() {\n"
                                 "    v_texture_position = texture_position;\n"
                                 "    gl_Position = mvp * vec4(position, 0.0, 1.0);\n"
                                 "}\n";

static const char fragment_src[] = "precision mediump float;\n"
                                   "uniform vec4 avcolor;\n"
                                   "uniform sampler2D texture;\n"
                                   "uniform bool use_texture;\n"
                                   "varying vec2 v_texture_position;\n"
                                   "void main() {\n"
                                   "    if (use_texture) {\n"
                                   "        gl_FragColor = texture2D(texture, v_texture_position);\n"
                                   "    } else {\n"
                                   "        gl_FragColor = avcolor;\n"
                                   "    }\n"
                                   "}\n";

/* Initial vertex buffer size — grows dynamically if needed */
#define MAX_VERTICES 262144
/* Hard cap to prevent runaway allocation (~256 MB for vertices + commands) */
#define MAX_VERTICES_HARD (MAX_VERTICES * 16)

/* Vertex: position (x,y) + texcoord (u,v) */
typedef struct {
    float x, y;
    float u, v;
} Vertex;

/* Draw command types — includes clip state changes interleaved with draws */
enum draw_cmd_type {
    CMD_TRIANGLES,
    CMD_LINES,
    CMD_LINE_STRIP,
    CMD_LINE_LOOP,
    CMD_TEXTURED_QUADS,
    CMD_CLIP_ON,       /* glEnable(GL_SCISSOR_TEST) */
    CMD_CLIP_OFF,      /* glDisable(GL_SCISSOR_TEST) */
    CMD_CLIP_RECT,     /* glScissor(x, y, w, h) */
    CMD_STENCIL_CLEAR, /* clear stencil, enable stencil, INCR on draw */
    CMD_STENCIL_HOLES, /* switch to DECR on draw */
    CMD_STENCIL_APPLY, /* re-enable color, EQUAL(1), KEEP */
    CMD_STENCIL_END,   /* disable stencil test */
};

/* A batched draw command — carries per-command color and clip state */
typedef struct {
    enum draw_cmd_type type;
    GLuint tex;
    int first;
    int count;
    float r, g, b, a;                   /* color for this command */
    int clip_x, clip_y, clip_w, clip_h; /* for CMD_CLIP_RECT */
} DrawCmd;

/* Graphics GC (drawing context — color, linewidth, dashes) */
struct graphics_gc_priv {
    float fr, fg, fb, fa;
    float br, bg, bb, ba;
    struct color c;
    struct color bg_color;
    int linewidth;
    int dash_count;
    int dash_mask;
};

/* Graphics image */
struct graphics_image_priv {
    GLuint tex;
    int w, h;
    unsigned char *pixels;
    int stride;
};

/* Deferred texture deletion */
typedef struct tex_delete {
    GLuint tex;
    struct tex_delete *next;
} TexDelete;

/* Overlay state */
struct graphics_priv {
    /* GTK widget */
    GtkWidget *glarea;
    GtkWidget *win;

    /* GL resources */
    GLuint program;
    GLuint vbo;
    GLint mvp_loc;
    GLint pos_loc;
    GLint tex_pos_loc;
    GLint color_loc;
    GLint tex_loc;
    GLint use_tex_loc;
    GLuint white_tex;

    /* MVP matrix */
    float mvp[16];
    float display_rotation;
    int rotation_center_x, rotation_center_y;

    /* Viewport */
    int width, height;
    int win_w, win_h;

    /* Draw command buffer (dynamically allocated, grows as needed) */
    Vertex *vertices;
    int vertex_count;
    int vertex_capacity;
    DrawCmd *commands;
    int cmd_count;
    int cmd_capacity;
    int dirty;
    int draw_depth; /* guard against nested draw_mode_begin (gui_internal_highlight_do) */

    /* Drag offset (applied in MVP) */
    int dx, dy;

    /* Current color */
    float cur_r, cur_g, cur_b, cur_a;

    /* Deferred texture deletion */
    TexDelete *tex_delete_list;

    /* Background GC */
    struct graphics_gc_priv *bg_gc;

    /* Overlay linkage */
    struct graphics_priv *parent;
    struct graphics_priv *overlays;
    struct graphics_priv *next;
    int overlay_enabled;
    struct point p;
    int wraparound;

    /* Navit core linkage */
    struct navit *nav;
    struct callback_list *cbl;
    struct window window;
    struct font_freetype_methods freetype_methods;

    /* Window state */
    int delay;
    unsigned int pid;

    /* Button debounce */
    struct timeval button_press[8];
    struct timeval button_release[8];
    int timeout;
};

/* Forward declarations */
static struct graphics_priv *graphics_gtkglarea_new_helper(struct graphics_methods *meth);
static void get_data_window(struct graphics_priv *this, unsigned int xid);

/* ----------------------------------------------------------------
 * GL shader helpers
 * ---------------------------------------------------------------- */

static GLuint load_shader(const char *source, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gchar log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        dbg(lvl_error, "shader compile error: %s", log);
    }
    return shader;
}

static void setup_program(struct graphics_priv *gr) {
    GLuint vs = load_shader(vertex_src, GL_VERTEX_SHADER);
    GLuint fs = load_shader(fragment_src, GL_FRAGMENT_SHADER);

    gr->program = glCreateProgram();
    glAttachShader(gr->program, vs);
    glAttachShader(gr->program, fs);
    glLinkProgram(gr->program);

    GLint ok;
    glGetProgramiv(gr->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        gchar log[512];
        glGetProgramInfoLog(gr->program, sizeof(log), NULL, log);
        dbg(lvl_error, "program link error: %s", log);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    gr->mvp_loc = glGetUniformLocation(gr->program, "mvp");
    gr->pos_loc = glGetAttribLocation(gr->program, "position");
    gr->tex_pos_loc = glGetAttribLocation(gr->program, "texture_position");
    gr->color_loc = glGetUniformLocation(gr->program, "avcolor");
    gr->tex_loc = glGetUniformLocation(gr->program, "texture");
    gr->use_tex_loc = glGetUniformLocation(gr->program, "use_texture");

    /* Create VBO */
    glGenBuffers(1, &gr->vbo);

    /* Create a 1x1 white texture for solid-color draws */
    unsigned char white[] = {255, 255, 255, 255};
    glGenTextures(1, &gr->white_tex);
    glBindTexture(GL_TEXTURE_2D, gr->white_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
}

/* ----------------------------------------------------------------
 * MVP matrix helpers (orthographic projection)
 * ---------------------------------------------------------------- */

static void mvp_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mvp_ortho(float *m, float l, float r, float b, float t, float n, float f) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (r - l);
    m[5] = 2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    m[15] = 1.0f;
}

static void compute_mvp(struct graphics_priv *gr) {
    mvp_ortho(gr->mvp, 0, gr->width, (float)gr->height, 0, -1.0f, 1.0f);

    /* Apply drag offset */
    gr->mvp[12] += gr->dx * (2.0f / gr->width);
    gr->mvp[13] -= gr->dy * (2.0f / gr->height);

    /* Apply display rotation around center */
    if (gr->display_rotation != 0.0f && !gr->parent) {
        float cx = gr->width / 2.0f;
        float cy = gr->height / 2.0f;
        /* Translate to origin, rotate, translate back */
        float t1[16], t2[16], tmp[16];
        mvp_identity(t1);
        t1[12] = -cx;
        t1[13] = -cy;
        mvp_identity(t2);
        t2[12] = cx;
        t2[13] = cy;

        /* tmp = t2 * rot */
        float rot_angle = gr->display_rotation * G_PI / 180.0f;
        float cr = cosf(rot_angle);
        float sr = sinf(rot_angle);
        float rot_mat[16] = {cr, sr, 0, 0, -sr, cr, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        /* tmp = t2 * rot * t1 * mvp */
        /* Step 1: rot * t1 */
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                tmp[i * 4 + j] = 0;
                for (int k = 0; k < 4; k++)
                    tmp[i * 4 + j] += rot_mat[i * 4 + k] * t1[k * 4 + j];
            }
        /* Step 2: t2 * (rot * t1) */
        float tmp2[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                tmp2[i * 4 + j] = 0;
                for (int k = 0; k < 4; k++)
                    tmp2[i * 4 + j] += t2[i * 4 + k] * tmp[k * 4 + j];
            }
        /* Step 3: (t2 * rot * t1) * mvp */
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                tmp[i * 4 + j] = 0;
                for (int k = 0; k < 4; k++)
                    tmp[i * 4 + j] += tmp2[i * 4 + k] * gr->mvp[k * 4 + j];
            }
        memcpy(gr->mvp, tmp, sizeof(tmp));
    }
}

/* ----------------------------------------------------------------
 * Submit helpers — add vertices and commands to the batch
 * ---------------------------------------------------------------- */

static void ensure_vertex_capacity(struct graphics_priv *gr, int needed) {
    if (needed <= gr->vertex_capacity)
        return;
    int new_cap = gr->vertex_capacity;
    while (new_cap < needed)
        new_cap *= 2;
    if (new_cap > MAX_VERTICES_HARD)
        new_cap = MAX_VERTICES_HARD;
    if (new_cap <= gr->vertex_capacity)
        return;
    gr->vertices = g_realloc(gr->vertices, new_cap * sizeof(Vertex));
    gr->vertex_capacity = new_cap;
}

static void ensure_cmd_capacity(struct graphics_priv *gr, int needed) {
    if (needed <= gr->cmd_capacity)
        return;
    int new_cap = gr->cmd_capacity;
    while (new_cap < needed)
        new_cap *= 2;
    if (new_cap > MAX_VERTICES_HARD / 3)
        new_cap = MAX_VERTICES_HARD / 3;
    if (new_cap <= gr->cmd_capacity)
        return;
    gr->commands = g_realloc(gr->commands, new_cap * sizeof(DrawCmd));
    gr->cmd_capacity = new_cap;
}

static void queue_tex_delete(struct graphics_priv *gr, GLuint tex) {
    TexDelete *td = g_new(TexDelete, 1);
    td->tex = tex;
    td->next = gr->tex_delete_list;
    gr->tex_delete_list = td;
}

static void submit_color(struct graphics_priv *gr, struct graphics_gc_priv *gc) {
    gr->cur_r = gc->fr;
    gr->cur_g = gc->fg;
    gr->cur_b = gc->fb;
    gr->cur_a = gc->fa;
}

static int submit_vertices(struct graphics_priv *gr, enum draw_cmd_type type, GLuint tex, const Vertex *verts,
                           int count) {
    ensure_vertex_capacity(gr, gr->vertex_count + count);
    ensure_cmd_capacity(gr, gr->cmd_count + 1);
    if (gr->vertex_count + count > gr->vertex_capacity) {
        dbg(lvl_error, "gtkglarea: vertex buffer limit reached (%d + %d > %d)", gr->vertex_count, count,
            gr->vertex_capacity);
        return -1;
    }
    if (gr->cmd_count > 0) {
        DrawCmd *prev = &gr->commands[gr->cmd_count - 1];
        if (prev->type == type && prev->tex == tex && prev->r == gr->cur_r && prev->g == gr->cur_g
            && prev->b == gr->cur_b && prev->a == gr->cur_a && type != CMD_LINE_STRIP && type != CMD_LINE_LOOP) {
            prev->count += count;
            memcpy(&gr->vertices[gr->vertex_count], verts, count * sizeof(Vertex));
            gr->vertex_count += count;
            return gr->vertex_count - count;
        }
    }
    int first = gr->vertex_count;
    memcpy(&gr->vertices[gr->vertex_count], verts, count * sizeof(Vertex));
    gr->vertex_count += count;
    DrawCmd *cmd = &gr->commands[gr->cmd_count];
    cmd->type = type;
    cmd->tex = tex;
    cmd->first = first;
    cmd->count = count;
    cmd->r = gr->cur_r;
    cmd->g = gr->cur_g;
    cmd->b = gr->cur_b;
    cmd->a = gr->cur_a;
    gr->cmd_count++;
    return first;
}

/* ----------------------------------------------------------------
 * Graphics GC methods
 * ---------------------------------------------------------------- */

static void gc_destroy(struct graphics_gc_priv *gc) {
    g_free(gc);
}

static void gc_set_linewidth(struct graphics_gc_priv *gc, int w) {
    gc->linewidth = w;
}

static void gc_set_dashes(struct graphics_gc_priv *gc, int width, int offset, unsigned char *dash_list, int n) {
    /* TODO: implement dash pattern in fragment shader */
    gc->dash_count = n;
}

static void gc_set_foreground(struct graphics_gc_priv *gc, struct color *c) {
    gc->fr = c->r / 65535.0f;
    gc->fg = c->g / 65535.0f;
    gc->fb = c->b / 65535.0f;
    gc->fa = c->a / 65535.0f;
    gc->c = *c;
}

static void gc_set_background(struct graphics_gc_priv *gc, struct color *c) {
    gc->br = c->r / 65535.0f;
    gc->bg = c->g / 65535.0f;
    gc->bb = c->b / 65535.0f;
    gc->ba = c->a / 65535.0f;
    gc->bg_color = *c;
}

static struct graphics_gc_methods gc_methods = {
    gc_destroy, gc_set_linewidth, gc_set_dashes, gc_set_foreground, gc_set_background, NULL, /* gc_set_texture */
};

/* ----------------------------------------------------------------
 * Drawing primitives
 * ---------------------------------------------------------------- */

static void draw_mode(struct graphics_priv *gr, enum draw_mode_num mode) {
    if (gr->parent) {
        /* Overlay child — manage overlay's own buffer */
        if (mode == draw_mode_begin || mode == draw_mode_begin_clear) {
            if (gr->draw_depth > 0) {
                gr->draw_depth++;
                return;
            }
            gtk_gl_area_make_current(GTK_GL_AREA(gr->glarea));
            /* Delete old overlay textures before overwriting */
            while (gr->tex_delete_list) {
                TexDelete *next = gr->tex_delete_list->next;
                glDeleteTextures(1, &gr->tex_delete_list->tex);
                g_free(gr->tex_delete_list);
                gr->tex_delete_list = next;
            }
            gr->vertex_count = 0;
            gr->cmd_count = 0;
            gr->draw_depth = 1;
        }
        if (mode == draw_mode_end) {
            if (gr->draw_depth <= 1) {
                gr->draw_depth = 0;
                struct graphics_priv *root = gr;
                while (root->parent)
                    root = root->parent;
                gtk_widget_queue_draw(root->glarea);
                return;
            }
            gr->draw_depth--;
        }
        return;
    }
    /* Root graphics */
    if (mode == draw_mode_begin || mode == draw_mode_begin_clear) {
        if (gr->draw_depth > 0) {
            gr->draw_depth++;
            return; /* Nested begin */
        }
        gtk_gl_area_make_current(GTK_GL_AREA(gr->glarea));
        /* Delete old textures before overwriting the buffer */
        while (gr->tex_delete_list) {
            TexDelete *next = gr->tex_delete_list->next;
            glDeleteTextures(1, &gr->tex_delete_list->tex);
            g_free(gr->tex_delete_list);
            gr->tex_delete_list = next;
        }
        gr->vertex_count = 0;
        gr->cmd_count = 0;
        gr->draw_depth = 1;
    }
    if (mode == draw_mode_end) {
        if (gr->draw_depth <= 1) {
            gr->draw_depth = 0;
            gtk_widget_queue_draw(gr->glarea);
            return; /* Final or unbalanced end */
        }
        gr->draw_depth--;
    }
}

static void draw_rectangle(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int w, int h) {
    if (gr->parent && !gr->overlay_enabled)
        return;
    submit_color(gr, gc);
    Vertex verts[6] = {
        {p->x,     p->y,     0, 0},
        {p->x + w, p->y,     1, 0},
        {p->x + w, p->y + h, 1, 1},
        {p->x,     p->y,     0, 0},
        {p->x + w, p->y + h, 1, 1},
        {p->x,     p->y + h, 0, 1},
    };
    submit_vertices(gr, CMD_TRIANGLES, 0, verts, 6);
    gr->dirty = 1;
}

static void draw_lines(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count) {
    if (gr->parent && !gr->overlay_enabled)
        return;
    submit_color(gr, gc);
    Vertex *verts = g_new(Vertex, count);
    for (int i = 0; i < count; i++) {
        verts[i].x = p[i].x;
        verts[i].y = p[i].y;
        verts[i].u = verts[i].v = 0;
    }
    submit_vertices(gr, CMD_LINE_STRIP, 0, verts, count);
    g_free(verts);
    gr->dirty = 1;
}

static void draw_circle(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int r) {
    if (gr->parent && !gr->overlay_enabled)
        return;
    submit_color(gr, gc);
    int segments = 72;
    if (segments > gr->vertex_capacity - gr->vertex_count)
        segments = gr->vertex_capacity - gr->vertex_count;
    Vertex *verts = g_new(Vertex, segments);
    for (int i = 0; i < segments; i++) {
        float angle = 2.0f * G_PI * i / segments;
        verts[i].x = p->x + r * cosf(angle);
        verts[i].y = p->y + r * sinf(angle);
        verts[i].u = verts[i].v = 0;
    }
    submit_vertices(gr, CMD_LINE_LOOP, 0, verts, segments);
    g_free(verts);
    gr->dirty = 1;
}

static long long cross2d(int ax, int ay, int bx, int by, int cx, int cy) {
    return (long long)(bx - ax) * (cy - ay) - (long long)(by - ay) * (cx - ax);
}

static int point_in_triangle(int ax, int ay, int bx, int by, int cx, int cy, int px, int py) {
    long long d1 = cross2d(ax, ay, bx, by, px, py);
    long long d2 = cross2d(bx, by, cx, cy, px, py);
    long long d3 = cross2d(cx, cy, ax, ay, px, py);
    return (d1 > 0 && d2 > 0 && d3 > 0) || (d1 < 0 && d2 < 0 && d3 < 0);
}

static void draw_polygon(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count) {
    if (gr->parent && !gr->overlay_enabled)
        return;
    if (count < 3)
        return;
    submit_color(gr, gc);

    /* Copy to local buffer, strip closing point (first==last) and consecutive duplicates */
    struct point tmp[count];
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (n > 0 && p[i].x == tmp[n - 1].x && p[i].y == tmp[n - 1].y)
            continue;
        tmp[n++] = p[i];
    }
    if (n > 2 && tmp[0].x == tmp[n - 1].x && tmp[0].y == tmp[n - 1].y)
        n--;
    if (n < 3)
        return;

    /* Remove collinear vertices (cross product == 0) to help ear-clipping.
     * Use a separate output buffer to avoid corrupting neighbor reads. */
    {
        struct point reduced[count];
        int w = 0;
        for (int i = 0; i < n; i++) {
            int prev = (i - 1 + n) % n;
            int next = (i + 1) % n;
            if (cross2d(tmp[prev].x, tmp[prev].y, tmp[i].x, tmp[i].y, tmp[next].x, tmp[next].y) != 0)
                reduced[w++] = tmp[i];
        }
        for (int i = 0; i < w; i++)
            tmp[i] = reduced[i];
        n = w;
    }
    if (n < 3)
        return;

    int max_tris = n - 2;
    Vertex *verts = g_new(Vertex, max_tris * 3);
    int tri_count = 0;

    int *idx = g_new(int, n);
    for (int i = 0; i < n; i++)
        idx[i] = i;
    int rem = n;

    /* Determine winding direction from signed area (integer, no precision loss) */
    long long area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += (long long)tmp[i].x * tmp[j].y - (long long)tmp[j].x * tmp[i].y;
    }
    int ccw = (area > 0);

    /* Ear-clipping triangulation */
    while (rem > 2) {
        int ear_found = 0;
        for (int i = 0; i < rem; i++) {
            int prev = (i - 1 + rem) % rem;
            int next = (i + 1) % rem;

            long long cross = cross2d(tmp[idx[prev]].x, tmp[idx[prev]].y, tmp[idx[i]].x, tmp[idx[i]].y,
                                      tmp[idx[next]].x, tmp[idx[next]].y);
            int is_convex = ccw ? (cross > 0) : (cross < 0);
            if (!is_convex)
                continue;

            int ear = 1;
            for (int j = 0; j < rem; j++) {
                if (j == prev || j == i || j == next)
                    continue;
                if (point_in_triangle(tmp[idx[prev]].x, tmp[idx[prev]].y, tmp[idx[i]].x, tmp[idx[i]].y,
                                      tmp[idx[next]].x, tmp[idx[next]].y, tmp[idx[j]].x, tmp[idx[j]].y)) {
                    ear = 0;
                    break;
                }
            }
            if (!ear)
                continue;

            int vi = tri_count * 3;
            verts[vi].x = tmp[idx[prev]].x;
            verts[vi].y = tmp[idx[prev]].y;
            verts[vi].u = verts[vi].v = 0;
            verts[vi + 1].x = tmp[idx[i]].x;
            verts[vi + 1].y = tmp[idx[i]].y;
            verts[vi + 1].u = verts[vi + 1].v = 0;
            verts[vi + 2].x = tmp[idx[next]].x;
            verts[vi + 2].y = tmp[idx[next]].y;
            verts[vi + 2].u = verts[vi + 2].v = 0;
            tri_count++;

            memmove(&idx[i], &idx[i + 1], (rem - i - 1) * sizeof(int));
            rem--;
            ear_found = 1;
            break;
        }
        if (!ear_found)
            break;
    }

    /* Fan fallback if ear-clipping failed to triangulate fully */
    if (tri_count < max_tris) {
        dbg(lvl_warning, "fan fallback: ear-clipping got %d/%d triangles for %d-vertex polygon at (%d,%d)", tri_count,
            max_tris, n, tmp[0].x, tmp[0].y);
        tri_count = 0;
        for (int i = 1; i < n - 1; i++) {
            int vi = tri_count * 3;
            verts[vi].x = tmp[0].x;
            verts[vi].y = tmp[0].y;
            verts[vi].u = verts[vi].v = 0;
            verts[vi + 1].x = tmp[i].x;
            verts[vi + 1].y = tmp[i].y;
            verts[vi + 1].u = verts[vi + 1].v = 0;
            verts[vi + 2].x = tmp[i + 1].x;
            verts[vi + 2].y = tmp[i + 1].y;
            verts[vi + 2].u = verts[vi + 2].v = 0;
            tri_count++;
        }
    }

    submit_vertices(gr, CMD_TRIANGLES, 0, verts, tri_count * 3);
    g_free(verts);
    g_free(idx);
    gr->dirty = 1;
}

static void display_text_draw(struct font_freetype_text *text, struct graphics_priv *gr, struct graphics_gc_priv *fg,
                              struct graphics_gc_priv *bg, struct point *p) {
    int i, x, y, stride;
    struct font_freetype_glyph *g, **gp;
    struct color transparent = {0x0000, 0x0000, 0x0000, 0x0000};
    struct color black = fg->c;
    struct color white = bg ? bg->c : (struct color){0xffff, 0xffff, 0xffff, 0xffff};

    if (bg) {
        if (COLOR_IS_WHITE(black) && COLOR_IS_BLACK(white)) {
            black = (struct color){65535, 65535, 65535, 65535};
            white = (struct color){0, 0, 0, 65535};
        } else if (COLOR_IS_BLACK(black) && COLOR_IS_WHITE(white)) {
            white = (struct color){65535, 65535, 65535, 65535};
            black = (struct color){0, 0, 0, 65535};
        }
    } else {
        white = (struct color){0, 0, 0, 0};
    }

    /* Draw shadow/background glyphs */
    gp = text->glyph;
    i = text->glyph_count;
    x = p->x << 6;
    y = p->y << 6;
    while (i-- > 0) {
        g = *gp++;
        if (g->w && g->h && bg) {
            stride = (g->w + 2) * 4;
            unsigned char *shadow = g_malloc(stride * (g->h + 2));
            gr->freetype_methods.get_shadow(g, shadow, stride, &white, &transparent);

            /* Upload shadow as GL texture and draw */
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g->w + 2, g->h + 2, 0, GL_BGRA, GL_UNSIGNED_BYTE, shadow);

            int gx = ((x + g->x) >> 6) - 1;
            int gy = ((y + g->y) >> 6) - 1;
            gr->cur_r = gr->cur_g = gr->cur_b = 1.0f;
            gr->cur_a = 1.0f;
            Vertex verts[6] = {
                {gx,            gy,            0, 0},
                {gx + g->w + 2, gy,            1, 0},
                {gx + g->w + 2, gy + g->h + 2, 1, 1},
                {gx,            gy,            0, 0},
                {gx + g->w + 2, gy + g->h + 2, 1, 1},
                {gx,            gy + g->h + 2, 0, 1},
            };
            submit_vertices(gr, CMD_TEXTURED_QUADS, tex, verts, 6);
            g_free(shadow);
            queue_tex_delete(gr, tex);
            gr->dirty = 1;
        }
        x += g->dx;
        y += g->dy;
    }

    /* Draw foreground glyphs */
    x = p->x << 6;
    y = p->y << 6;
    gp = text->glyph;
    i = text->glyph_count;
    while (i-- > 0) {
        g = *gp++;
        if (g->w && g->h) {
            stride = g->w * 4;
            unsigned char *glyph = g_malloc(stride * g->h);
            gr->freetype_methods.get_glyph(g, glyph, stride, &black, bg ? &white : &transparent, &transparent);

            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g->w, g->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, glyph);

            int gx = (x + g->x) >> 6;
            int gy = (y + g->y) >> 6;
            gr->cur_r = gr->cur_g = gr->cur_b = 1.0f;
            gr->cur_a = 1.0f;
            Vertex verts[6] = {
                {gx,        gy,        0, 0},
                {gx + g->w, gy,        1, 0},
                {gx + g->w, gy + g->h, 1, 1},
                {gx,        gy,        0, 0},
                {gx + g->w, gy + g->h, 1, 1},
                {gx,        gy + g->h, 0, 1},
            };
            submit_vertices(gr, CMD_TEXTURED_QUADS, tex, verts, 6);
            g_free(glyph);
            queue_tex_delete(gr, tex);
            gr->dirty = 1;
        }
        x += g->dx;
        y += g->dy;
    }
}

static void draw_text(struct graphics_priv *gr, struct graphics_gc_priv *fg, struct graphics_gc_priv *bg,
                      struct graphics_font_priv *font, char *text, struct point *p, int dx, int dy) {
    if (!font) {
        dbg(lvl_error, "no font, returning");
        return;
    }
    if (gr->parent && !gr->overlay_enabled)
        return;
    struct font_freetype_text *t;
    t = gr->freetype_methods.text_new(text, (struct font_freetype_font *)font, dx, dy);
    display_text_draw(t, gr, fg, bg, p);
    gr->freetype_methods.text_destroy(t);
}

static void draw_image(struct graphics_priv *gr, struct graphics_gc_priv *fg, struct point *p,
                       struct graphics_image_priv *img) {
    if (!img)
        return;
    if (gr->parent && !gr->overlay_enabled)
        return;

    /* Lazy GL texture upload */
    if (!img->tex && img->pixels) {
        glGenTextures(1, &img->tex);
        glBindTexture(GL_TEXTURE_2D, img->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img->w, img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->pixels);
        g_free(img->pixels);
        img->pixels = NULL;
    }

    if (!img->tex)
        return;

    gr->cur_r = gr->cur_g = gr->cur_b = gr->cur_a = 1.0f;
    Vertex verts[6] = {
        {p->x,          p->y,          0, 0},
        {p->x + img->w, p->y,          1, 0},
        {p->x + img->w, p->y + img->h, 1, 1},
        {p->x,          p->y,          0, 0},
        {p->x + img->w, p->y + img->h, 1, 1},
        {p->x,          p->y + img->h, 0, 1},
    };
    submit_vertices(gr, CMD_TEXTURED_QUADS, img->tex, verts, 6);
    gr->dirty = 1;
}

static void draw_image_warp(struct graphics_priv *gr, struct graphics_gc_priv *fg, struct point *p, int count,
                            struct graphics_image_priv *img) {
    /* TODO: textured quad with 4-point transform */
    gr->dirty = 1;
}

static void draw_drag(struct graphics_priv *gr, struct point *p) {
    if (p) {
        gr->dx = p->x;
        gr->dy = p->y;
    } else {
        gr->dx = 0;
        gr->dy = 0;
    }
}

static int emit_cmd(struct graphics_priv *gr, enum draw_cmd_type type) {
    ensure_cmd_capacity(gr, gr->cmd_count + 1);
    if (gr->cmd_count >= gr->cmd_capacity)
        return -1;
    gr->commands[gr->cmd_count].type = type;
    gr->commands[gr->cmd_count].tex = 0;
    gr->commands[gr->cmd_count].first = 0;
    gr->commands[gr->cmd_count].count = 0;
    gr->cmd_count++;
    return 0;
}

static void draw_polygon_with_holes(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count,
                                    int hole_count, int *ccount, struct point **holes) {
    if (count < 3)
        return;

    /* Pass 1: stencil only — draw outer with INCR, holes with DECR.
     * CMD_STENCIL_CLEAR sets glColorMask(false) + GL_INCR.
     * CMD_STENCIL_HOLES switches to GL_DECR. */
    emit_cmd(gr, CMD_STENCIL_CLEAR);
    draw_polygon(gr, gc, p, count);
    if (hole_count > 0) {
        emit_cmd(gr, CMD_STENCIL_HOLES);
        for (int i = 0; i < hole_count; i++) {
            if (holes[i] && ccount[i] >= 3) {
                struct graphics_gc_priv hole_gc = *gc;
                hole_gc.fa = 0;
                draw_polygon(gr, &hole_gc, holes[i], ccount[i]);
            }
        }
    }

    /* Pass 2: color only where stencil==1 (polygon minus holes). */
    emit_cmd(gr, CMD_STENCIL_APPLY);
    draw_polygon(gr, gc, p, count);
    emit_cmd(gr, CMD_STENCIL_END);
    gr->dirty = 1;
}

/* ----------------------------------------------------------------
 * Image management
 * ---------------------------------------------------------------- */

static struct graphics_image_priv *image_new(struct graphics_priv *gr, struct graphics_image_methods *meth, char *path,
                                             int *w, int *h, struct point *hot, int rotation) {
    (void)rotation;
    if (!path || !*path)
        return NULL;

    GError *err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
    if (!pixbuf) {
        dbg(lvl_warning, "failed to load image '%s': %s", path, err->message);
        g_error_free(err);
        return NULL;
    }

    int orig_w = gdk_pixbuf_get_width(pixbuf);
    int orig_h = gdk_pixbuf_get_height(pixbuf);

    /* Scale if requested */
    if ((*w != orig_w || *h != orig_h) && *w != IMAGE_W_H_UNSET && *h != IMAGE_W_H_UNSET) {
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, *w, *h, GDK_INTERP_BILINEAR);
        g_object_unref(pixbuf);
        pixbuf = scaled;
        orig_w = *w;
        orig_h = *h;
    }

    *w = orig_w;
    *h = orig_h;
    hot->x = orig_w / 2;
    hot->y = orig_h / 2;

    /* Ensure we have an alpha channel for GL_RGBA upload */
    if (!gdk_pixbuf_get_has_alpha(pixbuf)) {
        GdkPixbuf *rgba = gdk_pixbuf_add_alpha(pixbuf, FALSE, 0, 0, 0);
        g_object_unref(pixbuf);
        pixbuf = rgba;
    }

    /* Defer GL texture upload — need current GL context */
    struct graphics_image_priv *img = g_new0(struct graphics_image_priv, 1);
    img->w = orig_w;
    img->h = orig_h;
    img->stride = gdk_pixbuf_get_rowstride(pixbuf);
    img->pixels = g_malloc(img->stride * orig_h);
    memcpy(img->pixels, gdk_pixbuf_get_pixels(pixbuf), img->stride * orig_h);
    g_object_unref(pixbuf);
    return img;
}

static void image_free(struct graphics_priv *gr, struct graphics_image_priv *img) {
    (void)gr;
    if (img) {
        if (img->tex)
            glDeleteTextures(1, &img->tex);
        g_free(img->pixels);
        g_free(img);
    }
}

/* ----------------------------------------------------------------
 * GC and overlay management
 * ---------------------------------------------------------------- */

static struct graphics_gc_priv *gc_new(struct graphics_priv *gr, struct graphics_gc_methods *meth) {
    struct graphics_gc_priv *gc = g_new0(struct graphics_gc_priv, 1);
    gc->fr = 1.0f;
    gc->fg = 1.0f;
    gc->fb = 1.0f;
    gc->fa = 1.0f;
    *meth = gc_methods;
    return gc;
}

static void background_gc(struct graphics_priv *gr, struct graphics_gc_priv *gc) {
    gr->bg_gc = gc;
}

static struct graphics_priv *overlay_new(struct graphics_priv *gr, struct graphics_methods *meth, struct point *p,
                                         int w, int h, int wraparound) {
    struct graphics_priv *this = graphics_gtkglarea_new_helper(meth);
    this->parent = gr;
    this->p = *p;
    this->width = w;
    this->height = h;
    this->wraparound = wraparound;
    this->glarea = gr->glarea;
    this->next = gr->overlays;
    gr->overlays = this;
    return this;
}

static void overlay_disable(struct graphics_priv *gr, int disable) {
    for (struct graphics_priv *ov = gr->overlays; ov; ov = ov->next)
        ov->overlay_enabled = !disable;
    gr->overlay_enabled = !disable;
}

static void overlay_resize(struct graphics_priv *gr, struct point *p, int w, int h, int wraparound) {
    gr->p = *p;
    gr->width = w;
    gr->height = h;
    gr->wraparound = wraparound;
}

/* ----------------------------------------------------------------
 * Display rotation
 * ---------------------------------------------------------------- */

static void set_display_rotation(struct graphics_priv *gr, double angle_degrees, int center_x, int center_y) {
    gr->display_rotation = angle_degrees;
    gr->rotation_center_x = center_x;
    gr->rotation_center_y = center_y;
}

/* ----------------------------------------------------------------
 * Clip and scroll (GL scissor + MVP translation)
 * ---------------------------------------------------------------- */

static void set_clip(struct graphics_priv *gr, struct point *p, int w, int h) {
    ensure_cmd_capacity(gr, gr->cmd_count + 2);
    if (gr->cmd_count < gr->cmd_capacity) {
        gr->commands[gr->cmd_count].type = CMD_CLIP_ON;
        gr->cmd_count++;
    }
    if (gr->cmd_count < gr->cmd_capacity) {
        /* Convert from top-left origin to GL scissor (bottom-left origin) */
        struct graphics_priv *cur;
        int off_x = 0, off_y = 0;
        for (cur = gr; cur->parent; cur = cur->parent) {
            off_x += cur->p.x;
            off_y += cur->p.y;
        }
        int win_h = cur->height;
        gr->commands[gr->cmd_count].type = CMD_CLIP_RECT;
        gr->commands[gr->cmd_count].clip_x = p->x + off_x;
        gr->commands[gr->cmd_count].clip_y = win_h - (p->y + h + off_y);
        gr->commands[gr->cmd_count].clip_w = w;
        gr->commands[gr->cmd_count].clip_h = h;
        gr->cmd_count++;
    }
}

static void set_clip_rects(struct graphics_priv *gr, struct point *p1, int w1, int h1, struct point *p2, int w2,
                           int h2) {
    /* GL doesn't support two scissor rects; use the union bounding box */
    struct graphics_priv *cur;
    int off_x = 0, off_y = 0;
    for (cur = gr; cur->parent; cur = cur->parent) {
        off_x += cur->p.x;
        off_y += cur->p.y;
    }
    int win_h = cur->height;
    int x_min = (p1->x < p2->x ? p1->x : p2->x) + off_x;
    int y_min = (p1->y < p2->y ? p1->y : p2->y) + off_y;
    int x_max1 = p1->x + w1 + off_x;
    int y_max1 = p1->y + h1 + off_y;
    int x_max2 = p2->x + w2 + off_x;
    int y_max2 = p2->y + h2 + off_y;
    int x_max = x_max1 > x_max2 ? x_max1 : x_max2;
    int y_max = y_max1 > y_max2 ? y_max1 : y_max2;
    ensure_cmd_capacity(gr, gr->cmd_count + 2);
    if (gr->cmd_count < gr->cmd_capacity) {
        gr->commands[gr->cmd_count].type = CMD_CLIP_ON;
        gr->cmd_count++;
    }
    if (gr->cmd_count < gr->cmd_capacity) {
        gr->commands[gr->cmd_count].type = CMD_CLIP_RECT;
        gr->commands[gr->cmd_count].clip_x = x_min;
        gr->commands[gr->cmd_count].clip_y = win_h - y_max;
        gr->commands[gr->cmd_count].clip_w = x_max - x_min;
        gr->commands[gr->cmd_count].clip_h = y_max - y_min;
        gr->cmd_count++;
    }
}

static void clear_clip(struct graphics_priv *gr) {
    ensure_cmd_capacity(gr, gr->cmd_count + 1);
    if (gr->cmd_count < gr->cmd_capacity) {
        gr->commands[gr->cmd_count].type = CMD_CLIP_OFF;
        gr->cmd_count++;
    }
}

static int scroll_surface(struct graphics_priv *gr, int dx, int dy) {
    (void)gr;
    (void)dx;
    (void)dy;
    /* No-op: navit's scroll+clip+redraw flow already updates the map transformation
     * and redraws the full map at the correct position via draw_mode_begin/end.
     * Adding an MVP offset here would double-shift the geometry. */
    return 1;
}

/* ----------------------------------------------------------------
 * DPI
 * ---------------------------------------------------------------- */

static navit_float get_dpi(struct graphics_priv *gr) {
    navit_float dpi = 96;
    GdkDisplay *display = gtk_widget_get_display(gr->glarea);
    if (display) {
        GtkNative *native = gtk_widget_get_native(gr->glarea);
        if (native) {
            GdkSurface *surface = gtk_native_get_surface(native);
            if (surface) {
                GdkMonitor *monitor = gdk_display_get_monitor_at_surface(display, surface);
                if (monitor)
                    dpi = 96 * gdk_monitor_get_scale_factor(monitor);
            }
        }
    }
    return dpi;
}

/* ----------------------------------------------------------------
 * Window management
 * ---------------------------------------------------------------- */

static int graphics_gtkglarea_fullscreen(struct window *w, int on) {
    struct graphics_priv *gr = w->priv;
    if (on)
        gtk_window_fullscreen(GTK_WINDOW(gr->win));
    else
        gtk_window_unfullscreen(GTK_WINDOW(gr->win));
    return 1;
}

static void graphics_gtkglarea_disable_suspend(struct window *w) {
}

static void close_request(GtkWindow *win, gpointer data) {
    struct graphics_priv *gr = data;
    callback_list_call_attr_0(gr->cbl, attr_window_closed);
}

static void get_data_window(struct graphics_priv *this, unsigned int xid) {
    if (!this->win) {
        this->win = gtk_window_new();
        gtk_window_set_default_size(GTK_WINDOW(this->win), this->win_w, this->win_h);
        gtk_window_set_title(GTK_WINDOW(this->win), "Navit");
        gtk_window_set_icon_name(GTK_WINDOW(this->win), "navit");
        gtk_window_set_decorated(GTK_WINDOW(this->win), FALSE);
        g_signal_connect(this->win, "close-request", G_CALLBACK(close_request), this);
    }
    gtk_window_set_child(GTK_WINDOW(this->win), this->glarea);
    gtk_widget_set_cursor_from_name(this->glarea, "default");
    gtk_widget_set_visible(this->win, TRUE);
    gtk_widget_set_focusable(this->glarea, TRUE);
    gtk_widget_set_sensitive(this->glarea, TRUE);
    gtk_widget_grab_focus(this->glarea);
}

static int set_attr(struct graphics_priv *gr, struct attr *attr) {
    switch (attr->type) {
    case attr_windowid:
        get_data_window(gr, attr->u.num);
        return 1;
    default:
        return 0;
    }
}

static void *get_data(struct graphics_priv *this, const char *type) {
    if (!strcmp(type, "gtk_widget"))
        return this->glarea;
    if (!strcmp(type, "window")) {
        this->window.fullscreen = graphics_gtkglarea_fullscreen;
        this->window.disable_suspend = graphics_gtkglarea_disable_suspend;
        this->window.priv = this;

        char *cp = getenv("NAVIT_XID");
        unsigned int xid = 0;
        if (cp)
            xid = strtol(cp, NULL, 0);
        if (!(this->delay & 1))
            get_data_window(this, xid);
        return &this->window;
    }
    return NULL;
}

static void graphics_destroy(struct graphics_priv *gr) {
    /* Free pending texture deletes */
    while (gr->tex_delete_list) {
        TexDelete *next = gr->tex_delete_list->next;
        g_free(gr->tex_delete_list);
        gr->tex_delete_list = next;
    }
    if (gr->program)
        glDeleteProgram(gr->program);
    if (gr->vbo)
        glDeleteBuffers(1, &gr->vbo);
    if (gr->white_tex)
        glDeleteTextures(1, &gr->white_tex);
    g_free(gr->vertices);
    g_free(gr->commands);
    g_free(gr);
}

/* ----------------------------------------------------------------
 * GtkGLArea callbacks
 * ---------------------------------------------------------------- */

static gboolean on_glarea_realize(GtkWidget *widget, gpointer data) {
    struct graphics_priv *gr = data;
    gtk_gl_area_make_current(GTK_GL_AREA(widget));
    if (gtk_gl_area_get_error(GTK_GL_AREA(widget)))
        return FALSE;

    setup_program(gr);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    return FALSE;
}

static void exec_draw_cmds(struct graphics_priv *gr, DrawCmd *commands, int cmd_count, Vertex *vertices,
                           int vertex_count, GLuint vbo, float *mvp) {
    glUseProgram(gr->program);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUniformMatrix4fv(gr->mvp_loc, 1, GL_FALSE, mvp);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(Vertex), vertices, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(gr->pos_loc);
    glVertexAttribPointer(gr->pos_loc, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)0);
    if (gr->tex_pos_loc >= 0) {
        glEnableVertexAttribArray(gr->tex_pos_loc);
        glVertexAttribPointer(gr->tex_pos_loc, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)(2 * sizeof(float)));
    }

    float last_r = -1, last_g = -1, last_b = -1, last_a = -1;

    for (int i = 0; i < cmd_count; i++) {
        DrawCmd *cmd = &commands[i];

        switch (cmd->type) {
        case CMD_CLIP_ON:
            glEnable(GL_SCISSOR_TEST);
            continue;
        case CMD_CLIP_OFF:
            glDisable(GL_SCISSOR_TEST);
            continue;
        case CMD_CLIP_RECT:
            glScissor(cmd->clip_x, cmd->clip_y, cmd->clip_w, cmd->clip_h);
            continue;
        case CMD_STENCIL_CLEAR:
            glEnable(GL_STENCIL_TEST);
            glClear(GL_STENCIL_BUFFER_BIT);
            glStencilFunc(GL_ALWAYS, 0, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            continue;
        case CMD_STENCIL_HOLES:
            glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
            continue;
        case CMD_STENCIL_APPLY:
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            continue;
        case CMD_STENCIL_END:
            glDisable(GL_STENCIL_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            continue;
        default:
            break;
        }

        if (cmd->r != last_r || cmd->g != last_g || cmd->b != last_b || cmd->a != last_a) {
            glUniform4f(gr->color_loc, cmd->r, cmd->g, cmd->b, cmd->a);
            last_r = cmd->r;
            last_g = cmd->g;
            last_b = cmd->b;
            last_a = cmd->a;
        }

        if (cmd->tex) {
            glUniform1i(gr->use_tex_loc, 1);
            glBindTexture(GL_TEXTURE_2D, cmd->tex);
        } else {
            glUniform1i(gr->use_tex_loc, 0);
            glBindTexture(GL_TEXTURE_2D, gr->white_tex);
        }

        GLenum mode;
        switch (cmd->type) {
        case CMD_TRIANGLES:
            mode = GL_TRIANGLES;
            break;
        case CMD_LINES:
            mode = GL_LINES;
            break;
        case CMD_LINE_STRIP:
            mode = GL_LINE_STRIP;
            break;
        case CMD_LINE_LOOP:
            mode = GL_LINE_LOOP;
            break;
        case CMD_TEXTURED_QUADS:
            mode = GL_TRIANGLES;
            break;
        default:
            continue;
        }
        glDrawArrays(mode, cmd->first, cmd->count);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

static void render_overlay(struct graphics_priv *root_gr, struct graphics_priv *ov) {
    if (!root_gr->overlay_enabled)
        return;
    for (; ov; ov = ov->next) {
        if (!ov->overlay_enabled || ov->vertex_count == 0)
            continue;

        /* Accumulate full position from overlay up to root */
        float tx = 0, ty = 0;
        for (struct graphics_priv *cur = ov; cur && cur != root_gr; cur = cur->parent) {
            tx += cur->p.x * (2.0f / root_gr->width);
            ty += -cur->p.y * (2.0f / root_gr->height);
        }

        float ov_mvp[16];
        mvp_ortho(ov_mvp, 0, root_gr->width, (float)root_gr->height, 0, -1.0f, 1.0f);
        ov_mvp[12] += tx;
        ov_mvp[13] += ty;

        exec_draw_cmds(root_gr, ov->commands, ov->cmd_count, ov->vertices, ov->vertex_count, root_gr->vbo, ov_mvp);

        /* Recurse for nested child overlays */
        if (ov->overlays)
            render_overlay(root_gr, ov->overlays);
    }
}

static gboolean on_glarea_render(GtkGLArea *glarea, GdkGLContext *context, gpointer data) {
    struct graphics_priv *gr = data;
    (void)glarea;
    (void)context;

    /* Skip while a frame is being constructed to avoid showing partial/incomplete state */
    if (gr->draw_depth > 0)
        return TRUE;

    /* Clear to black — the background rectangle from the vertex buffer covers the viewport */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    /* Render root draw commands — data persists between renders until overwritten */
    if (gr->vertex_count > 0 || gr->cmd_count > 0) {
        float root_mvp[16];
        compute_mvp(gr);
        memcpy(root_mvp, gr->mvp, sizeof(root_mvp));
        exec_draw_cmds(gr, gr->commands, gr->cmd_count, gr->vertices, gr->vertex_count, gr->vbo, root_mvp);
    }

    /* Draw overlays (recursively for nested overlays) */
    render_overlay(gr, gr->overlays);

    return TRUE;
}

static void on_glarea_resize(GtkGLArea *glarea, int width, int height, gpointer data) {
    struct graphics_priv *gr = data;
    gr->width = width;
    gr->height = height;

    glViewport(0, 0, width, height);

    navit_set_render_margin(gr->nav, (width > height ? width : height) / 4);

    callback_list_call_attr_2(gr->cbl, attr_resize, GINT_TO_POINTER(width), GINT_TO_POINTER(height));
}

/* ----------------------------------------------------------------
 * Input handling via GTK4 event controllers
 * ---------------------------------------------------------------- */

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
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));

    gettimeofday(&tv, &tz);

    if (gdk_event_get_event_type(event) == GDK_TOUCH_BEGIN) {
        p.x = (int)x;
        p.y = (int)y;
        callback_list_call_attr_3(this->cbl, attr_button, GINT_TO_POINTER(1), GINT_TO_POINTER(1), (void *)&p);
        return;
    }

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
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));

    gettimeofday(&tv, &tz);

    if (gdk_event_get_event_type(event) == GDK_TOUCH_END) {
        p.x = (int)x;
        p.y = (int)y;
        callback_list_call_attr_3(this->cbl, attr_button, GINT_TO_POINTER(0), GINT_TO_POINTER(1), (void *)&p);
        return;
    }

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

static void on_gesture_update(GtkGesture *gesture, GdkEventSequence *sequence, gpointer user_data) {
    struct graphics_priv *this = user_data;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    struct point p;
    double x, y;

    (void)sequence;
    if (!event)
        return;
    gdk_event_get_position(event, &x, &y);
    p.x = (int)x;
    p.y = (int)y;
    callback_list_call_attr_1(this->cbl, attr_motion, (void *)&p);
}

static void on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct point p;
    (void)controller;

    p.x = (int)x;
    p.y = (int)y;
    callback_list_call_attr_1(this->cbl, attr_motion, (void *)&p);
}

static void on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct point p;
    int button;
    double x, y;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    (void)dx;

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

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state,
                               gpointer data) {
    struct graphics_priv *this = data;
    (void)controller;
    (void)keycode;
    (void)state;

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
    return TRUE;
}

/* ----------------------------------------------------------------
 * Plugin init and graphics_new
 * ---------------------------------------------------------------- */

static struct graphics_methods graphics_methods = {
    graphics_destroy,        /* graphics_destroy */
    draw_mode,               /* draw_mode */
    draw_lines,              /* draw_lines */
    draw_polygon,            /* draw_polygon */
    draw_rectangle,          /* draw_rectangle */
    draw_circle,             /* draw_circle */
    draw_text,               /* draw_text */
    draw_image,              /* draw_image */
    draw_image_warp,         /* draw_image_warp */
    draw_drag,               /* draw_drag */
    NULL,                    /* font_new — set from freetype */
    gc_new,                  /* gc_new */
    background_gc,           /* background_gc */
    overlay_new,             /* overlay_new */
    image_new,               /* image_new */
    get_data,                /* get_data */
    image_free,              /* image_free */
    NULL,                    /* get_text_bbox — set from freetype */
    overlay_disable,         /* overlay_disable */
    overlay_resize,          /* overlay_resize */
    set_attr,                /* set_attr */
    NULL,                    /* show_native_keyboard */
    NULL,                    /* hide_native_keyboard */
    get_dpi,                 /* get_dpi */
    draw_polygon_with_holes, /* draw_polygon_with_holes */
    set_display_rotation,    /* set_display_rotation */
    set_clip,                /* set_clip */
    set_clip_rects,          /* set_clip_rects */
    clear_clip,              /* clear_clip */
    scroll_surface,          /* scroll */
};

static struct graphics_priv *graphics_gtkglarea_new_helper(struct graphics_methods *meth) {
    struct font_priv *(*font_freetype_new)(void *meth);
    font_freetype_new = plugin_get_category_font("freetype");
    if (!font_freetype_new)
        return NULL;

    struct graphics_priv *this = g_new0(struct graphics_priv, 1);
    this->vertex_capacity = MAX_VERTICES;
    this->vertices = g_new(Vertex, MAX_VERTICES);
    this->cmd_capacity = MAX_VERTICES / 3;
    this->commands = g_new(DrawCmd, MAX_VERTICES / 3);
    font_freetype_new(&this->freetype_methods);
    *meth = graphics_methods;
    meth->font_new = (struct graphics_font_priv
                      * (*)(struct graphics_priv *, struct graphics_font_methods *, char *, int,
                            int)) this->freetype_methods.font_new;
    meth->get_text_bbox = (void (*)(struct graphics_priv *, struct graphics_font_priv *, char *, int, int,
                                    struct point *, int))this->freetype_methods.get_text_bbox;
    return this;
}

static struct graphics_priv *graphics_gtkglarea_new(struct navit *nav, struct graphics_methods *meth,
                                                    struct attr **attrs, struct callback_list *cbl) {
    struct attr *attr;

    if (!event_request_system("glib", "graphics_gtkglarea_new"))
        return NULL;

    struct graphics_priv *this = graphics_gtkglarea_new_helper(meth);
    if (!this)
        return NULL;

    this->nav = nav;
    this->cbl = cbl;
    this->win_w = 792;
    this->win_h = 547;
    this->delay = 0;

    if ((attr = attr_search(attrs, attr_w)))
        this->win_w = attr->u.num;
    if ((attr = attr_search(attrs, attr_h)))
        this->win_h = attr->u.num;
    if ((attr = attr_search(attrs, attr_timeout)))
        this->delay = attr->u.num;

    /* Create GtkGLArea with GLES 2.0 context */
    this->glarea = gtk_gl_area_new();
    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(this->glarea), GDK_GL_API_GLES);
    gtk_gl_area_set_required_version(GTK_GL_AREA(this->glarea), 2, 0);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(this->glarea), FALSE);
    /* Stencil buffer needed for polygon hole cutting (even-odd fill).
     * An alternative is a single-pass shader evaluating winding-number per fragment,
     * but that adds texture uploads per polygon and heavier fragment work —
     * we avoid it because it hurts performance on low-end mobile GPUs. */
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(this->glarea), TRUE);

    g_signal_connect(this->glarea, "realize", G_CALLBACK(on_glarea_realize), this);
    g_signal_connect(this->glarea, "render", G_CALLBACK(on_glarea_render), this);
    g_signal_connect(this->glarea, "resize", G_CALLBACK(on_glarea_resize), this);

    /* Set up input controllers */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_gesture_click_pressed), this);
    g_signal_connect(click, "released", G_CALLBACK(on_gesture_click_released), this);
    g_signal_connect(click, "update", G_CALLBACK(on_gesture_update), this);
    gtk_widget_add_controller(this->glarea, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), this);
    gtk_widget_add_controller(this->glarea, motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), this);
    gtk_widget_add_controller(this->glarea, scroll);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), this);
    gtk_widget_add_controller(this->glarea, key);

    this->timeout = 100;
    for (int i = 0; i < 8; i++) {
        this->button_press[i].tv_sec = 0;
        this->button_press[i].tv_usec = 0;
        this->button_release[i].tv_sec = 0;
        this->button_release[i].tv_usec = 0;
    }

    return this;
}

void plugin_init(void) {
    plugin_register_category_graphics("gtkglarea", graphics_gtkglarea_new);
}
