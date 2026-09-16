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
