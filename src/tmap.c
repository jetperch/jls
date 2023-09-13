/*
 * Copyright 2023 Jetperch LLC
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
    if (sample_rate <= 0) {
        JLS_LOGE("Invalid sample_rate");
    }
    struct jls_tmap_s * s = malloc(sizeof(struct jls_tmap_s));
    if (NULL == s) {
        return NULL;
    }
    s->sample_id = malloc(ENTRIES_ALLOC_INIT * sizeof(double));
    if (NULL == s->sample_id) {
        free(s);
        return NULL;
    }
    s->utc = malloc(ENTRIES_ALLOC_INIT * sizeof(double));
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
    int64_t * p1;
    int64_t * p2;
    if (self->entries_length >= self->entries_alloc) {
        size_t entries_alloc = self->entries_alloc * 2;
        p1 = realloc(self->sample_id, entries_alloc * sizeof(struct jls_utc_summary_entry_s));
        p2 = realloc(self->utc, entries_alloc * sizeof(struct jls_utc_summary_entry_s));
        if ((NULL == p1) || (NULL == p2)) {
            // out of memory, do the best we can
            self->entries_length = self->entries_alloc - 1;
        } else {
            self->sample_id = p1;
            self->utc = p2;
            self->entries_alloc = entries_alloc;
        }
    }
    if (self->entries_length) {
        if (sample_id == self->sample_id[self->entries_length - 1]) {
            --self->entries_length;  // overwrite
        } else if (sample_id <= self->sample_id[self->entries_length - 1]) {
            JLS_LOGE("UTC add is not monotonically increasing: idx=%zu, %" PRIi64,
                     self->entries_length, sample_id);
            return JLS_ERROR_PARAMETER_INVALID;  // ignore
        }
    }
    self->sample_id[self->entries_length] = sample_id;
    self->utc[self->entries_length] = timestamp;
    ++self->entries_length;
    return 0;
}

int32_t jls_tmap_add_cbk(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size) {
    struct jls_tmap_s * self = (struct jls_tmap_s *) user_data;
    for (uint32_t i = 0; i < size; ++i) {
        jls_tmap_add(self, utc[i].sample_id, utc[i].timestamp);
    }
    return 0;
}

int64_t interp_i64(struct jls_tmap_s * self, int64_t x0, int64_t const * x, int64_t const * y) {
    // binary search for x index with value less than or equal to x0
    size_t low = 0;
    size_t high = self->entries_length;
    size_t mid;
    while (low < high) {
        mid = (low + high + 1) / 2;
        if (x0 == x[mid]) {
            low = mid;
            break;
        } else if (x0 < x[mid]) {
            high = mid - 1;
        } else if (x0 > x[mid]) {
            low = mid;
        }
    }
    if (low >= (self->entries_length - 1)) {
        low = self->entries_length - 2;
    }

    // interpolate
    double dk = (double) (x0 - x[low]);
    double ds = (double) (x[low + 1] - x[low]);
    double dt = (double) (y[low + 1] - y[low]);
    double slope = dt / ds;
    int64_t k = (int64_t) round(dk * slope);
    return y[low] + k;
}

int32_t jls_tmap_sample_id_to_timestamp(struct jls_tmap_s * self, int64_t sample_id, int64_t * timestamp) {
    if (self->entries_length == 0) {
        return JLS_ERROR_UNAVAILABLE;
    } else if (self->entries_length == 1) {
        if (self->sample_rate <= 0) {
            return JLS_ERROR_UNAVAILABLE;
        }
        double dsample = (double) (sample_id - self->sample_id[0]);
        double dt = dsample / self->sample_rate;
        dt *= JLS_TIME_SECOND;
        *timestamp = self->utc[0] + (int64_t) dt;
    } else {
        *timestamp = interp_i64(self, sample_id, self->sample_id, self->utc);
    }
    return 0;
}

int32_t jls_tmap_timestamp_to_sample_id(struct jls_tmap_s * self, int64_t timestamp, int64_t * sample_id) {
    if (self->entries_length == 0) {
        return JLS_ERROR_UNAVAILABLE;
    } else if (self->entries_length == 1) {
        if (self->sample_rate <= 0) {
            return JLS_ERROR_UNAVAILABLE;
        }
        double dt = (double) (timestamp - self->utc[0]);
        dt *= (1.0 / JLS_TIME_SECOND);
        *sample_id = self->sample_id[0] + (int64_t) (dt * self->sample_rate);
    } else {
        *sample_id = interp_i64(self, timestamp, self->utc, self->sample_id);
    }
    return 0;
}
