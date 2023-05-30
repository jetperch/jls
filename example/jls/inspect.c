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

#include "jls/raw.h"
#include "cstr.h"
#include "jls_util_prv.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>


#define PAYLOAD_MAX_SIZE (32U * 1024U * 1024U)


static int usage(void) {
    printf("usage: jls inspect <path> [--chunk <offset>] \n");
    return 1;
}

static void offset_display(const char * name, int64_t value) {
    printf("%s=0x%08" PRIx32 "_%08" PRIx32 "\n", name,
           (uint32_t) (value >> 32), (uint32_t) (value & 0xffffffff));
}

static void payload_header_printf(struct jls_payload_header_s * header) {
    printf("timestamp=%" PRIi64 "\n", header->timestamp);
    printf("entry_count=%" PRIu32 "\n", header->entry_count);
    printf("entry_size_bits=%" PRIu16 "\n", header->entry_size_bits);
}

static int32_t chunk_printf(struct jls_raw_s * raw, int64_t offset) {
    struct jls_chunk_header_s hdr;
    uint32_t payload_length_max = 0;
    ROE(jls_raw_chunk_seek(raw, offset));
    ROE(jls_raw_rd_header(raw, &hdr));

    offset_display("offset", offset);
    offset_display("prev", hdr.item_prev);
    offset_display("next", hdr.item_next);
    printf("length=0x%08" PRIu64 " %d\n", (uint64_t) hdr.payload_length, (int) hdr.payload_length);
    printf("tag=%s %d\n", jls_tag_to_name(hdr.tag), (int) hdr.tag);
    printf("chunk_meta=0x%04x %d.%d\n", hdr.chunk_meta,
           (int) (hdr.chunk_meta & 0x0ff), (int) ((hdr.chunk_meta >> 12) & 0x0f));
    payload_length_max = 0x10000 + hdr.payload_length;
    uint8_t * payload = malloc(payload_length_max);
    if (NULL == payload) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    ROE(jls_raw_rd_payload(raw, payload_length_max, payload));

    switch (hdr.tag) {
        case JLS_TAG_SOURCE_DEF: break;
        case JLS_TAG_SIGNAL_DEF: break;
        case JLS_TAG_TRACK_FSR_DEF: break;
        case JLS_TAG_TRACK_FSR_HEAD: break;
        case JLS_TAG_TRACK_FSR_DATA: {
            struct jls_fsr_data_s * d = (struct jls_fsr_data_s *) payload;
            payload_header_printf(&d->header);
            break;
        }
        case JLS_TAG_TRACK_FSR_INDEX: {
            struct jls_fsr_index_s * index = (struct jls_fsr_index_s *) payload;
            payload_header_printf(&index->header);
            for (uint32_t i = 0; i < index->header.entry_count; ++i) {
                uint64_t k = (uint64_t) index->offsets[i];
                printf("  %" PRIu32 " 0x%08" PRIx32 "_%08" PRIx32 "\n",
                        i, (uint32_t) (k >> 32), (uint32_t) (k & 0xffffffff));
            }
            break;
        }
        case JLS_TAG_TRACK_FSR_SUMMARY: {
            struct jls_fsr_f32_summary_s * s = (struct jls_fsr_f32_summary_s * ) payload;
            payload_header_printf(&s->header);
            break;
        }
        case JLS_TAG_TRACK_VSR_DEF: break;
        case JLS_TAG_TRACK_VSR_HEAD: break;
        case JLS_TAG_TRACK_VSR_DATA: break;
        case JLS_TAG_TRACK_VSR_INDEX: break;
        case JLS_TAG_TRACK_VSR_SUMMARY: break;
        case JLS_TAG_TRACK_ANNOTATION_DEF: break;
        case JLS_TAG_TRACK_ANNOTATION_HEAD: break;
        case JLS_TAG_TRACK_ANNOTATION_DATA: break;
        case JLS_TAG_TRACK_ANNOTATION_INDEX: break;
        case JLS_TAG_TRACK_ANNOTATION_SUMMARY: break;
        case JLS_TAG_TRACK_UTC_DEF: break;
        case JLS_TAG_TRACK_UTC_HEAD: break;
        case JLS_TAG_TRACK_UTC_DATA: break;
        case JLS_TAG_TRACK_UTC_INDEX: break;
        case JLS_TAG_TRACK_UTC_SUMMARY: break;
        case JLS_TAG_USER_DATA: break;
        case JLS_TAG_END: break;
        default: break;
    }
    free(payload);
    return 0;
}

int on_inspect(struct app_s * self, int argc, char * argv[]) {
    struct jls_raw_s * raw = NULL;
    int64_t offset;
    char * path = NULL;
    int pos_arg = 0;
    (void) self;

    while (argc) {
        if (argv[0][0] != '-') {
            if (pos_arg == 0) {
                path = argv[0];
                ROE(jls_raw_open(&raw, path, "r"));
            } else {
                return usage();
            }
            ARG_CONSUME();
            ++pos_arg;
        } else if (0 == strcmp(argv[0], "--chunk")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            ROE(jls_cstr_to_i64(argv[0], &offset));
            ARG_CONSUME();
            ROE(chunk_printf(raw, offset));
        } else {
            return usage();
        }
    }
    if (pos_arg != 1) {
        return usage();
    }

    jls_raw_close(raw);
    return 0;
}
