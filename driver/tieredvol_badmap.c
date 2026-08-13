// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bio.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/bitmap.h>
#include "tieredvol.h"

void tv_badmap_init(struct tieredvol_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->ndisks; i++) {
		u64 n_chunks;
		unsigned long *bmp;

		n_chunks = ctx->disk_sectors[i] / (ctx->meta.chunk_size >> 9);
		bmp = bitmap_zalloc(n_chunks, GFP_KERNEL);
		if (!bmp) {
			pr_warn("tieredvol: badmap alloc fail for disk[%d], %llu chunks\n",
				i, n_chunks);
			ctx->badmaps[i].bitmap = NULL;
			ctx->badmaps[i].n_chunks = 0;
			continue;
		}
		ctx->badmaps[i].bitmap = bmp;
		ctx->badmaps[i].n_chunks = n_chunks;
	}
}

void tv_badmap_destroy(struct tieredvol_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->ndisks; i++) {
		bitmap_free(ctx->badmaps[i].bitmap);
		ctx->badmaps[i].bitmap = NULL;
		ctx->badmaps[i].n_chunks = 0;
	}
}

bool tv_badmap_test(struct tieredvol_ctx *ctx, int disk, u64 chunk_no)
{
	if (disk < 0 || disk >= ctx->ndisks)
		return false;
	if (!ctx->badmaps[disk].bitmap)
		return false;
	if (chunk_no >= ctx->badmaps[disk].n_chunks)
		return false;
	return test_bit(chunk_no, ctx->badmaps[disk].bitmap);
}

void tv_badmap_set(struct tieredvol_ctx *ctx, int disk, u64 chunk_no)
{
	if (disk < 0 || disk >= ctx->ndisks)
		return;
	if (!ctx->badmaps[disk].bitmap)
		return;
	if (chunk_no >= ctx->badmaps[disk].n_chunks)
		return;
	set_bit(chunk_no, ctx->badmaps[disk].bitmap);
}

void tv_badmap_clear(struct tieredvol_ctx *ctx, int disk, u64 chunk_no)
{
	if (disk < 0 || disk >= ctx->ndisks)
		return;
	if (!ctx->badmaps[disk].bitmap)
		return;
	if (chunk_no >= ctx->badmaps[disk].n_chunks)
		return;
	clear_bit(chunk_no, ctx->badmaps[disk].bitmap);
}

static void tv_badmap_rebuild_endio(struct bio *bio)
{
	complete(bio->bi_private);
}

void tv_badmap_rebuild(struct tieredvol_ctx *ctx)
{
	int d, recovered = 0, failed = 0;
	unsigned int chunk_bytes = ctx->meta.chunk_size;
	struct page *pg;

	pg = alloc_pages(GFP_NOIO, get_order(chunk_bytes));
	if (!pg) {
		pr_err("tieredvol: rebuild OOM\n");
		return;
	}

	for (d = 0; d < ctx->ndisks; d++) {
		u64 chunk;

		if (!ctx->badmaps[d].bitmap)
			continue;

		for (chunk = 0; chunk < ctx->badmaps[d].n_chunks; chunk++) {
			struct bio *bio;
			struct completion done;
			sector_t sector = (chunk * chunk_bytes) >> 9;

			if (!test_bit(chunk, ctx->badmaps[d].bitmap))
				continue;

			init_completion(&done);
			bio = bio_alloc(ctx->devs[d]->bdev, 1,
					REQ_OP_READ, GFP_NOIO);
			if (!bio) {
				clear_bit(chunk, ctx->badmaps[d].bitmap);
				failed++;
				continue;
			}
			bio->bi_iter.bi_sector = sector;
			bio->bi_private = &done;
			bio->bi_end_io = tv_badmap_rebuild_endio;
			if (bio_add_page(bio, pg, chunk_bytes, 0) !=
			    chunk_bytes) {
				bio_put(bio);
				clear_bit(chunk, ctx->badmaps[d].bitmap);
				failed++;
				continue;
			}
			submit_bio(bio);
			wait_for_completion(&done);

			if (bio->bi_status == BLK_STS_OK) {
				clear_bit(chunk, ctx->badmaps[d].bitmap);
				recovered++;
			} else {
				clear_bit(chunk, ctx->badmaps[d].bitmap);
				tv_log(TV_LOG_WARN, TV_LOG_RECOVER,
				       "bad chunk %llu lost", chunk);
				failed++;
			}
			bio_put(bio);

			cond_resched();
		}
	}

	put_page(pg);
	pr_info("tieredvol: badmap rebuild done: %d recovered, %d failed\n",
		recovered, failed);
}
