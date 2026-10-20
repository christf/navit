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
#include <stdlib.h>
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
#define CIRCLE_SEGMENTS 72
/* Separates the strips of a merged indexed line-strip draw. Valid because the vertex count is
 * far below 2^32. */
#define GL_RESTART_INDEX 0xFFFFFFFFu
/* Margin around the viewport kept in the retained map texture, as a divisor of the larger screen
 * dimension. Mirrors the cairo backend's RENDER_MARGIN_DEFAULT_DIVISOR. */
#define GTKGL_MAP_MARGIN_DIVISOR 4

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
    float linewidth;                    /* for the line commands (CMD_LINES/STRIP/LOOP) */
    int clip_x, clip_y, clip_w, clip_h; /* for CMD_CLIP_RECT */
    /* Screen-space bounds of the command's vertices, used to skip geometry that cannot touch the
     * region being rendered. */
    float min_x, min_y, max_x, max_y;
} DrawCmd;

/* A run of consecutive line-strip commands with identical state, drawn from the vertex buffer
 * through an index range that separates the strips with the primitive-restart marker. */
struct line_run {
    int index_start;
    int index_count;
    int span;
};

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
    GLuint index_vbo;
    GLuint *index_buf;
    int index_capacity;
    struct line_run *line_runs;
    int line_runs_capacity;
    GLint mvp_loc;
    GLint pos_loc;
    GLint tex_pos_loc;
    GLint color_loc;
    GLint tex_loc;
    GLint use_tex_loc;
    GLuint white_tex;

    /* Off-screen multisample framebuffer. GtkGLArea renders into its own single-sampled
     * framebuffer and exposes no multisampling option, so the scene is rendered here and
     * resolved into the GtkGLArea framebuffer with glBlitFramebuffer. msaa_samples == 0
     * disables the path and rendering goes straight to the GtkGLArea framebuffer. */
    GLuint msaa_fbo;
    GLuint msaa_color_rb;
    GLuint msaa_stencil_rb;
    int msaa_samples;
    GLenum msaa_color_format;
    int msaa_alloc_w, msaa_alloc_h;

    /* Retained map frame. The root display list is rendered once into this off-screen target,
     * padded by render_margin on every side, and then presented translated by the drag offset
     * (and the pinch preview transform) instead of re-issuing every draw command on each pan
     * frame. This is the GL equivalent of the cairo backend's scroll_surface; it keeps panning
     * cheap at dense zoom levels, where replaying the retained command list costs ~150ms/frame. */
    GLuint map_msaa_fbo;
    GLuint map_msaa_color_rb;
    GLuint map_msaa_stencil_rb;
    GLuint map_fbo;
    GLuint map_tex;
    GLuint map_stencil_rb;
    int map_alloc_w, map_alloc_h;
    int map_valid;
    int render_margin; /* logical margin kept around the viewport in the map texture */

    /* MVP matrix */
    float mvp[16];
    float display_rotation;
    int rotation_center_x, rotation_center_y;

    /* Viewport. width/height hold the logical (GTK) size navit lays out in, matching the cairo
     * backend; phys_w/phys_h are the GL framebuffer size in device pixels (logical * scale).
     * scale is the monitor scale factor, needed to convert navit's logical coordinates to the
     * device pixels that GL APIs take (scissor rects, line width). */
    int width, height;
    int phys_w, phys_h;
    int scale;
    int win_w, win_h;

    /* Draw command buffer (dynamically allocated, grows as needed) */
    Vertex *vertices;
    int vertex_count;
    int vertex_capacity;
    DrawCmd *commands;
    int cmd_count;
    int cmd_capacity;
    /* Scratch buffers for the culled subset that is actually uploaded for a render. */
    DrawCmd *batch_commands;
    int batch_cmd_capacity;
    Vertex *batch_vertices;
    int batch_vertex_capacity;
    int dirty;
    int draw_depth; /* guard against nested draw_mode_begin (gui_internal_highlight_do) */

    /* Drag offset (applied in MVP) */
    int dx, dy;

    /* Current color */
    float cur_r, cur_g, cur_b, cur_a;

    /* Current line width, only meaningful for the line commands */
    float cur_linewidth;

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

    /* Last known pointer position. A GdkScrollEvent carries no position, so the
     * scroll handler has to use the most recent motion event instead. */
    struct point last_motion;

    /* Button debounce */
    struct timeval button_press[8];
    struct timeval button_release[8];
    int timeout;

    /* Map scale in effect when the pinch began, so the gesture scale can be applied absolutely */
    long pinch_base_scale;
    /* The frame rendered while the fingers move: the retained one scaled by preview_scale about
     * the gesture's start midpoint and translated by that midpoint's movement. No display list
     * rebuild runs until the gesture ends. */
    float preview_scale, preview_cx, preview_cy;
    float pinch_base_cx, pinch_base_cy;
    /* Set once a pinch has fetched the wider region, so zooming out only rebuilds once per gesture. */
    int pinch_prefetched;
    /* Pinch gesture, kept for the click update handler to know whether a pinch owns the touch */
    GtkGesture *zoom_gesture;
    /* A single-finger touch drag is in progress: its press was forwarded and the release is still
     * pending. A pinch takes the sequence over and ends the drag itself. */
    int touch_drag;
};

/* Forward declarations */
static struct graphics_priv *graphics_gtkglarea_new_helper(struct graphics_methods *meth);
static void get_data_window(struct graphics_priv *this, unsigned int xid);
static void msaa_destroy(struct graphics_priv *gr);
static void map_destroy(struct graphics_priv *gr);
static void exec_draw_cmds(struct graphics_priv *gr, DrawCmd *commands, int cmd_count, Vertex *vertices,
                           int vertex_count, GLuint vbo, float *mvp);

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
    glGenBuffers(1, &gr->index_vbo);

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

/* out = a * b for column-major 4x4 matrices. out must not alias a or b. */
static void mvp_mul(float *out, const float *a, const float *b) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] + a[8 + row] * b[col * 4 + 2]
                                 + a[12 + row] * b[col * 4 + 3];
        }
    }
}

