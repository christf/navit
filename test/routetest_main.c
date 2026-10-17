#include "atom.h"
#include "config.h"
#include "coord.h"
#include "debug.h"
#include "event.h"
#include "event_glib.h"
#include "file.h"
#include "item.h"
#include "item_type_def.h"
#include "main.h"
#include "map.h"
#include "mapset.h"
#include "navit_nls.h"
#include "plugin.h"
#include "projection.h"
#include "roadprofile.h"
#include "route.h"
#include "transform.h"
#include "vehicleprofile.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

/* Vehicle profiles matching navit_shipped.xml */
#define CAR_FLAGS (AF_CAR)
#define CAR_FORWARD_MASK (AF_CAR | AF_HIGH_OCCUPANCY_CAR_ONLY | AF_ONEWAYREV)
#define CAR_REVERSE_MASK (AF_CAR | AF_HIGH_OCCUPANCY_CAR_ONLY | AF_ONEWAY)
#define BIKE_FLAGS (AF_BIKE)
#define BIKE_FORWARD_MASK (AF_BIKE | AF_ONEWAYREV)
#define BIKE_REVERSE_MASK (AF_BIKE | AF_ONEWAY)

#define POLL_MAX_ITERS 4000
#define POLL_PUMP_ITERS 50
#define PROFILE_SPEED 50
#define PROFILE_ROUTE_DEPTH "4:2000000"

static enum item_type profile_item_types[] = {type_street_0,
                                              type_street_1_city,
                                              type_street_2_city,
                                              type_street_3_city,
                                              type_street_4_city,
                                              type_highway_city,
                                              type_street_1_land,
                                              type_street_2_land,
                                              type_street_3_land,
                                              type_street_4_land,
                                              type_street_n_lanes,
                                              type_ramp,
                                              type_roundabout,
                                              type_ferry,
                                              type_living_street,
                                              type_street_service,
                                              type_street_parking_lane,
                                              type_track_paved,
                                              type_track_gravelled,
                                              type_track_unpaved,
                                              type_track_ground,
                                              type_path,
                                              type_cycleway,
                                              type_footway,
                                              type_street_pedestrian,
                                              type_none};

static struct vehicleprofile *make_profile(char *name, long flags, long fwd_mask, long rev_mask) {
    struct vehicleprofile *vp;
    struct roadprofile *rp;
    struct attr name_a, flags_a, fwd_a, rev_a, speed_a, types_a, rp_a, depth_a;
    struct attr *vp_attrs[6], *rp_attrs[3];

    speed_a.type = attr_speed;
    speed_a.u.num = PROFILE_SPEED;
    types_a.type = attr_item_types;
    types_a.u.item_types = profile_item_types;
    rp_attrs[0] = &speed_a;
    rp_attrs[1] = &types_a;
    rp_attrs[2] = NULL;
    rp = roadprofile_new(NULL, rp_attrs);

    name_a.type = attr_name;
    name_a.u.str = name;
    flags_a.type = attr_flags;
    flags_a.u.num = flags;
    fwd_a.type = attr_flags_forward_mask;
    fwd_a.u.num = fwd_mask;
    rev_a.type = attr_flags_reverse_mask;
    rev_a.u.num = rev_mask;
    depth_a.type = attr_route_depth;
    depth_a.u.str = PROFILE_ROUTE_DEPTH;
    vp_attrs[0] = &name_a;
    vp_attrs[1] = &flags_a;
    vp_attrs[2] = &fwd_a;
    vp_attrs[3] = &rev_a;
    vp_attrs[4] = &depth_a;
    vp_attrs[5] = NULL;
    vp = vehicleprofile_new(NULL, vp_attrs);
    if (!vp)
        return NULL;

    rp_a.type = attr_roadprofile;
    rp_a.u.navit_object = (struct navit_object *)rp;
    vehicleprofile_add_attr(vp, &rp_a);
    return vp;
}

static struct route *make_route(struct mapset *ms, struct vehicleprofile *vp) {
    struct route *r = route_new(NULL, NULL);
    route_set_mapset(r, ms);
    route_set_profile(r, vp);
    return r;
}

static void pump(unsigned int iters) {
    unsigned int i;
    for (i = 0; i < iters; i++)
        g_main_context_iteration(NULL, FALSE);
}

static int route_status_done(struct route *r) {
    struct attr status;
    status.type = attr_route_status;
    if (route_get_attr(r, attr_route_status, &status, NULL)) {
        long s = status.u.num;
        if (s == route_status_path_done_new || s == route_status_path_done_incremental)
            return 1;
        if (s == route_status_not_found)
            return 0;
        if (s == route_status_no_destination)
            return -1;
    }
    return -2;
}

