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
#include "zipfile.h"
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifndef _MSC_VER
#    include <getopt.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
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
            else {
                item_bin_write(item_bin, out_nodes);
                tile_sizing_write_file(out_nodes, item_bin);
            }
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

static void phase34_process_item(struct tile_info *info, struct item_bin *ib, FILE *reference) {
    int max;
    struct attr_bin *a;
    if (filter_unknown(ib))
        return;
    if (ib->type < 0x80000000)
        processed_nodes++;
    else
        processed_ways++;
    max = item_order_by_type(ib->type);
    a = item_bin_get_attr_bin(ib, attr_order, NULL);
    if (a) {
        int max2 = ((struct range *)(a + 1))->max;
        if (max < max2)
            max = max2;
    }
    tile_write_item_minmax(info, ib, reference, 0, max);
}

static void phase34_process_file(struct tile_info *info, FILE *in, FILE *reference) {
    struct item_bin *ib;

    while ((ib = read_item(in))) {
        phase34_item_pos++;
        phase34_process_item(info, ib, reference);
    }
}

static void phase34_process_file_range(struct tile_info *info, FILE *in, FILE *reference) {
    struct item_bin *ib;
    int min, max;

    while ((ib = read_item_range(in, &min, &max))) {
        phase34_item_pos++;
        if (filter_unknown(ib))
            continue;
        if (ib->type < 0x80000000)
            processed_nodes++;
        else
            processed_ways++;
        tile_write_item_minmax(info, ib, reference, min, max);
    }
}

static void phase34_collect(struct tile_info *info, FILE **in, FILE **reference, int in_count, int with_range) {
    int i;

    processed_nodes = processed_nodes_out = processed_ways = processed_relations = processed_tiles = 0;
    bytes_read = 0;
    sig_alrm(0);
    phase34_item_pos = -1;
    if (!info->write)
        tile_hash = g_hash_table_new(g_str_hash, g_str_equal);
    for (i = 0; i < in_count; i++) {
        if (in[i]) {
            if (with_range)
                phase34_process_file_range(info, in[i], reference ? reference[i] : NULL);
            else
                phase34_process_file(info, in[i], reference ? reference[i] : NULL);
        }
    }
}

static void phase34_finish(struct tile_info *info, struct zip_info *zip_info) {
    if (!info->write)
        merge_tiles(info);
    sig_alrm(0);
    sig_alrm_end();
    write_tilesdir(info, zip_info, info->tilesdir_out);
}