static void compute_mvp(struct graphics_priv *gr, float *out) {
    float tmp[16], tmp2[16];

    mvp_ortho(out, 0, gr->width, (float)gr->height, 0, -1.0f, 1.0f);

    /* Drag offset is a pure screen-space translation */
    if (gr->dx || gr->dy) {
        float d[16];
        mvp_identity(d);
        d[12] = gr->dx;
        d[13] = gr->dy;
        mvp_mul(tmp, out, d);
        memcpy(out, tmp, sizeof(tmp));
    }

    /* Pinch preview transforms the retained frame until the gesture ends, so the map tracks the
     * two fingers without rebuilding the display list. p' = now + s (p - base), i.e. scaled about
     * the start midpoint and moved with the current one. */
    if (gr->preview_scale != 1.0f || gr->preview_cx != gr->pinch_base_cx || gr->preview_cy != gr->pinch_base_cy) {
        float s = gr->preview_scale;
        float c[16];
        mvp_identity(c);
        c[0] = s;
        c[5] = s;
        c[12] = gr->preview_cx - s * gr->pinch_base_cx;
        c[13] = gr->preview_cy - s * gr->pinch_base_cy;
        mvp_mul(tmp, out, c);
        memcpy(out, tmp, sizeof(tmp));
    }

    /* Rotate the projected scene around the vehicle cursor, matching the cairo backend:
     * p' = R(-angle) * (p - center) + center. */
    if (gr->display_rotation != 0.0f && !gr->parent) {
        float cx = gr->rotation_center_x;
        float cy = gr->rotation_center_y;
        float rot_angle = -gr->display_rotation * G_PI / 180.0f;
        float cr = cosf(rot_angle);
        float sr = sinf(rot_angle);
        float rot[16] = {cr, sr, 0, 0, -sr, cr, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float to_center[16], from_center[16];

        mvp_identity(to_center);
        to_center[12] = cx;
        to_center[13] = cy;
        mvp_identity(from_center);
        from_center[12] = -cx;
        from_center[13] = -cy;

        mvp_mul(tmp, out, to_center);
        mvp_mul(tmp2, tmp, rot);
        mvp_mul(out, tmp2, from_center);
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
    gr->cur_linewidth = gc->linewidth;
}

static void vertex_bounds(const Vertex *verts, int count, float *min_x, float *min_y, float *max_x, float *max_y) {
    *min_x = *max_x = verts[0].x;
    *min_y = *max_y = verts[0].y;
    for (int i = 1; i < count; i++) {
        if (verts[i].x < *min_x)
            *min_x = verts[i].x;
        if (verts[i].x > *max_x)
            *max_x = verts[i].x;
        if (verts[i].y < *min_y)
            *min_y = verts[i].y;
        if (verts[i].y > *max_y)
            *max_y = verts[i].y;
    }
}

static int is_draw_cmd(enum draw_cmd_type type) {
    return type == CMD_TRIANGLES || type == CMD_LINES || type == CMD_LINE_STRIP || type == CMD_LINE_LOOP
           || type == CMD_TEXTURED_QUADS;
}

static void ensure_batch_capacity(struct graphics_priv *gr, int cmd_need, int vertex_need) {
    if (cmd_need > gr->batch_cmd_capacity) {
        int new_cap = gr->batch_cmd_capacity ? gr->batch_cmd_capacity : 4096;
        while (new_cap < cmd_need)
            new_cap *= 2;
        gr->batch_commands = g_realloc(gr->batch_commands, new_cap * sizeof(DrawCmd));
        gr->batch_cmd_capacity = new_cap;
    }
    if (vertex_need > gr->batch_vertex_capacity) {
        int new_cap = gr->batch_vertex_capacity ? gr->batch_vertex_capacity : 16384;
        while (new_cap < vertex_need)
            new_cap *= 2;
        gr->batch_vertices = g_realloc(gr->batch_vertices, new_cap * sizeof(Vertex));
        gr->batch_vertex_capacity = new_cap;
    }
}

static void batch_append_span(struct graphics_priv *gr, const DrawCmd *src, int first, int count, int *cc, int *vc) {
    ensure_batch_capacity(gr, *cc + 1, *vc + count);
    float min_x, min_y, max_x, max_y;
    vertex_bounds(&gr->vertices[first], count, &min_x, &min_y, &max_x, &max_y);
    DrawCmd *prev = *cc > 0 ? &gr->batch_commands[*cc - 1] : NULL;
    if (prev && prev->type == src->type && prev->tex == src->tex && prev->r == src->r && prev->g == src->g
        && prev->b == src->b && prev->a == src->a && prev->linewidth == src->linewidth && src->type != CMD_LINE_STRIP
        && src->type != CMD_LINE_LOOP) {
        prev->count += count;
        if (min_x < prev->min_x)
            prev->min_x = min_x;
        if (min_y < prev->min_y)
            prev->min_y = min_y;
        if (max_x > prev->max_x)
            prev->max_x = max_x;
        if (max_y > prev->max_y)
            prev->max_y = max_y;
    } else {
        gr->batch_commands[*cc] = *src;
        gr->batch_commands[*cc].first = *vc;
        gr->batch_commands[*cc].count = count;
        gr->batch_commands[*cc].min_x = min_x;
        gr->batch_commands[*cc].min_y = min_y;
        gr->batch_commands[*cc].max_x = max_x;
        gr->batch_commands[*cc].max_y = max_y;
        (*cc)++;
    }
    memcpy(&gr->batch_vertices[*vc], &gr->vertices[first], count * sizeof(Vertex));
    *vc += count;
}

/* Copy the commands that can touch the given screen-space rectangle into the scratch batch,
 * compacting their vertices so that only the visible subset is uploaded. State commands (clip,
 * stencil) are kept verbatim so the sequences stay valid. Line strips are split into the runs of
 * segments that cross the rectangle: a road spanning the whole map has a huge bounding box but
 * usually only a few segments inside one rectangle. submit_vertices merges adjacent same-state
 * primitives, so a merged building/landuse command has a map-wide bounding box as well; those are
 * culled per triangle (or line pair) instead of per command. */
static int build_culled_batch(struct graphics_priv *gr, float min_x, float min_y, float max_x, float max_y,
                              int *out_vertex_count) {
    int vc = 0, cc = 0;
    for (int i = 0; i < gr->cmd_count; i++) {
        DrawCmd *cmd = &gr->commands[i];
        if (!is_draw_cmd(cmd->type)) {
            ensure_batch_capacity(gr, cc + 1, vc);
            gr->batch_commands[cc] = *cmd;
            cc++;
            continue;
        }
        if (cmd->max_x < min_x || cmd->min_x > max_x || cmd->max_y < min_y || cmd->min_y > max_y)
            continue;
        if (cmd->type == CMD_LINE_STRIP && cmd->count >= 2) {
            int run_start = -1;
            for (int k = 0; k < cmd->count - 1; k++) {
                const Vertex *v0 = &gr->vertices[cmd->first + k];
                const Vertex *v1 = &gr->vertices[cmd->first + k + 1];
                float sx0 = v0->x < v1->x ? v0->x : v1->x;
                float sx1 = v0->x < v1->x ? v1->x : v0->x;
                float sy0 = v0->y < v1->y ? v0->y : v1->y;
                float sy1 = v0->y < v1->y ? v1->y : v0->y;
                int hit = sx1 >= min_x && sx0 <= max_x && sy1 >= min_y && sy0 <= max_y;
                if (hit) {
                    if (run_start < 0)
                        run_start = k;
                } else if (run_start >= 0) {
                    batch_append_span(gr, cmd, cmd->first + run_start, k - run_start + 1, &cc, &vc);
                    run_start = -1;
                }
            }
            if (run_start >= 0)
                batch_append_span(gr, cmd, cmd->first + run_start, cmd->count - run_start, &cc, &vc);
        } else if (cmd->type == CMD_TRIANGLES || cmd->type == CMD_TEXTURED_QUADS || cmd->type == CMD_LINES) {
            int group = cmd->type == CMD_LINES ? 2 : (cmd->type == CMD_TEXTURED_QUADS ? 6 : 3);
            int run_start = -1;
            int k = 0;
            while (k + group <= cmd->count) {
                float sx0 = gr->vertices[cmd->first + k].x, sx1 = sx0;
                float sy0 = gr->vertices[cmd->first + k].y, sy1 = sy0;
                for (int j = 1; j < group; j++) {
                    const Vertex *v = &gr->vertices[cmd->first + k + j];
                    if (v->x < sx0)
                        sx0 = v->x;
                    if (v->x > sx1)
                        sx1 = v->x;
                    if (v->y < sy0)
                        sy0 = v->y;
                    if (v->y > sy1)
                        sy1 = v->y;
                }
                int hit = sx1 >= min_x && sx0 <= max_x && sy1 >= min_y && sy0 <= max_y;
                if (hit) {
                    if (run_start < 0)
                        run_start = k;
                } else if (run_start >= 0) {
                    batch_append_span(gr, cmd, cmd->first + run_start, k - run_start, &cc, &vc);
                    run_start = -1;
                }
                k += group;
            }
            if (run_start >= 0)
                batch_append_span(gr, cmd, cmd->first + run_start, k - run_start, &cc, &vc);
        } else {
            batch_append_span(gr, cmd, cmd->first, cmd->count, &cc, &vc);
        }
    }
    *out_vertex_count = vc;
    dbg(lvl_debug, "cull: rect=[%.0f,%.0f..%.0f,%.0f] out=%dcmds/%dverts", min_x, min_y, max_x, max_y, cc, vc);
    return cc;
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
    float min_x, min_y, max_x, max_y;
    vertex_bounds(verts, count, &min_x, &min_y, &max_x, &max_y);
    if (gr->cmd_count > 0) {
        DrawCmd *prev = &gr->commands[gr->cmd_count - 1];
        if (prev->type == type && prev->tex == tex && prev->r == gr->cur_r && prev->g == gr->cur_g
            && prev->b == gr->cur_b && prev->a == gr->cur_a && prev->linewidth == gr->cur_linewidth
            && type != CMD_LINE_STRIP && type != CMD_LINE_LOOP) {
            prev->count += count;
            if (min_x < prev->min_x)
                prev->min_x = min_x;
            if (min_y < prev->min_y)
                prev->min_y = min_y;
            if (max_x > prev->max_x)
                prev->max_x = max_x;
            if (max_y > prev->max_y)
                prev->max_y = max_y;
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
    cmd->linewidth = gr->cur_linewidth;
    cmd->min_x = min_x;
    cmd->min_y = min_y;
    cmd->max_x = max_x;
    cmd->max_y = max_y;
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
    int segments = CIRCLE_SEGMENTS;
    if (gr->parent && !gr->overlay_enabled)
        return;
    submit_color(gr, gc);
    if (segments > gr->vertex_capacity - gr->vertex_count)
        segments = gr->vertex_capacity - gr->vertex_count;
    Vertex *verts = g_new(Vertex, segments);
    /* navit's draw_circle() passes a DIAMETER, not a radius: every other backend halves it
     * (gtk_drawing_area's cairo_arc(r/2), win32's r = r/2, svg_debug's r/2, qt's r/2 width).
     * Using r as the radius makes every circle twice as large as the reference backends. */
    float radius = r / 2.0f;
    for (int i = 0; i < segments; i++) {
        float angle = 2.0f * G_PI * i / segments;
        verts[i].x = p->x + radius * cosf(angle);
        verts[i].y = p->y + radius * sinf(angle);
        verts[i].u = verts[i].v = 0;
    }
    submit_vertices(gr, CMD_LINE_LOOP, 0, verts, segments);
    g_free(verts);
    gr->dirty = 1;
}

static long long cross2d(int ax, int ay, int bx, int by, int cx, int cy) {
    return (long long)(bx - ax) * (cy - ay) - (long long)(by - ay) * (cx - ax);
}

/* A vertex sitting exactly on an edge of the candidate ear blocks it just like one strictly
 * inside does. With a strict test such a vertex is missed, the ear is not a real ear, clipping
 * it flips the winding of the residual ring, and every vertex left in it then looks reflex so
 * clipping stalls. On a thin concave shape - a lake or wood close to new-moon - the ring ends
 * up wound the other way and the fill degenerates into a half-disc between the two tips. */
static int point_in_triangle(int ax, int ay, int bx, int by, int cx, int cy, int px, int py) {
    long long d1 = cross2d(ax, ay, bx, by, px, py);
    long long d2 = cross2d(bx, by, cx, cy, px, py);
    long long d3 = cross2d(cx, cy, ax, ay, px, py);
    return (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
}

static void draw_polygon(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count) {
    if (gr->parent && !gr->overlay_enabled)
        return;
    if (count < 3)
        return;
    submit_color(gr, gc);

    /* Copy to local buffer, strip the closing point and collapse points at most one pixel
     * apart. Where two boundary arcs meet tangentially - the cusp of a crescent - rounding
     * scatters points one pixel apart along both arcs and the ring crosses itself, and a
     * self-crossing ring has no triangulation any ear-clipping order can find. */
    struct point tmp[count];
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (n > 0) {
            int dx = p[i].x - tmp[n - 1].x;
            int dy = p[i].y - tmp[n - 1].y;
            if (dx * dx + dy * dy <= 1)
                continue;
        }
        tmp[n++] = p[i];
    }
    if (n > 2) {
        int dx = tmp[n - 1].x - tmp[0].x;
        int dy = tmp[n - 1].y - tmp[0].y;
        if (dx * dx + dy * dy <= 1)
            n--;
    }
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
        if (ear_found)
            continue;

        /* A ring of collinear vertices can never yield an ear, because the closed
         * containment test sees the neighbours as lying on the candidate edge. Such
         * vertices enclose no area, so drop them and keep clipping instead of stalling. */
        int w = 0;
        for (int i = 0; i < rem; i++) {
            int prev = (i - 1 + rem) % rem;
            int next = (i + 1) % rem;
            if (cross2d(tmp[idx[prev]].x, tmp[idx[prev]].y, tmp[idx[i]].x, tmp[idx[i]].y, tmp[idx[next]].x,
                        tmp[idx[next]].y)
                != 0)
                idx[w++] = idx[i];
        }
        if (w < rem && w >= 3) {
            rem = w;
            continue;
        }
        break;
    }

    /* Clipping can still stall on a ring that has collapsed to a sliver of collinear vertices.
     * Fan only what is left of the ring: fanning the whole polygon from tmp[0] instead would
     * discard the correct ears and fill the convex hull, which on a concave shape such as a
     * crescent is the half-disc between the two tips that the fill is supposed to leave empty. */
    if (rem > 2) {
        dbg(lvl_warning, "ear clipping stalled with %d of %d vertices left, filling the remainder as a fan", rem, n);
        for (int i = 1; i < rem - 1; i++) {
            int vi = tri_count * 3;
            verts[vi].x = tmp[idx[0]].x;
            verts[vi].y = tmp[idx[0]].y;
            verts[vi].u = verts[vi].v = 0;
            verts[vi + 1].x = tmp[idx[i]].x;
            verts[vi + 1].y = tmp[idx[i]].y;
            verts[vi + 1].u = verts[vi + 1].v = 0;
            verts[vi + 2].x = tmp[idx[i + 1]].x;
            verts[vi + 2].y = tmp[idx[i + 1]].y;
            verts[vi + 2].u = verts[vi + 2].v = 0;
            tri_count++;
        }
    }

    submit_vertices(gr, CMD_TRIANGLES, 0, verts, tri_count * 3);
    g_free(verts);
    g_free(idx);
    gr->dirty = 1;
}

/* The monitor scale factor, taken from the root graphics so overlays (whose own copy is
 * never updated) resolve it too. Text is rasterized at this density and drawn at logical
 * size, keeping glyphs crisp on hidpi screens instead of upscaling a logical-resolution
 * texture through the device-pixel viewport. */
static int effective_scale(struct graphics_priv *gr) {
    while (gr->parent)
        gr = gr->parent;
    return gr->scale > 0 ? gr->scale : 1;
}

/* Snap a logical coordinate to a whole device pixel so each glyph quad samples its texture
 * 1:1 instead of blurring across a pixel boundary. */
static float snap_device(float v, float sc) {
    return roundf(v * sc) / sc;
}

static void display_text_draw(struct font_freetype_text *text, struct graphics_priv *gr, struct graphics_gc_priv *fg,
                              struct graphics_gc_priv *bg, struct point *p) {
    int i, x, y, stride, sc = effective_scale(gr);
    float fsc = sc;
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
            /* The halo must stay one logical pixel wide. The glyph is rasterized at the device
             * pixel density, so a one-device-pixel halo would shrink to 1/sc of a logical pixel;
             * dilate by sc instead. The quad below already spans 2*sc device pixels (its -1.0f
             * and +2.0f offsets are logical), so texture and quad stay in step. */
            stride = (g->w + 2 * sc) * 4;
            unsigned char *shadow = g_malloc(stride * (g->h + 2 * sc));
            gr->freetype_methods.get_shadow(g, shadow, stride, &white, &transparent, sc);

            /* Upload shadow as GL texture and draw */
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g->w + 2 * sc, g->h + 2 * sc, 0, GL_BGRA, GL_UNSIGNED_BYTE, shadow);

            float gx = snap_device((float)(x >> 6) + (float)g->x / (fsc * 64.0f) - 1.0f, fsc);
            float gy = snap_device((float)(y >> 6) + (float)g->y / (fsc * 64.0f) - 1.0f, fsc);
            float gw = snap_device((float)g->w / fsc + 2.0f, fsc);
            float gh = snap_device((float)g->h / fsc + 2.0f, fsc);
            gr->cur_r = gr->cur_g = gr->cur_b = 1.0f;
            gr->cur_a = 1.0f;
            Vertex verts[6] = {
                {gx,      gy,      0, 0},
                {gx + gw, gy,      1, 0},
                {gx + gw, gy + gh, 1, 1},
                {gx,      gy,      0, 0},
                {gx + gw, gy + gh, 1, 1},
                {gx,      gy + gh, 0, 1},
            };
            submit_vertices(gr, CMD_TEXTURED_QUADS, tex, verts, 6);
            g_free(shadow);
            queue_tex_delete(gr, tex);
            gr->dirty = 1;
        }
        x += g->dx / sc;
        y += g->dy / sc;
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

            float gx = snap_device((float)(x >> 6) + (float)g->x / (fsc * 64.0f), fsc);
            float gy = snap_device((float)(y >> 6) + (float)g->y / (fsc * 64.0f), fsc);
            float gw = snap_device((float)g->w / fsc, fsc);
            float gh = snap_device((float)g->h / fsc, fsc);
            gr->cur_r = gr->cur_g = gr->cur_b = 1.0f;
            gr->cur_a = 1.0f;
            Vertex verts[6] = {
                {gx,      gy,      0, 0},
                {gx + gw, gy,      1, 0},
                {gx + gw, gy + gh, 1, 1},
                {gx,      gy,      0, 0},
                {gx + gw, gy + gh, 1, 1},
                {gx,      gy + gh, 0, 1},
            };
            submit_vertices(gr, CMD_TEXTURED_QUADS, tex, verts, 6);
            g_free(glyph);
            queue_tex_delete(gr, tex);
            gr->dirty = 1;
        }
        x += g->dx / sc;
        y += g->dy / sc;
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
    /* dx/dy form a unit rotation matrix (0x10000 == 1.0); scaling both by the monitor factor
     * makes freetype rasterize each glyph at device resolution while display_text_draw divides
     * the resulting metrics back to logical size, so labels stay sharp on hidpi screens. */
    int sc = effective_scale(gr);
    struct font_freetype_text *t;
    t = gr->freetype_methods.text_new(text, (struct font_freetype_font *)font, dx * sc, dy * sc);
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
    /* For an overlay this carries its top-left position, as in the cairo/opengl/egl backends:
     * the vehicle moves its cursor this way and render_overlay places it from gr->p. For the
     * root it is a rigid translation of the already-projected frame during a pan. */
    if (gr->parent) {
        if (p)
            gr->p = *p;
        return;
    }
    if (p) {
        gr->dx = p->x;
        gr->dy = p->y;
    } else {
        gr->dx = 0;
        gr->dy = 0;
    }
    /* The offset only shifts the retained texture at present time, so queue the present here;
     * the caller may skip its draw_mode_end while a display-list replay owns the open frame. */
    gtk_widget_queue_draw(gr->glarea);
}

