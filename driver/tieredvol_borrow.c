// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_borrow.c — Weight-borrowing for tiered striping.
 *
 * Static weighted striping keeps its deterministic mapping as the placement
 * authority. When a disk is backlogged (in_flight > watermark), chunks that
 * would be written to it are temporarily redirected ("borrowed") into the
 * least-loaded disk's over-provisioned borrow area. Every redirected chunk
 * is recorded in a persistent per-chunk table, so reads and re-writes
 * resolve to the same destination after a reload.
 *
 * This is what the removed ADAPTIVE policy lacked: a dynamic mapper with no
 * placement record silently returns different disks for the same logical
 * address on read (verified: verify=crc32c failed on write in the 8/13
 * regression), corrupting data. Borrowing never changes the mapping for a
 * chunk that was not redirected, and redirected chunks always resolve via
 * the table.
 */
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"

#define TV_BORROW_MAGIC 0x54564252U /* "TVBR" */
#define TV_BORROW_VERSION 2U

int tv_borrow_init(struct tieredvol_ctx *ctx)
{
	u64 seg_len = 0;
	u32 i;

	ctx->borrow.enabled = false;
	ctx->borrow.block_size = 0;
	spin_lock_init(&ctx->borrow.lock);
	memset(ctx->borrow.area_blocks, 0, sizeof(ctx->borrow.area_blocks));
	memset(ctx->borrow.used_blocks, 0, sizeof(ctx->borrow.used_blocks));
	memset(ctx->borrow.area_base_sector, 0,
	       sizeof(ctx->borrow.area_base_sector));
	memset(ctx->borrow.borrow_write_bytes, 0,
	       sizeof(ctx->borrow.borrow_write_bytes));
	ctx->borrow.entries = NULL;
	ctx->borrow.n_blocks = 0;
	ctx->borrow.n_borrowed = 0;
	ctx->borrow.watermark_bytes = 0;

	/* Single-segment only (all shipped configs use seg0). */
	if (ctx->meta.segment_count != 1)
		return 0;
	if (ctx->meta.segments[0].disk_count < 2)
		return 0;

	seg_len = ctx->meta.segments[0].logical_end -
		  ctx->meta.segments[0].logical_begin;
	if (seg_len == 0 || seg_len % ctx->meta.chunk_size != 0)
		return 0;

	if (ctx->meta.runtime_borrow_enable != 1)
		return 0;

	/* Mirror and borrow both overlay placement; keep them exclusive. */
	if (ctx->meta.segments[0].mirror_enabled)
		return 0;

	/* Borrow granularity: the block layer caps bios at 128 KB on this
	 * box (max_sectors = 256), so require chunk-aligned requests would
	 * never trigger. Track placement at block_size = chunk/8 instead;
	 * a redirect always covers the whole block, so a borrowed block is
	 * always fully written and reads resolve to a complete copy.
	 */
	ctx->borrow.block_size = ctx->meta.chunk_size >> 3;
	if (ctx->borrow.block_size == 0 ||
	    seg_len % ctx->borrow.block_size != 0)
		return 0;
	ctx->borrow.n_blocks = seg_len / ctx->borrow.block_size;
	ctx->borrow.watermark_bytes =
		ctx->meta.runtime_borrow_watermark_kb ?
		ctx->meta.runtime_borrow_watermark_kb * 1024U :
		ctx->borrow.block_size * 4U;

	for (i = 0; i < ctx->ndisks; i++) {
		u32 mb = ctx->meta.runtime_borrow_area_mb[i];
		u64 avail_sectors = ctx->disk_sectors[i];
		u64 area_sectors;
		u32 block_sectors = ctx->borrow.block_size >>
				    TV_SECTOR_SHIFT;

		if (mb == 0)
			continue;
		area_sectors = (u64)mb << 20 >> TV_SECTOR_SHIFT;
		if (area_sectors >= avail_sectors)
			continue;
		ctx->borrow.area_blocks[i] =
			(u32)(area_sectors / block_sectors);
		ctx->borrow.area_base_sector[i] =
			avail_sectors - area_sectors;
	}

	ctx->borrow.entries = kcalloc(ctx->borrow.n_blocks,
				      sizeof(struct tv_borrow_entry),
				      GFP_KERNEL);
	if (!ctx->borrow.entries)
		return -ENOMEM;

	ctx->borrow.enabled = true;

	tv_borrow_load(ctx, ctx->config_path, ctx->borrow.n_blocks);

	pr_info("tieredvol: borrow enabled block=%uB watermark=%uB n_blocks=%llu\n",
		ctx->borrow.block_size, ctx->borrow.watermark_bytes,
		(unsigned long long)ctx->borrow.n_blocks);
	for (i = 0; i < ctx->ndisks; i++)
		pr_info("tieredvol:   borrow disk[%u]=%u blocks (base sector %llu)\n",
			i, ctx->borrow.area_blocks[i],
			(unsigned long long)ctx->borrow.area_base_sector[i]);

	return 0;
}

