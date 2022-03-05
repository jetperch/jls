/*
 * Copyright 2020-2022 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jls/statistics.h"
#include <float.h>
#include <math.h>

void jls_statistics_reset(struct jls_statistics_s *s) {
    s->k = 0;
    s->mean = 0.0;
    s->s = 0.0;
    s->min = DBL_MAX;
    s->max = -DBL_MAX;
}

void jls_statistics_invalid(struct jls_statistics_s *s) {
    s->mean = NAN;
    s->s = NAN;
    s->min = NAN;
    s->max = NAN;
}

void jls_statistics_compute_f32(struct jls_statistics_s * s, const float * x, uint64_t length) {
    if (length <= 0) {
        jls_statistics_reset(s);
        return;
    }
    float v;
    double v_mean = 0.0;
    float v_min = FLT_MAX;
    float v_max = -FLT_MAX;
    double v_var = 0.0;
    for (uint64_t i = 0; i < length; ++i) {
        v = x[i];
        v_mean += v;
        if (v < v_min) {
            v_min = v;
        }
        if (v > v_max) {
            v_max = v;
        }
    }
    v_mean /= length;
    double m;
    for (uint64_t i = 0; i < length; ++i) {
        m = x[i] - v_mean;
        v_var += (m * m);
    }
    s->k = length;
    s->mean = v_mean;
    s->s = v_var;
    s->min = v_min;
    s->max = v_max;
}

void jls_statistics_compute_f64(struct jls_statistics_s * s, const double * x, uint64_t length)  {
    if (length <= 0) {
        jls_statistics_reset(s);
        return;
    }
    double v;
    double v_mean = 0.0;
    double v_min = DBL_MAX;
    double v_max = -DBL_MAX;
    double v_var = 0.0;
    for (uint64_t i = 0; i < length; ++i) {
        v = x[i];
        v_mean += v;
        if (v < v_min) {
            v_min = v;
        }
        if (v > v_max) {
            v_max = v;
        }
    }
    v_mean /= length;
    double m;
    for (uint64_t i = 0; i < length; ++i) {
        m = x[i] - v_mean;
        v_var += (m * m);
    }
    s->k = length;
    s->mean = v_mean;
    s->s = v_var;
    s->min = v_min;
    s->max = v_max;
}

void jls_statistics_add(struct jls_statistics_s *s, double x) {
    double m_old;
    double m_new;
    ++s->k;
    m_old = s->mean;
    m_new = s->mean + (x - s->mean) / (double) s->k;
    s->mean = m_new;
    s->s += (x - m_old) * (x - m_new);
    if (x < s->min) {
        s->min = x;
    }
    if (x > s->max) {
        s->max = x;
    }
}

double jls_statistics_var(struct jls_statistics_s *s) {
    if (s->k <= 1) {
        return 0.0;
    }
    return s->s / (double) (s->k - 1); // use k - 1 = Bessel's correction for sample variance
}

void jls_statistics_copy(struct jls_statistics_s *tgt,
                         const struct jls_statistics_s *src) {
    tgt->k = src->k;
    tgt->mean = src->mean;
    tgt->s = src->s;
    tgt->min = src->min;
    tgt->max = src->max;
}

void jls_statistics_combine(struct jls_statistics_s *tgt,
                            const struct jls_statistics_s *a,
                            const struct jls_statistics_s *b) {
    uint64_t kt;
    double f1;
    double m1_diff;
    double m2_diff;
    kt = a->k + b->k;
    if (kt == 0) {
        jls_statistics_reset(tgt);
    } else if (a->k == 0) {
        jls_statistics_copy(tgt, b);
    } else if (b->k == 0) {
        jls_statistics_copy(tgt, a);
    } else {
        f1 = a->k / (double) kt;
        double mean_new = f1 * a->mean + (1.0 - f1) * b->mean;
        m1_diff = a->mean - mean_new;
        m2_diff = b->mean - mean_new;
        tgt->s = (a->s + a->k * m1_diff * m1_diff) +
                 (b->s + b->k * m2_diff * m2_diff);
        tgt->mean = mean_new;
        tgt->min = (a->min < b->min) ? a->min : b->min;
        tgt->max = (a->max > b->max) ? a->max : b->max;
        tgt->k = kt;
    }
}