static int emit_cmd(struct graphics_priv *gr, enum draw_cmd_type type) {
    ensure_cmd_capacity(gr, gr->cmd_count + 1);
    if (gr->cmd_count >= gr->cmd_capacity)
        return -1;
    gr->commands[gr->cmd_count].type = type;
    gr->commands[gr->cmd_count].tex = 0;
    gr->commands[gr->cmd_count].first = 0;
    gr->commands[gr->cmd_count].count = 0;
    gr->commands[gr->cmd_count].min_x = 0;
    gr->commands[gr->cmd_count].min_y = 0;
    gr->commands[gr->cmd_count].max_x = 0;
    gr->commands[gr->cmd_count].max_y = 0;
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
    gc->linewidth = 1;
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
    if (gr->index_vbo)
        glDeleteBuffers(1, &gr->index_vbo);
    if (gr->white_tex)
        glDeleteTextures(1, &gr->white_tex);
    /* Deleting the multisample objects needs the widget's context current. gtk_gl_area_make_current
     * is a no-op on an unrealized widget, so only bother when the area is realized. */
    if ((gr->msaa_fbo || gr->map_fbo) && gr->glarea && gtk_widget_get_realized(gr->glarea)) {
        gtk_gl_area_make_current(GTK_GL_AREA(gr->glarea));
        msaa_destroy(gr);
        map_destroy(gr);
    }
    g_free(gr->vertices);
    g_free(gr->commands);
    g_free(gr->batch_vertices);
    g_free(gr->batch_commands);
    g_free(gr->index_buf);
    g_free(gr->line_runs);
    g_free(gr);
}

