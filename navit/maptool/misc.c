/*
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

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#include "attr.h"
#include "attr_type_def.h"
#include "config.h"
#include "coord.h"
#include "debug.h"
#include "geom.h"
#include "item.h"
#include "item_type_def.h"
#include "map.h"
#include "maptool.h"
#include "types.h"
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef HAVE_LZMA
#    include <lzma.h>
#endif
#ifndef _MSC_VER
#    include <getopt.h>
#    include <unistd.h>
#endif

struct zip_info;

#define phase1_coord_max 16384

struct rect world_bbox = {
    {WORLD_BOUNDINGBOX_MIN_X, WORLD_BOUNDINGBOX_MIN_Y},
    {WORLD_BOUNDINGBOX_MAX_X, WORLD_BOUNDINGBOX_MAX_Y},
};

void bbox_extend(struct coord *c, struct rect *r) {
    if (c->x < r->l.x)
        r->l.x = c->x;
    if (c->y < r->l.y)
        r->l.y = c->y;
    if (c->x > r->h.x)
        r->h.x = c->x;
    if (c->y > r->h.y)
        r->h.y = c->y;
}

void bbox(struct coord *c, int count, struct rect *r) {
    if (!count)
        return;
    r->l = *c;
    r->h = *c;
    while (--count) {
        c++;
        bbox_extend(c, r);
    }
}

int contains_bbox(int xl, int yl, int xh, int yh, struct rect *r) {
    if (r->h.x < xl || r->h.x > xh) {
        return 0;
    }
    if (r->l.x > xh || r->l.x < xl) {
        return 0;
    }
    if (r->h.y < yl || r->h.y > yh) {
        return 0;
    }
    if (r->l.y > yh || r->l.y < yl) {
        return 0;
    }
    return 1;
}

int bbox_contains_coord(struct rect *r, struct coord *c) {
    if (r->h.x < c->x)
        return 0;
    if (r->l.x > c->x)
        return 0;
    if (r->h.y < c->y)
        return 0;
    if (r->l.y > c->y)
        return 0;
    return 1;
}

int bbox_contains_bbox(struct rect *out, struct rect *in) {
    if (out->h.x < in->h.x)
        return 0;
    if (out->l.x > in->l.x)
        return 0;
    if (out->h.y < in->h.y)
        return 0;
    if (out->l.y > in->l.y)
        return 0;
    return 1;
}

long long bbox_area(struct rect const *r) {
    return ((long long)r->h.x - r->l.x) * (r->h.y - r->l.y);
}

void phase1_map(GList *maps, FILE *out_ways, FILE *out_nodes) {
    struct map_rect *mr;
    struct item *item;
    int count;
    struct coord ca[phase1_coord_max];
    struct attr attr;
    struct item_bin *item_bin;

    while (maps) {
        mr = map_rect_new(maps->data, NULL);
        while ((item = map_rect_get_item(mr))) {
            count = item_coord_get(item, ca, item->type < type_line ? 1 : phase1_coord_max);
            item_bin = init_item(item->type);
            item_bin_add_coord(item_bin, ca, count);
            while (item_attr_get(item, attr_any, &attr)) {
                if (attr.type >= attr_type_string_begin && attr.type <= attr_type_string_end) {
                    attr.u.str = map_convert_string(maps->data, attr.u.str);
                    if (attr.u.str) {
                        item_bin_add_attr(item_bin, &attr);
                        map_convert_free(attr.u.str);
                    }
                } else
                    item_bin_add_attr(item_bin, &attr);
            }
            if (item->type >= type_line)
                item_bin_write(item_bin, out_ways);
            else
                item_bin_write(item_bin, out_nodes);
        }
        map_rect_destroy(mr);
        maps = g_list_next(maps);
    }
}

int item_order_by_type(enum item_type type) {
    int max = 14;
    switch (type) {
    case type_town_label_1e7:
    case type_town_label_5e6:
        max = 3;
        break;
    case type_town_label_2e6:
    case type_town_label_1e6:
        max = 5;
        break;
    case type_town_label_5e5:
    case type_district_label_1e7:
    case type_district_label_5e6:
    case type_district_label_2e6:
    case type_district_label_1e6:
    case type_district_label_5e5:
        max = 6;
        break;
    case type_town_label_2e5:
    case type_town_label_1e5:
    case type_district_label_2e5:
    case type_district_label_1e5:
    case type_street_n_lanes:
    case type_highway_city:
    case type_highway_land:
    case type_ramp:
        max = 8;
        break;
    case type_town_label_5e4:
    case type_town_label_2e4:
    case type_town_label_1e4:
    case type_district_label_5e4:
    case type_district_label_2e4:
    case type_district_label_1e4:
        max = 9;
        break;
    case type_poly_water_tiled:
        if (experimental)
            max = 9;
        break;
    case type_street_4_land:
    case type_street_4_city:
        max = 10;
        break;
    case type_town_label_5e3:
    case type_town_label_2e3:
    case type_town_label_1e3:
    case type_district_label_5e3:
    case type_district_label_2e3:
    case type_district_label_1e3:
    case type_street_3_city:
    case type_street_3_land:
        max = 12;
        break;
    default:
        break;
    }
    return max;
}

static inline int filter_unknown(struct item_bin *ib) {
    if (ignore_unknown && (ib->type == type_point_unkn || ib->type == type_street_unkn || ib->type == type_none))
        return 1;
    return 0;
}

void dump(FILE *in) {
    struct item_bin *ib;
    while ((ib = read_item(in))) {
        if (filter_unknown(ib))
            continue;
        dump_itembin(ib);
    }
}

static struct tile_head tile_killer;

/* Persistent thread pool reused across slices */
static GAsyncQueue *tile_queue = NULL;
static GAsyncQueue *tile_done_queue = NULL;
static struct tile_process_thread *worker_threads = NULL;
static int worker_count = 0;