void tv_borrow_destroy(struct tieredvol_ctx *ctx)
{
	if (ctx->borrow.enabled) {
		tv_borrow_save(ctx);
		ctx->borrow.enabled = false;
	}
	kfree(ctx->borrow.entries);
	ctx->borrow.entries = NULL;
}

bool tv_borrow_lookup(struct tieredvol_ctx *ctx, u64 logical,
		      int *disk, u64 *sector)
{
	u64 block;
	struct tv_borrow_entry *e;
	unsigned long flags;

	if (!ctx->borrow.entries)
		return false;
	if (logical < ctx->meta.segments[0].logical_begin)
		return false;
	block = (logical - ctx->meta.segments[0].logical_begin) /
		ctx->borrow.block_size;
	if (block >= ctx->borrow.n_blocks)
		return false;

	spin_lock_irqsave(&ctx->borrow.lock, flags);
	e = &ctx->borrow.entries[block];
	if (e->valid) {
		*disk = e->dst_disk;
		*sector = e->dst_sector +
			  ((logical % ctx->borrow.block_size) >>
			   TV_SECTOR_SHIFT);
		spin_unlock_irqrestore(&ctx->borrow.lock, flags);
		return true;
	}
	spin_unlock_irqrestore(&ctx->borrow.lock, flags);
	return false;
}

static int tv_borrow_pick_dst(struct tieredvol_ctx *ctx, int src_disk,
			      u32 need_blocks)
{
	int best = -1;
	u32 best_load = (u32)-1;
	int i;

	for (i = 0; i < ctx->ndisks; i++) {
		u32 load;
		u32 avail;

		if (i == src_disk)
			continue;
		if (ctx->deg.degraded[i])
			continue;
		avail = ctx->borrow.area_blocks[i] -
			ctx->borrow.used_blocks[i];
		if (need_blocks > avail)
			continue;
		load = (u32)atomic_read(&ctx->io.in_flight_bytes[i]);
		if (load < best_load) {
			best_load = load;
			best = i;
		}
	}
	return best;
}

/* Redirect a WRITE range away from a backlogged disk. Returns true when
 * the caller should submit to *disk at *sector instead.
 *
 * A range that is already fully borrowed always redirects (re-write path,
 * any length/offset). A range covering blocks not yet borrowed requires the
 * whole range to be block-aligned and allocates new borrow-area slots for
 * every unborrowed block in it (all-or-none; any shortfall keeps the range
 * on the original disk). Each borrow-area slot holds exactly one borrow
 * block, so a redirected block is always fully written and safe to read.
 */
