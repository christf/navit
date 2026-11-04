#include "gui_internal_poi_route.h"
#include "callback.h"
#include "coord.h"
#include "debug.h"
#include "event.h"
#include "graphics.h"
#include "gui_internal.h"
#include "gui_internal_keyboard.h"
#include "gui_internal_menu.h"
#include "gui_internal_poi.h"
#include "gui_internal_priv.h"
#include "gui_internal_widget.h"
#include "item.h"
#include "map.h"
#include "mapset.h"
#include "navit.h"
#include "navit_nls.h"
#include "point.h"
#include "route.h"
#include "transform.h"
#include "util.h"
#include "vehicle.h"
#include <glib.h>
#include <limits.h>
#include <stdlib.h>

#define POI_ROUTE_BATCH_SIZE 2000
#define POI_ROUTE_PAGESIZE 50
#define POI_ROUTE_DEFAULT_RADIUS 2000
#define POI_ROUTE_DETOUR_FACTOR 1.3
#define POI_ROUTE_OFFROAD_SPEED 11

enum poi_route_sort_mode {
    poi_route_sort_along,
    poi_route_sort_detour,
};

static void poi_route_cmd_sort(struct gui_priv *this, struct widget *wm, void *data);
static void poi_route_cmd_more(struct gui_priv *this, struct widget *wm, void *data);
static void poi_route_cmd_radius(struct gui_priv *this, struct widget *wm, void *data);
static void poi_route_cmd_set_waypoint(struct gui_priv *this, struct widget *wm, void *data);
static int poi_route_cmp_along(const void *a, const void *b);
static int poi_route_cmp_detour(const void *a, const void *b);

struct poi_route_param {
    struct poi_param p;
    int perp_radius;
    int sort_mode;
};

/**
 * @brief The remaining route as a polyline, used to compute distances along the route.
 */
struct poi_route_polyline {
    struct coord *c; /**< Coordinates of the polyline, in projection_mg */
    int count;       /**< Number of coordinates */
    int *cum;        /**< cum[i] is the distance along the polyline from the start to c[i], in meters */
    int total;       /**< Total length of the polyline, in meters */
};

struct poi_route_item {
    int dist_along;
    int perp_dist;
    int detour_dist;
    int detour_time;
    char *label;
    struct item item;
    struct coord c;
    struct coord route_c;
};

struct poi_route_search_state {
    struct gui_priv *this;
    struct poi_route_param *param;
    struct coord center;
    enum projection pro;

    struct poi_route_polyline pl;

    struct mapset_handle *h;
    struct map *m;
    struct map_rect *mr;
    struct map_selection *sel, *selm;
    struct callback *idle_cb;
    struct event_idle *idle_ev;
    int cancel;
    int spin_frame;

    struct poi_route_item *items;
    int nitems, capitems;
    int pagenb;

    struct widget *wbox, *topbox, *searching_label, *cancel_button;
};

static void poi_route_param_free(void *p) {
    if (((struct poi_route_param *)p)->p.filterstr)
        g_free(((struct poi_route_param *)p)->p.filterstr);
    if (((struct poi_route_param *)p)->p.filter)
        g_list_free(((struct poi_route_param *)p)->p.filter);
    g_free(p);
}

static struct poi_route_param *poi_route_param_clone(struct poi_route_param *p) {
    struct poi_param *inner;
    struct poi_route_param *r = g_new0(struct poi_route_param, 1);
    inner = gui_internal_poi_param_clone(&p->p);
    r->p = *inner;
    g_free(inner);
    r->perp_radius = p->perp_radius;
    r->sort_mode = p->sort_mode;
    return r;
}

static void poi_route_estimate_detour(int perp_dist, int *detour_dist, int *detour_time) {
    *detour_dist = perp_dist * 2 * POI_ROUTE_DETOUR_FACTOR;
    *detour_time = *detour_dist / POI_ROUTE_OFFROAD_SPEED;
}

static int poi_route_cmp_along(const void *a, const void *b) {
    const struct poi_route_item *ra = a, *rb = b;
    return ra->dist_along - rb->dist_along;
}

