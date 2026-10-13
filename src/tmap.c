/*
 * Copyright 2023-2025 Jetperch LLC
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

#include "jls/ec.h"
#include "jls/tmap.h"
#include "jls/log.h"
#include "jls/time.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>


#define ENTRIES_ALLOC_INIT  (1000)


struct jls_tmap_s {
    double sample_rate;
    size_t entries_length;
    size_t entries_alloc;
    int64_t * sample_id;
    int64_t * utc;
};


struct jls_tmap_s * jls_tmap_alloc(double sample_rate) {
    if (!(sample_rate > 0)) {
        // allowed: single-entry conversions then return JLS_ERROR_UNAVAILABLE
        JLS_LOGW("Invalid sample_rate");
    }
    struct jls_tmap_s * s = malloc(sizeof(struct jls_tmap_s));
    if (NULL == s) {
        return NULL;
    }
    s->sample_id = malloc(ENTRIES_ALLOC_INIT * sizeof(int64_t));
    if (NULL == s->sample_id) {
        free(s);
        return NULL;
    }
    s->utc = malloc(ENTRIES_ALLOC_INIT * sizeof(int64_t));
    if (NULL == s->utc) {
        free(s->sample_id);
        free(s);
        return NULL;
    }
    s->sample_rate = sample_rate;
    s->entries_length = 0;
    s->entries_alloc = ENTRIES_ALLOC_INIT;
    return s;
}

void jls_tmap_free(struct jls_tmap_s * self) {
    if (NULL != self) {
        if (NULL != self->sample_id) {
            free(self->sample_id);
            self->sample_id = NULL;
        }
        if (NULL != self->utc) {
            free(self->utc);
            self->utc = NULL;
        }
        self->entries_alloc = 0;
        self->entries_length = 0;
        free(self);
    }
}

int32_t jls_tmap_add(struct jls_tmap_s * self, int64_t sample_id, int64_t timestamp) {
    size_t idx = self->entries_length;
    if (idx) {
        if (sample_id == self->sample_id[idx - 1]) {
            --idx;  // overwrite
        } else if (sample_id < self->sample_id[idx - 1]) {
            JLS_LOGE("UTC add is not monotonically increasing: idx=%zu, %" PRIi64, idx, sample_id);
            return JLS_ERROR_PARAMETER_INVALID;  // ignore
        }
    }
    // interpolation and the timestamp search require strictly increasing utc
    if (idx && (timestamp <= self->utc[idx - 1])) {
        JLS_LOGW("UTC timestamp is not monotonically increasing: idx=%zu, %" PRIi64, idx, timestamp);
        return JLS_ERROR_PARAMETER_INVALID;  // ignore
    }
    if (idx >= self->entries_alloc) {
        if (self->entries_alloc > (SIZE_MAX / (2 * sizeof(int64_t)))) {
            return JLS_ERROR_NOT_ENOUGH_MEMORY;
        }
        size_t entries_alloc = self->entries_alloc * 2;
        // keep each pointer valid as soon as its realloc succeeds
        int64_t * p = realloc(self->sample_id, entries_alloc * sizeof(int64_t));
        if (NULL == p) {
            return JLS_ERROR_NOT_ENOUGH_MEMORY;
        }
        self->sample_id = p;
        p = realloc(self->utc, entries_alloc * sizeof(int64_t));
        if (NULL == p) {
            return JLS_ERROR_NOT_ENOUGH_MEMORY;
        }
        self->utc = p;
        self->entries_alloc = entries_alloc;
    }
    self->sample_id[idx] = sample_id;
    self->utc[idx] = timestamp;
    self->entries_length = idx + 1;
    return 0;
}

int32_t jls_tmap_add_cbk(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size) {
    struct jls_tmap_s * self = (struct jls_tmap_s *) user_data;
    for (uint32_t i = 0; i < size; ++i) {
        jls_tmap_add(self, utc[i].sample_id, utc[i].timestamp);
    }
    return 0;
}

size_t jls_tmap_length(struct jls_tmap_s * self) {
    return self->entries_length;
}

// Saturate: hostile UTC values must not overflow or cast out of range (UB).
static int64_t f64_to_i64(double x) {
    if (isnan(x)) {
        return 0;
    } else if (x >= 9223372036854775807.0) {
        return INT64_MAX;
    } else if (x <= -9223372036854775808.0) {
        return INT64_MIN;
    }
    return (int64_t) x;
}

// a + round(k), saturating; integer add keeps precision for large a
static int64_t add_i64_f64(int64_t a, double k) {
    k = round(k);
    if (isnan(k)) {
        return a;
    } else if ((k >= 9223372036854775807.0) || (k <= -9223372036854775808.0)) {
        return f64_to_i64((double) a + k);
    }
    int64_t b = (int64_t) k;
    if ((b > 0) && (a > (INT64_MAX - b))) {
        return INT64_MAX;
    } else if ((b < 0) && (a < (INT64_MIN - b))) {
        return INT64_MIN;
    }
    return a + b;
}

// a - b without int64 overflow, exact in uint64 before rounding to double
static double diff_f64(int64_t a, int64_t b) {
    if (a >= b) {
        return (double) ((uint64_t) a - (uint64_t) b);
    }
    return -(double) ((uint64_t) b - (uint64_t) a);
}

static int64_t interp_i64(struct jls_tmap_s * self, int64_t x0, int64_t const * x, int64_t const * y) {
    // binary search for x index with value less than or equal to x0
    size_t low = 0;
    size_t high = self->entries_length - 1;  // caller guarantees >= 2 entries
    size_t mid;
    while (low < high) {
        mid = (low + high + 1) / 2;
        if (x0 == x[mid]) {
            low = mid;
            break;
        } else if (x0 < x[mid]) {
            high = mid - 1;
        } else {
            low = mid;
        }
    }
    if (low >= (self->entries_length - 1)) {
        low = self->entries_length - 2;
    }

    double dk = diff_f64(x0, x[low]);
    double ds = diff_f64(x[low + 1], x[low]);
    double dt = diff_f64(y[low + 1], y[low]);
    if (!(ds > 0)) {
        return y[low];  // jls_tmap_add enforces increasing x; defensive
    }
    return add_i64_f64(y[low], dk * (dt / ds));
}

int32_t jls_tmap_sample_id_to_timestamp(struct jls_tmap_s * self, int64_t sample_id, int64_t * timestamp) {
    if (self->entries_length == 0) {
        return JLS_ERROR_UNAVAILABLE;
    } else if (self->entries_length == 1) {
        if (!(self->sample_rate > 0)) {
            return JLS_ERROR_UNAVAILABLE;
        }
        double dsample = diff_f64(sample_id, self->sample_id[0]);
        double dt = dsample / self->sample_rate;
        dt *= JLS_TIME_SECOND;
        *timestamp = add_i64_f64(self->utc[0], trunc(dt));
    } else {
        *timestamp = interp_i64(self, sample_id, self->sample_id, self->utc);
    }
    return 0;
}

int32_t jls_tmap_timestamp_to_sample_id(struct jls_tmap_s * self, int64_t timestamp, int64_t * sample_id) {
    if (self->entries_length == 0) {
        return JLS_ERROR_UNAVAILABLE;
    } else if (self->entries_length == 1) {
        if (!(self->sample_rate > 0)) {
            return JLS_ERROR_UNAVAILABLE;
        }
        double dt = diff_f64(timestamp, self->utc[0]);
        dt *= (1.0 / JLS_TIME_SECOND);
        *sample_id = add_i64_f64(self->sample_id[0], trunc(dt * self->sample_rate));
    } else {
        *sample_id = interp_i64(self, timestamp, self->utc, self->sample_id);
    }
    return 0;
}

int32_t jls_tmap_get(struct jls_tmap_s * self, size_t index, struct jls_utc_summary_entry_s * entry) {
    if (!entry || (index >= self->entries_length)) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    entry->sample_id = self->sample_id[index];
    entry->timestamp = self->utc[index];
    return 0;
}
