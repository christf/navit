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

#include "kalman.h"
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define KF_N 4
#define KF_PI 3.14159265358979323846
#define KF_RAD2DEG 57.29577951308232087680
#define KF_DEG2RAD 0.01745329251994329577
#define KF_EARTH_R 6371000.0
#define KF_ACCEL_VAR 4.0
#define KF_POS_VAR 100.0
#define KF_VEL_VAR 4.0

struct kalman_filter {
    double x[KF_N];
    double P[KF_N * KF_N];
    struct timeval last_update;
    int initialized;
};

static void mat4_mul(const double *A, const double *B, double *C) {
    int i, j, k;
    for (i = 0; i < KF_N; i++)
        for (j = 0; j < KF_N; j++) {
            double s = 0;
            for (k = 0; k < KF_N; k++)
                s += A[i * KF_N + k] * B[k * KF_N + j];
            C[i * KF_N + j] = s;
        }
}

static void mat4_transpose(const double *A, double *AT) {
    int i, j;
    for (i = 0; i < KF_N; i++)
        for (j = 0; j < KF_N; j++)
            AT[j * KF_N + i] = A[i * KF_N + j];
}

static void mat4_identity(double *A) {
    int i;
    memset(A, 0, KF_N * KF_N * sizeof(double));
    for (i = 0; i < KF_N; i++)
        A[i * KF_N + i] = 1.0;
}

static void mat4_add(const double *A, const double *B, double *C) {
    int i;
    for (i = 0; i < KF_N * KF_N; i++)
        C[i] = A[i] + B[i];
}

static void mat4_sub(const double *A, const double *B, double *C) {
    int i;
    for (i = 0; i < KF_N * KF_N; i++)
        C[i] = A[i] - B[i];
}

static int mat4_inverse(const double *A, double *Ainv) {
    double aug[KF_N][2 * KF_N];
    int i, j, k;
    for (i = 0; i < KF_N; i++) {
        for (j = 0; j < KF_N; j++) {
            aug[i][j] = A[i * KF_N + j];
            aug[i][j + KF_N] = (i == j) ? 1.0 : 0.0;
        }
    }
    for (i = 0; i < KF_N; i++) {
        int pivot = i;
        double maxv = fabs(aug[i][i]);
        for (k = i + 1; k < KF_N; k++) {
            if (fabs(aug[k][i]) > maxv) {
                maxv = fabs(aug[k][i]);
                pivot = k;
            }
        }
        if (maxv < 1e-12)
            return 0;
        if (pivot != i) {
            for (j = 0; j < 2 * KF_N; j++) {
                double tmp = aug[i][j];
                aug[i][j] = aug[pivot][j];
                aug[pivot][j] = tmp;
            }
        }
        {
            double scale = aug[i][i];
            for (j = 0; j < 2 * KF_N; j++)
                aug[i][j] /= scale;
        }
        for (k = 0; k < KF_N; k++) {
            double factor;
            if (k == i)
                continue;
            factor = aug[k][i];
            for (j = 0; j < 2 * KF_N; j++)
                aug[k][j] -= factor * aug[i][j];
        }
    }
    for (i = 0; i < KF_N; i++)
        for (j = 0; j < KF_N; j++)
            Ainv[i * KF_N + j] = aug[i][j + KF_N];
    return 1;
}

static int mat2_inverse(const double *A, double *Ainv) {
    double det = A[0] * A[3] - A[1] * A[2];
    if (fabs(det) < 1e-12)
        return 0;
    Ainv[0] = A[3] / det;
    Ainv[1] = -A[1] / det;
    Ainv[2] = -A[2] / det;
    Ainv[3] = A[0] / det;
    return 1;
}