static int poi_route_cmp_detour(const void *a, const void *b) {
    const struct poi_route_item *ra = a, *rb = b;
    return ra->detour_dist - rb->detour_dist;
}

static void poi_route_polyline_free(struct poi_route_polyline *pl) {
    g_free(pl->c);
    g_free(pl->cum);
    pl->c = NULL;
    pl->cum = NULL;
    pl->count = 0;
    pl->total = 0;
}

/**
 * @brief Builds a polyline of the remaining route from the route path map.
 *
 * Iterates the items of the route map in order and collects the coordinates of all
 * {@code type_street_route} items into a single polyline, then computes cumulative
 * distances along it.
 *
 * @param route The active route
 * @param pl Receives the polyline
 * @return 1 on success, 0 if the route has no usable path
 */
static int poi_route_polyline_build(struct route *route, struct poi_route_polyline *pl) {
    struct map *map;
    struct map_rect *mr;
    struct item *item;
    struct coord c;
    int cap = 256;
    int i;

    memset(pl, 0, sizeof(*pl));
    map = route ? route_get_map(route) : NULL;
    if (!map)
        return 0;
    mr = map_rect_new(map, NULL);
    if (!mr)
        return 0;
    pl->c = g_new(struct coord, cap);
    while ((item = map_rect_get_item(mr))) {
        if (item->type != type_street_route)
            continue;
        while (item_coord_get(item, &c, 1)) {
            if (pl->count && c.x == pl->c[pl->count - 1].x && c.y == pl->c[pl->count - 1].y)
                continue;
            if (pl->count >= cap) {
                cap *= 2;
                pl->c = g_renew(struct coord, pl->c, cap);
            }
            pl->c[pl->count++] = c;
        }
    }
    map_rect_destroy(mr);
    if (pl->count < 2) {
        poi_route_polyline_free(pl);
        return 0;
    }
    pl->cum = g_new(int, pl->count);
    pl->cum[0] = 0;
    for (i = 1; i < pl->count; i++)
        pl->cum[i] = pl->cum[i - 1] + transform_distance(projection_mg, &pl->c[i - 1], &pl->c[i]);
    pl->total = pl->cum[pl->count - 1];
    dbg(lvl_debug, "route polyline: %d points, %d meters", pl->count, pl->total);
    return 1;
}

static int poi_route_polyline_project(struct poi_route_polyline *pl, struct coord *c, int *dist_along,
                                      struct coord *route_c) {
    struct coord local_route_c;
    int sq, pos;
    if (!route_c)
        route_c = &local_route_c;
    sq = transform_distance_polyline_sq(pl->c, pl->count, c, route_c, &pos);
    if (sq == INT_MAX || pos >= pl->count)
        return 0;
    if (dist_along) {
        *dist_along = pl->cum[pos] + transform_distance(projection_mg, &pl->c[pos], route_c);
        if (*dist_along > pl->total)
            *dist_along = pl->total;
    }
    return 1;
}

static int poi_route_get_vehicle_pos(struct navit *nav, struct coord *c) {
    struct attr vattr, pattr;
    if (!navit_get_attr(nav, attr_vehicle, &vattr, NULL) || !vattr.u.vehicle)
        return 0;
    if (!vehicle_get_attr(vattr.u.vehicle, attr_position_coord_geo, &pattr, NULL))
        return 0;
    transform_from_geo(projection_mg, pattr.u.coord_geo, c);
    return 1;
}

static void poi_route_format_distance(int meters, char *buf, size_t len) {
    if (meters >= 100000)
        snprintf(buf, len, "%dkm", meters / 1000);
    else if (meters >= 1000)
        snprintf(buf, len, _("%d.%dkm"), meters / 1000, (meters % 1000) / 100);
    else
        snprintf(buf, len, "%dm", meters);
}

static void poi_route_format_km(int meters, char *buf, size_t len) {
    snprintf(buf, len, _("%d.%d"), meters / 1000, (meters % 1000) / 100);
}