static int run_case(char *map_path, struct coord *from, struct coord *to, struct vehicleprofile *vp, int expect_found) {
    struct attr type_a, data_a, desc_a, map_a;
    struct attr *map_attrs[4];
    struct map *map;
    struct mapset *ms;
    struct route *r;
    struct pcoord pos, dst;
    int status, i;

    type_a.type = attr_type;
    type_a.u.str = "binfile";
    data_a.type = attr_data;
    data_a.u.str = map_path;
    desc_a.type = attr_description;
    desc_a.u.str = "routetest";
    map_attrs[0] = &type_a;
    map_attrs[1] = &data_a;
    map_attrs[2] = &desc_a;
    map_attrs[3] = NULL;
    map = map_new(NULL, map_attrs);
    if (!map) {
        printf("FAIL: map_new for '%s'\n", map_path);
        return 1;
    }
    ms = mapset_new(NULL, NULL);
    map_a.type = attr_map;
    map_a.u.map = map;
    mapset_add_attr(ms, &map_a);

    r = make_route(ms, vp);
    pos.pro = projection_mg;
    pos.x = from->x;
    pos.y = from->y;
    dst.pro = projection_mg;
    dst.x = to->x;
    dst.y = to->y;
    route_set_position(r, &pos);
    fprintf(stderr, "CASE %s %d,%d -> %d,%d\n", map_path, pos.x, pos.y, to->x, to->y);
    route_set_destination(r, &dst, 1);

    status = -2;
    for (i = 0; i < POLL_MAX_ITERS && status == -2; i++) {
        pump(POLL_PUMP_ITERS);
        status = route_status_done(r);
    }

    printf("status=%d %s\n", status, status == 1 ? "path_done" : status == 0 ? "not_found" : "error");
    if (status < 0) {
        printf("FAIL: routing did not complete (status=%d)\n", status);
        return 1;
    }
    if ((status == 1) != expect_found) {
        printf("FAIL: expected %s but got %s\n", expect_found ? "route found" : "route not found",
               status == 1 ? "route found" : "route not found");
        return 1;
    }
    printf("PASS\n");
    return 0;
}

/* Position on a fixture street: same longitude as the street, latitude in
 * the middle of the relevant segment, so the point is guaranteed to lie on
 * the map's way. Computed from the fixture coordinates at startup. */
static struct coord fixture_pos(double lat, double lng) {
    struct coord_geo g = {.lat = lat, .lng = lng};
    struct coord c;
    transform_from_geo(projection_mg, &g, &c);
    return c;
}

int main(int argc, char **argv) {
    struct plugin *pl;
    struct vehicleprofile *vp_car, *vp_bike;
    struct attr path_a, active_a;
    struct attr *pl_attrs[3];
    struct coord a1 = fixture_pos(52.1025, 11.600); /* Barrier Street, before the bollard */
    struct coord p1 = fixture_pos(52.105, 11.600);  /* the bollard itself */
    struct coord b2 = fixture_pos(52.1075, 11.600); /* Barrier Street, beyond the bollard */
    struct coord s1 = fixture_pos(52.1025, 11.610); /* Detour Street */
    int rc = 0;

    if (argc < 3) {
        printf("usage: %s <corridor.bin> <detour.bin> [cb.bin] [lg.bin] [reg.bin] [binfile-plugin]\n", argv[0]);
        return 2;
    }

    event_glib_init();
    if (!event_request_system("glib", "routetest")) {
        printf("FAIL: could not init glib event system\n");
        return 1;
    }
    atom_init();
    main_init(argv[0]);
    navit_nls_main_init();
    debug_init(argv[0]);
    file_init();
    route_init();

    path_a.type = attr_path;
    if (argc >= 7)
        path_a.u.str = argv[6];
    else
        path_a.u.str = getenv("NAVIT_TEST_PLUGIN");
    if (!path_a.u.str)
        path_a.u.str = "libmap_binfile.so";
    active_a.type = attr_active;
    active_a.u.num = 1;
    pl_attrs[0] = &path_a;
    pl_attrs[1] = &active_a;
    pl_attrs[2] = NULL;
    pl = plugin_new(NULL, pl_attrs);
    if (!pl) {
        printf("FAIL: could not load binfile plugin\n");
        return 1;
    }

    vp_car = make_profile("car", CAR_FLAGS, CAR_FORWARD_MASK, CAR_REVERSE_MASK);
    vp_bike = make_profile("bike", BIKE_FLAGS, BIKE_FORWARD_MASK, BIKE_REVERSE_MASK);
    if (!vp_car || !vp_bike) {
        printf("FAIL: could not create vehicle profiles\n");
        return 1;
    }

    printf("== corridor map: car BEFORE bollard -> BEYOND bollard (expect not found) ==\n");
    rc |= run_case(argv[1], &a1, &b2, vp_car, 0);

    printf("== corridor map: car -> bollard itself (expect not found: a blocked point cannot be a destination) ==\n");
    rc |= run_case(argv[1], &a1, &p1, vp_car, 0);

    printf("== corridor map: bike BEFORE -> BEYOND bollard (expect found) ==\n");
    rc |= run_case(argv[1], &a1, &b2, vp_bike, 1);

    printf("== detour map: car side street -> BEYOND bollard around (expect found) ==\n");
    rc |= run_case(argv[2], &s1, &b2, vp_car, 1);

    if (argc >= 4) {
        printf("== cycle_barrier map: car -> BEYOND (expect not found) ==\n");
        rc |= run_case(argv[3], &a1, &b2, vp_car, 0);

        printf("== cycle_barrier map: bike untagged -> BEYOND (expect found) ==\n");
        rc |= run_case(argv[3], &a1, &b2, vp_bike, 1);
    }
    if (argc >= 5) {
        printf("== lift_gate map: car -> BEYOND (expect not found) ==\n");
        rc |= run_case(argv[4], &a1, &b2, vp_car, 0);

        printf("== lift_gate map: bike -> BEYOND (expect found) ==\n");
        rc |= run_case(argv[4], &a1, &b2, vp_bike, 1);
    }
    if (argc >= 6) {
        printf("== regression map: car -> BEYOND (expect not found) ==\n");
        rc |= run_case(argv[5], &a1, &b2, vp_car, 0);

        printf("== regression map: bike -> BEYOND (expect found; proves per-node flag reset) ==\n");
        rc |= run_case(argv[5], &a1, &b2, vp_bike, 1);
    }
    return rc;
}