static void kf_predict(struct kalman_filter *kf, double dt) {
    double F[KF_N * KF_N];
    double Ft[KF_N * KF_N];
    double FP[KF_N * KF_N];
    double FPFt[KF_N * KF_N];
    double Q[KF_N * KF_N];
    double dt2, dt3, dt4;
    int i;

    if (dt <= 0 || dt > 10.0)
        return;

    mat4_identity(F);
    F[0 * KF_N + 2] = dt;
    F[1 * KF_N + 3] = dt;

    dt2 = dt * dt;
    dt3 = dt2 * dt;
    dt4 = dt2 * dt2;

    memset(Q, 0, sizeof(Q));
    Q[0 * KF_N + 0] = dt4 / 4.0 * KF_ACCEL_VAR;
    Q[1 * KF_N + 1] = dt4 / 4.0 * KF_ACCEL_VAR;
    Q[2 * KF_N + 2] = dt2 * KF_ACCEL_VAR;
    Q[3 * KF_N + 3] = dt2 * KF_ACCEL_VAR;
    Q[0 * KF_N + 2] = dt3 / 2.0 * KF_ACCEL_VAR;
    Q[2 * KF_N + 0] = dt3 / 2.0 * KF_ACCEL_VAR;
    Q[1 * KF_N + 3] = dt3 / 2.0 * KF_ACCEL_VAR;
    Q[3 * KF_N + 1] = dt3 / 2.0 * KF_ACCEL_VAR;

    kf->x[0] += kf->x[2] * dt;
    kf->x[1] += kf->x[3] * dt;

    mat4_transpose(F, Ft);
    mat4_mul(F, kf->P, FP);
    mat4_mul(FP, Ft, FPFt);
    mat4_add(FPFt, Q, kf->P);

    for (i = 0; i < KF_N * KF_N; i += KF_N + 1)
        if (kf->P[i] < 0)
            kf->P[i] = fabs(kf->P[i]);
}

static void kf_update_pos(struct kalman_filter *kf, double x, double y) {
    double H[2 * KF_N] = {1, 0, 0, 0, 0, 1, 0, 0};
    double z[2] = {x, y};
    double yinn[2];
    double S[4], Sinv[4];
    double PHt[KF_N * 2];
    double K[KF_N * 2];
    double KH[KF_N * KF_N];
    double IKH[KF_N * KF_N];
    int i, j, k;

    for (i = 0; i < 2; i++) {
        double s = 0;
        for (j = 0; j < KF_N; j++)
            s += H[i * KF_N + j] * kf->x[j];
        yinn[i] = z[i] - s;
    }

    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++) {
            double s = 0;
            for (k = 0; k < KF_N; k++)
                s += H[i * KF_N + k] * kf->P[k * KF_N + j];
            S[i * 2 + j] = s;
        }
    S[0] += KF_POS_VAR;
    S[3] += KF_POS_VAR;

    if (!mat2_inverse(S, Sinv))
        return;

    for (i = 0; i < KF_N; i++)
        for (j = 0; j < 2; j++) {
            double s = 0;
            for (k = 0; k < KF_N; k++)
                s += kf->P[i * KF_N + k] * H[j * KF_N + k];
            PHt[i * 2 + j] = s;
        }

    for (i = 0; i < KF_N; i++)
        for (j = 0; j < 2; j++) {
            double s = 0;
            for (k = 0; k < 2; k++)
                s += PHt[i * 2 + k] * Sinv[k * 2 + j];
            K[i * 2 + j] = s;
        }

    for (i = 0; i < KF_N; i++) {
        double s = 0;
        for (j = 0; j < 2; j++)
            s += K[i * 2 + j] * yinn[j];
        kf->x[i] += s;
    }

    mat4_identity(KH);
    for (i = 0; i < KF_N; i++)
        for (j = 0; j < KF_N; j++) {
            double s = 0;
            for (k = 0; k < 2; k++)
                s += K[i * 2 + k] * H[k * KF_N + j];
            KH[i * KF_N + j] = s;
        }
    mat4_sub(kf->P, KH, IKH);
    memcpy(kf->P, IKH, sizeof(IKH));

    for (i = 0; i < KF_N * KF_N; i += KF_N + 1)
        if (kf->P[i] < 0)
            kf->P[i] = fabs(kf->P[i]);
}

