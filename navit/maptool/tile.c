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
#include "coord.h"
#include "debug.h"
#include "geom.h"
#include "item_type_def.h"
#include "maptool.h"
#include "types.h"
#include <assert.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#    include <getopt.h>
#    include <unistd.h>
#endif

struct zip_info;

GList *aux_tile_list;
struct tile_head *tile_head_root;
GHashTable *strings_hash, *tile_hash, *tile_hash2;

static char *string_hash_lookup(const char *key) {
    char *key_ptr = NULL;

    if (strings_hash == NULL) {
        strings_hash = g_hash_table_new(g_str_hash, g_str_equal);
    }

    if ((key_ptr = g_hash_table_lookup(strings_hash, key)) == NULL) {
        key_ptr = g_strdup(key);
        g_hash_table_insert(strings_hash, key_ptr, (gpointer)key_ptr);
    }
    return key_ptr;
}

static char **th_get_subtile(const struct tile_head *th, int idx) {
    char *subtile_ptr = NULL;
    subtile_ptr = (char *)th + sizeof(struct tile_head) + idx * sizeof(char *);
    return (char **)subtile_ptr;
}

int tile(struct rect *r, char *suffix, char *ret, int max, int overlap, struct rect *tr) {
    int x0, x2, x4;
    int y0, y2, y4;
    int xo, yo;
    int i;
    struct rect rr = *r;

    x0 = world_bbox.l.x;
    y0 = world_bbox.l.y;
    x4 = world_bbox.h.x;
    y4 = world_bbox.h.y;

    if (rr.l.x < x0)
        rr.l.x = x0;
    if (rr.h.x < x0)
        rr.h.x = x0;
    if (rr.l.y < y0)
        rr.l.y = y0;
    if (rr.h.y < y0)
        rr.h.y = y0;
    if (rr.l.x > x4)
        rr.l.x = x4;
    if (rr.h.x > x4)
        rr.h.x = x4;
    if (rr.l.y > y4)
        rr.l.y = y4;
    if (rr.h.y > y4)
        rr.h.y = y4;

    for (i = 0; i < max; i++) {
        x2 = (x0 + x4) / 2;
        y2 = (y0 + y4) / 2;
        xo = (x4 - x0) * overlap / 100;
        yo = (y4 - y0) * overlap / 100;
        if (contains_bbox(x0, y0, x2 + xo, y2 + yo, &rr)) {
            strcat(ret, "d");
            x4 = x2 + xo;
            y4 = y2 + yo;
        } else if (contains_bbox(x2 - xo, y0, x4, y2 + yo, &rr)) {
            strcat(ret, "c");
            x0 = x2 - xo;
            y4 = y2 + yo;
        } else if (contains_bbox(x0, y2 - yo, x2 + xo, y4, &rr)) {
            strcat(ret, "b");
            x4 = x2 + xo;
            y0 = y2 - yo;
        } else if (contains_bbox(x2 - xo, y2 - yo, x4, y4, &rr)) {
            strcat(ret, "a");
            x0 = x2 - xo;
            y0 = y2 - yo;
        } else
            break;
    }
    if (tr) {
        tr->l.x = x0;
        tr->l.y = y0;
        tr->h.x = x4;
        tr->h.y = y4;
    }
    if (suffix)
        strcat(ret, suffix);
    return i;
}

void tile_bbox(char *tile, struct rect *r, int overlap) {
    struct coord c;
    int xo, yo;
    *r = world_bbox;
    while (*tile) {
        c.x = (r->l.x + r->h.x) / 2;
        c.y = (r->l.y + r->h.y) / 2;
        xo = (r->h.x - r->l.x) * overlap / 100;
        yo = (r->h.y - r->l.y) * overlap / 100;
        switch (*tile) {
        case 'a':
            r->l.x = c.x - xo;
            r->l.y = c.y - yo;
            break;
        case 'b':
            r->h.x = c.x + xo;
            r->l.y = c.y - yo;
            break;
        case 'c':
            r->l.x = c.x - xo;
            r->h.y = c.y + yo;
            break;
        case 'd':
            r->h.x = c.x + xo;
            r->h.y = c.y + yo;
            break;
        }
        tile++;
    }
}

int tile_len(char *tile) {
    int ret = 0;
    while (tile[0] >= 'a' && tile[0] <= 'd') {
        tile++;
        ret++;
    }
    return ret;
}

