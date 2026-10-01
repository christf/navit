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

#include "vehicle_demo.h"
#include "callback.h"
#include "config.h"
#include "coord.h"
#include "debug.h"
#include "event.h"
#include "item.h"
#include "map.h"
#include "navit.h"
#include "plugin.h"
#include "route.h"
#include "track.h"
#include "transform.h"
#include "util.h"
#include "vehicle.h"
#include <glib.h>
#include <string.h>

/**
 * @defgroup vehicle-demo Vehicle Demo
 * @ingroup vehicle-plugins
 * @brief The Vehicle for a demo. It followes the route automatically
 *
 */

struct vehicle_priv {
    int interval;
    int position_set;
    struct callback_list *cbl;
    struct navit *navit;
    struct route *route;
    struct coord_geo geo;
    double config_speed;
    double speed;
    double direction;
    struct callback *timer_callback;
    struct event_timeout *timer;
    char *timep;
    enum attr_position_valid valid; /**< Whether the vehicle has valid position data **/
    double height;
    /** Route geometry as a flat waypoint list, and the cursor into it. The
     * map_rect is rebuilt on every tick, so the walk cannot rely on item
     * iteration state to remember where it got to: it has to be kept here. */
    GArray *waypoints;
    int waypoint_index;
    /** Position reached by the previous tick. A step usually ends in the middle
     * of a segment rather than on a waypoint, so the walk has to resume from
     * here and not from the last waypoint it passed. */
    struct coord current;
    int current_valid;
};

static void vehicle_demo_destroy(struct vehicle_priv *priv) {
    if (priv->timer)
        event_remove_timeout(priv->timer);
    callback_destroy(priv->timer_callback);
    if (priv->waypoints)
        g_array_free(priv->waypoints, TRUE);
    g_free(priv->timep);
    g_free(priv);
}

static int vehicle_demo_position_attr_get(struct vehicle_priv *priv, enum attr_type type, struct attr *attr) {
    int *flags;
    switch (type) {
    case attr_position_speed:
        attr->u.numd = &priv->speed;
        break;
    case attr_position_direction:
        attr->u.numd = &priv->direction;
        break;
    case attr_position_coord_geo:
        attr->u.coord_geo = &priv->geo;
        break;
    case attr_position_time_iso8601:
        g_free(priv->timep);
        priv->timep = current_to_iso8601();
        attr->u.str = priv->timep;
        break;
    case attr_position_fix_type:
        if ((flags = tracking_get_current_flags(navit_get_tracking(priv->navit)))) {
            if (*flags & AF_UNDERGROUND)
                attr->u.num = 0;
        } else
            attr->u.num = 2;
        break;
    case attr_position_sats_used:
        attr->u.num = 3 + ((rand() % 2 + 1) * (rand() % 2 == 0 ? -1 : 1));
        break;
    case attr_position_height:
        attr->u.numd = &priv->height;
        break;
    case attr_position_valid:
        attr->u.num = priv->valid;
        break;
    default:
        return 0;
    }
    attr->type = type;
    return 1;
}

static int vehicle_demo_set_attr_do(struct vehicle_priv *priv, struct attr *attr) {
    switch (attr->type) {
    case attr_navit:
        priv->navit = attr->u.navit;
        break;
    case attr_route:
        priv->route = attr->u.route;
        break;
    case attr_speed:
        priv->config_speed = attr->u.num;
        break;
    case attr_position_height:
        priv->height = *attr->u.numd;
        break;
    case attr_interval:
        priv->interval = attr->u.num;
        if (priv->timer)
            event_remove_timeout(priv->timer);
        priv->timer = event_add_timeout(priv->interval, 1, priv->timer_callback);
        break;
    case attr_position_coord_geo:
        priv->geo = *(attr->u.coord_geo);
        if (priv->valid != attr_position_valid_valid) {
            priv->valid = attr_position_valid_valid;
            callback_list_call_attr_0(priv->cbl, attr_position_valid);
        }
        priv->position_set = 1;
        dbg(lvl_debug, "position_set %f %f", priv->geo.lat, priv->geo.lng);
        break;
    case attr_profilename:
    case attr_source:
    case attr_name:
    case attr_follow:
    case attr_active:
        // Ignore; used by Navit's infrastructure, but not relevant for this vehicle.
        break;
    default:
        dbg(lvl_error, "unsupported attribute %s", attr_to_name(attr->type));
        return 0;
    }
    return 1;
}

static int vehicle_demo_set_attr(struct vehicle_priv *priv, struct attr *attr) {
    return vehicle_demo_set_attr_do(priv, attr);
}

struct vehicle_methods vehicle_demo_methods = {
    vehicle_demo_destroy,
    vehicle_demo_position_attr_get,
    vehicle_demo_set_attr,
};

/* Collect the route geometry into a flat waypoint list, so the per-tick walk
 * can resume where the previous tick stopped instead of starting over. The
 * route map yields one item per street; item_coord_get() walks the coordinates
 * of each in turn and advancing to the next item continues the line. */
