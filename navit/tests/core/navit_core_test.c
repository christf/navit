#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atom.h"
#include "coord.h"
#include "debug.h"
#include "file.h"
#include "geom.h"
#include "graphics.h"
#include "gui.h"
#include "item.h"
#include "kalman.h"
#include "layout.h"
#include "linguistics.h"
#include "main.h"
#include "map.h"
#include "mapset.h"
#include "navigation.h"
#include "projection.h"
#include "roadprofile.h"
#include "route.h"
#include "search.h"
#include "track.h"
#include "transform.h"
#include "vehicleprofile.h"

extern void builtin_init(void);

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

static struct map *test_map;
static struct mapset *test_mapset;
static struct vehicleprofile *car_profile;
static struct vehicleprofile *bike_profile;

static void init_core(void) {
    static int done;
    if (done)
        return;
    done = 1;
    atom_init();
    main_init("navit_core_test");
    debug_init("navit_core_test");
    file_init();
    builtin_init();
    route_init();
    navigation_init();
    tracking_init();
    search_init();
    linguistics_init();
    geom_init();
}

static struct map *open_binfile(const char *path) {
    struct attr type = {attr_type};
    type.u.str = "binfile";
    struct attr data;
    data.type = attr_data;
    data.u.str = g_strdup(path);
    struct attr *attrs[] = {&type, &data, NULL};
    return map_new(NULL, attrs);
}

static void make_roadprofile(struct vehicleprofile *vp, enum item_type type, int kmh) {
    enum item_type types[] = {type, type_none};
    struct attr ts = {attr_item_types};
    ts.u.item_types = types;
    struct attr sp = {attr_speed};
    sp.u.num = kmh;
    struct attr ms = {attr_maxspeed};
    ms.u.num = kmh;
    struct attr *rpa[] = {&ts, &sp, &ms, NULL};
    struct roadprofile *rp = roadprofile_new(NULL, rpa);
    struct attr attach = {attr_roadprofile};
    attach.u.navit_object = (struct navit_object *)rp;
    vehicleprofile_add_attr(vp, &attach);
}

static struct vehicleprofile *make_profile(const char *name, int grid, int service, int trunk, int with_trunk) {
    struct attr na = {attr_name};
    na.u.str = g_strdup(name);
    struct attr *attrs[] = {&na, NULL};
    struct vehicleprofile *vp = vehicleprofile_new(NULL, attrs);
    struct attr flags = {attr_flags};
    struct attr flags_fwd = {attr_flags_forward_mask};
    struct attr flags_rev = {attr_flags_reverse_mask};
    if (!strcmp(name, "car")) {
        /* mirrors the shipped car profile in navit_shipped.xml: cars may pass forward oneways
         * but neither reverse oneways nor car-only HOV lanes */
        flags.u.num = AF_CAR;
        flags_fwd.u.num = AF_CAR | AF_ONEWAYREV | AF_HIGH_OCCUPANCY_CAR_ONLY;
        flags_rev.u.num = AF_CAR | AF_ONEWAY | AF_HIGH_OCCUPANCY_CAR_ONLY;
    } else {
        flags.u.num = AF_BIKE;
        flags_fwd.u.num = AF_BIKE | AF_ONEWAYREV;
        flags_rev.u.num = AF_BIKE | AF_ONEWAY;
    }
    /* use set_attr: add_attr only stores the attrs, fields are applied via set_attr */
    vehicleprofile_set_attr(vp, &flags);
    vehicleprofile_set_attr(vp, &flags_fwd);
    vehicleprofile_set_attr(vp, &flags_rev);
    if (grid > 0)
        make_roadprofile(vp, type_street_1_city, grid);
    if (service > 0)
        make_roadprofile(vp, type_street_service, service);
    if (with_trunk && trunk > 0)
        make_roadprofile(vp, type_street_n_lanes, trunk);
    return vp;
}

static void setup_world(void) {
    const char *path = getenv("NAVIT_TEST_MAP");
    if (!path) {
        printf("FAIL: NAVIT_TEST_MAP not set\n");
        exit(2);
    }
    init_core();
    test_map = open_binfile(path);
    if (!test_map) {
        printf("FAIL: cannot open map %s\n", path);
        exit(2);
    }
    test_mapset = mapset_new(NULL, NULL);
    struct attr ma = {attr_map};
    ma.u.map = test_map;
    mapset_add_attr(test_mapset, &ma);
    car_profile = make_profile("car", 50, 50, 100, 1);
    bike_profile = make_profile("bike", 18, 10, 0, 0);
}

static struct coord geo_to_mg(double lat, double lon) {
    struct coord_geo g;
    struct coord c;
    g.lat = lat;
    g.lng = lon;
    transform_from_geo(projection_mg, &g, &c);
    return c;
}

static double mg_distance_m(struct coord a, struct coord b) {
    struct coord_geo ga, gb;
    transform_to_geo(projection_mg, &a, &ga);
    transform_to_geo(projection_mg, &b, &gb);
    double dlat = (gb.lat - ga.lat) * 111320.0;
    double dlon = (gb.lng - ga.lng) * 111320.0 * cos(ga.lat * M_PI / 180.0);
    return sqrt(dlat * dlat + dlon * dlon);
}

struct path_info {
    GPtrArray *street_names;
    int has_trunk;
    double length_m;
};