/* ----------------------------------------------------------------
 * Multisample anti-aliasing
 *
 * Closely spaced parallel 1px lines (adjacent train tracks, road + sidewalks)
 * alias against the pixel grid and beat into a moire pattern. GtkGLArea's own
 * framebuffer is single-sampled and its API offers no multisampling, so the
 * scene is rendered into an off-screen multisampled FBO which is resolved into
 * the GtkGLArea framebuffer at the end of ::render. The multisample resolve
 * also smooths polygon edges and glyph outlines.
 *
 * Needs ES 3.0+ (core glRenderbufferStorageMultisample / glBlitFramebuffer) or
 * GL_EXT_framebuffer_multisample + GL_EXT_framebuffer_blit on ES 2.0. Without
 * either, rendering stays direct and the buffers are never allocated.
 * ---------------------------------------------------------------- */

#define GTKGL_MSAA_SAMPLES 4

static int msaa_supported(void) {
    if (epoxy_gl_version() >= 30)
        return 1;
    return epoxy_has_gl_extension("GL_EXT_framebuffer_multisample")
           && epoxy_has_gl_extension("GL_EXT_framebuffer_blit");
}

static void msaa_setup(struct graphics_priv *gr) {
    if (!msaa_supported()) {
        dbg(lvl_debug, "gtkglarea: no multisample framebuffer support, MSAA disabled");
        return;
    }
    /* RGBA8 keeps the full 8-bit-per-channel output the Cairo backend produces; RGBA4 would
     * band the map colours. RGBA8 renderbuffer storage needs ES 3.0 or OES_rgb8_rgba8. */
    gr->msaa_color_format = (epoxy_gl_version() >= 30 || epoxy_has_gl_extension("GL_OES_rgb8_rgba8")) ? GL_RGBA8
                                                                                                      : GL_RGBA4;

    glGenFramebuffers(1, &gr->msaa_fbo);
    glGenRenderbuffers(1, &gr->msaa_color_rb);
    glGenRenderbuffers(1, &gr->msaa_stencil_rb);
    gr->msaa_samples = GTKGL_MSAA_SAMPLES;
    dbg(lvl_info, "gtkglarea: MSAA enabled with %d samples", gr->msaa_samples);
}