static GArray *demo_scan_waypoints(struct route *route) {
    struct map *route_map = route_get_map(route);
    struct map_rect *mr;
    struct item *item;
    GArray *wp = g_array_new(FALSE, FALSE, sizeof(struct coord));

    if (!route_map)
        return wp;
    mr = map_rect_new(route_map, NULL);
    if (!mr)
        return wp;
    if ((item = map_rect_get_item(mr)) != NULL && item->type == type_route_start)
        item = map_rect_get_item(mr);
    while (item && item->type != type_street_route)
        item = map_rect_get_item(mr);
    while (item) {
        struct coord c;
        while (item_coord_get(item, &c, 1)) {
            /* Skip duplicate joins between consecutive street items. */
            if (wp->len) {
                struct coord *prev = &g_array_index(wp, struct coord, wp->len - 1);
                if (prev->x == c.x && prev->y == c.y)
                    continue;
            }
            g_array_append_val(wp, c);
        }
        item = map_rect_get_item(mr);
    }
    map_rect_destroy(mr);
    return wp;
}

/* True if both lists hold exactly the same coordinates. */
static int demo_waypoints_equal(const GArray *a, const GArray *b) {
    guint i;
    if (!a || !b || a->len != b->len)
        return 0;
    for (i = 0; i < a->len; i++) {
        const struct coord *ca = &g_array_index(a, struct coord, i);
        const struct coord *cb = &g_array_index(b, struct coord, i);
        if (ca->x != cb->x || ca->y != cb->y)
            return 0;
    }
    return 1;
}

/* Re-anchor the walk cursor after the geometry changed.
 *
 * The route is recomputed from the moving vehicle on every position update, so
 * most of these changes are just the leading part of the path being dropped as
 * the vehicle drives along it. Discarding the cursor there sends the vehicle
 * back to waypoint 0 every tick, which shows up as a vehicle that creeps back
 * and forth around the route start and never reaches the destination.
 *
 * So instead of resetting, look for the segment of the new geometry closest to
 * where the vehicle actually is and continue from there. That also handles a
 * real reroute: route_get_position() snaps the route start to the nearest street
 * to the vehicle, so the new path always begins next to it and the cursor ends
 * up on the new geometry either way. Returns 0 if there is no position to
 * anchor to yet, in which case the caller starts from the beginning. */
static int demo_relocate_cursor(GArray *wp, struct coord *cur) {
    guint i, best = 0;
    int best_dist = 0, have = 0;
    struct coord best_pnt = {0, 0};
    struct coord from = *cur;

    if (wp->len < 2)
        return 0;
    for (i = 0; i + 1 < wp->len; i++) {
        const struct coord *a = &g_array_index(wp, struct coord, i);
        const struct coord *b = &g_array_index(wp, struct coord, i + 1);
        double dx = b->x - a->x, dy = b->y - a->y;
        double len2 = dx * dx + dy * dy;
        double t = 0.0, px, py;
        struct coord pnt;
        int dist;
        if (len2 > 0.0) {
            t = ((double)(from.x - a->x) * dx + (double)(from.y - a->y) * dy) / len2;
            if (t < 0.0)
                t = 0.0;
            if (t > 1.0)
                t = 1.0;
        }
        px = a->x + dx * t;
        py = a->y + dy * t;
        pnt.x = (int)px;
        pnt.y = (int)py;
        dist = transform_distance(projection_mg, &from, &pnt);
        if (!have || dist < best_dist) {
            have = 1;
            best = i;
            best_dist = dist;
            best_pnt = pnt;
        }
    }
    *cur = best_pnt;
    return (int)best;
}

