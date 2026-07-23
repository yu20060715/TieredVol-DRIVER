#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device-mapper.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include "tieredvol.h"

#define DM_MSG_PREFIX "tieredvol"

static int tieredvol_split_and_submit(struct bio *bio, struct tieredvol_ctx *ctx,
				      struct tieredvol_map *m)
{
	sector_t remaining = bio_sectors(bio);

	while (remaining > 0) {
		struct tieredvol_map cur;
		u64 logical = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;
		u64 split_sectors;
		struct bio *split;

		cur = tv_map_logical(logical, &ctx->meta);
		if (cur.disk < 0 || cur.disk >= ctx->ndisks) {
			pr_err("tieredvol: map failed for sector %llu\n",
			       (unsigned long long)bio->bi_iter.bi_sector);
			return -EIO;
		}

		split_sectors = cur.remaining >> SECTOR_SHIFT;
		if (split_sectors == 0) {
			pr_err("tieredvol: zero remaining for sector %llu\n",
			       (unsigned long long)bio->bi_iter.bi_sector);
			return -EIO;
		}

		if (split_sectors >= remaining) {
			bio_set_dev(bio, ctx->devs[cur.disk]->bdev);
			bio->bi_iter.bi_sector =
				cur.offset >> SECTOR_SHIFT;
			submit_bio_noacct(bio);
			return 0;
		}

		split = bio_split(bio, split_sectors, GFP_NOIO, &ctx->fs);
		if (!split) {
			pr_err("tieredvol: bio_split failed\n");
			return -ENOMEM;
		}

		bio_set_dev(split, ctx->devs[cur.disk]->bdev);
		split->bi_iter.bi_sector = cur.offset >> SECTOR_SHIFT;
		submit_bio_noacct(split);

		remaining -= split_sectors;
	}

	return 0;
}

static int tieredvol_map(struct dm_target *ti, struct bio *bio)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (bio->bi_opf & REQ_PREFLUSH || bio->bi_opf & REQ_FUA) {
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	return tieredvol_split_and_submit(bio, ctx, NULL);
}

static int tieredvol_ctr(struct dm_target *ti, unsigned int argc,
			 char **argv)
{
	struct tieredvol_ctx *ctx;
	int ret, i;

	if (argc != 1) {
		ti->error = "tieredvol: expected 1 argument (config path)";
		return -EINVAL;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ti->error = "tieredvol: out of memory";
		return -ENOMEM;
	}

	ctx->ti = ti;
	spin_lock_init(&ctx->map_lock);

	ret = bioset_init(&ctx->fs, 64, 0, 0);
	if (ret) {
		ti->error = "tieredvol: bioset_init failed";
		goto free_ctx;
	}

	ret = tv_metadata_load_kernel(&ctx->meta, argv[0]);
	if (ret) {
		ti->error = "tieredvol: failed to load metadata";
		goto exit_bioset;
	}

	if (ctx->meta.disk_count == 0 || ctx->meta.disk_count > TV_MAX_DISKS) {
		ti->error = "tieredvol: invalid disk count";
		ret = -EINVAL;
		goto exit_bioset;
	}

	ctx->ndisks = ctx->meta.disk_count;

	ctx->devs = kcalloc(ctx->ndisks, sizeof(*ctx->devs), GFP_KERNEL);
	ctx->disk_sectors = kcalloc(ctx->ndisks, sizeof(*ctx->disk_sectors),
				    GFP_KERNEL);
	if (!ctx->devs || !ctx->disk_sectors) {
		ti->error = "tieredvol: out of memory for devs";
		ret = -ENOMEM;
		goto free_devs;
	}

	for (i = 0; i < ctx->ndisks; i++) {
		ret = dm_get_device(ti, ctx->meta.disk_names[i],
				    dm_table_get_mode(ti->table),
				    &ctx->devs[i]);
		if (ret) {
			ti->error = "tieredvol: device lookup failed";
			goto put_devices;
		}
		ctx->disk_sectors[i] = bdev_nr_sectors(ctx->devs[i]->bdev);
	}

	for (i = 0; i < ctx->ndisks; i++)
		pr_info("tieredvol: disk[%d] %s -> %pg (%llu sectors)\n",
			i, ctx->meta.disk_names[i], ctx->devs[i]->bdev,
			(unsigned long long)ctx->disk_sectors[i]);

	if (ctx->meta.segment_count == 0) {
		ti->error = "tieredvol: no segments";
		ret = -EINVAL;
		goto put_devices;
	}

	for (i = 0; i < (int)ctx->meta.segment_count; i++)
		pr_info("tieredvol: segment[%d] [%llu, %llu) stripe=%llu disks=%u\n",
			i,
			(unsigned long long)ctx->meta.segments[i].logical_begin,
			(unsigned long long)ctx->meta.segments[i].logical_end,
			(unsigned long long)ctx->meta.segments[i].stripe_size,
			ctx->meta.segments[i].disk_count);

	ti->private = ctx;
	return 0;

put_devices:
	for (i = i - 1; i >= 0; i--)
		dm_put_device(ti, ctx->devs[i]);
free_devs:
	kfree(ctx->devs);
	kfree(ctx->disk_sectors);
exit_bioset:
	bioset_exit(&ctx->fs);
free_ctx:
	kfree(ctx);
	return ret;
}