static void poi_route_state_destroy(struct poi_route_search_state *st) {
    int i;

    if (st->idle_ev) {
        event_remove_idle(st->idle_ev);
        st->idle_ev = NULL;
    }
    if (st->idle_cb) {
        callback_destroy(st->idle_cb);
        st->idle_cb = NULL;
    }
    if (st->mr) {
        map_rect_destroy(st->mr);
        st->mr = NULL;
    }
    if (st->selm) {
        map_selection_destroy(st->selm);
        st->selm = NULL;
    }
    if (st->sel) {
        map_selection_destroy(st->sel);
        st->sel = NULL;
    }
    if (st->h) {
        mapset_close(st->h);
        st->h = NULL;
    }
    for (i = 0; i < st->nitems; i++)
        g_free(st->items[i].label);
    g_free(st->items);
    poi_route_polyline_free(&st->pl);
    if (st->param)
        poi_route_param_free(st->param);
    st->this->search_active = 0;
    g_free(st);
}

static void poi_route_state_data_free(void *data) {
    poi_route_state_destroy(data);
}

static void poi_route_state_detach(struct poi_route_search_state *st) {
    if (st->wbox && st->wbox->data == st) {
        st->wbox->data = NULL;
        st->wbox->data_free = NULL;
    }
}

static void poi_route_sort_items(struct poi_route_search_state *st) {
    if (st->param->sort_mode == poi_route_sort_detour)
        qsort(st->items, st->nitems, sizeof(struct poi_route_item), poi_route_cmp_detour);
    else
        qsort(st->items, st->nitems, sizeof(struct poi_route_item), poi_route_cmp_along);
}

static void poi_route_render_results(struct poi_route_search_state *st);

static struct widget *poi_route_result_widget(struct poi_route_search_state *st, struct poi_route_item *d) {
    struct gui_priv *this = st->this;
    struct widget *wh, *wv, *wi, *wl;
    struct graphics_image *icon;
    char distbuf[32], dirbuf[32] = "", kmbuf[32];
    char *s1, *s2;

    poi_route_format_distance(d->dist_along, distbuf, sizeof(distbuf));
    get_compass_direction(dirbuf, transform_get_angle_delta(&st->center, &d->c, 0), 1);

    if (d->label && d->label[0])
        s1 = g_strdup_printf("%s %s %s", distbuf, dirbuf, d->label);
    else
        s1 = g_strdup_printf("%s %s %s", distbuf, dirbuf, item_to_name(d->item.type));

    poi_route_format_km(d->detour_dist, kmbuf, sizeof(kmbuf));
    s2 = g_strdup_printf(_("+%dmin (+%skm detour)"), (d->detour_time + 30) / 60, kmbuf);

    icon = gui_internal_poi_icon(this, &d->item);
    if (!icon)
        icon = image_new_xs(this, "gui_inactive");

    wh = gui_internal_box_new(this, gravity_left_center | orientation_horizontal | flags_fill);
    wi = gui_internal_image_new(this, icon);
    gui_internal_widget_append(wh, wi);
    wv = gui_internal_box_new(this, gravity_left_center | orientation_vertical | flags_fill);
    wl = gui_internal_label_new(this, s1);
    gui_internal_widget_append(wv, wl);
    wl = gui_internal_label_font_new(this, s2, 1);
    gui_internal_widget_append(wv, wl);
    gui_internal_widget_append(wh, wv);
    wh->name = g_strdup(s1);
    wh->speech = g_strdup(s1);
    wh->c.x = d->c.x;
    wh->c.y = d->c.y;
    wh->c.pro = st->pro;
    wh->func = poi_route_cmd_set_waypoint;
    wh->data = d;
    wh->state |= STATE_SENSITIVE;
    g_free(s1);
    g_free(s2);
    return wh;
}