static void msaa_alloc(struct graphics_priv *gr, int width, int height) {
    if (gr->msaa_samples <= 0 || !gr->msaa_fbo || width <= 0 || height <= 0)
        return;
    if (gr->msaa_alloc_w == width && gr->msaa_alloc_h == height)
        return;

    /* The resize signal is emitted with the GtkGLArea framebuffer bound; keep it and restore it
     * so the caller's glViewport and subsequent draws still target the widget. */
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, gr->msaa_fbo);

    glBindRenderbuffer(GL_RENDERBUFFER, gr->msaa_color_rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, gr->msaa_samples, gr->msaa_color_format, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gr->msaa_color_rb);

    /* Stencil only — the hole-filling path uses the stencil buffer, depth is never used and
     * GtkGLArea is created without a depth buffer. */
    glBindRenderbuffer(GL_RENDERBUFFER, gr->msaa_stencil_rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, gr->msaa_samples, GL_STENCIL_INDEX8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gr->msaa_stencil_rb);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        dbg(lvl_warning, "gtkglarea: multisample FBO incomplete at %dx%d, MSAA disabled", width, height);
        gr->msaa_samples = 0;
    } else {
        gr->msaa_alloc_w = width;
        gr->msaa_alloc_h = height;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prev);
}

static void msaa_destroy(struct graphics_priv *gr) {
    if (!gr->msaa_fbo)
        return;
    glDeleteFramebuffers(1, &gr->msaa_fbo);
    glDeleteRenderbuffers(1, &gr->msaa_color_rb);
    glDeleteRenderbuffers(1, &gr->msaa_stencil_rb);
    gr->msaa_fbo = gr->msaa_color_rb = gr->msaa_stencil_rb = 0;
    gr->msaa_samples = 0;
}

/* ----------------------------------------------------------------
 * Retained map frame
 *
 * The root display list is rendered into map_tex once per rebuild and afterwards presented
 * translated by the drag offset (and pinch preview transform). The texture is padded by
 * render_margin on every side so a drag within that margin shows new content without redrawing;
 * navit recenters before the offset can exceed the margin. This is the GL counterpart of the
 * cairo backend's scroll_surface and keeps a pan frame at the cost of one textured quad instead
 * of replaying the whole retained command list.
 * ---------------------------------------------------------------- */

static void map_destroy(struct graphics_priv *gr) {
    if (gr->map_msaa_fbo)
        glDeleteFramebuffers(1, &gr->map_msaa_fbo);
    if (gr->map_msaa_color_rb)
        glDeleteRenderbuffers(1, &gr->map_msaa_color_rb);
    if (gr->map_msaa_stencil_rb)
        glDeleteRenderbuffers(1, &gr->map_msaa_stencil_rb);
    if (gr->map_stencil_rb)
        glDeleteRenderbuffers(1, &gr->map_stencil_rb);
    if (gr->map_fbo)
        glDeleteFramebuffers(1, &gr->map_fbo);
    if (gr->map_tex)
        glDeleteTextures(1, &gr->map_tex);
    gr->map_msaa_fbo = gr->map_msaa_color_rb = gr->map_msaa_stencil_rb = 0;
    gr->map_fbo = gr->map_tex = gr->map_stencil_rb = 0;
    gr->map_alloc_w = gr->map_alloc_h = 0;
    gr->map_valid = 0;
}

static void map_alloc(struct graphics_priv *gr, int width, int height) {
    if (width <= 0 || height <= 0)
        return;
    if (gr->map_fbo && gr->map_alloc_w == width && gr->map_alloc_h == height)
        return;

    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    map_destroy(gr);

    /* Single-sampled texture the map is resolved into and sampled from when presenting. */
    GLenum tex_ifmt = (epoxy_gl_version() >= 30 || epoxy_has_gl_extension("GL_OES_rgb8_rgba8")) ? GL_RGBA8 : GL_RGBA;
    glGenTextures(1, &gr->map_tex);
    glBindTexture(GL_TEXTURE_2D, gr->map_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, tex_ifmt, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &gr->map_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gr->map_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gr->map_tex, 0);
    glGenRenderbuffers(1, &gr->map_stencil_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, gr->map_stencil_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gr->map_stencil_rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        dbg(lvl_error, "gtkglarea: map framebuffer incomplete at %dx%d", width, height);
        map_destroy(gr);
        glBindFramebuffer(GL_FRAMEBUFFER, prev);
        return;
    }

    if (gr->msaa_samples > 0) {
        glGenFramebuffers(1, &gr->map_msaa_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, gr->map_msaa_fbo);
        glGenRenderbuffers(1, &gr->map_msaa_color_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, gr->map_msaa_color_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, gr->msaa_samples, gr->msaa_color_format, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gr->map_msaa_color_rb);
        glGenRenderbuffers(1, &gr->map_msaa_stencil_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, gr->map_msaa_stencil_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, gr->msaa_samples, GL_STENCIL_INDEX8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gr->map_msaa_stencil_rb);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            dbg(lvl_warning, "gtkglarea: multisample map framebuffer incomplete at %dx%d", width, height);
            glDeleteFramebuffers(1, &gr->map_msaa_fbo);
            glDeleteRenderbuffers(1, &gr->map_msaa_color_rb);
            glDeleteRenderbuffers(1, &gr->map_msaa_stencil_rb);
            gr->map_msaa_fbo = gr->map_msaa_color_rb = gr->map_msaa_stencil_rb = 0;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prev);
    gr->map_alloc_w = width;
    gr->map_alloc_h = height;
    gr->map_valid = 0;
    dbg(lvl_debug, "gtkglarea: map texture %dx%d (msaa=%d)", width, height, gr->map_msaa_fbo != 0);
}