static void tile_extend(char *tile, struct item_bin *ib, GList **tiles_list) {
    struct tile_head *th = NULL;
    if (debug_tile(tile))
        fprintf(stderr, "Tile:Writing %d bytes to '%s' (%p,%p) 0x%x " LONGLONG_FMT "\n", (ib->len + 1) * 4, tile,
                g_hash_table_lookup(tile_hash, tile), tile_hash2 ? g_hash_table_lookup(tile_hash2, tile) : NULL,
                ib->type, item_bin_get_id(ib));
    if (tile_hash2)
        th = g_hash_table_lookup(tile_hash2, tile);
    if (!th)
        th = g_hash_table_lookup(tile_hash, tile);
    if (!th) {
        th = g_malloc(sizeof(struct tile_head) + sizeof(char *));
        // strcpy(th->subtiles, tile);
        th->num_subtiles = 1;
        th->total_size = 0;
        th->total_size_used = 0;
        th->zipnum = 0;
        th->zip_data = NULL;
        th->name = string_hash_lookup(tile);
        *th_get_subtile(th, 0) = th->name;

        if (tile_hash2)
            g_hash_table_insert(tile_hash2, string_hash_lookup(th->name), th);
        if (tiles_list)
            *tiles_list = g_list_append(*tiles_list, string_hash_lookup(th->name));
        processed_tiles++;
        if (debug_tile(tile))
            fprintf(stderr, "new '%s'\n", tile);
    }
    th->total_size += ib->len * 4 + 4;
    if (debug_tile(tile))
        fprintf(stderr, "New total size of %s(%p):%d\n", th->name, th, th->total_size);
    g_hash_table_insert(tile_hash, string_hash_lookup(th->name), th);
}

static void write_item_dynamic(char *tile, struct item_bin *ib, struct tile_info *info) {
    struct tile_head *th;
    int size;

    th = g_hash_table_lookup(tile_hash2, tile);
    if (!th)
        th = g_hash_table_lookup(tile_hash, tile);
    if (!th) {
        th = g_malloc0(sizeof(struct tile_head) + sizeof(char *));
        th->num_subtiles = 1;
        th->total_size = 0;
        th->total_size_used = 0;
        th->zipnum = 0;
        th->zip_data = NULL;
        th->name = string_hash_lookup(tile);
        th->compression_level = info ? info->compression_level : 0;
        *th_get_subtile(th, 0) = th->name;
        if (tile_hash2)
            g_hash_table_insert(tile_hash2, string_hash_lookup(th->name), th);
        if (info && info->tiles_list)
            *info->tiles_list = g_list_append(*info->tiles_list, string_hash_lookup(th->name));
        processed_tiles++;
        /* link into tile_head_root list */
        th->next = tile_head_root;
        tile_head_root = th;
    }
    th->process = 1;
    size = (ib->len + 1) * 4;
    {
        int needed = th->total_size_used + size;
        if (needed < th->total_size_used) {
            fprintf(stderr, "Tile '%s' size overflow\n", tile);
            exit(1);
        }
        th->zip_data = g_realloc(th->zip_data, needed);
    }
    memcpy(th->zip_data + th->total_size_used, ib, size);
    th->total_size_used += size;
    th->total_size = th->total_size_used;
}

static void write_item(char *tile, struct item_bin *ib, FILE *reference, struct tile_info *info) {
    struct tile_head *th;
    int size;

    if (info && info->dynamic) {
        write_item_dynamic(tile, ib, info);
        return;
    }

    th = g_hash_table_lookup(tile_hash2, tile);
    if (debug_itembin(ib)) {
        fprintf(stderr, "tile head %p\n", th);
    }
    if (!th)
        th = g_hash_table_lookup(tile_hash, tile);
    if (th) {
        if (debug_itembin(ib)) {
            fprintf(stderr, "Match %s %d %s\n", tile, th->process, th->name);
            dump_itembin(ib);
        }
        if (th->process != 0 && th->process != 1) {
            fprintf(stderr, "error with tile '%s' of length %d\n", tile, (int)strlen(tile));
            abort();
        }
        if (!th->process) {
            if (reference)
                fseek(reference, 8, SEEK_CUR);
            return;
        }
        if (debug_tile(tile))
            fprintf(stderr, "Data:Writing %d bytes to '%s' (%p,%p) 0x%x\n", (ib->len + 1) * 4, tile,
                    g_hash_table_lookup(tile_hash, tile), tile_hash2 ? g_hash_table_lookup(tile_hash2, tile) : NULL,
                    ib->type);
        size = (ib->len + 1) * 4;
        if (th->total_size_used + size > th->total_size) {
            fprintf(stderr, "Overflow in tile %s (used %d max %d item %d)\n", tile, th->total_size_used, th->total_size,
                    size);
            exit(1);
            return;
        }
        if (reference) {
            int offset = th->total_size_used / 4;
            dbg_assert(fwrite(&th->zipnum, sizeof(th->zipnum), 1, reference) == 1);
            dbg_assert(fwrite(&offset, sizeof(th->total_size_used), 1, reference) == 1);
        }
        if (th->zip_data)
            memcpy(th->zip_data + th->total_size_used, ib, size);
        th->total_size_used += size;
        if (th->total_size_used == th->total_size && info && info->tile_compress_queue && th->name[0]) {
            g_async_queue_push((GAsyncQueue *)info->tile_compress_queue, th);
            info->tiles_pushed++;
        }
    } else {
        fprintf(stderr, "no tile hash found for %s\n", tile);
        exit(1);
    }
}