static void poi_route_render_results(struct poi_route_search_state *st) {
    struct gui_priv *this = st->this;
    struct widget *wsort, *wt, *wtable, *row, *wbottom;
    char buffer[128];
    static const int radii[] = {1, 2, 5, 10, 50};
    int first, last, i;

    gui_internal_widget_children_destroy(this, st->wbox);

    wt = gui_internal_box_new(this, gravity_left_center | orientation_horizontal | flags_fill);
    gui_internal_widget_append(st->wbox, wt);
    snprintf(buffer, sizeof(buffer), _("Sort: %s"),
             st->param->sort_mode == poi_route_sort_detour ? _("Lowest Detour") : _("Along Route"));
    wsort = gui_internal_button_new_with_callback(this, buffer, image_new_xs(this, "gui_active"),
                                                  gravity_left_center | orientation_horizontal | flags_fill,
                                                  poi_route_cmd_sort, st);
    gui_internal_widget_append(wt, wsort);

    wtable =
        gui_internal_widget_table_new(this, gravity_left_top | flags_fill | flags_expand | orientation_vertical, 1);
    gui_internal_widget_append(st->wbox, wtable);

    first = st->pagenb * POI_ROUTE_PAGESIZE;
    last = MIN(first + POI_ROUTE_PAGESIZE, st->nitems);
    for (i = first; i < last; i++) {
        row = gui_internal_widget_table_row_new(this, gravity_left | flags_fill | orientation_horizontal);
        gui_internal_widget_append(row, poi_route_result_widget(st, &st->items[i]));
        gui_internal_widget_append(wtable, row);
    }

    wbottom = gui_internal_box_new(this, gravity_left_center | orientation_horizontal | flags_fill);
    gui_internal_widget_append(st->wbox, wbottom);
    if (last < st->nitems) {
        snprintf(buffer, sizeof(buffer), _("Get more (%d of %d)..."), last, st->nitems);
        wt = gui_internal_label_new(this, buffer);
        gui_internal_widget_append(wbottom, wt);
        wt->func = poi_route_cmd_more;
        wt->data = st;
        wt->state |= STATE_SENSITIVE;
    } else {
        gui_internal_widget_append(wbottom, gui_internal_label_new(this, _("Within route:")));
        for (i = 0; i < sizeof(radii) / sizeof(radii[0]); i++) {
            if (radii[i] * 1000 == st->param->perp_radius)
                snprintf(buffer, sizeof(buffer), "[%dkm]", radii[i]);
            else
                snprintf(buffer, sizeof(buffer), " %dkm ", radii[i]);
            wt = gui_internal_label_new(this, buffer);
            gui_internal_widget_append(wbottom, wt);
            wt->func = poi_route_cmd_radius;
            wt->data = st;
            wt->datai = radii[i];
            wt->state |= STATE_SENSITIVE;
        }
    }

    graphics_draw_mode(this->gra, draw_mode_begin);
    gui_internal_menu_render(this);
    graphics_draw_mode(this->gra, draw_mode_end);
}

static void poi_route_display_results(struct poi_route_search_state *st) {
    struct gui_priv *this = st->this;

    if (st->idle_ev) {
        event_remove_idle(st->idle_ev);
        st->idle_ev = NULL;
    }
    if (st->idle_cb) {
        callback_destroy(st->idle_cb);
        st->idle_cb = NULL;
    }
    if (st->mr) {
        map_rect_destroy(st->mr);
        st->mr = NULL;
    }
    if (st->selm) {
        map_selection_destroy(st->selm);
        st->selm = NULL;
    }
    if (st->sel) {
        map_selection_destroy(st->sel);
        st->sel = NULL;
    }
    if (st->h) {
        mapset_close(st->h);
        st->h = NULL;
    }
    this->search_active = 0;

    if (st->searching_label) {
        st->wbox->children = g_list_remove(st->wbox->children, st->searching_label);
        gui_internal_widget_destroy(this, st->searching_label);
        st->searching_label = NULL;
    }
    if (st->cancel_button) {
        struct widget *button_bar = gui_internal_menu_data(this)->button_bar;
        if (button_bar) {
            button_bar->children = g_list_remove(button_bar->children, st->cancel_button);
            gui_internal_widget_destroy(this, st->cancel_button);
        }
        st->cancel_button = NULL;
    }

    poi_route_sort_items(st);
    poi_route_render_results(st);
}