/* Draw the retained map texture, transformed by mvp, as a single quad covering the padded
 * drawable rect. Reuses the command executor so shader state and the vertex format stay in one
 * place. */
static void present_map(struct graphics_priv *gr, float *mvp) {
    float m = (float)gr->render_margin;
    float w = (float)gr->width;
    float h = (float)gr->height;
    /* y is down in drawable space but the texture's v = 0 is its bottom row, hence v = 1 at the
     * top (y = -margin). */
    Vertex quad[6] = {
        {-m,    -m,    0.0f, 1.0f},
        {w + m, -m,    1.0f, 1.0f},
        {w + m, h + m, 1.0f, 0.0f},
        {-m,    -m,    0.0f, 1.0f},
        {w + m, h + m, 1.0f, 0.0f},
        {-m,    h + m, 0.0f, 0.0f},
    };
    DrawCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_TEXTURED_QUADS;
    cmd.tex = gr->map_tex;
    cmd.first = 0;
    cmd.count = 6;
    cmd.r = cmd.g = cmd.b = cmd.a = 1.0f;
    exec_draw_cmds(gr, &cmd, 1, quad, 6, gr->vbo, mvp);
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
    /* Blend RGB normally, but leave the destination alpha untouched (GL_ZERO, GL_ONE). The
     * GtkGLArea surface is opaque: the whole framebuffer must stay alpha = 1 so the window
     * compositor does not blend the desktop back over translucent elements (which washed out
     * semi-transparent OSD backgrounds). glBlendFunc would blend the alpha channel too, letting
     * a translucent quad's alpha leak into the framebuffer alpha. */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    msaa_setup(gr);

    return FALSE;
}

static void ensure_line_run_buffers(struct graphics_priv *gr, int run_capacity, int index_capacity) {
    if (run_capacity > gr->line_runs_capacity) {
        gr->line_runs = g_realloc(gr->line_runs, run_capacity * sizeof(*gr->line_runs));
        gr->line_runs_capacity = run_capacity;
    }
    if (index_capacity > gr->index_capacity) {
        gr->index_buf = g_realloc(gr->index_buf, index_capacity * sizeof(*gr->index_buf));
        gr->index_capacity = index_capacity;
    }
}

/* Collapse runs of consecutive line-strip commands with identical state into index ranges. The
 * strips of a run are separated by GL_RESTART_INDEX so a single indexed draw can emit all of them
 * without connecting the end of one strip to the start of the next. Line strips are the bulk of a
 * dense map frame; batching them cuts the per-frame draw-call count by an order of magnitude.
 * Non-indexed drawing stays the fallback where primitive restart is unavailable. */
static int build_line_runs(struct graphics_priv *gr, DrawCmd *commands, int cmd_count) {
    memset(gr->line_runs, 0, cmd_count * sizeof(*gr->line_runs));
    int index = 0;
    for (int i = 0; i < cmd_count;) {
        DrawCmd *first = &commands[i];
        if (first->type != CMD_LINE_STRIP) {
            i++;
            continue;
        }
        int index_start = index;
        int j = i;
        while (j < cmd_count) {
            DrawCmd *run = &commands[j];
            if (run->type != CMD_LINE_STRIP || run->tex != first->tex || run->r != first->r || run->g != first->g
                || run->b != first->b || run->a != first->a || run->linewidth != first->linewidth)
                break;
            for (int k = 0; k < run->count; k++)
                gr->index_buf[index++] = (GLuint)(run->first + k);
            gr->index_buf[index++] = GL_RESTART_INDEX;
            j++;
        }
        gr->line_runs[i].index_start = index_start;
        gr->line_runs[i].index_count = index - index_start;
        gr->line_runs[i].span = j - i;
        i = j;
    }
    return index;
}

static int prepare_draw(struct graphics_priv *gr, DrawCmd *commands, int cmd_count, Vertex *vertices, int vertex_count,
                        GLuint vbo, float *mvp, int *merge_strips_out) {
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

    int merge_strips = epoxy_gl_version() >= 30 && cmd_count > 0 && !getenv("NAVIT_NO_MERGE");
    *merge_strips_out = merge_strips;
    int index_count = 0;
    if (merge_strips) {
        ensure_line_run_buffers(gr, cmd_count, vertex_count + cmd_count);
        index_count = build_line_runs(gr, commands, cmd_count);
        if (index_count > 0) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gr->index_vbo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_count * sizeof(GLuint), gr->index_buf, GL_DYNAMIC_DRAW);
            if (epoxy_is_desktop_gl()) {
                glEnable(GL_PRIMITIVE_RESTART);
                glPrimitiveRestartIndex(GL_RESTART_INDEX);
            } else {
                /* ES 3.0 has no glPrimitiveRestartIndex; the restart value is fixed to the
                 * maximum index for the element type. */
                glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
            }
        }
    }
    return index_count;
}

static void draw_commands(struct graphics_priv *gr, DrawCmd *commands, int cmd_count, int merge_strips) {

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
        if (cmd->type == CMD_LINES || cmd->type == CMD_LINE_STRIP || cmd->type == CMD_LINE_LOOP) {
            /* Honour graphics_gc_set_linewidth(). Without this every line keeps the GL default
             * of 1px, which is what made e.g. the speed warner's d/10 ring invisible. */
            /* linewidth is a navit logical width; glLineWidth takes device pixels. */
            int sc = gr->scale > 0 ? gr->scale : 1;
            float w = (cmd->linewidth > 0.0f ? cmd->linewidth : 1.0f) * (float)sc;
            /* Some drivers (notably llvmpipe/Mesa and ANGLE) silently clamp widths above 1 and
             * just widen the line themselves, so only lower-bound it here and let the driver
             * decide the rest. */
            glLineWidth(w);
        }
        if (merge_strips && cmd->type == CMD_LINE_STRIP && gr->line_runs[i].span > 1) {
            glDrawElements(mode, gr->line_runs[i].index_count, GL_UNSIGNED_INT,
                           (void *)(size_t)(gr->line_runs[i].index_start * sizeof(GLuint)));
            i += gr->line_runs[i].span - 1;
        } else {
            glDrawArrays(mode, cmd->first, cmd->count);
        }
    }
}

