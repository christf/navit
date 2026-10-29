#ifndef NAVIT_VEHICLE_DEMO_H
#define NAVIT_VEHICLE_DEMO_H

struct vehicle;

/* Test seam: run one demo-vehicle timer tick synchronously so tests can step
 * the simulation deterministically instead of waiting on the wall clock. */
void vehicle_demo_test_tick(struct vehicle *v);

#endif
