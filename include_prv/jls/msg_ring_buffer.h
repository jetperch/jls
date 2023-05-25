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

/**
 * @file
 *
 * @brief Message ring buffer to support the data_link implementation.
 */

#ifndef JLS_STREAM_MESSAGE_RING_BUFFER_H__
#define JLS_STREAM_MESSAGE_RING_BUFFER_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * @ingroup jls
 * @defgroup jls_mrb Ring buffer for variable-length messages.
 *
 * @brief Provide a simple ring buffer for first-in, first-out messages.
 *
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/// The message ring buffer instance.
struct jls_mrb_s {
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;    ///< number of messages
    uint8_t * buf;
    uint32_t buf_size;  // Size of buf in bytes
};

/**
 * @brief Initialize the message ring buffer.
 *
 * @param self The ring buffer instance.
 * @param buffer The underlying memory to use for this buffer.
 * @param buffer_size The size of buffer in bytes.
 */
void jls_mrb_init(struct jls_mrb_s * self, uint8_t * buffer, uint32_t buffer_size);

/**
 * @brief Clear all data from the memory buffer.
 *
 * @param self The ring buffer instance.
 */
void jls_mrb_clear(struct jls_mrb_s * self);

/**
 * @brief Get the amount of buffer used, in bytes.
 * @param self The ring buffer instance.
 * @return The number of bytes used.
 * @see self->count for the number of messages.
 */
uint32_t jls_mrb_used_bytes(struct jls_mrb_s * self);

/**
 * @brief Allocate a message on the ring buffer.
 *
 * @param self The ring buffer instance.
 * @param size The desired size of the buffer.
 * @return The buffer or NULL on out of space.
 */
uint8_t * jls_mrb_alloc(struct jls_mrb_s * self, uint32_t size);

/**
 * @brief Peek at the next message from the buffer.
 *
 * @param self The message ring buffer instance.
 * @param[out] size The size of buffer in bytes.
 * @return The buffer or NULL on empty.
 */
uint8_t * jls_mrb_peek(struct jls_mrb_s * self, uint32_t * size);

/**
 * @brief Pop the next message from the buffer.
 *
 * @param self The message ring buffer instance.
 * @param[out] size The size of buffer in bytes.
 * @return The buffer or NULL on empty.
 */
uint8_t * jls_mrb_pop(struct jls_mrb_s * self, uint32_t * size);


#ifdef __cplusplus
}
#endif

/** @} */

#endif  /* JLS_STREAM_MESSAGE_RING_BUFFER_H__ */