static void finish_draw(struct graphics_priv *gr, int index_count) {
    if (index_count > 0) {
        if (epoxy_is_desktop_gl())
            glDisable(GL_PRIMITIVE_RESTART);
        else
            glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

static void exec_draw_cmds(struct graphics_priv *gr, DrawCmd *commands, int cmd_count, Vertex *vertices,
                           int vertex_count, GLuint vbo, float *mvp) {
    int merge_strips = 0;
    int index_count = prepare_draw(gr, commands, cmd_count, vertices, vertex_count, vbo, mvp, &merge_strips);
    draw_commands(gr, commands, cmd_count, merge_strips);
    finish_draw(gr, index_count);
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
    int batch_verts = 0;

    /* While a frame is under construction the sliced replay holds draw_depth open across
     * main-loop iterations. Present the retained texture at the current offset so a pan keeps
     * tracking the cursor, but never bake the partial command list into the map texture.
     * Return FALSE (not TRUE): ::render uses the default true_handled accumulator, so a
     * TRUE return would halt emission and silently suppress any other ::render handler
     * (and GTK's own render closure, which performs the frame error check). */
    int frame_open = gr->draw_depth > 0;

    /* GtkGLArea renders into its own framebuffer (not the default 0); remember it so the
     * multisampled scene can be resolved into it at the end. */
    GLint screen_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &screen_fb);

    int msaa = gr->msaa_samples > 0 && gr->msaa_fbo && gr->phys_w > 0 && gr->phys_h > 0
               && gr->msaa_alloc_w == gr->phys_w && gr->msaa_alloc_h == gr->phys_h;

    /* Render the retained map frame into its off-screen texture, but only when its content
     * changed. A drag or pinch preview only changes the MVP, so those frames reuse the texture
     * and cost a single textured quad instead of replaying the whole display list. */
    int margin_px = gr->render_margin * (gr->scale > 0 ? gr->scale : 1);
    int map_w = gr->phys_w + 2 * margin_px;
    int map_h = gr->phys_h + 2 * margin_px;
    if (gr->phys_w > 0 && gr->phys_h > 0 && !frame_open)
        map_alloc(gr, map_w, map_h);

    if (gr->map_fbo && !frame_open && (gr->dirty || !gr->map_valid)) {
        if (gr->bg_gc)
            glClearColor(gr->bg_gc->c.r / 65535.0f, gr->bg_gc->c.g / 65535.0f, gr->bg_gc->c.b / 65535.0f, 1.0f);
        else
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, gr->map_msaa_fbo ? gr->map_msaa_fbo : gr->map_fbo);
        glViewport(0, 0, map_w, map_h);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        if (gr->vertex_count > 0 || gr->cmd_count > 0) {
            /* Ortho over the padded drawable rect: x in [-margin, width+margin],
             * y in [height+margin, -margin] (screen y down). Cull to that rect: the display list
             * covers the larger pan prefetch area, whose outer part is never visible here. */
            float base[16];
            mvp_ortho(base, -(float)gr->render_margin, (float)(gr->width + gr->render_margin),
                      (float)(gr->height + gr->render_margin), -(float)gr->render_margin, -1.0f, 1.0f);
            int batch_cmds = build_culled_batch(gr, -(float)gr->render_margin - 2.0f, -(float)gr->render_margin - 2.0f,
                                                (float)(gr->width + gr->render_margin) + 2.0f,
                                                (float)(gr->height + gr->render_margin) + 2.0f, &batch_verts);
            if (batch_cmds > 0)
                exec_draw_cmds(gr, gr->batch_commands, batch_cmds, gr->batch_vertices, batch_verts, gr->vbo, base);
        }
        if (gr->map_msaa_fbo) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, gr->map_msaa_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gr->map_fbo);
            glBlitFramebuffer(0, 0, map_w, map_h, 0, 0, map_w, map_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        gr->map_valid = 1;
        gr->dirty = 0;
    }

    /* Present the retained map, then the overlays on top of it. */
    if (msaa)
        glBindFramebuffer(GL_FRAMEBUFFER, gr->msaa_fbo);
    else
        glBindFramebuffer(GL_FRAMEBUFFER, screen_fb);
    glViewport(0, 0, gr->phys_w, gr->phys_h);

    if (gr->bg_gc)
        glClearColor(gr->bg_gc->c.r / 65535.0f, gr->bg_gc->c.g / 65535.0f, gr->bg_gc->c.b / 65535.0f, 1.0f);
    else
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    if (gr->map_valid && gr->map_tex) {
        float present[16];
        compute_mvp(gr, present);
        present_map(gr, present);
    }

    /* Draw overlays (recursively for nested overlays) */
    render_overlay(gr, gr->overlays);

    /* Resolve the multisampled scene into the GtkGLArea framebuffer. Same-size NEAREST blit, no
     * flip: both framebuffers share GL's bottom-left origin. */
    if (msaa) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gr->msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, screen_fb);
        glBlitFramebuffer(0, 0, gr->phys_w, gr->phys_h, 0, 0, gr->phys_w, gr->phys_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, screen_fb);
    }

    return FALSE;
}

static void on_glarea_resize(GtkGLArea *glarea, int width, int height, gpointer data) {
    struct graphics_priv *gr = data;
    /* GtkGLArea reports its framebuffer size in device pixels (allocation * scale factor).
     * Hand the logical size to navit so layout math matches the gtk_drawing_area/cairo
     * backend, which paints in logical units and lets GTK scale them by the monitor factor.
     * The device-pixel size is retained for the GL output (viewport, MSAA, blit). */
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(glarea));
    if (scale < 1)
        scale = 1;
    gr->scale = scale;
    gr->phys_w = width;
    gr->phys_h = height;
    gr->width = width / scale;
    gr->height = height / scale;

    glViewport(0, 0, width, height);

    msaa_alloc(gr, width, height);

    /* Margin kept around the viewport in the retained map texture; must match what navit uses
     * for its pan recenter threshold so a drag is always still inside the texture. */
    gr->render_margin = (gr->width > gr->height ? gr->width : gr->height) / GTKGL_MAP_MARGIN_DIVISOR;

    navit_set_render_margin(gr->nav, gr->render_margin);

    callback_list_call_attr_2(gr->cbl, attr_resize, GINT_TO_POINTER(gr->width), GINT_TO_POINTER(gr->height));
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
        /* The second finger of a pinch must not restart the drag the first one began. */
        if (this->zoom_gesture && gtk_gesture_is_recognized(this->zoom_gesture))
            return;
        this->touch_drag = 1;
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
        /* A touch with no drag left to end belongs to a pinch: the pinch already ended the drag
         * and rebuilt the view, so the finger positions must not pan it a second time. */
        if (!this->touch_drag)
            return;
        this->touch_drag = 0;
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
    this->last_motion = p;
    /* While a pinch is recognized the two-finger motion drives the zoom; forwarding it as a
     * drag would also pan the map under the fingers. */
    if (this->zoom_gesture && gtk_gesture_is_recognized(this->zoom_gesture)) {
        /* The midpoint still moves during a pinch even when the finger spacing (and so
         * scale-changed) does not, e.g. panning after zooming out; track it here so the
         * release recenters using the final midpoint, not a stale one. */
        double cx, cy;
        if (gtk_gesture_get_bounding_box_center(this->zoom_gesture, &cx, &cy)) {
            cx = (double)(int)cx;
            cy = (double)(int)cy;
            if (this->preview_cx != (float)cx || this->preview_cy != (float)cy) {
                this->preview_cx = (float)cx;
                this->preview_cy = (float)cy;
                gtk_widget_queue_draw(this->glarea);
            }
        }
        return;
    }
    callback_list_call_attr_1(this->cbl, attr_motion, (void *)&p);
}

