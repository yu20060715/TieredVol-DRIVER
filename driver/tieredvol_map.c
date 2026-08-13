// SPDX-License-Identifier: GPL-2.0-only
#include "tieredvol.h"
#include <linux/random.h>

/* Binary search for segment containing logical byte offset.
 * Segments must be sorted by logical_begin (validated in constructor).
 * Returns segment index, or -1 if not found.
 */
static int tv_find_segment(u64 logical, const struct tieredvol_metadata *meta)
{
	int lo = 0, hi = (int)meta->segment_count - 1;

	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		const struct tieredvol_segment *seg = &meta->segments[mid];

		if (logical < seg->logical_begin)
			hi = mid - 1;
		else if (logical >= seg->logical_end)
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

struct tieredvol_map tv_map_logical(u64 logical,
				    struct tieredvol_metadata *meta,
				    u32 chunk_size)
{
	struct tieredvol_map err = { .disk = -1, .offset = 0, .length = 0 };
	int seg_idx, disk_idx;
	const struct tieredvol_segment *seg;
	u64 stripe_no, offset_in;
	u64 boundary[TV_MAX_DISKS + 1];
	int i;

	if (!meta || meta->segment_count == 0)
		return err;

	seg_idx = tv_find_segment(logical, meta);
	if (seg_idx < 0)
		return err;

	seg = &meta->segments[seg_idx];

	if (seg->disk_count == 0 || seg->disk_count > TV_MAX_DISKS)
		return err;

	stripe_no = (logical - seg->logical_begin) / seg->stripe_size;
	offset_in = (logical - seg->logical_begin) % seg->stripe_size;

	boundary[0] = 0;
	for (i = 0; i < (int)seg->disk_count; i++)
		boundary[i + 1] = boundary[i] +
			(u64)seg->weight[i] * chunk_size;

	disk_idx = -1;
	for (i = 0; i < (int)seg->disk_count; i++) {
		if (offset_in >= boundary[i] && offset_in < boundary[i + 1]) {
			disk_idx = i;
			break;
		}
	}

	if (disk_idx < 0)
		return err;

	{
		struct tieredvol_map map;

		map.disk = (int)seg->disk_index[disk_idx];
		map.seg_idx = seg_idx;
		map.offset = stripe_no * (u64)seg->weight[disk_idx] *
			     chunk_size +
			     (offset_in - boundary[disk_idx]);
		map.length = (u64)seg->weight[disk_idx] * chunk_size;

		return map;
	}
}

struct tieredvol_map tv_map_logical_random(u64 logical,
					  struct tieredvol_metadata *meta,
					  u32 chunk_size)
{
	struct tieredvol_map err = { .disk = -1, .offset = 0, .length = 0 };
	int seg_idx;
	const struct tieredvol_segment *seg;
	u64 stripe_no, offset_in;
	int disk_idx;

	if (!meta || meta->segment_count == 0)
		return err;

	seg_idx = tv_find_segment(logical, meta);
	if (seg_idx < 0)
		return err;

	seg = &meta->segments[seg_idx];

	if (seg->disk_count == 0 || seg->disk_count > TV_MAX_DISKS)
		return err;

	stripe_no = (logical - seg->logical_begin) / seg->stripe_size;
	offset_in = (logical - seg->logical_begin) % seg->stripe_size;

	/* Deterministic per-stripe selection (stripe_no % disk_count) so a
	 * given logical offset always maps to the same disk on both reads
	 * and writes. A true get_random_u32() selection here would map the
	 * same offset to different disks and corrupt data on rewrite.
	 */
	disk_idx = (int)(stripe_no % seg->disk_count);

	{
		struct tieredvol_map map;
		u64 disk_chunk = (u64)seg->weight[disk_idx] * chunk_size;

		map.disk = (int)seg->disk_index[disk_idx];
		map.seg_idx = seg_idx;
		map.offset = stripe_no * disk_chunk +
			     (offset_in % disk_chunk);
		map.length = disk_chunk;

		return map;
	}
}