static void kf_update_pos_vel(struct kalman_filter *kf, double x, double y, double vx, double vy) {
    double z[KF_N] = {x, y, vx, vy};
    double S[KF_N * KF_N];
    double Sinv[KF_N * KF_N];
    double K[KF_N * KF_N];
    double IKH[KF_N * KF_N];
    double yinn[KF_N];
    int i, j;

    for (i = 0; i < KF_N; i++)
        yinn[i] = z[i] - kf->x[i];

    for (i = 0; i < KF_N; i++)
        for (j = 0; j < KF_N; j++)
            S[i * KF_N + j] = kf->P[i * KF_N + j];
    S[0 * KF_N + 0] += KF_POS_VAR;
    S[1 * KF_N + 1] += KF_POS_VAR;
    S[2 * KF_N + 2] += KF_VEL_VAR;
    S[3 * KF_N + 3] += KF_VEL_VAR;

    if (!mat4_inverse(S, Sinv))
        return;

    mat4_mul(kf->P, Sinv, K);

    for (i = 0; i < KF_N; i++) {
        double s = 0;
        for (j = 0; j < KF_N; j++)
            s += K[i * KF_N + j] * yinn[j];
        kf->x[i] += s;
    }

    for (i = 0; i < KF_N; i++)
        for (j = 0; j < KF_N; j++)
            IKH[i * KF_N + j] = (i == j ? 1.0 : 0.0) - K[i * KF_N + j];
    mat4_mul(IKH, kf->P, S);
    memcpy(kf->P, S, sizeof(S));

    for (i = 0; i < KF_N * KF_N; i += KF_N + 1)
        if (kf->P[i] < 0)
            kf->P[i] = fabs(kf->P[i]);
}

static double get_time_since(const struct timeval *t1, const struct timeval *t2) {
    return (double)(t2->tv_sec - t1->tv_sec) + (double)(t2->tv_usec - t1->tv_usec) / 1000000.0;
}

struct kalman_filter *kalman_new(void) {
    struct kalman_filter *kf = g_new0(struct kalman_filter, 1);
    return kf;
}

void kalman_destroy(struct kalman_filter *kf) {
    g_free(kf);
}

void kalman_reset(struct kalman_filter *kf) {
    memset(kf, 0, sizeof(*kf));
}

void kalman_update(struct kalman_filter *kf, double dt, double x, double y, double vx, double vy, int use_velocity) {
    struct timeval now;

    if (!kf->initialized) {
        kf->x[0] = x;
        kf->x[1] = y;
        kf->x[2] = vx;
        kf->x[3] = vy;
        mat4_identity(kf->P);
        kf->P[0 * KF_N + 0] = KF_POS_VAR * 10;
        kf->P[1 * KF_N + 1] = KF_POS_VAR * 10;
        kf->P[2 * KF_N + 2] = KF_VEL_VAR * 10;
        kf->P[3 * KF_N + 3] = KF_VEL_VAR * 10;
        gettimeofday(&kf->last_update, NULL);
        kf->initialized = 1;
        return;
    }

    gettimeofday(&now, NULL);
    if (dt <= 0)
        dt = get_time_since(&kf->last_update, &now);
    if (dt <= 0)
        dt = 0.001;
    if (dt > 10.0)
        dt = 10.0;

    kf_predict(kf, dt);

    if (use_velocity)
        kf_update_pos_vel(kf, x, y, vx, vy);
    else
        kf_update_pos(kf, x, y);

    kf->last_update = now;
}

double kalman_get_heading(struct kalman_filter *kf) {
    if (!kf->initialized)
        return 0;

    if (fabs(kf->x[2]) < 0.01 && fabs(kf->x[3]) < 0.01)
        return 0;

    return atan2(kf->x[2], kf->x[3]) * KF_RAD2DEG;
}

double kalman_get_speed(struct kalman_filter *kf) {
    if (!kf->initialized)
        return 0;
    return sqrt(kf->x[2] * kf->x[2] + kf->x[3] * kf->x[3]);
}

void kalman_get_position(struct kalman_filter *kf, double *x, double *y) {
    struct timeval now;
    double dt;

    if (!kf->initialized) {
        *x = 0;
        *y = 0;
        return;
    }

    gettimeofday(&now, NULL);
    dt = get_time_since(&kf->last_update, &now);
    if (dt > 0 && dt < 5.0) {
        *x = kf->x[0] + kf->x[2] * dt;
        *y = kf->x[1] + kf->x[3] * dt;
    } else {
        *x = kf->x[0];
        *y = kf->x[1];
    }
}

int kalman_is_initialized(const struct kalman_filter *kf) {
    return kf->initialized;
}

void kalman_get_filtered_position(struct kalman_filter *kf, double *x, double *y) {
    if (!kf->initialized) {
        *x = 0;
        *y = 0;
        return;
    }
    *x = kf->x[0];
    *y = kf->x[1];
}

void kalman_set_position(struct kalman_filter *kf, double x, double y) {
    if (!kf->initialized)
        return;
    kf->x[0] = x;
    kf->x[1] = y;
}