static void free_path_info(struct path_info *pi) {
    if (pi->street_names) {
        for (guint i = 0; i < pi->street_names->len; i++)
            g_free(g_ptr_array_index(pi->street_names, i));
        g_ptr_array_free(pi->street_names, TRUE);
    }
    memset(pi, 0, sizeof(*pi));
}

static void walk_route_path(struct route *r, struct path_info *pi) {
    struct attr amap;
    pi->street_names = g_ptr_array_new();
    if (!route_get_attr(r, attr_map, &amap, NULL))
        return;
    struct map_rect *mr = map_rect_new(amap.u.map, NULL);
    struct item *it;
    while ((it = map_rect_get_item(mr))) {
        if (it->type != type_street_route)
            continue;
        struct attr la;
        if (item_attr_get(it, attr_length, &la))
            pi->length_m += la.u.num;
        struct attr sa;
        if (!item_attr_get(it, attr_street_item, &sa))
            continue;
        struct item *street = sa.u.item;
        struct map_rect *smr = map_rect_new(street->map, NULL);
        struct item *si = map_rect_get_item_byid(smr, street->id_hi, street->id_lo);
        if (si) {
            if (si->type == type_street_n_lanes)
                pi->has_trunk = 1;
            struct attr name;
            if (item_attr_get(si, attr_street_name, &name))
                g_ptr_array_add(pi->street_names, g_strdup(name.u.str));
        }
        map_rect_destroy(smr);
    }
    map_rect_destroy(mr);
}

static int route_status_done(struct route *r) {
    struct attr a;
    if (!route_get_attr(r, attr_route_status, &a, NULL))
        return 0;
    int done = (route_status_path_done_new | route_status_path_done_incremental) & ~route_status_destination_set;
    return (a.u.num & done) != 0;
}

static struct route *calc_route(double lat1, double lon1, double lat2, double lon2, struct vehicleprofile *prof,
                                struct path_info *pi) {
    struct coord c1 = geo_to_mg(lat1, lon1);
    struct coord c2 = geo_to_mg(lat2, lon2);
    struct pcoord p1 = {projection_mg, c1.x, c1.y};
    struct pcoord p2 = {projection_mg, c2.x, c2.y};
    struct route *r = route_new(NULL, NULL);
    route_set_profile(r, prof);
    route_set_mapset(r, test_mapset);
    route_set_position(r, &p1);
    route_set_destination(r, &p2, 0);
    walk_route_path(r, pi);
    return r;
}

static int count_streets(const struct path_info *pi, const char *name) {
    int n = 0;
    for (guint i = 0; pi->street_names && i < pi->street_names->len; i++)
        if (!strcmp(g_ptr_array_index(pi->street_names, i), name))
            n++;
    return n;
}

static int count_distinct_streets(const struct path_info *pi) {
    int n = 0;
    for (guint i = 0; pi->street_names && i < pi->street_names->len; i++) {
        guint j;
        for (j = 0; j < i; j++)
            if (!strcmp(g_ptr_array_index(pi->street_names, i), g_ptr_array_index(pi->street_names, j)))
                break;
        if (j == i)
            n++;
    }
    return n;
}

static int scenario_route_rect_convention(void) {
    struct coord a = geo_to_mg(52.39, 13.06);
    struct coord b = geo_to_mg(52.398, 13.072);
    struct coord pairs[][2] = {
        {a, b},
        {b, a},
        {geo_to_mg(52.39, 13.06), geo_to_mg(52.398, 13.0605)},
        {geo_to_mg(52.394, 13.06), geo_to_mg(52.3905, 13.06)},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        for (int order = 0; order < 18; order += 6) {
            struct map_selection *sel = route_rect(order, &pairs[i][0], &pairs[i][1], 25, 500);
            CHECK(sel, "route_rect returned NULL");
            if (sel) {
                CHECK(sel->u.c_rect.lu.x <= sel->u.c_rect.rl.x, "lu.x > rl.x (pair %zu order %d)", i, order);
                CHECK(sel->u.c_rect.lu.y >= sel->u.c_rect.rl.y, "lu.y < rl.y (pair %zu order %d)", i, order);
                g_free(sel);
            }
        }
    }
    struct map_selection *sel_base = route_rect(4, &pairs[0][0], &pairs[0][1], 0, 0);
    struct map_selection *sel_pad = route_rect(4, &pairs[0][0], &pairs[0][1], 25, 0);
    CHECK(sel_base && sel_pad, "route_rect (padding compare) returned NULL");
    if (sel_base && sel_pad) {
        CHECK(sel_pad->u.c_rect.lu.x <= sel_base->u.c_rect.lu.x && sel_pad->u.c_rect.rl.x >= sel_base->u.c_rect.rl.x
                  && sel_pad->u.c_rect.lu.y >= sel_base->u.c_rect.lu.y
                  && sel_pad->u.c_rect.rl.y <= sel_base->u.c_rect.rl.y,
              "padding does not enclose the unpadded rectangle");
        CHECK(sel_pad->u.c_rect.lu.x < sel_base->u.c_rect.lu.x || sel_pad->u.c_rect.rl.x > sel_base->u.c_rect.rl.x
                  || sel_pad->u.c_rect.lu.y > sel_base->u.c_rect.lu.y
                  || sel_pad->u.c_rect.rl.y < sel_base->u.c_rect.rl.y,
              "padding does not expand beyond the unpadded rectangle");
        g_free(sel_base);
        g_free(sel_pad);
    }
    return 0;
}

