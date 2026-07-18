#include <stdio.h>
#include <string.h>
#include "tiered_sched.h"

TV_MAP tv_map_logical(uint64_t logical, TV_METADATA *meta) {
    TV_MAP map = {0, 0, 0};
    if (!meta || meta->segment_count == 0) return map;

    /* Find segment by linear scan (could be binary search for large seg counts) */
    int seg_idx = -1;
    for (int i = 0; i < (int)meta->segment_count; i++) {
        if (logical >= meta->segments[i].logical_begin &&
            logical <  meta->segments[i].logical_end) {
            seg_idx = i;
            break;
        }
    }

    /* If not found, use last segment */
    if (seg_idx < 0) seg_idx = meta->segment_count - 1;

    TV_SEGMENT *seg = &meta->segments[seg_idx];

    uint64_t stripe_no = logical / seg->stripe_size;
    uint64_t offset_in = logical % seg->stripe_size;

    /* Build prefix sum boundary */
    uint64_t boundary[TV_MAX_DISKS + 1];
    boundary[0] = 0;
    for (int i = 0; i < (int)seg->disk_count; i++) {
        boundary[i + 1] = boundary[i] + (uint64_t)seg->weight[i] * TV_CHUNK_SIZE;
    }

    /* Find which disk this offset falls into */
    int disk_idx = 0;
    for (int i = 0; i < (int)seg->disk_count; i++) {
        if (offset_in >= boundary[i] && offset_in < boundary[i + 1]) {
            disk_idx = i;
            break;
        }
    }

    map.disk = (int)seg->disk_index[disk_idx];
    map.offset = stripe_no * (uint64_t)seg->weight[disk_idx] * TV_CHUNK_SIZE
               + (offset_in - boundary[disk_idx]);
    map.length = (uint64_t)seg->weight[disk_idx] * TV_CHUNK_SIZE;

    return map;
}

uint64_t tv_map_reverse(int disk_index, uint64_t disk_offset, TV_METADATA *meta) {
    if (!meta || meta->segment_count == 0) return 0;

    for (int i = 0; i < (int)meta->segment_count; i++) {
        TV_SEGMENT *seg = &meta->segments[i];

        /* Check if this disk is in this segment */
        int disk_pos = -1;
        for (int j = 0; j < (int)seg->disk_count; j++) {
            if ((int)seg->disk_index[j] == disk_index) {
                disk_pos = j;
                break;
            }
        }
        if (disk_pos < 0) continue;

        uint64_t chunk_bytes = (uint64_t)seg->weight[disk_pos] * TV_CHUNK_SIZE;
        if (chunk_bytes == 0) continue;

        uint64_t stripe_no = disk_offset / chunk_bytes;
        uint64_t off_in_chunk = disk_offset % chunk_bytes;

        /* Rebuild boundary */
        uint64_t boundary[TV_MAX_DISKS + 1];
        boundary[0] = 0;
        for (int j = 0; j < (int)seg->disk_count; j++) {
            boundary[j + 1] = boundary[j] + (uint64_t)seg->weight[j] * TV_CHUNK_SIZE;
        }

        return seg->logical_begin + stripe_no * seg->stripe_size
             + boundary[disk_pos] + off_in_chunk;
    }

    return 0;
}