static void poi_route_idle_process(struct poi_route_search_state *st) {
    struct gui_priv *this = st->this;
    struct item *item;
    struct coord c, route_c;
    struct attr attr;
    struct poi_route_item *d;
    char *label;
    int batch = POI_ROUTE_BATCH_SIZE;
    int dist_along, perp_dist, detour_dist, detour_time;

    if (!st->cancel && !g_list_find(this->root.children, st->topbox))
        st->cancel = 1;
    if (st->cancel) {
        poi_route_state_detach(st);
        poi_route_state_destroy(st);
        return;
    }
    while (batch--) {
        while (1) {
            if (st->mr) {
                item = map_rect_get_item(st->mr);
                if (item)
                    break;
                map_rect_destroy(st->mr);
                st->mr = NULL;
            }
            if (st->selm) {
                map_selection_destroy(st->selm);
                st->selm = NULL;
            }
            st->m = mapset_next(st->h, 1);
            if (!st->m)
                goto done;
            st->selm = map_selection_dup_pro(st->sel, st->pro, map_projection(st->m));
            st->mr = map_rect_new(st->m, st->selm);
        }
        if (!gui_internal_pois_item_selected(&st->param->p, item))
            continue;
        if (!item_coord_get_pro(item, &c, 1, st->pro))
            continue;
        if (!coord_rect_contains(&st->sel->u.c_rect, &c))
            continue;
        if (!poi_route_polyline_project(&st->pl, &c, &dist_along, &route_c))
            continue;
        perp_dist = transform_distance(projection_mg, &c, &route_c);
        if (perp_dist > st->param->perp_radius)
            continue;
        item_attr_rewind(item);
        if (item->type == type_house_number)
            label = gui_internal_compose_item_address_string(item, 1);
        else if (item_attr_get(item, attr_label, &attr))
            label = map_convert_string(item->map, attr.u.str);
        else
            label = g_strdup("");

        if (st->nitems >= st->capitems) {
            st->capitems = st->capitems ? st->capitems * 2 : 256;
            st->items = g_renew(struct poi_route_item, st->items, st->capitems);
        }
        d = &st->items[st->nitems++];
        memset(d, 0, sizeof(*d));
        d->label = label;
        d->item = *item;
        d->c = c;
        d->route_c = route_c;
        d->dist_along = dist_along;
        d->perp_dist = perp_dist;
        poi_route_estimate_detour(perp_dist, &detour_dist, &detour_time);
        d->detour_dist = detour_dist;
        d->detour_time = detour_time;
    }
    st->spin_frame++;
    {
        static const short dx[12] = {0, 6, 10, 12, 10, 6, 0, -6, -10, -12, -10, -6};
        static const short dy[12] = {-12, -10, -6, 0, 6, 10, 12, 10, 6, 0, -6, -10};
        struct point center, clear_area, runner;
        int idx;
        center.x = st->searching_label->p.x - 30;
        center.y = st->searching_label->p.y + st->searching_label->h / 2;
        idx = st->spin_frame % 12;
        runner.x = center.x + dx[idx];
        runner.y = center.y + dy[idx];
        clear_area.x = center.x - 14;
        clear_area.y = center.y - 14;
        graphics_draw_mode(this->gra, draw_mode_begin);
        gui_internal_widget_render(this, st->searching_label);
        graphics_draw_rectangle(this->gra, this->background, &clear_area, 29, 29);
        graphics_draw_circle(this->gra, this->foreground, &center, 24);
        runner.x -= 3;
        runner.y -= 3;
        graphics_draw_rectangle(this->gra, this->foreground, &runner, 7, 7);
        graphics_draw_mode(this->gra, draw_mode_end);
    }
    return;

done:
    dbg(lvl_debug, "found %d POIs along route", st->nitems);
    poi_route_display_results(st);
}

static void poi_route_idle_cb(struct poi_route_search_state *st) {
    poi_route_idle_process(st);
}

static void poi_route_cmd_cancel(struct gui_priv *this, struct widget *wm, void *data) {
    struct poi_route_search_state *st = data;
    st->cancel = 1;
}

static void poi_route_cmd_sort(struct gui_priv *this, struct widget *wm, void *data) {
    struct poi_route_search_state *st = data;
    st->param->sort_mode = !st->param->sort_mode;
    st->pagenb = 0;
    poi_route_sort_items(st);
    poi_route_render_results(st);
}

static void poi_route_cmd_more(struct gui_priv *this, struct widget *wm, void *data) {
    struct poi_route_search_state *st = data;
    st->pagenb++;
    poi_route_render_results(st);
}