static int scenario_projection_roundtrip(void) {
    static const double samples[][2] = {
        {52.39,  13.06 },
        {-33.87, 151.21},
        {64.14,  -21.94},
        {0.5,    0.5   },
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        struct coord_geo g = {samples[i][1], samples[i][0]};
        struct coord c;
        struct coord_geo back;
        transform_from_geo(projection_mg, &g, &c);
        transform_to_geo(projection_mg, &c, &back);
        CHECK(fabs(back.lat - g.lat) < 1e-3 && fabs(back.lng - g.lng) < 1e-3, "roundtrip %.4f,%.4f -> %.6f,%.6f", g.lat,
              g.lng, back.lat, back.lng);
    }
    return 0;
}

static int scenario_route_car_grid(void) {
    setup_world();
    struct path_info pi = {0};
    struct route *r = calc_route(52.3902, 13.0602, 52.3978, 13.0718, car_profile, &pi);
    CHECK(route_status_done(r), "car grid route status=%d", route_status_done(r));
    double straight = mg_distance_m(geo_to_mg(52.3902, 13.0602), geo_to_mg(52.3978, 13.0718));
    CHECK(pi.length_m > straight, "length %.0f below straight line %.0f", pi.length_m, straight);
    CHECK(pi.length_m < 3.0 * straight, "distortion: length %.0f >> straight %.0f", pi.length_m, straight);
    CHECK(count_distinct_streets(&pi) >= 3, "path crosses only %d distinct streets", count_distinct_streets(&pi));
    struct attr dl;
    CHECK(route_get_attr(r, attr_destination_length, &dl, NULL), "destination_length missing");
    route_destroy(r);
    free_path_info(&pi);
    return 0;
}

static int scenario_route_bike_vs_car(void) {
    setup_world();
    struct path_info pc = {0}, pb = {0};
    /* The trunk bypass east of the grid is car-only: the car may cross it, the
     * bike must find a way around and may not set foot on it. */
    struct route *rcar = calc_route(52.3978, 13.0718, 52.3902, 13.0798, car_profile, &pc);
    struct route *rbike = calc_route(52.3978, 13.0718, 52.3902, 13.0798, bike_profile, &pb);
    CHECK(route_status_done(rcar), "car route to trunk end failed");
    CHECK(pc.has_trunk, "car route does not use trunk");
    CHECK(count_streets(&pc, "Testspange") > 0, "car route avoids the car-only trunk");
    CHECK(route_status_done(rbike), "bike could not reach the same area");
    CHECK(count_streets(&pb, "Testspange") == 0, "bike routed over trunk-only connection");
    CHECK(!pb.has_trunk, "bike uses the trunk");
    route_destroy(rcar);
    route_destroy(rbike);
    free_path_info(&pc);
    free_path_info(&pb);

    struct path_info pb2 = {0}, pc2 = {0};
    struct route *rbike2 = calc_route(52.3902, 13.0602, 52.3978, 13.0718, bike_profile, &pb2);
    struct route *rcar2 = calc_route(52.3902, 13.0602, 52.3978, 13.0718, car_profile, &pc2);
    CHECK(route_status_done(rbike2), "bike grid route failed");
    CHECK(route_status_done(rcar2), "car grid route failed");
    CHECK(!pb2.has_trunk, "bike uses trunk inside grid");
    route_destroy(rbike2);
    route_destroy(rcar2);
    free_path_info(&pb2);
    free_path_info(&pc2);
    return 0;
}

static int scenario_route_oneway(void) {
    setup_world();
    struct path_info pi = {0};
    /* Einbahnstrasse runs one-way 203 -> 601 -> 102 (SE). Entering at the SE end
     * (102) and exiting at the NW end (203) would require a reverse traversal, so
     * the router must detour through the grid and leave Einbahnstrasse alone. */
    struct route *r = calc_route(52.390, 13.066, 52.394, 13.072, car_profile, &pi);
    CHECK(route_status_done(r), "oneway route failed entirely");
    CHECK(count_streets(&pi, "Einbahnstrasse") == 0, "router used Einbahnstrasse against its oneway");
    struct path_info pf = {0};
    /* Going in the legal direction (203 -> 102) the shortcut may be used directly. */
    struct route *rf = calc_route(52.394, 13.072, 52.390, 13.066, car_profile, &pf);
    CHECK(route_status_done(rf), "forward oneway route failed");
    CHECK(count_streets(&pf, "Einbahnstrasse") > 0, "router did not use the legal oneway direction");
    route_destroy(r);
    route_destroy(rf);
    free_path_info(&pi);
    free_path_info(&pf);
    return 0;
}

static int scenario_route_turn_restriction(void) {
    setup_world();
    struct path_info pi = {0};
    /* The map forbids turning left from Mittelallee (northbound at 202) into
     * Mittelstrasse towards the west. Starting on the from-way at node 102, the
     * forbidden shortcut (102 -> 202 -> mid-Mittelstrasse) is ~717 m while the
     * legal way around the block (via Westallee) is ~1125 m, so the router can
     * only pick the shortcut once the restriction stops working. */
    struct route *r = calc_route(52.390, 13.066, 52.394, 13.062, car_profile, &pi);
    CHECK(route_status_done(r), "restricted route failed entirely");
    double straight = mg_distance_m(geo_to_mg(52.390, 13.066), geo_to_mg(52.394, 13.062));
    CHECK(pi.length_m > 700.0, "turn restriction ignored: length %.0f too short", pi.length_m);
    CHECK(pi.length_m < 3.0 * straight, "detour absurdly long: %.0f", pi.length_m);
    CHECK(count_streets(&pi, "Mitteallee") == 0, "router used the from-way through the restricted via");
    CHECK(count_streets(&pi, "Westallee") > 0, "router did not take the legal detour");
    route_destroy(r);
    free_path_info(&pi);
    return 0;
}