static void vehicle_demo_timer(struct vehicle_priv *priv) {
    struct coord *nxt, ci;
    int slen, len, dx, dy;

    len = (priv->config_speed * priv->interval / 1000) / 3.6;
    dbg(lvl_debug, "###### Entering simulation loop");
    if (!priv->config_speed)
        return;
    if (!priv->route && !priv->navit) {
        if (priv->position_set)
            callback_list_call_attr_0(priv->cbl, attr_position_coord_geo);
        return;
    }
    {
        struct route *route = priv->route ? priv->route : navit_get_route(priv->navit);
        if (route) {
            /* Rescan the geometry every tick, like the original walk did, so a
             * recalculated route is picked up: route_get_map() hands back the
             * same map object for a given route, so neither the route pointer
             * nor the map pointer changes when a new path is computed. Keep the
             * walk anchored to the vehicle's own position across the change. */
            GArray *fresh = demo_scan_waypoints(route);
            if (!demo_waypoints_equal(priv->waypoints, fresh)) {
                struct coord anchor = priv->current;
                int anchored = priv->current_valid;
                if (priv->waypoints)
                    g_array_free(priv->waypoints, TRUE);
                priv->waypoints = fresh;
                priv->waypoint_index = anchored ? demo_relocate_cursor(fresh, &anchor) : 0;
                priv->current_valid = anchored;
                if (anchored)
                    priv->current = anchor;
                dbg(lvl_debug, "demo loaded %u waypoints, continuing at %d", fresh->len, priv->waypoint_index + 1);
            } else {
                g_array_free(fresh, TRUE);
            }
        } else if (priv->waypoints) {
            /* No route at all: drop the geometry rather than keep walking a
             * path that is no longer backed by one. */
            g_array_free(priv->waypoints, TRUE);
            priv->waypoints = NULL;
            priv->waypoint_index = 0;
            priv->current_valid = 0;
        }
    }
    if (!priv->waypoints || priv->waypoints->len < 2) {
        if (priv->position_set)
            callback_list_call_attr_0(priv->cbl, attr_position_coord_geo);
        return;
    }

    priv->position_set = 1;
    if (!priv->current_valid) {
        priv->current = g_array_index(priv->waypoints, struct coord, 0);
        priv->current_valid = 1;
    }
    while (priv->waypoint_index < (int)priv->waypoints->len - 1 && len > 0) {
        nxt = &g_array_index(priv->waypoints, struct coord, priv->waypoint_index + 1);
        slen = transform_distance(projection_mg, &priv->current, nxt);
        if (slen < 1) {
            /* Degenerate segment: step over it. */
            priv->waypoint_index++;
            continue;
        }
        if (slen <= len) {
            /* This step reaches or passes the next waypoint. */
            len -= slen;
            priv->current = *nxt;
            priv->waypoint_index++;
            if (len == 0)
                break;
            continue;
        }
        /* The step ends inside this segment: remember where, so the next tick
         * resumes from here instead of restarting at the last waypoint. */
        dx = nxt->x - priv->current.x;
        dy = nxt->y - priv->current.y;
        priv->direction = transform_get_angle_delta(&priv->current, nxt, 0);
        ci.x = priv->current.x + dx * len / slen;
        ci.y = priv->current.y + dy * len / slen;
        priv->current = ci;
        priv->speed = priv->config_speed
                      + ((rand() % 5 + 1) * (rand() % 2 == 0 ? -1 : 1));  // a little random + or - 1 to 5 km/h
        priv->height = priv->height
                       + ((rand() % 50 + 1) * (rand() % 2 == 0 ? -1 : 1));  // a little random + or - 1 to 50 m
        dbg(lvl_debug, "ci=0x%x,0x%x", ci.x, ci.y);
        transform_to_geo(projection_mg, &ci, &priv->geo);
        if (priv->valid != attr_position_valid_valid) {
            priv->valid = attr_position_valid_valid;
            callback_list_call_attr_0(priv->cbl, attr_position_valid);
        }
        callback_list_call_attr_0(priv->cbl, attr_position_coord_geo);
        return;
    }

    /* The loop above only exits early when a step lands inside a segment, and
     * that case has already returned. Reaching here means the whole remaining
     * distance was consumed by whole waypoint hops, so this tick's distance is
     * spent but the route may well continue. Only the cursor running off the end
     * of the geometry means the destination is reached. */
    if (priv->waypoint_index < (int)priv->waypoints->len - 1) {
        transform_to_geo(projection_mg, &priv->current, &priv->geo);
        if (priv->valid != attr_position_valid_valid) {
            priv->valid = attr_position_valid_valid;
            callback_list_call_attr_0(priv->cbl, attr_position_valid);
        }
        callback_list_call_attr_0(priv->cbl, attr_position_coord_geo);
        return;
    }

    priv->waypoint_index = (int)priv->waypoints->len - 1;
    transform_to_geo(projection_mg, &priv->current, &priv->geo);
    priv->speed = 0;
    dbg(lvl_debug, "destination reached");
    if (priv->valid != attr_position_valid_valid) {
        priv->valid = attr_position_valid_valid;
        callback_list_call_attr_0(priv->cbl, attr_position_valid);
    }
    callback_list_call_attr_0(priv->cbl, attr_position_coord_geo);
}

void vehicle_demo_test_tick(struct vehicle *v) {
    vehicle_demo_timer(vehicle_get_priv(v));
}

static struct vehicle_priv *vehicle_demo_new(struct vehicle_methods *meth, struct callback_list *cbl,
                                             struct attr **attrs) {
    struct vehicle_priv *ret;

    dbg(lvl_debug, "enter");
    ret = g_new0(struct vehicle_priv, 1);
    ret->cbl = cbl;
    ret->interval = 1000;
    ret->config_speed = 40;
    ret->height = -10;
    ret->timer_callback = callback_new_1(callback_cast(vehicle_demo_timer), ret);
    ret->valid = attr_position_valid_invalid;
    *meth = vehicle_demo_methods;
    while (attrs && *attrs)
        vehicle_demo_set_attr_do(ret, *attrs++);
    if (!ret->timer)
        ret->timer = event_add_timeout(ret->interval, 1, ret->timer_callback);
    return ret;
}

void plugin_init(void) {
    dbg(lvl_debug, "enter");
    plugin_register_category_vehicle("demo", vehicle_demo_new);
}

/** @} */