void tile_write_item_to_tile(struct tile_info *info, struct item_bin *ib, FILE *reference, char *name) {
    if (info->dynamic || info->write)
        write_item(name, ib, reference, info);
    else
        tile_extend(name, ib, info->tiles_list);
}

void tile_write_item_minmax(struct tile_info *info, struct item_bin *ib, FILE *reference, int min, int max) {
    /*TODO: make slice_trigger and slice_target configurable by commandline parameter.
     * bonus: find out why there is a 'min' parameter here
     */
    int slice_trigger = 4;
    int slice_target = 7;
    struct rect r;
    char buffer[1024];
    bbox((struct coord *)(ib + 1), ib->clen / 2, &r);
    buffer[0] = '\0';
    tile(&r, info->suffix, buffer, max, overlap, NULL);
    if ((ib->type >= type_area) && (ib->type != type_poly_water_tiled) && (tile_len(buffer) < slice_trigger)) {
        itembin_nicer_slicer(info, ib, reference, buffer, slice_target);
    } else {
        tile_write_item_to_tile(info, ib, reference, buffer);
    }
}

int add_aux_tile(struct zip_info *zip_info, char *name, char *filename, int size) {
    struct aux_tile *at;
    GList *l;
    l = aux_tile_list;
    while (l) {
        at = l->data;
        if (!g_strcmp0(at->name, name)) {
            return -1;
        }
        l = g_list_next(l);
    }
    at = g_new0(struct aux_tile, 1);
    at->name = g_strdup(name);
    at->filename = g_strdup(filename);
    at->size = size;
    aux_tile_list = g_list_append(aux_tile_list, at);
    fprintf(stderr, "Adding %s as %s\n", filename, name);
    return zip_add_member(zip_info);
}

int write_aux_tiles(struct zip_info *zip_info) {
    GList *l = aux_tile_list;
    struct aux_tile *at;
    char *buffer;
    FILE *f;
    int count = 0;

    while (l) {
        at = l->data;
        buffer = g_malloc(at->size);
        f = fopen(at->filename, "rb");
        assert(f != NULL);

        if (fread(buffer, at->size, 1, f) == 0) {
            dbg(lvl_warning, "fread failed");
            fclose(f);
        } else {
            fclose(f);
            write_zipmember(zip_info, at->name, zip_get_maxnamelen(zip_info), buffer, at->size);
            count++;
            l = g_list_next(l);
            zip_add_member(zip_info);
        }
        g_free(buffer);
    }
    return count;
}

static int add_tile_hash(struct tile_head *th) {
    int idx, len, maxnamelen = 0;
    char **data;

    for (idx = 0; idx < th->num_subtiles; idx++) {

        data = th_get_subtile(th, idx);

        if (debug_tile(((char *)data)) || debug_tile(th->name)) {
            fprintf(stderr, "Parent for '%s' is '%s'\n", *data, th->name);
        }

        g_hash_table_insert(tile_hash2, *data, th);

        len = strlen(*data);

        if (len > maxnamelen) {
            maxnamelen = len;
        }
    }
    return maxnamelen;
}

void tile_cleanup(void) {
    struct tile_head *th = tile_head_root;
    while (th) {
        struct tile_head *next = th->next;
        g_free(th->zip_data);
        if (th->comp_data && th->comp_data != th->zip_data)
            g_free(th->comp_data);
        g_free(th);
        th = next;
    }
    tile_head_root = NULL;
    if (tile_hash) {
        g_hash_table_destroy(tile_hash);
        tile_hash = NULL;
    }
    if (tile_hash2) {
        g_hash_table_destroy(tile_hash2);
        tile_hash2 = NULL;
    }
}

int create_tile_hash(void) {
    struct tile_head *th;
    int len, maxnamelen = 0;

    if (tile_hash2)
        g_hash_table_destroy(tile_hash2);
    tile_hash2 = g_hash_table_new(g_str_hash, g_str_equal);
    th = tile_head_root;
    while (th) {
        len = add_tile_hash(th);
        if (len > maxnamelen)
            maxnamelen = len;
        th = th->next;
    }
    return maxnamelen;
}

struct attr map_information_attrs[32];

void index_init(struct zip_info *info, int version) {
    struct item_bin *item_bin;
    int i;
    map_information_attrs[0].type = attr_version;
    map_information_attrs[0].u.num = version;
    item_bin = init_item(type_map_information);
    for (i = 0; i < 32; i++) {
        if (!map_information_attrs[i].type)
            break;
        item_bin_add_attr(item_bin, &map_information_attrs[i]);
    }
    item_bin_write(item_bin, zip_get_index(info));
}