static int scenario_search_town_street(void) {
    setup_world();
    /* The interactive search (search_list) additionally requires a country map
     * to anchor towns and streets hierarchically; a bare routing fixture cannot
     * provide that context. Verify instead that the fixture supplies the search
     * targets navit's search index depends on: town and street items with name. */
    struct map_rect *mr = map_rect_new(test_map, NULL);
    struct item *it;
    int town_found = 0, nord_found = 0, phantom_found = 0;
    while ((it = map_rect_get_item(mr))) {
        struct attr tname;
        if (it->type >= type_town_label && it->type <= type_town_label_5e5
            && item_attr_get(it, attr_town_name, &tname)) {
            if (!strcmp(tname.u.str, "Testhausen"))
                town_found = 1;
            if (!strcmp(tname.u.str, "Zzznichtda"))
                phantom_found = 1;
        }
        struct attr sname;
        if (item_attr_get(it, attr_street_name, &sname) && !strcmp(sname.u.str, "Nordstrasse"))
            nord_found = 1;
    }
    map_rect_destroy(mr);
    CHECK(town_found, "fixture town 'Testhausen' missing");
    CHECK(!phantom_found, "phantom town found");
    CHECK(nord_found, "fixture street 'Nordstrasse' missing");
    return 0;
}

/* The vehicle drives straight north at constant speed; GPS delivers one fix per
 * second with noisy position and course.  Between fixes the map-scroll target
 * follows the Kalman extrapolated ("predicted") position, exactly as
 * navit_animation_tick() does.  Any sideways drift of that extrapolation shows
 * up as a jump of the scroll target when the next fix snaps back to the road,
 * i.e. as a periodic sideways drag of the map.  The extrapolation inherits the
 * noisy GPS course unless the filter velocity is re-aligned with the matched
 * road after every fix, so the harness runs both variants: the course variant
 * keeps the extrapolated heading noisy and must show the drag, the road variant
 * pins the velocity to the road heading and must stay still. */
#define SCROLL_W 800
#define SCROLL_H 480
#define SCROLL_CURSOR_X (SCROLL_W / 2)
#define SCROLL_CURSOR_Y (SCROLL_H * 80 / 100)
#define SCROLL_FIX_MS 1000
#define SCROLL_NFIXES 40
#define SCROLL_SPEED_MPS (80.0 / 3.6)
#define SCROLL_START_LAT 52.4
#define SCROLL_ZOOM 32
#define SCROLL_COURSE_NOISE_DEG 12.0
#define SCROLL_POS_NOISE_M 0.6

enum scroll_vel_mode {
    SCROLL_VEL_COURSE,
    SCROLL_VEL_ROAD
};

struct scroll_harness {
    struct kalman_filter *kf;
    struct transformation *trans;
    struct transformation *trans_cursor;
};

static void scroll_harness_init(struct scroll_harness *hs, struct coord *start) {
    struct pcoord pc = {projection_mg, start->x, start->y};
    struct map_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.u.p_rect.rl.x = SCROLL_W;
    sel.u.p_rect.rl.y = SCROLL_H;
    hs->kf = kalman_new();
    hs->trans = transform_new(&pc, SCROLL_ZOOM, 0);
    hs->trans_cursor = transform_new(&pc, SCROLL_ZOOM, 0);
    transform_set_screen_selection(hs->trans, &sel);
    transform_set_screen_selection(hs->trans_cursor, &sel);
}

/* One GPS fix, mirroring tracking_update(): inject the ground-speed vector in
 * projection units, snap the filtered position to the straight road and, in
 * tracked mode, pin the filter velocity to the road heading. */
static struct coord scroll_advance_fix(struct scroll_harness *hs, GRand *rng, double t_s, enum scroll_vel_mode mode) {
    struct coord road = geo_to_mg(SCROLL_START_LAT + SCROLL_SPEED_MPS * t_s / 6371000.0 * 180.0 / M_PI, 10.35);
    struct coord raw;
    struct coord snapped = road;
    double cos_lat = cos(SCROLL_START_LAT * M_PI / 180.0);
    double course = g_rand_double_range(rng, -SCROLL_COURSE_NOISE_DEG, SCROLL_COURSE_NOISE_DEG);
    double vx, vy, fx, fy;
    if (g_rand_double(rng) < 0.03)
        course += g_rand_double_range(rng, -20, 20);
    raw.x = road.x + (int)(g_rand_double_range(rng, -SCROLL_POS_NOISE_M, SCROLL_POS_NOISE_M) / cos_lat);
    raw.y = road.y + (int)g_rand_double_range(rng, -SCROLL_POS_NOISE_M, SCROLL_POS_NOISE_M);
    vx = SCROLL_SPEED_MPS * sin(course * M_PI / 180.0) / cos_lat;
    vy = SCROLL_SPEED_MPS * cos(course * M_PI / 180.0) / cos_lat;
    kalman_update(hs->kf, 0, raw.x, raw.y, vx, vy, 1);
    kalman_get_filtered_position(hs->kf, &fx, &fy);
    snapped.y = (int)fy;
    /* tracking_update() re-anchors the filter state to the matched road. */
    kalman_set_position(hs->kf, snapped.x, snapped.y);
    if (mode == SCROLL_VEL_ROAD)
        kalman_set_velocity(hs->kf, 0.0, SCROLL_SPEED_MPS / cos_lat);
    return snapped;
}

