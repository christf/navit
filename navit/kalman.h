/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2026 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#ifndef NAVIT_KALMAN_H
#define NAVIT_KALMAN_H

struct kalman_filter;

struct kalman_filter *kalman_new(void);
void kalman_destroy(struct kalman_filter *kf);
void kalman_reset(struct kalman_filter *kf);
void kalman_update(struct kalman_filter *kf, double dt, double x, double y, double vx, double vy, int use_velocity);
double kalman_get_heading(struct kalman_filter *kf);
double kalman_get_speed(struct kalman_filter *kf);
void kalman_get_position(struct kalman_filter *kf, double *x, double *y);
void kalman_get_filtered_position(struct kalman_filter *kf, double *x, double *y);
void kalman_set_position(struct kalman_filter *kf, double x, double y);
int kalman_is_initialized(const struct kalman_filter *kf);

#endif