static void poi_route_cmd_radius(struct gui_priv *this, struct widget *wm, void *data) {
    struct poi_route_search_state *st = data;
    struct poi_route_param *param = poi_route_param_clone(st->param);

    param->perp_radius = wm->datai * 1000;
    param->p.pagenb = 0;
    gui_internal_prune_menu_count(this, 1, 0);
    gui_internal_cmd_pois_along_route(this, NULL, param);
    poi_route_param_free(param);
}

/**
 * @brief Shows a short notice to the user in a minimal menu.
 */
static void poi_route_show_notice(struct gui_priv *this, const char *text) {
    struct widget *wb, *w, *wl;

    gui_internal_prune_menu(this, NULL);
    wb = gui_internal_menu(this, _("POIs along Route"));
    w = gui_internal_box_new(this, gravity_center | orientation_vertical | flags_expand | flags_fill);
    gui_internal_widget_append(wb, w);
    wl = gui_internal_label_new(this, text);
    gui_internal_widget_append(w, wl);
    wl = gui_internal_button_label(this, _("OK"), -1);
    wl->func = gui_internal_back;
    wl->state |= STATE_SENSITIVE;
}

/**
 * @brief Adds the selected POI as a waypoint at its geographic position along the route.
 *
 * Rebuilds the route polyline to obtain the {@code dist_along} key of the POI and of every
 * existing waypoint, snaps the POI onto the routable road network, inserts the POI before
 * the first waypoint that lies further along the route (defaulting to a position right
 * before the final destination), and starts an asynchronous route recalculation.
 */
static void poi_route_cmd_set_waypoint(struct gui_priv *this, struct widget *wm, void *data) {
    struct poi_route_item *d = data;
    struct route *route = navit_get_route(this->nav);
    struct poi_route_polyline pl;
    struct pcoord *dst, snappc;
    struct coord tmp;
    char *desc;
    int dstcount, insert_pos, i, wp_dist, poi_dist;

    if (!route || !poi_route_polyline_build(route, &pl)) {
        gui_internal_prune_menu(this, NULL);
        return;
    }
    if (!poi_route_polyline_project(&pl, &d->c, &poi_dist, NULL)) {
        poi_route_polyline_free(&pl);
        gui_internal_prune_menu(this, NULL);
        return;
    }

    /* Only insert the waypoint if the POI actually snaps onto the routable road network.
       Otherwise routing cannot build a path through it and the whole route would end up
       in the not_found state (leaving no route to follow at all). */
    snappc.x = d->c.x;
    snappc.y = d->c.y;
    snappc.pro = projection_mg;
    if (!route_snap_coord(navit_get_mapset(this->nav), navit_get_vehicleprofile(this->nav), &snappc, &snappc)) {
        poi_route_polyline_free(&pl);
        poi_route_show_notice(this, _("This POI cannot be reached by road and was not added as a waypoint."));
        return;
    }

    dstcount = navit_get_destination_count(this->nav) + 1;
    dst = g_alloca(dstcount * sizeof(struct pcoord));
    dstcount = navit_get_destinations(this->nav, dst, dstcount);

    insert_pos = dstcount - 1;
    if (insert_pos < 0)
        insert_pos = 0;
    for (i = 0; i < dstcount - 1; i++) {
        tmp.x = dst[i].x;
        tmp.y = dst[i].y;
        if (!poi_route_polyline_project(&pl, &tmp, &wp_dist, NULL))
            continue;
        if (wp_dist > poi_dist) {
            insert_pos = i;
            break;
        }
    }

    for (i = dstcount; i > insert_pos; i--)
        dst[i] = dst[i - 1];
    dst[insert_pos].x = snappc.x;
    dst[insert_pos].y = snappc.y;
    dst[insert_pos].pro = projection_mg;

    desc = (d->label && d->label[0]) ? d->label : item_to_name(d->item.type);
    navit_add_destination_description(this->nav, &dst[insert_pos], desc);
    navit_set_destinations(this->nav, dst, dstcount + 1, desc, 1);
    poi_route_polyline_free(&pl);
    gui_internal_prune_menu(this, NULL);
}