struct tile_process_thread {
    int number;
    GAsyncQueue *queue;
    GThread *thread;
    char *scratch_buf;
    size_t scratch_size;
    int compression_method;
#ifdef HAVE_LZMA
    struct lzma_arena arena;
    lzma_allocator allocator;
#endif
};

static gpointer process_tile_worker(gpointer data) {
    struct tile_process_thread *me = (struct tile_process_thread *)data;
    struct tile_head *th;

    while ((th = (struct tile_head *)g_async_queue_pop(me->queue)) != &tile_killer) {
        int data_size = th->total_size;
        th->crc = crc32(0, NULL, 0);
        th->crc = crc32(th->crc, (unsigned char *)th->zip_data, data_size);

#ifdef HAVE_LZMA
        me->arena.pos = 0;
        th->comp_data = compress_for_zip(th->zip_data, data_size, th->compression_level,
                                          me->compression_method,
                                          &th->comp_size, &th->zipmthd,
                                          &me->scratch_buf, &me->scratch_size, &me->allocator);
#else
        th->comp_data = compress_for_zip(th->zip_data, data_size, th->compression_level,
                                          me->compression_method,
                                          &th->comp_size, &th->zipmthd,
                                          &me->scratch_buf, &me->scratch_size, NULL);
#endif
        if (!th->comp_data) {
            th->comp_data = th->zip_data;
            th->zipmthd = 0;
        }

        g_async_queue_push(tile_done_queue, th);
    }

    g_thread_exit(NULL);
    return NULL;
}

void tile_worker_pool_init(int n_threads, int compression_level, int compression_method) {
    int i;
    if (n_threads <= 0)
        return;
    worker_count = n_threads;
    tile_queue = g_async_queue_new();
    tile_done_queue = g_async_queue_new();
    worker_threads = g_malloc0(sizeof(struct tile_process_thread) * n_threads);
    for (i = 0; i < n_threads; i++) {
        worker_threads[i].number = i;
        worker_threads[i].queue = tile_queue;
        worker_threads[i].compression_method = compression_method;
#ifdef HAVE_LZMA
        if (compression_level > 0) {
            uint64_t mem = lzma_easy_encoder_memusage(compression_level);
            if (mem == UINT64_MAX) {
                fprintf(stderr, "Invalid LZMA compression level %d\n", compression_level);
                exit(1);
            }
            worker_threads[i].arena.size = mem + 65536;
            worker_threads[i].arena.buf = g_malloc0(worker_threads[i].arena.size);
            worker_threads[i].arena.pos = 0;
            worker_threads[i].allocator.alloc = arena_alloc;
            worker_threads[i].allocator.free = arena_free;
            worker_threads[i].allocator.opaque = &worker_threads[i].arena;
        }
#endif
        worker_threads[i].thread = g_thread_new("tile_worker", process_tile_worker, &worker_threads[i]);
        if (!worker_threads[i].thread) {
            fprintf(stderr, "Failed to create tile worker thread %d\n", i);
            exit(1);
        }
    }
}