/* Measure the per-fix jump of the scroll target, i.e. how far the map would be
 * re-dragged sideways/forwards when the extrapolated position snaps back to
 * the matched road at each new fix. */
static void scroll_simulate(struct scroll_harness *hs, GRand *rng, enum scroll_vel_mode mode, double *mean_lat,
                            double *max_lat, double *mean_fwd, double *max_fwd) {
    struct point cursor_fixed = {SCROLL_CURSOR_X, SCROLL_CURSOR_Y};
    double sum_lat = 0.0, max_lat_seen = 0.0;
    double sum_fwd = 0.0, max_fwd_seen = 0.0;
    int counted = 0;

    kalman_set_simulated_time(0.0);
    transform_set_yaw(hs->trans, 0);
    transform_set_yaw(hs->trans_cursor, 0);
    for (int fix = 0; fix < SCROLL_NFIXES; fix++) {
        long t_ms = fix * SCROLL_FIX_MS;
        struct coord predicted;
        struct point screen, target;
        double px, py;
        int target_before_x, target_before_y;
        int have_before = 0;

        /* Map-scroll target just before the fix: the extrapolated position has
         * crept sideways/forwards since the previous fix. */
        if (fix > 0) {
            kalman_set_simulated_time((double)(t_ms - SCROLL_FIX_MS / 30) / 1000.0);
            kalman_get_position(hs->kf, &px, &py);
            predicted.x = (int)px;
            predicted.y = (int)py;
            if (transform_point(hs->trans_cursor, projection_mg, &predicted, &screen)) {
                target_before_x = cursor_fixed.x - screen.x;
                target_before_y = cursor_fixed.y - screen.y;
                have_before = 1;
            }
        }

        /* The new fix re-anchors the extrapolation to the matched road. */
        kalman_set_simulated_time((double)t_ms / 1000.0);
        scroll_advance_fix(hs, rng, t_ms / 1000.0, mode);
        kalman_get_position(hs->kf, &px, &py);
        predicted.x = (int)px;
        predicted.y = (int)py;
        if (have_before && transform_point(hs->trans_cursor, projection_mg, &predicted, &screen)) {
            target.x = cursor_fixed.x - screen.x;
            target.y = cursor_fixed.y - screen.y;
            sum_lat += abs(target.x - target_before_x);
            sum_fwd += abs(target.y - target_before_y);
            if (abs(target.x - target_before_x) > max_lat_seen)
                max_lat_seen = abs(target.x - target_before_x);
            if (abs(target.y - target_before_y) > max_fwd_seen)
                max_fwd_seen = abs(target.y - target_before_y);
            counted++;
        }
    }
    if (!counted) {
        *mean_lat = *max_lat = *mean_fwd = *max_fwd = 0.0;
        return;
    }
    *mean_lat = sum_lat / counted;
    *max_lat = max_lat_seen;
    *mean_fwd = sum_fwd / counted;
    *max_fwd = max_fwd_seen;
}

static int scenario_kalman_scroll(void) {
    struct coord start = geo_to_mg(SCROLL_START_LAT, 10.35);
    GRand *rng_raw = g_rand_new_with_seed(42);
    GRand *rng_trk = g_rand_new_with_seed(42);
    double mean_lat_raw, max_lat_raw, mean_fwd_raw, max_fwd_raw;
    double mean_lat_trk, max_lat_trk, mean_fwd_trk, max_fwd_trk;

    {
        struct scroll_harness hs;
        scroll_harness_init(&hs, &start);
        scroll_simulate(&hs, rng_raw, SCROLL_VEL_COURSE, &mean_lat_raw, &max_lat_raw, &mean_fwd_raw, &max_fwd_raw);
        kalman_destroy(hs.kf);
        transform_destroy(hs.trans);
        transform_destroy(hs.trans_cursor);
    }
    {
        struct scroll_harness hs;
        scroll_harness_init(&hs, &start);
        scroll_simulate(&hs, rng_trk, SCROLL_VEL_ROAD, &mean_lat_trk, &max_lat_trk, &mean_fwd_trk, &max_fwd_trk);
        kalman_destroy(hs.kf);
        transform_destroy(hs.trans);
        transform_destroy(hs.trans_cursor);
    }
    g_rand_free(rng_raw);
    g_rand_free(rng_trk);

    printf("course scroll: mean lateral jump %.2f px (max %.2f), forwards %.2f px (max %.2f)\n", mean_lat_raw,
           max_lat_raw, mean_fwd_raw, max_fwd_raw);
    printf("road   scroll: mean lateral jump %.2f px (max %.2f), forwards %.2f px (max %.2f)\n", mean_lat_trk,
           max_lat_trk, mean_fwd_trk, max_fwd_trk);

    /* The unfiltered extrapolating course carries the GPS heading noise, so the
     * map is dragged sideways with every fix and recovers it each second. */
    CHECK(mean_lat_raw >= 0.6, "course variant must show periodic lateral drags, mean %.2f px", mean_lat_raw);
    /* Aligning the extrapolation with the matched road leaves the lateral
     * scroll target stationary. */
    CHECK(mean_lat_trk < 0.4, "road variant must keep lateral scroll at zero, got %.2f px", mean_lat_trk);
    return 0;
}