static void poi_route_start_search(struct gui_priv *this, struct poi_route_param *param) {
    struct poi_route_search_state *st;
    struct route *route;
    struct attr route_status;
    struct widget *wb, *w;
    struct coord_rect r;
    double margin;
    int i;

    route = navit_get_route(this->nav);
    if (!route || !route_get_attr(route, attr_route_status, &route_status, NULL)
        || (route_status.u.num != route_status_path_done_new
            && route_status.u.num != route_status_path_done_incremental)) {
        dbg(lvl_warning, "no active route, cannot search POIs along route");
        poi_route_param_free(param);
        return;
    }

    st = g_new0(struct poi_route_search_state, 1);
    st->this = this;
    st->param = param;
    st->pro = projection_mg;
    st->pagenb = 0;
    if (!poi_route_polyline_build(route, &st->pl)) {
        dbg(lvl_warning, "failed to build route polyline");
        poi_route_state_destroy(st);
        return;
    }
    if (!poi_route_get_vehicle_pos(this->nav, &st->center))
        st->center = st->pl.c[0];

    wb = gui_internal_menu(this, _("POIs along Route"));
    st->topbox = g_list_last(this->root.children)->data;
    w = gui_internal_box_new(this, gravity_top_center | orientation_vertical | flags_expand | flags_fill);
    gui_internal_widget_append(wb, w);
    st->wbox = w;
    w->data = st;
    w->data_free = poi_route_state_data_free;
    st->searching_label = gui_internal_label_new(this, _("Searching ..."));
    gui_internal_widget_append(w, st->searching_label);
    if (gui_internal_menu_data(this)->button_bar) {
        st->cancel_button = gui_internal_button_label(this, _("Cancel"), 1);
        st->cancel_button->func = poi_route_cmd_cancel;
        st->cancel_button->data = st;
        st->cancel_button->state |= STATE_SENSITIVE;
        gui_internal_widget_append(gui_internal_menu_data(this)->button_bar, st->cancel_button);
    }

    r.lu = st->pl.c[0];
    r.rl = st->pl.c[0];
    for (i = 1; i < st->pl.count; i++)
        coord_rect_extend(&r, &st->pl.c[i]);
    margin = param->perp_radius * transform_scale(MAX(abs(r.lu.y), abs(r.rl.y))) + 1000;
    st->sel = g_new0(struct map_selection, 1);
    st->sel->order = 18;
    st->sel->range = item_range_all;
    st->sel->u.c_rect.lu.x = r.lu.x - margin;
    st->sel->u.c_rect.lu.y = r.lu.y + margin;
    st->sel->u.c_rect.rl.x = r.rl.x + margin;
    st->sel->u.c_rect.rl.y = r.rl.y - margin;

    st->h = mapset_open(navit_get_mapset(this->nav));
    st->m = NULL;
    st->mr = NULL;
    st->selm = NULL;
    st->idle_cb = callback_new_1(callback_cast(poi_route_idle_cb), st);
    st->idle_ev = event_add_idle(100, st->idle_cb);
    this->search_active = 1;

    graphics_draw_mode(this->gra, draw_mode_begin);
    gui_internal_menu_render(this);
    graphics_draw_mode(this->gra, draw_mode_end);
}

/**
 * @brief Event handler for the text filter dialog of the route POI search.
 */
static void poi_route_filter_do(struct gui_priv *this, struct widget *wm, void *data) {
    struct widget *w = data;
    struct poi_route_param *param;

    if (!w->text)
        return;
    param = g_new0(struct poi_route_param, 1);
    param->perp_radius = POI_ROUTE_DEFAULT_RADIUS;
    gui_internal_poi_param_set_filter(&param->p, w->text);
    gui_internal_cmd_pois_along_route(this, wm, param);
    poi_route_param_free(param);
}

static void poi_route_filter_changed(struct gui_priv *this, struct widget *wm, void *data) {
    if (wm->text && wm->reason == gui_internal_reason_keypress_finish)
        poi_route_filter_do(this, wm, wm);
}

/**
 * @brief Text filter dialog for the route POI search, same pattern as plain POI search.
 */