static void tieredvol_dtr(struct dm_target *ti)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	for (i = 0; i < ctx->ndisks; i++)
		dm_put_device(ti, ctx->devs[i]);

	kfree(ctx->devs);
	kfree(ctx->disk_sectors);
	bioset_exit(&ctx->fs);
	kfree(ctx);
}

static void tieredvol_io_hints(struct dm_target *ti,
			       struct queue_limits *limits)
{
	limits->logical_block_size = 512;
	limits->physical_block_size = 512;
	limits->io_min = 512;
}

static int tieredvol_iterate_devices(struct dm_target *ti,
				     iterate_devices_callout_fn fn,
				     void *data)
{
	struct tieredvol_ctx *ctx = ti->private;
	int ret = 0;
	int i;

	for (i = 0; !ret && i < ctx->ndisks; i++)
		ret = fn(ti, ctx->devs[i], 0,
			 bdev_nr_sectors(ctx->devs[i]->bdev), data);

	return ret;
}

static void tieredvol_status(struct dm_target *ti, status_type_t type,
			     unsigned int status_flags, char *result,
			     unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	switch (type) {
	case STATUSTYPE_INFO:
		snprintf(result, maxlen, "%u disks %u segments",
			 ctx->ndisks, ctx->meta.segment_count);
		break;
	case STATUSTYPE_TABLE:
		snprintf(result, maxlen, "%s", ctx->meta.disk_names[0]);
		break;
	case STATUSTYPE_IMA:
		result[0] = '\0';
		break;
	}
}

static int tieredvol_message(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	if (argc == 1 && strcmp(argv[0], "status") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++)
			off += snprintf(result + off, maxlen - off,
					"disk[%d]=%s(w=%u) ",
					i, ctx->meta.disk_names[i],
					ctx->meta.segments[0].weight[i]);
		return 0;
	}

	return -EINVAL;
}

static struct target_type tieredvol_target = {
	.name   = "tieredvol",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr    = tieredvol_ctr,
	.dtr    = tieredvol_dtr,
	.map    = tieredvol_map,
	.status = tieredvol_status,
	.message = tieredvol_message,
	.io_hints = tieredvol_io_hints,
	.iterate_devices = tieredvol_iterate_devices,
};

static int __init tieredvol_init(void)
{
	int ret;

	ret = dm_register_target(&tieredvol_target);
	if (ret < 0) {
		pr_err("tieredvol: registration failed: %d\n", ret);
		return ret;
	}

	pr_info("tieredvol: module loaded\n");
	return 0;
}

static void __exit tieredvol_exit(void)
{
	dm_unregister_target(&tieredvol_target);
	pr_info("tieredvol: module unloaded\n");
}

module_init(tieredvol_init);
module_exit(tieredvol_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TieredVol");
MODULE_DESCRIPTION("Weighted striped dm target for tiered storage");
MODULE_VERSION("2.0.0");