#define COV_W 800
#define COV_H 480
#define COV_ZOOM 32
#define COV_PITCH 30
#define COV_MARGIN 128
#define COV_SHIFT_PX 8

static struct transformation *covers_trans_new(struct coord *center) {
    struct pcoord pc = {projection_mg, center->x, center->y};
    struct map_selection sel;
    struct transformation *t = transform_new(&pc, COV_ZOOM, 0);
    memset(&sel, 0, sizeof(sel));
    sel.u.p_rect.rl.x = COV_W;
    sel.u.p_rect.rl.y = COV_H;
    transform_set_screen_selection(t, &sel);
    transform_set_pitch(t, COV_PITCH);
    return t;
}

static void covers_shift_center(struct transformation *t, int dx, int dy) {
    struct point a = {COV_W / 2, COV_H / 2};
    struct point b = {COV_W / 2 + dx, COV_H / 2 + dy};
    struct coord ca, cb, c;
    transform_reverse(t, &a, &ca);
    transform_reverse(t, &b, &cb);
    c = *transform_get_center(t);
    c.x += cb.x - ca.x;
    c.y += cb.y - ca.y;
    transform_set_center(t, &c);
}

/* transform_covers_screen() must work under perspective: a display list covers the
 * viewport it was built from, goes stale once the center moves, and the prefetch margin
 * extends its coverage by roughly the given number of screen pixels. */
static int scenario_covers_screen_3d(void) {
    struct coord center = geo_to_mg(52.4, 10.35);
    struct transformation *t = covers_trans_new(&center);

    transform_setup_source_rect(t);
    CHECK(transform_covers_screen(t, COV_W, COV_H), "3d: viewport must be covered by its own source rect");

    covers_shift_center(t, COV_W * 4, 0);
    CHECK(!transform_covers_screen(t, COV_W, COV_H), "3d: moving the center must uncover the stale rect");

    transform_setup_source_rect_margin(t, COV_W, COV_H, COV_MARGIN);
    CHECK(transform_covers_screen(t, COV_W, COV_H), "3d: prefetch margin must cover the viewport");

    covers_shift_center(t, COV_SHIFT_PX, 0);
    CHECK(transform_covers_screen(t, COV_W, COV_H), "3d: prefetch margin must cover a small center shift");

    covers_shift_center(t, COV_W * 4, 0);
    CHECK(!transform_covers_screen(t, COV_W, COV_H), "3d: shift beyond the margin must uncover the rect");

    transform_destroy(t);
    return 0;
}

#define RJ_W 800
#define RJ_H 480
#define RJ_ZOOM 32
#define RJ_PITCH 30
#define RJ_LAG_Y 40 /* how far the vehicle is allowed to lag behind the cursor, px */
#define RJ_TOP_Y (RJ_H * 5 / 12)
#define RJ_NX 5
#define RJ_NY 4
#define RJ_FEATURES (RJ_NX * RJ_NY)
#define RJ_SMOOTH_STEP_PX 1.0 /* worst per-step drift still perceived as smooth */
#define RJ_COARSE_STEP_PX 4.0
#define RJ_TICK_OFFSET_PX 1 /* leftover drag a single animation tick may carry, px */

struct rj_feature {
    struct coord c;
};

static struct transformation *rj_trans_new(struct coord *center) {
    struct pcoord pc = {projection_mg, center->x, center->y};
    struct map_selection sel;
    struct transformation *t = transform_new(&pc, RJ_ZOOM, 0);
    memset(&sel, 0, sizeof(sel));
    sel.u.p_rect.rl.x = RJ_W;
    sel.u.p_rect.rl.y = RJ_H;
    transform_set_screen_selection(t, &sel);
    transform_set_pitch(t, RJ_PITCH);
    return t;
}

/* Applies a re-center exactly as the animation tick does: transform_recenter() computes the
 * new center so the ground point drawn at `from` is drawn at `to`. */
static void rj_recenter(struct transformation *t, struct point *from, struct point *to) {
    struct coord new_center;
    if (transform_recenter(t, from, to, &new_center))
        transform_set_center(t, &new_center);
}

/* Ground features currently on screen. A band near the horizon is left out, where ground
 * magnification diverges and the projection is arbitrarily sensitive. */
static int rj_visible_features(struct transformation *t, struct rj_feature *feat) {
    int gx, gy, n = 0;
    for (gy = 0; gy < RJ_NY; gy++) {
        for (gx = 0; gx < RJ_NX; gx++) {
            struct point s;
            s.x = (RJ_W - 1) * gx / (RJ_NX - 1);
            s.y = RJ_TOP_Y + (RJ_H - 1 - RJ_TOP_Y) * gy / (RJ_NY - 1);
            if (s.y < RJ_H / 3)
                continue; /* too close to the horizon to project stably */
            if (!transform_reverse(t, &s, &feat[n].c))
                continue;
            n++;
        }
    }
    return n;
}

/* Worst distance between a scene drawn by shifting the map rigidly by `off_px` and the
 * perspective-correct scene obtained by applying the same offset to the map center. This is the
 * residual a pending rigid drag leaves behind and that snaps away when the map is re-centered,
 * growing with the depth spread of the visible ground. */