static void on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct point p;
    (void)controller;

    p.x = (int)x;
    p.y = (int)y;
    this->last_motion = p;
    if (this->zoom_gesture && gtk_gesture_is_recognized(this->zoom_gesture))
        return;
    callback_list_call_attr_1(this->cbl, attr_motion, (void *)&p);
}

static void on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct point p;
    int button;
    (void)controller;
    (void)dx;

    /* A GdkScrollEvent does not carry a position: gdk_event_get_position() returns
     * FALSE and writes NaN for it, which would become INT_MIN once cast to int and
     * send the map far off when used as the zoom invariant point. Use the last known
     * pointer position instead. */
    p = this->last_motion;
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

static void on_gesture_zoom_begin(GtkGesture *gesture, GdkEventSequence *sequence, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct graphics *gra;
    double cx, cy;
    (void)sequence;
    this->pinch_base_scale = navit_get_scale(this->nav);
    this->preview_scale = 1.0f;
    this->pinch_prefetched = 0;
    /* Truncate to whole pixels: the release hands these to navit_pinch_to as struct point, so the
     * preview has to use the same integer anchors to stay pixel-identical. */
    if (gtk_gesture_get_bounding_box_center(GTK_GESTURE(gesture), &cx, &cy)) {
        this->pinch_base_cx = this->preview_cx = (float)(int)cx;
        this->pinch_base_cy = this->preview_cy = (float)(int)cy;
    } else {
        this->pinch_base_cx = this->preview_cx = (float)this->last_motion.x;
        this->pinch_base_cy = this->preview_cy = (float)this->last_motion.y;
    }
    /* The first finger started a drag before the pinch was recognised, and its motion is no
     * longer forwarded, so hand that drag to the transform before the touch-up ends it here. */
    navit_pinch_begin(this->nav);
    this->touch_drag = 0;
    /* The map scales under the fingers, so hide the cursor and OSD for the duration of the pinch. */
    gra = navit_get_graphics(this->nav);
    if (gra)
        graphics_overlay_disable(gra, 1);
    gtk_widget_queue_draw(this->glarea);
}

static void on_gesture_zoom_scale_changed(GtkGestureZoom *gesture, double scale, gpointer user_data) {
    struct graphics_priv *this = user_data;
    double cx, cy;
    long target;

    if (scale <= 0.0)
        return;
    if (gtk_gesture_get_bounding_box_center(GTK_GESTURE(gesture), &cx, &cy)) {
        this->preview_cx = (float)(int)cx;
        this->preview_cy = (float)(int)cy;
    } else {
        this->preview_cx = (float)this->last_motion.x;
        this->preview_cy = (float)this->last_motion.y;
    }
    /* Only re-render the retained frame; the display list is rebuilt once the gesture ends. The
     * preview is snapped to the integer zoom level the release applies, so lifting the fingers
     * re-renders the very same view instead of moving it by the quantisation remainder. */
    target = navit_pinch_target(this->nav, this->pinch_base_scale, scale);
    this->preview_scale = (float)((double)this->pinch_base_scale / (double)target);
    /* Zooming out reveals area beyond the viewport, so fetch the pan prefetch margin once; the
     * scaled-back frame then has map to show instead of the background at its edges. */
    if (this->preview_scale < 1.0f && !this->pinch_prefetched) {
        this->pinch_prefetched = 1;
        navit_draw_prefetch(this->nav);
    }
    gtk_widget_queue_draw(this->glarea);
}

static void on_gesture_zoom_end(GtkGesture *gesture, GdkEventSequence *sequence, gpointer user_data) {
    struct graphics_priv *this = user_data;
    struct graphics *gra = navit_get_graphics(this->nav);
    struct point base, now;
    long target;

    (void)gesture;
    (void)sequence;
    if (gra)
        graphics_overlay_disable(gra, 0);
    base.x = (int)this->pinch_base_cx;
    base.y = (int)this->pinch_base_cy;
    now.x = (int)this->preview_cx;
    now.y = (int)this->preview_cy;
    if (this->preview_scale == 1.0f && base.x == now.x && base.y == now.y) {
        gtk_widget_queue_draw(this->glarea);
        return;
    }
    target = navit_pinch_target(this->nav, this->pinch_base_scale, this->preview_scale);
    this->preview_scale = 1.0f;
    this->preview_cx = this->pinch_base_cx;
    this->preview_cy = this->pinch_base_cy;
    navit_pinch_to(this->nav, target, &base, &now);
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
    /* No set_clip/clear_clip/scroll: navit's scroll+clip recenter path replays the whole display
     * list on the calling thread (seconds at dense zoom levels), while the async rebuild replays
     * it on a worker. Returning NULL makes graphics_scroll() fail and navit use that path. */
    NULL, /* set_clip */
    NULL, /* set_clip_rects */
    NULL, /* clear_clip */
    NULL, /* scroll */
};

static struct graphics_priv *graphics_gtkglarea_new_helper(struct graphics_methods *meth) {
    struct font_priv *(*font_freetype_new)(void *meth);
    font_freetype_new = plugin_get_category_font("freetype");
    if (!font_freetype_new)
        return NULL;

    struct graphics_priv *this = g_new0(struct graphics_priv, 1);
    /* Overlays start enabled, matching the opengl/egl backends. g_new0 would leave this at 0,
     * which the render_overlay/draw guards read as disabled, so a cursor (or any overlay created
     * without a later overlay_disable(...,0)) would never be drawn. */
    this->overlay_enabled = 1;
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

    GtkGesture *zoom = gtk_gesture_zoom_new();
    g_signal_connect(zoom, "begin", G_CALLBACK(on_gesture_zoom_begin), this);
    g_signal_connect(zoom, "scale-changed", G_CALLBACK(on_gesture_zoom_scale_changed), this);
    g_signal_connect(zoom, "end", G_CALLBACK(on_gesture_zoom_end), this);
    gtk_widget_add_controller(this->glarea, GTK_EVENT_CONTROLLER(zoom));
    this->zoom_gesture = zoom;

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), this);
    gtk_widget_add_controller(this->glarea, key);

    this->timeout = 100;
    this->scale = 1;
    this->preview_scale = 1.0f;
    this->last_motion.x = this->width / 2;
    this->last_motion.y = this->height / 2;
    for (int i = 0; i < 8; i++) {
        this->button_press[i].tv_sec = 0;
        this->button_press[i].tv_usec = 0;
        this->button_release[i].tv_sec = 0;
        this->button_release[i].tv_usec = 0;
    }

    return this;
}

void plugin_init(void) {
    if (!g_get_prgname())
        g_set_prgname("navit");
    gtk_init();
    plugin_register_category_graphics("gtkglarea", graphics_gtkglarea_new);
}