bool tv_borrow_redirect(struct tieredvol_ctx *ctx, int src_disk,
			u64 logical, u64 length,
			int *disk, u64 *sector)
{
	u64 off;
	u64 block;
	u32 n_blocks;
	u32 need = 0;
	int dst = -1;
	u32 block_sectors = ctx->borrow.block_size >> TV_SECTOR_SHIFT;
	u32 slot;
	unsigned long flags;
	u64 c;

	if (!ctx->borrow.entries)
		return false;
	if (length == 0)
		return false;
	if (logical < ctx->meta.segments[0].logical_begin)
		return false;
	off = logical - ctx->meta.segments[0].logical_begin;
	block = off / ctx->borrow.block_size;
	n_blocks = (u32)(((off + length - 1) / ctx->borrow.block_size) -
			 block + 1);
	if (block + n_blocks > ctx->borrow.n_blocks)
		return false;

	spin_lock_irqsave(&ctx->borrow.lock, flags);
	for (c = block; c < block + n_blocks; c++) {
		if (!ctx->borrow.entries[c].valid)
			need++;
	}

	if (need == 0) {
		/* Already redirected: reuse recorded destination(s). */
		dst = ctx->borrow.entries[block].dst_disk;
		for (c = block + 1; c < block + n_blocks; c++) {
			if (ctx->borrow.entries[c].dst_disk != dst) {
				spin_unlock_irqrestore(&ctx->borrow.lock, flags);
				return false;
			}
		}
		*disk = dst;
		*sector = ctx->borrow.entries[block].dst_sector +
			  ((logical % ctx->borrow.block_size) >>
			   TV_SECTOR_SHIFT);
		spin_unlock_irqrestore(&ctx->borrow.lock, flags);
		return true;
	}

	/* New borrows only when borrow is enabled, the source is backlogged
	 * and the range covers whole blocks (no holes in the borrow-area
	 * copy). */
	if (!ctx->borrow.enabled)
		return false;
	if ((u32)atomic_read(&ctx->io.in_flight_bytes[src_disk]) <
	    ctx->borrow.watermark_bytes) {
		spin_unlock_irqrestore(&ctx->borrow.lock, flags);
		return false;
	}
	if (logical % ctx->borrow.block_size != 0 ||
	    length % ctx->borrow.block_size != 0) {
		spin_unlock_irqrestore(&ctx->borrow.lock, flags);
		return false;
	}

	dst = tv_borrow_pick_dst(ctx, src_disk, need);
	if (dst < 0) {
		spin_unlock_irqrestore(&ctx->borrow.lock, flags);
		return false;
	}

	slot = ctx->borrow.used_blocks[dst];
	for (c = block; c < block + n_blocks; c++) {
		struct tv_borrow_entry *e = &ctx->borrow.entries[c];

		if (e->valid)
			continue;
		e->valid = 1;
		e->dst_disk = dst;
		e->dst_sector = ctx->borrow.area_base_sector[dst] +
				 ((u64)slot * block_sectors);
		slot++;
	}
	ctx->borrow.used_blocks[dst] = slot;
	ctx->borrow.n_borrowed += need;
	spin_unlock_irqrestore(&ctx->borrow.lock, flags);

	atomic64_add(length, &ctx->borrow.borrow_write_bytes[dst]);
	*disk = dst;
	*sector = ctx->borrow.area_base_sector[dst] +
		  ((u64)(slot - need) * block_sectors);
	return true;
}

/* ---- Persistence: binary file "<config>.borrow" ---- */

static void tv_borrow_path(const struct tieredvol_ctx *ctx, char *buf,
			   size_t len)
{
	snprintf(buf, len, "%s.borrow", ctx->config_path);
}