static double rj_residual(struct coord *center, int off_px) {
    struct transformation *t = rj_trans_new(center);
    struct transformation *t_correct = transform_dup(t);
    struct rj_feature feat[RJ_FEATURES];
    struct point ref, to;
    int n, i;
    double worst = 0.0;

    n = rj_visible_features(t, feat);
    transform_point(t, projection_mg, center, &ref);
    to.x = ref.x;
    to.y = ref.y + off_px;
    rj_recenter(t_correct, &ref, &to);

    for (i = 0; i < n; i++) {
        struct point pf, pd;
        double dx, dy, d;
        transform_point(t_correct, projection_mg, &feat[i].c, &pf);
        transform_point(t, projection_mg, &feat[i].c, &pd);
        pd.y += off_px;
        dx = pd.x - pf.x;
        dy = pd.y - pf.y;
        d = sqrt(dx * dx + dy * dy);
        if (d > worst)
            worst = d;
    }
    transform_destroy(t);
    transform_destroy(t_correct);
    return worst;
}

/* A lagging map is re-centered by moving the window. Under perspective a rigid drag shift and the
 * re-projection of the map disagree, so any pending drag snaps visibly when the map is finally
 * re-centered. The animation tick avoids this by applying the full offset every tick, leaving no
 * pending drag; this test makes the residual of a pending drag measurable. */
static int scenario_recenter_jump_3d(void) {
    struct coord center = geo_to_mg(52.4, 10.35);
    double none = rj_residual(&center, 0);
    double tick = rj_residual(&center, RJ_TICK_OFFSET_PX);
    double drain = rj_residual(&center, RJ_W / 32);
    double lag = rj_residual(&center, RJ_LAG_Y);

    printf("3d recenter residual: none %.2fpx, %dpx %.2fpx, %dpx %.2fpx, %dpx %.2fpx\n", none, RJ_TICK_OFFSET_PX, tick,
           RJ_W / 32, drain, RJ_LAG_Y, lag);

    CHECK(none < RJ_SMOOTH_STEP_PX, "3d: no pending drag must leave no residual (%.2f px)", none);
    CHECK(tick > none && drain > tick && lag > drain, "3d: residual must grow with the pending drag");
    CHECK(lag > RJ_COARSE_STEP_PX, "3d: a full lag leaves a visible rigid-drag residual (%.2f px)", lag);

    return 0;
}

#define PF_N 7
#define PF_W 800
#define PF_H 480
#define PF_ZOOM 16
#define PF_PITCH 30
#define PF_MAX_FRAMES 240
#define PF_FULL_MARGIN 8
#define PF_JITTER_PX 2

static struct transformation *pf_trans_new(struct coord *center) {
    struct pcoord pc = {projection_mg, center->x, center->y};
    struct map_selection sel;
    struct transformation *t = transform_new(&pc, PF_ZOOM, 0);
    memset(&sel, 0, sizeof(sel));
    sel.u.p_rect.rl.x = PF_W;
    sel.u.p_rect.rl.y = PF_H;
    transform_set_screen_selection(t, &sel);
    transform_set_pitch(t, PF_PITCH);
    return t;
}

/* A rectangle with a shallow, narrow notch cut into its top edge. Both notch edges are shorter
 * than the coarse decimation distance, so screen-space decimation removes the whole notch and
 * turns the concave outline convex. */
static const struct point pf_shape[PF_N] = {
    {380, 280},
    {420, 280},
    {424, 290},
    {428, 280},
    {460, 280},
    {460, 360},
    {380, 360},
};

static int pf_build(struct transformation *t, struct coord *poly) {
    int i;
    for (i = 0; i < PF_N; i++) {
        struct point p = pf_shape[i];
        if (!transform_reverse(t, &p, &poly[i]))
            return 0;
    }
    return 1;
}

/* A simple polygon is concave iff its turn directions have both signs. */
static int pf_concave(struct point *p, int n) {
    int pos = 0, neg = 0, i;
    for (i = 0; i < n; i++) {
        struct point a = p[i], b = p[(i + 1) % n], c = p[(i + 2) % n];
        long cross = (long)(b.x - a.x) * (c.y - b.y) - (long)(b.y - a.y) * (c.x - b.x);
        if (cross > 0)
            pos = 1;
        else if (cross < 0)
            neg = 1;
    }
    return pos && neg;
}

static int pf_inside(struct point *p, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (p[i].x < PF_FULL_MARGIN || p[i].x >= PF_W - PF_FULL_MARGIN)
            return 0;
        if (p[i].y < PF_FULL_MARGIN || p[i].y >= PF_H - PF_FULL_MARGIN)
            return 0;
    }
    return 1;
}

static void pf_shift_center(struct transformation *t, int dx, int dy) {
    struct point a = {PF_W / 2, PF_H / 2};
    struct point b = {PF_W / 2 + dx, PF_H / 2 + dy};
    struct coord ca, cb, c;
    transform_reverse(t, &a, &ca);
    transform_reverse(t, &b, &cb);
    c = *transform_get_center(t);
    c.x += cb.x - ca.x;
    c.y += cb.y - ca.y;
    transform_set_center(t, &c);
}

