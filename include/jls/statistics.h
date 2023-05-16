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

#ifndef JLS_STATISTICS_H
#define JLS_STATISTICS_H

#include "jls/cmacro.h"
#include <stdint.h>

/**
 * @ingroup jls
 * @defgroup jls_statistics Statistics
 *
 * @brief JLS statistics.
 *
 * @{
 */

JLS_CPP_GUARD_START

/**
 * @brief The statistics instance for a single variable.
 *
 * This structure and associated "methods" compute mean, sample variance, 
 * minimum and maximum over samples.  The statistics are computed in a single 
 * pass and are available at any time with minimal additional computation.
 *
 * @see https://en.wikipedia.org/wiki/Variance
 * @see https://en.wikipedia.org/wiki/Unbiased_estimation_of_standard_deviation
 * @see https://www.johndcook.com/blog/standard_deviation/
 * @see https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
 */
struct jls_statistics_s {
    uint64_t k;    ///< Number of samples.
    double mean;   ///< mean (average value).
    double s;      ///< Scaled running variance.
    double min;    ///< Minimum value.
    double max;    ///< Maximum value.
};

/**
 * @brief Reset the statistics to 0 samples.
 *
 * @param s The statistics instance.
 */
JLS_API void jls_statistics_reset(struct jls_statistics_s * s);

/**
 * @brief Mark the statistics as invalid.
 *
 * @param s The statistics instance which will have all statistics marked
 *      as NaN.
 */
JLS_API void jls_statistics_invalid(struct jls_statistics_s * s);

/**
 * @brief Compute the statistics over an array.
 *
 * @param s The statistics instance.
 * @param x The value array.
 * @param length The number of elements in x.
 *
 * Use the "traditional" two pass method.  Compute mean in first pass,
 * then variance in second pass.
 */
JLS_API void jls_statistics_compute_f32(struct jls_statistics_s * s, const float * x, uint64_t length);

/**
 * @brief Compute the statistics over an array.
 *
 * @param s The statistics instance.
 * @param x The value array.
 * @param length The number of elements in x.
 */
JLS_API void jls_statistics_compute_f64(struct jls_statistics_s * s, const double * x, uint64_t length);

/**
 * @brief Add a new sample into the statistics.
 *
 * @param s The statistics instance.
 * @param x The new value.
 */
JLS_API void jls_statistics_add(struct jls_statistics_s * s, double x);

/**
 * @brief Get the sample variance.
 *
 * @param s The statistics instance.
 * @return The sample variance
 *
 * Sample variance uses k-1 denominator, also called the Bessel correction,
 * which is what you want for estimating variance from samples.
 * "Standard" population variance uses k as the denominator which tends to
 * underestimate true variance.
 */
JLS_API double jls_statistics_var(struct jls_statistics_s * s);

/**
 * @brief Copy one statistics instance to another.
 *
 * @param tgt The target statistics instance.
 * @param src The source statistics instance.
 */
JLS_API void jls_statistics_copy(struct jls_statistics_s * tgt, struct jls_statistics_s const * src);

/**
 * @brief Compute the combined statistics over two statistics instances.
 *
 * @param tgt The target statistics instance.  It is safe to use a or b for tgt.
 * @param a The first statistics instance to combine.
 * @param b The first statistics instance to combine.
 */
JLS_API void jls_statistics_combine(struct jls_statistics_s * tgt,
                                    struct jls_statistics_s const * a,
                                    struct jls_statistics_s const * b);

JLS_CPP_GUARD_END

/** @} */

#endif  // JLS_STATISTICS_H