int tv_borrow_save(struct tieredvol_ctx *ctx)
{
	char path[272];
	struct file *f;
	loff_t pos = 0;
	size_t entry_bytes;
	void *snapshot;
	u32 magic = TV_BORROW_MAGIC;
	u32 version = TV_BORROW_VERSION;
	u64 n;
	ssize_t w;
	int ret = 0;
	unsigned long flags;

	if (!ctx->borrow.enabled || !ctx->borrow.entries)
		return 0;

	tv_borrow_path(ctx, path, sizeof(path));
	entry_bytes = ctx->borrow.n_blocks *
		      sizeof(struct tv_borrow_entry);

	snapshot = kzalloc(entry_bytes, GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	/* Snapshot under the lock; the file I/O below must run unlocked —
	 * filp_open/kernel_write sleep, so doing them while holding the
	 * spinlock (irqs disabled) wedged the machine (sleep-while-atomic).
	 */
	spin_lock_irqsave(&ctx->borrow.lock, flags);
	memcpy(snapshot, ctx->borrow.entries, entry_bytes);
	n = ctx->borrow.n_blocks;
	spin_unlock_irqrestore(&ctx->borrow.lock, flags);

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(f)) {
		ret = PTR_ERR(f);
		goto out;
	}
	if (kernel_write(f, &magic, sizeof(magic), &pos) != sizeof(magic) ||
	    kernel_write(f, &version, sizeof(version), &pos) != sizeof(version) ||
	    kernel_write(f, &n, sizeof(n), &pos) != sizeof(n)) {
		ret = -EIO;
	} else {
		w = kernel_write(f, snapshot, entry_bytes, &pos);
		ret = (w == (ssize_t)entry_bytes) ? 0 : -EIO;
	}
	if (ret == 0)
		vfs_fsync(f, 1);
	filp_close(f, NULL);
out:
	kfree(snapshot);
	if (ret)
		pr_warn("tieredvol: borrow save failed: %d\n", ret);
	else
		pr_info("tieredvol: borrow table saved to %s (%zu B)\n",
			path, entry_bytes + 16);
	return ret;
}

int tv_borrow_load(struct tieredvol_ctx *ctx, const char *path_in,
		   u64 n_blocks)
{
	char path[272];
	struct file *f;
	u32 magic, version;
	u64 n;
	loff_t pos = 0;
	int i;

	if (!ctx->borrow.enabled || !ctx->borrow.entries)
		return 0;
	if (n_blocks != ctx->borrow.n_blocks)
		return 0;

	snprintf(path, sizeof(path), "%s.borrow", path_in);
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return 0; /* no saved table: fresh volume */

	if (kernel_read(f, &magic, sizeof(magic), &pos) != sizeof(magic) ||
	    magic != TV_BORROW_MAGIC)
		goto bad;
	if (kernel_read(f, &version, sizeof(version), &pos) != sizeof(version) ||
	    version != TV_BORROW_VERSION)
		goto bad;
	if (kernel_read(f, &n, sizeof(n), &pos) != sizeof(n) ||
	    n != ctx->borrow.n_blocks)
		goto bad;
	if (kernel_read(f, ctx->borrow.entries,
			n * sizeof(struct tv_borrow_entry), &pos) !=
	    (ssize_t)(n * sizeof(struct tv_borrow_entry)))
		goto bad;
	filp_close(f, NULL);

	/* Rebuild per-disk used-block watermark. */
	for (i = 0; i < ctx->ndisks; i++)
		ctx->borrow.used_blocks[i] = 0;
	{
		u32 block_sectors = ctx->borrow.block_size >>
				    TV_SECTOR_SHIFT;
		u64 c;

		for (c = 0; c < ctx->borrow.n_blocks; c++) {
			struct tv_borrow_entry *e = &ctx->borrow.entries[c];
			u32 slot;

			if (!e->valid)
				continue;
			/* Corrupt/out-of-range entry: drop it rather than
			 * letting the runtime deref an invalid dst_disk or
			 * map onto the wrong area.
			 */
			if (e->dst_disk >= ctx->ndisks ||
			    e->dst_sector <
				    ctx->borrow.area_base_sector[e->dst_disk]) {
				e->valid = 0;
				continue;
			}
			slot = (u32)((e->dst_sector -
				      ctx->borrow.area_base_sector[e->dst_disk]) /
				     block_sectors);
			if (slot >= ctx->borrow.area_blocks[e->dst_disk]) {
				e->valid = 0;
				continue;
			}
			if (slot >= ctx->borrow.used_blocks[e->dst_disk])
				ctx->borrow.used_blocks[e->dst_disk] = slot + 1;
			ctx->borrow.n_borrowed++;
		}
	}
	pr_info("tieredvol: borrow table loaded (%llu blocks)\n",
		(unsigned long long)ctx->borrow.n_borrowed);
	return 0;
bad:
	filp_close(f, NULL);
	pr_warn("tieredvol: borrow table %s invalid (magic/version/format), "
		"ignoring (fresh volume)\n", path);
	return 0;
}