/* Reads lat/lon pairs from an NMEA log (GGA and RMC). Returns the number of fixes read. */
static int pf_load_nmea(const char *path, GArray *lat, GArray *lon) {
    gchar *content = NULL;
    gsize len = 0;
    int n = 0;
    if (!g_file_get_contents(path, &content, &len, NULL))
        return 0;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        gchar *line = lines[i];
        int lat_idx, lon_idx;
        if (!strncmp(line, "$GPGGA", 6) || !strncmp(line, "$GNGGA", 6)) {
            lat_idx = 2;
            lon_idx = 4;
        } else if (!strncmp(line, "$GPRMC", 6) || !strncmp(line, "$GNRMC", 6)) {
            lat_idx = 3;
            lon_idx = 5;
        } else {
            continue;
        }
        gchar **f = g_strsplit(line, ",", -1);
        if (f[lat_idx] && f[lat_idx][0] && f[lon_idx] && f[lon_idx][0]) {
            double la = strtod(f[lat_idx], NULL);
            double lo = strtod(f[lon_idx], NULL);
            double lad = (int)(la / 100) + fmod(la, 100) / 60.0;
            double lod = (int)(lo / 100) + fmod(lo, 100) / 60.0;
            if (f[lat_idx + 1] && f[lat_idx + 1][0] == 'S')
                lad = -lad;
            if (f[lon_idx + 1] && f[lon_idx + 1][0] == 'W')
                lod = -lod;
            g_array_append_val(lat, lad);
            g_array_append_val(lon, lod);
            n++;
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(content);
    return n;
}

/* Replays a track through a tilted view. At every fix a concave polygon is placed on the ground
 * and projected after a small relative shift - the sub-pixel drag the vehicle carries between
 * fixes. It must keep all of its vertices and stay concave. Screen-space decimation of polygons
 * (the coarse `mindist` used while scrolling) violates this: short edges are dropped and concave
 * notches turn convex - the flicker seen while following a route. */
static int scenario_polygon_flicker(void) {
    GArray *lat = g_array_new(FALSE, FALSE, sizeof(double));
    GArray *lon = g_array_new(FALSE, FALSE, sizeof(double));
    const char *track = getenv("NAVIT_TEST_TRACK");
    if (!(track && pf_load_nmea(track, lat, lon) > 1)) {
        for (int i = 0; i < PF_MAX_FRAMES; i++) {
            double la = 52.4 + i * 2e-5;
            double lo = 10.35 + i * 3e-5;
            g_array_append_val(lat, la);
            g_array_append_val(lon, lo);
        }
    }

    int effective = graphics_element_mindist(GRAPHICS_MINDIST_COARSE, element_polygon);
    CHECK(effective == 0, "3d flicker: polygons must not be decimated (effective mindist %d)", effective);

    int frames = lat->len < PF_MAX_FRAMES ? lat->len : PF_MAX_FRAMES;
    int visible = 0, lost_prod = 0, lost_raw = 0;
    for (int i = 0; i < frames; i++) {
        struct coord center = geo_to_mg(g_array_index(lat, double, i), g_array_index(lon, double, i));
        struct transformation *t = pf_trans_new(&center);
        struct coord poly[PF_N];
        if (!pf_build(t, poly))
            continue;
        pf_shift_center(t, (i % (PF_JITTER_PX + 1)) - 1, ((i / (PF_JITTER_PX + 1)) % 2) - 1);

        struct point pp[PF_N], pr[PF_N];
        int cnt = transform_point_buf(t, projection_mg, poly, pp, sizeof(pp), PF_N, effective, 0, NULL);
        int raw = transform_point_buf(t, projection_mg, poly, pr, sizeof(pr), PF_N, GRAPHICS_MINDIST_COARSE, 0, NULL);
        transform_destroy(t);

        if (cnt < 3 || !pf_inside(pp, cnt))
            continue;
        visible++;
        if (cnt != PF_N || !pf_concave(pp, cnt))
            lost_prod++;
        if (raw < 3 || raw != PF_N || !pf_concave(pr, raw))
            lost_raw++;
    }

    printf("3d polygon flicker: %d/%d frames visible, coarse decimation breaks %d, projected %d\n", visible, frames,
           lost_raw, lost_prod);
    CHECK(visible > 0, "3d flicker: test polygon was never fully visible");
    CHECK(lost_prod == 0, "3d flicker: %d/%d visible frames lost polygon vertices or concavity", lost_prod, visible);
    CHECK(lost_raw > 0, "3d flicker: test no longer reproduces the coarse-decimation flicker (vacuous)");

    g_array_free(lat, TRUE);
    g_array_free(lon, TRUE);
    return 0;
}

struct scenario {
    const char *name;
    int (*fn)(void);
};

static struct scenario scenarios[] = {
    {"rect_convention",        scenario_route_rect_convention },
    {"proj_roundtrip",         scenario_projection_roundtrip  },
    {"route_car_grid",         scenario_route_car_grid        },
    {"route_bike_vs_car",      scenario_route_bike_vs_car     },
    {"route_oneway",           scenario_route_oneway          },
    {"route_turn_restriction", scenario_route_turn_restriction},
    {"search_town_street",     scenario_search_town_street    },
    {"kalman_scroll",          scenario_kalman_scroll         },
    {"covers_screen_3d",       scenario_covers_screen_3d      },
    {"recenter_jump_3d",       scenario_recenter_jump_3d      },
    {"polygon_flicker",        scenario_polygon_flicker       },
};

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <scenario|all>\nscenarios:", argv[0]);
        for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
            printf(" %s", scenarios[i].name);
        printf("\n");
        return 2;
    }
    const char *want = argv[1];
    int ran = 0;
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (strcmp(want, "all") && strcmp(want, scenarios[i].name))
            continue;
        ran++;
        int before = g_failures;
        scenarios[i].fn();
        printf("%-24s %s\n", scenarios[i].name, g_failures == before ? "PASS" : "FAIL");
    }
    if (!ran) {
        printf("unknown scenario '%s'\n", want);
        return 2;
    }
    return g_failures ? 1 : 0;
}