static void poi_route_cmd_filter(struct gui_priv *this, struct widget *wm, void *data) {
    struct widget *wb, *w, *wr, *wk, *we;
    int keyboard_mode;

    keyboard_mode = VKBD_FLAG_2 | gui_internal_keyboard_init_mode(getenv("LANG"));
    wb = gui_internal_menu(this, _("Filter"));
    w = gui_internal_box_new(this, gravity_center | orientation_vertical | flags_expand | flags_fill);
    gui_internal_widget_append(wb, w);
    wr = gui_internal_box_new(this, gravity_top_center | orientation_vertical | flags_expand | flags_fill);
    gui_internal_widget_append(w, wr);
    we = gui_internal_box_new(this, gravity_left_center | orientation_horizontal | flags_fill);
    gui_internal_widget_append(wr, we);
    gui_internal_widget_append(we, wk = gui_internal_label_new(this, NULL));
    wk->state |= STATE_EDIT | STATE_EDITABLE;
    wk->func = poi_route_filter_changed;
    wk->background = this->background;
    wk->flags |= flags_expand | flags_fill;
    wk->name = g_strdup("POIsFilter");
    gui_internal_widget_append(we, wb = gui_internal_image_new(this, image_new_xs(this, "gui_active")));
    wb->state |= STATE_SENSITIVE;
    wb->func = poi_route_filter_do;
    wb->name = g_strdup("NameFilter");
    wb->data = wk;
    gui_internal_widget_append(we, wb = gui_internal_image_new(this, image_new_xs(this, "post")));
    wb->state |= STATE_SENSITIVE;
    wb->name = g_strdup("AddressFilter");
    wb->func = poi_route_filter_do;
    wb->data = wk;
    gui_internal_widget_append(we, wb = gui_internal_image_new(this, image_new_xs(this, "zipcode")));
    wb->state |= STATE_SENSITIVE;
    wb->name = g_strdup("AddressFilterZip");
    wb->func = poi_route_filter_do;
    wb->data = wk;

    if (this->keyboard)
        gui_internal_widget_append(w, gui_internal_keyboard(this, keyboard_mode));
    else
        gui_internal_keyboard_show_native(this, w, keyboard_mode, getenv("LANG"));
    gui_internal_menu_render(this);
}

/**
 * @brief Shows the category selector grid for the route POI search.
 */
static void poi_route_show_selector(struct gui_priv *this) {
    struct widget *wb, *w, *wl;
    int nitems, nrows, i;

    wb = gui_internal_menu(this, _("POIs along Route"));
    w = gui_internal_box_new(this, gravity_left_center | orientation_horizontal_vertical | flags_fill);
    w->background = this->background;
    w->w = this->root.w;
    w->cols = this->root.w / this->icon_s;
    nitems = selector_count;
    nrows = nitems / w->cols + (nitems % w->cols > 0);
    w->h = this->icon_l * nrows;
    for (i = 0; i < nitems; i++) {
        struct poi_route_param *p = g_new0(struct poi_route_param, 1);
        p->p.sel = 1;
        p->p.selnb = i;
        p->perp_radius = POI_ROUTE_DEFAULT_RADIUS;
        gui_internal_widget_append(
            w, wl = gui_internal_button_new_with_callback(this, NULL, image_new_s(this, selectors[i].icon),
                                                          gravity_left_center | orientation_vertical,
                                                          gui_internal_cmd_pois_along_route, p));
        wl->data_free = poi_route_param_free;
    }
    gui_internal_widget_append(w, wl = gui_internal_button_new_with_callback(
                                      this, NULL, image_new_s(this, "gui_search"),
                                      gravity_left_center | orientation_vertical, poi_route_cmd_filter, NULL));

    gui_internal_widget_append(wb, w);
    gui_internal_widget_pack(this, w);
    graphics_draw_mode(this->gra, draw_mode_begin);
    gui_internal_menu_render(this);
    graphics_draw_mode(this->gra, draw_mode_end);
}

void gui_internal_cmd_pois_along_route(struct gui_priv *this, struct widget *wm, void *data) {
    struct poi_route_param *param;

    if (this->search_active)
        return;
    if (!data) {
        poi_route_show_selector(this);
        return;
    }
    param = poi_route_param_clone(data);
    poi_route_start_search(this, param);
}
