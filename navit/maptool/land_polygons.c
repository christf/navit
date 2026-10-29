/*
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2011 Navit Team
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

#include "maptool.h"
#include "projection.h"
#include "shapefil.h"
#include "transform.h"
#include <glib.h>
#include <stdio.h>

#define IS_POLYGON(x)                                                                                                  \
    ((x)->nSHPType == SHPT_POLYGON || (x)->nSHPType == SHPT_POLYGONZ || (x)->nSHPType == SHPT_POLYGONM)

struct land_ring {
    int count;
    struct coord *coords;
    struct rect bbox;
};

struct land_polygon {
    int nrings;
    struct land_ring *rings;
    struct rect bbox;
};

static struct land_polygon *land_polygons;
static int land_polygon_count;
static int land_polygons_loaded;

static int ring_contains(struct land_ring *ring, struct coord *c) {
    int i, j, inside = 0;
    if (c->x < ring->bbox.l.x || c->x > ring->bbox.h.x || c->y < ring->bbox.l.y || c->y > ring->bbox.h.y)
        return 0;
    j = ring->count - 1;
    for (i = 0; i < ring->count; i++) {
        struct coord *a = &ring->coords[i];
        struct coord *b = &ring->coords[j];
        if (((a->y > c->y) != (b->y > c->y))
            && (c->x < (double)(b->x - a->x) * (c->y - a->y) / (double)(b->y - a->y) + a->x))
            inside = !inside;
        j = i;
    }
    return inside;
}

int land_polygons_contains(struct coord *c) {
    int i, j, inside;
    if (!land_polygons_loaded)
        return 0;
    for (i = 0; i < land_polygon_count; i++) {
        struct land_polygon *polygon = &land_polygons[i];
        if (c->x < polygon->bbox.l.x || c->x > polygon->bbox.h.x || c->y < polygon->bbox.l.y
            || c->y > polygon->bbox.h.y)
            continue;
        inside = 0;
        for (j = 0; j < polygon->nrings; j++)
            if (ring_contains(&polygon->rings[j], c))
                inside = !inside;
        if (inside)
            return 1;
    }
    return 0;
}

int land_polygons_get_bbox(struct rect *r) {
    int i;
    if (!land_polygons_loaded || !land_polygon_count)
        return 0;
    *r = land_polygons[0].bbox;
    for (i = 1; i < land_polygon_count; i++) {
        struct rect *b = &land_polygons[i].bbox;
        if (b->l.x < r->l.x)
            r->l.x = b->l.x;
        if (b->l.y < r->l.y)
            r->l.y = b->l.y;
        if (b->h.x > r->h.x)
            r->h.x = b->h.x;
        if (b->h.y > r->h.y)
            r->h.y = b->h.y;
    }
    return 1;
}

static void read_ring(SHPObject *obj, int part, struct land_ring *ring) {
    int i, start = obj->panPartStart[part];
    int end = part + 1 < obj->nParts ? obj->panPartStart[part + 1] : obj->nVertices;
    ring->count = end - start;
    ring->coords = g_new(struct coord, ring->count);
    for (i = 0; i < ring->count; i++) {
        struct coord_geo geo;
        geo.lng = obj->padfX[start + i];
        geo.lat = obj->padfY[start + i];
        transform_from_geo(projection_mg, &geo, &ring->coords[i]);
        if (i == 0) {
            ring->bbox.l = ring->bbox.h = ring->coords[i];
        } else {
            if (ring->coords[i].x < ring->bbox.l.x)
                ring->bbox.l.x = ring->coords[i].x;
            if (ring->coords[i].y < ring->bbox.l.y)
                ring->bbox.l.y = ring->coords[i].y;
            if (ring->coords[i].x > ring->bbox.h.x)
                ring->bbox.h.x = ring->coords[i].x;
            if (ring->coords[i].y > ring->bbox.h.y)
                ring->bbox.h.y = ring->coords[i].y;
        }
    }
}

void land_polygons_load(const char *filename) {
    SHPHandle handle;
    int n_entities, n_shape_type, i, j;

    if (land_polygons_loaded)
        return;
    handle = SHPOpen(filename, "rb");
    if (!handle) {
        fprintf(stderr, "Failed to open land polygons '%s'\n", filename);
        return;
    }
    SHPGetInfo(handle, &n_entities, &n_shape_type, NULL, NULL);
    land_polygons = g_new0(struct land_polygon, n_entities);
    for (i = 0; i < n_entities; i++) {
        SHPObject *obj = SHPReadObject(handle, i);
        struct land_polygon *polygon;
        if (!obj)
            continue;
        if (!IS_POLYGON(obj) || obj->nParts < 1 || obj->nVertices < 3) {
            SHPDestroyObject(obj);
            continue;
        }
        polygon = &land_polygons[land_polygon_count];
        polygon->rings = g_new0(struct land_ring, obj->nParts);
        for (j = 0; j < obj->nParts; j++) {
            read_ring(obj, j, &polygon->rings[polygon->nrings]);
            if (polygon->rings[polygon->nrings].count < 3)
                break;
            polygon->nrings++;
        }
        if (polygon->nrings != obj->nParts) {
            for (j = 0; j <= polygon->nrings; j++)
                g_free(polygon->rings[j].coords);
            g_free(polygon->rings);
            SHPDestroyObject(obj);
            continue;
        }
        polygon->bbox = polygon->rings[0].bbox;
        for (j = 1; j < polygon->nrings; j++) {
            if (polygon->rings[j].bbox.l.x < polygon->bbox.l.x)
                polygon->bbox.l.x = polygon->rings[j].bbox.l.x;
            if (polygon->rings[j].bbox.l.y < polygon->bbox.l.y)
                polygon->bbox.l.y = polygon->rings[j].bbox.l.y;
            if (polygon->rings[j].bbox.h.x > polygon->bbox.h.x)
                polygon->bbox.h.x = polygon->rings[j].bbox.h.x;
            if (polygon->rings[j].bbox.h.y > polygon->bbox.h.y)
                polygon->bbox.h.y = polygon->rings[j].bbox.h.y;
        }
        land_polygon_count++;
        SHPDestroyObject(obj);
    }
    SHPClose(handle);
    land_polygons_loaded = 1;
    fprintf(stderr, "Loaded %d land polygons from '%s'\n", land_polygon_count, filename);
}

void land_polygons_destroy(void) {
    int i, j;
    for (i = 0; i < land_polygon_count; i++) {
        for (j = 0; j < land_polygons[i].nrings; j++)
            g_free(land_polygons[i].rings[j].coords);
        g_free(land_polygons[i].rings);
    }
    g_free(land_polygons);
    land_polygons = NULL;
    land_polygon_count = 0;
    land_polygons_loaded = 0;
}