static int phase34(struct tile_info *info, struct zip_info *zip_info, FILE **in, FILE **reference, int in_count,
                   int with_range) {
    phase34_collect(info, in, reference, in_count, with_range);
    phase34_finish(info, zip_info);
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

#define TILE_SIZING_MAX_FILES 20

int tile_sizing_active = 0;
int tile_sizing_file = -1;
int tile_sizing_slot = -1;
long long tile_sizing_pos = 0;
FILE *tile_sizing_out = NULL;

static char *tile_sizing_names[TILE_SIZING_MAX_FILES];
static long long tile_sizing_counts[TILE_SIZING_MAX_FILES];
static int tile_sizing_sized[TILE_SIZING_MAX_FILES];
static int tile_sizing_count = 0;
static struct tile_info tile_sizing_info;

static int tile_sizing_slot_of(int file_index) {
    const char *name = tile_sizing_names[file_index];
    if (!g_strcmp0(name, "ways_split"))
        return 0;
    if (!g_strcmp0(name, "nodes"))
        return 1;
    if (!g_strcmp0(name, "way2poi_result"))
        return 2;
    return -1;
}

void tile_sizing_init(char **filenames, int count) {
    int i;
    tile_sizing_count = 0;
    for (i = 0; i < count && i < TILE_SIZING_MAX_FILES; i++) {
        tile_sizing_names[tile_sizing_count] = filenames[i];
        tile_sizing_counts[tile_sizing_count] = 0;
        tile_sizing_sized[tile_sizing_count] = 0;
        tile_sizing_count++;
    }
    memset(&tile_sizing_info, 0, sizeof(tile_sizing_info));
    tile_sizing_info.write = 0;
    tile_sizing_info.suffix = "";
    tile_hash = g_hash_table_new(g_str_hash, g_str_equal);
    tile_sizing_active = 1;
    tile_sizing_file = -1;
    tile_sizing_slot = -1;
    tile_sizing_out = NULL;
}

void tile_sizing_set_file(char *name, FILE *out) {
    int i;
    if (!tile_sizing_active) {
        tile_sizing_file = -1;
        tile_sizing_slot = -1;
        tile_sizing_out = NULL;
        return;
    }
    for (i = 0; i < tile_sizing_count; i++) {
        if (!g_strcmp0(tile_sizing_names[i], name)) {
            tile_sizing_file = i;
            tile_sizing_slot = tile_sizing_slot_of(i);
            tile_sizing_out = out;
            tile_sizing_pos = 0;
            tile_sizing_sized[i] = 1;
            return;
        }
    }
    tile_sizing_file = -1;
    tile_sizing_slot = -1;
    tile_sizing_out = NULL;
}

void tile_sizing_clear(void) {
    tile_sizing_file = -1;
    tile_sizing_slot = -1;
    tile_sizing_out = NULL;
}

void tile_sizing_write_file(FILE *out, struct item_bin *ib) {
    if (!tile_sizing_active || !out || out != tile_sizing_out)
        return;
    tile_sizing_pos++;
    tile_sizing_counts[tile_sizing_file]++;
    phase34_process_item(&tile_sizing_info, ib, NULL);
}

struct tile_sizing_reset_args {
    int file;
    int slot;
};

static void tile_sizing_reset_func(char *key, struct tile_head *th, struct tile_sizing_reset_args *args) {
    if (th->total_size_file[args->slot] > 0) {
        th->total_size -= th->total_size_file[args->slot];
        if (th->total_size < 0)
            th->total_size = 0;
        th->total_size_file[args->slot] = 0;
    }
    /* The file is about to be rewritten: forget its old completion position, so
     * the rewrite re-records it and it cannot exceed the file's final size. */
    if (th->completion_file == args->file) {
        th->completion_file = -1;
        th->completion_pos = 0;
    }
}

void tile_sizing_reset_file(char *name) {
    int i, slot;
    struct tile_sizing_reset_args args;
    if (!tile_sizing_active)
        return;
    for (i = 0; i < tile_sizing_count; i++) {
        if (!g_strcmp0(tile_sizing_names[i], name))
            break;
    }
    if (i >= tile_sizing_count)
        return;
    slot = tile_sizing_slot_of(i);
    if (slot < 0)
        return;
    args.file = i;
    args.slot = slot;
    g_hash_table_foreach(tile_hash, (GHFunc)tile_sizing_reset_func, &args);
    tile_sizing_counts[i] = 0;
}

int tile_sizing_complete(FILE **in, int in_count) {
    int i;
    if (!tile_sizing_active)
        return 0;
    for (i = 0; i < tile_sizing_count && i < in_count; i++)
        if (in[i] && !tile_sizing_sized[i])
            return 0;
    return 1;
}

static void tile_sizing_convert_func(char *key, struct tile_head *th, long long *offsets) {
    if (th->completion_file >= 0 && th->completion_file < tile_sizing_count) {
        /* tile_sizing_pos is 1-based while phase34_item_pos is 0-based */
        th->completion_pos = offsets[th->completion_file] + th->completion_pos - 1;
    }
}

void tile_sizing_finalize(void) {
    long long offsets[TILE_SIZING_MAX_FILES];
    long long total = 0;
    int i;
    for (i = 0; i < tile_sizing_count; i++) {
        offsets[i] = total;
        total += tile_sizing_counts[i];
    }
    g_hash_table_foreach(tile_hash, (GHFunc)tile_sizing_convert_func, offsets);
    phase34_item_pos = total - 1;
    tile_sizing_active = 0;
}

int phase4(FILE **in, int in_count, int with_range, char *suffix, FILE *tilesdir_out, struct zip_info *zip_info) {
    struct tile_info info;
    struct tile_head *th;
    FILE *pos;
    int ret;
    info.write = 0;
    info.maxlen = 0;
    info.suffix = suffix;
    info.tiles_list = NULL;
    info.tilesdir_out = tilesdir_out;
    if (tile_sizing_complete(in, in_count)) {
        /* Sizes and completion positions were collected while producing the item
         * files, so we can build the tilesdir without re-reading them. */
        tile_sizing_finalize();
        phase34_finish(&info, zip_info);
        ret = 0;
    } else {
        tile_sizing_active = 0;
        ret = phase34(&info, zip_info, in, NULL, in_count, with_range);
    }
    pos = tempfile(suffix, "tilesdir_pos", 1);
    if (pos) {
        for (th = tile_head_root; th; th = th->next)
            fprintf(pos, "%lld\n", th->completion_pos);
        fclose(pos);
    }
    return ret;
}

static struct tile_head tile_killer;

/* Persistent thread pool reused across slices */
static GAsyncQueue *tile_queue = NULL;
static GAsyncQueue *tile_done_queue = NULL;
static struct tile_process_thread *worker_threads = NULL;
static int worker_count = 0;

struct tile_process_thread {
    GAsyncQueue *queue;
    GThread *thread;
    char *scratch_buf;
    size_t scratch_size;
#ifdef HAVE_LZMA
    void *allocator;
#endif
};

static gpointer process_tile_worker(gpointer data) {
    struct tile_process_thread *me = (struct tile_process_thread *)data;
    struct tile_head *th;

    while ((th = (struct tile_head *)g_async_queue_pop(me->queue)) != &tile_killer) {
        int data_size = th->total_size_used;
        th->crc = crc32(0, NULL, 0);
        th->crc = crc32(th->crc, (unsigned char *)th->zip_data, data_size);

        th->comp_data = compress_for_zip(th->zip_data, data_size, th->compression_level, th->compression_method,
                                         &th->comp_size, &th->zipmthd, &me->scratch_buf, &me->scratch_size,
                                         me->allocator);
        if (!th->comp_data) {
            th->comp_data = th->zip_data;
            th->zipmthd = ZIP_COMPRESSION_STORED;
        }

        g_async_queue_push(tile_done_queue, th);
    }

    g_thread_exit(NULL);
    return NULL;
}

static void tile_worker_pool_init(int n_threads, struct zip_info *zip_info) {
    int i;
    int compression_level = zip_get_compression_level(zip_info);
    int compression_method = zip_get_compression_method(zip_info);
    if (n_threads <= 0) {
        fprintf(stderr, "Invalid thread count %d\n", n_threads);
        exit(1);
    }
    worker_count = n_threads;
    tile_queue = g_async_queue_new();
    tile_done_queue = g_async_queue_new();
    worker_threads = g_malloc0(sizeof(struct tile_process_thread) * n_threads);
    for (i = 0; i < n_threads; i++) {
        worker_threads[i].queue = tile_queue;
#ifdef HAVE_LZMA
        if (compression_level > 0 && compression_method == ZIP_COMPRESSION_LZMA)
            worker_threads[i].allocator = lzma_allocator_create(compression_level);
#endif
        worker_threads[i].thread = g_thread_new("tile_worker", process_tile_worker, &worker_threads[i]);
    }
}

static void tile_worker_pool_fini(void) {
    int i;
    if (!worker_threads)
        return;
    for (i = 0; i < worker_count; i++)
        g_async_queue_push(tile_queue, &tile_killer);
    for (i = 0; i < worker_count; i++) {
        g_thread_join(worker_threads[i].thread);
        g_free(worker_threads[i].scratch_buf);
#ifdef HAVE_LZMA
        lzma_allocator_destroy(worker_threads[i].allocator);
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

static int stream_compression_level = 0;
static int stream_compression_method = 0;

static GList *completed_pending = NULL;
static int next_zipnum = 0;
static int members_written = 0;

static gint tile_zipnum_cmp(gconstpointer a, gconstpointer b) {
    const struct tile_head *ta = a, *tb = b;
    return ta->zipnum - tb->zipnum;
}

static void tile_stream_dispatch(struct tile_head *th) {
    if (th->total_size_used != th->total_size) {
        fprintf(stderr, "Size error '%s': %d vs %d\n", th->name, th->total_size, th->total_size_used);
        exit(1);
    }
    th->compression_level = stream_compression_level;
    th->compression_method = stream_compression_method;
    g_async_queue_push(tile_queue, th);
}

static void stream_write_completed(struct zip_info *zip_info) {
    struct tile_head *th;
    while ((th = g_async_queue_try_pop(tile_done_queue)) != NULL)
        completed_pending = g_list_insert_sorted(completed_pending, th, tile_zipnum_cmp);
    while (completed_pending && ((struct tile_head *)completed_pending->data)->zipnum == next_zipnum) {
        th = completed_pending->data;
        completed_pending = g_list_delete_link(completed_pending, completed_pending);
        if (th->name[0]) {
            write_zipmember_raw(zip_info, th->name, zip_get_maxnamelen(zip_info), th->comp_data, th->comp_size,
                                th->total_size_used, th->crc, th->zipmthd);
            if (th->comp_data != th->zip_data)
                g_free(th->comp_data);
            if (!tile_data_mmap_active)
                g_free(th->zip_data);
            th->zip_data = NULL;
            th->comp_data = NULL;
            members_written++;
        }
        next_zipnum++;
    }
}

static void emit_index_submaps(struct tile_info *info) {
    struct tile_head *th;
    for (th = tile_head_root; th; th = th->next)
        if (th->name[strlen(info->suffix)])
            index_submap_add(info, th);
}

/* Disk-backed tile storage: phase 5 pre-sizes one temp file and mmaps it, so
 * tile buffers live in the page cache instead of heap memory. */
static char *tile_data_map = NULL;
static size_t tile_data_map_size = 0;
static FILE *tile_data_file = NULL;

static void tile_data_map_init(char *suffix) {
    struct tile_head *th;
    long long total = 0, off = 0;
#ifndef _MSC_VER
    for (th = tile_head_root; th; th = th->next)
        total += th->total_size;
    if (!total)
        return;
    tile_data_file = tempfile(suffix, "tiles_data", 1);
    if (!tile_data_file) {
        fprintf(stderr, "Cannot create tile data file\n");
        exit(1);
    }
    if (ftruncate(fileno(tile_data_file), total)) {
        perror("ftruncate");
        exit(1);
    }
    tile_data_map = mmap(NULL, (size_t)total, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(tile_data_file), 0);
    if (tile_data_map == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    tile_data_map_size = (size_t)total;
    for (th = tile_head_root; th; th = th->next) {
        th->zip_data = tile_data_map + off;
        th->zip_data_cap = th->total_size;
        off += th->total_size;
    }
    tile_data_mmap_active = 1;
#else
    (void)suffix;
#endif
}

static void tile_data_map_fini(void) {
    struct tile_head *th;
#ifndef _MSC_VER
    if (tile_data_map) {
        munmap(tile_data_map, tile_data_map_size);
        tile_data_map = NULL;
        tile_data_map_size = 0;
    }
    if (tile_data_file) {
        fclose(tile_data_file);
        tile_data_file = NULL;
    }
#endif
    for (th = tile_head_root; th; th = th->next)
        th->zip_data = NULL;
    tile_data_mmap_active = 0;
}

int phase5(FILE **in, FILE **references, int in_count, int with_range, char *suffix, struct zip_info *zip_info) {
    struct tile_info info;
    struct tile_head *th;
    int named_tiles = 0, i;

    create_tile_hash();

    stream_compression_level = zip_get_compression_level(zip_info);
    stream_compression_method = zip_get_compression_method(zip_info);
    completed_pending = NULL;
    next_zipnum = 0;
    members_written = 0;

    for (th = tile_head_root; th; th = th->next) {
        th->process = 1;
        th->total_size_used = 0;
        th->zip_data = NULL;
        th->zip_data_cap = 0;
        th->comp_data = NULL;
        if (th->name[0])
            named_tiles++;
    }

    tile_data_map_init(suffix);

    tile_worker_pool_init(thread_count, zip_info);
    tile_dispatch_func = tile_stream_dispatch;

    info.write = 1;
    info.maxlen = zip_get_maxnamelen(zip_info);
    info.suffix = suffix;
    info.tiles_list = NULL;
    info.tilesdir_out = NULL;

    phase34_item_pos = -1;
    emit_index_submaps(&info);

    processed_nodes = processed_nodes_out = processed_ways = processed_relations = processed_tiles = 0;
    bytes_read = 0;
    sig_alrm(0);
    for (i = 0; i < in_count; i++) {
        if (in[i]) {
            if (with_range)
                phase34_process_file_range(&info, in[i], references ? references[i] : NULL);
            else
                phase34_process_file(&info, in[i], references ? references[i] : NULL);
        }
        stream_write_completed(zip_info);
    }

    for (th = tile_head_root; th; th = th->next)
        if (th->process && th->name[0])
            tile_stream_dispatch(th);

    while (next_zipnum < named_tiles) {
        struct tile_head *t = g_async_queue_pop(tile_done_queue);
        completed_pending = g_list_insert_sorted(completed_pending, t, tile_zipnum_cmp);
        stream_write_completed(zip_info);
    }

    for (th = tile_head_root; th; th = th->next) {
        if (!th->name[0] && th->zip_data) {
            if (th->total_size_used != th->total_size) {
                fprintf(stderr, "Size error '%s': %d vs %d\n", th->name, th->total_size, th->total_size_used);
                exit(1);
            }
            dbg_assert(fwrite(th->zip_data, th->total_size_used, 1, zip_get_index(zip_info)) == 1);
        }
    }

    for (th = tile_head_root; th; th = th->next) {
        if (!tile_data_mmap_active)
            g_free(th->zip_data);
        th->zip_data = NULL;
        g_free(th->comp_data);
        th->comp_data = NULL;
    }

    g_list_free(completed_pending);
    completed_pending = NULL;
    tile_dispatch_func = NULL;
    tile_worker_pool_fini();
    tile_data_map_fini();
    zip_set_zipnum(zip_info, members_written);
    sig_alrm(0);
    sig_alrm_end();
    return 0;
}

void process_binfile(FILE *in, FILE *out) {
    struct item_bin *ib;
    while ((ib = read_item(in))) {
        item_bin_write(ib, out);
        tile_sizing_write_file(out, ib);
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
    while (fscanf(in, "%4095s", buffer) == 1) {
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