void tile_worker_pool_fini(void) {
    int i;
    if (!worker_threads)
        return;
    for (i = 0; i < worker_count; i++)
        g_async_queue_push(tile_queue, &tile_killer);
    for (i = 0; i < worker_count; i++) {
        g_thread_join(worker_threads[i].thread);
        g_free(worker_threads[i].scratch_buf);
#ifdef HAVE_LZMA
        g_free(worker_threads[i].arena.buf);
#endif
    }
    g_async_queue_unref(tile_queue);
    g_async_queue_unref(tile_done_queue);
    g_free(worker_threads);
    tile_queue = NULL;
    tile_done_queue = NULL;
    worker_threads = NULL;
    worker_count = 0;
}

void *tile_get_compress_queue(void) {
    return tile_queue;
}

/* Push all named tiles with data to the compression queue */
void tile_push_all_tiles(struct tile_info *info) {
    struct tile_head *th;
    for (th = tile_head_root; th; th = th->next) {
        if (th->process && th->name[0] && th->total_size > 0) {
            if (info->tile_compress_queue) {
                th->compression_level = info->compression_level;
                th->compression_method = info->compression_method;
                g_async_queue_push((GAsyncQueue *)info->tile_compress_queue, th);
            }
        }
    }
}

/* Write empty-name (index) tiles directly to the zip index */
void tile_write_index_tiles(struct zip_info *zi) {
    struct tile_head *th;
    for (th = tile_head_root; th; th = th->next) {
        if (th->process && !th->name[0]) {
            dbg_assert(fwrite(th->zip_data, th->total_size, 1, zip_get_index(zi)) == 1);
        }
    }
}

/* Consume the tile done queue and write compressed tiles to the zip.
 * Call only after tile_push_all_tiles() or equivalent. */
void tile_consume_done_queue(struct zip_info *zi, int tile_count) {
    int maxnamelen = zip_get_maxnamelen(zi);
    while (tile_count > 0) {
        struct tile_head *th = g_async_queue_pop(tile_done_queue);
        if (th->total_size != th->total_size_used) {
            fprintf(stderr, "Size error '%s': %d vs %d\n", th->name, th->total_size, th->total_size_used);
            exit(1);
        }
        th->zipnum = zip_get_zipnum(zi);
        write_zipmember_raw(zi, th->name, maxnamelen, th->comp_data, th->comp_size, th->total_size, th->crc,
                            th->zipmthd);
        zip_add_member(zi);
        if (th->comp_data != th->zip_data)
            g_free(th->comp_data);
        th->comp_data = NULL;
        tile_count--;
    }
}

void process_binfile(FILE *in, FILE *out) {
    struct item_bin *ib;
    while ((ib = read_item(in))) {
        item_bin_write(ib, out);
    }
}

void add_aux_tiles(char *name, struct zip_info *info) {
    char buffer[4096];
    char *s;
    FILE *in;
    FILE *tmp;
    in = fopen(name, "rb");
    if (!in)
        return;
    while (fscanf(in, "%s", buffer) == 1) {
        s = strchr(buffer, '/');
        if (s)
            s++;
        else
            s = buffer;
        tmp = fopen(buffer, "rb");
        if (tmp) {
            fseek(tmp, 0, SEEK_END);
            add_aux_tile(info, s, buffer, ftell(tmp));
            fclose(tmp);
        }
    }
    fclose(in);
}
