// SPDX-License-Identifier: GPL-2.0-only
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/crc32c.h>
#include "tieredvol.h"

#define TV_MAX_CONFIG_SIZE (1024 * 1024)

static int parse_u32(const char *s, u32 *out)
{
	unsigned long v;

	if (kstrtoul(s, 10, &v))
		return -EINVAL;
	if (v > (~0U))
		return -EINVAL;
	*out = (u32)v;
	return 0;
}

static int parse_u64(const char *s, u64 *out)
{
	unsigned long long v;

	if (kstrtoull(s, 10, &v))
		return -EINVAL;
	*out = v;
	return 0;
}

static int parse_line(char *line, char **key, char **val)
{
	char *eq;

	eq = strchr(line, '=');
	if (!eq)
		return -EINVAL;
	*eq = '\0';
	*key = line;
	*val = eq + 1;

	{
		char *nl = strchr(*val, '\n');

		if (nl)
			*nl = '\0';
		nl = strchr(*val, '\r');
		if (nl)
			*nl = '\0';
	}
	return 0;
}

static int parse_csv_u32(char *s, u32 *arr, int max, int *count)
{
	char *tok;
	int n = 0;

	for (tok = strsep(&s, ","); tok && n < max; tok = strsep(&s, ",")) {
		unsigned long v;

		if (kstrtoul(tok, 10, &v))
			return -EINVAL;
		if (v > (~0U))
			return -EINVAL;
		arr[n++] = (u32)v;
	}
	*count = n;
	return 0;
}

static int parse_num_prefix(const char *s, unsigned long *idx,
			    const char **suffix)
{
	const char *p = s;
	unsigned long val = 0;
	int found_digit = 0;

	while (*p >= '0' && *p <= '9') {
		val = val * 10 + (*p - '0');
		found_digit = 1;
		p++;
	}

	if (!found_digit)
		return -EINVAL;

	*idx = val;
	*suffix = p;
	return 0;
}

/* Compute CRC32C over file content, excluding the "crc32=" line.
 * Must match the kernel save path which computes crc32c(0, buf, off)
 * over the entire content buffer atomically.
 */
static u32 tv_compute_config_crc(const char *buf, size_t len)
{
	const char *crc_line;
	size_t crc_len;

	/* Find the crc32= line and compute CRC over everything else */
	crc_line = buf;
	crc_len = 0;

	/* Skip past the crc32= line */
	{
		const char *p = buf;
		const char *end = buf + len;
		size_t before_len = 0;

		while (p < end) {
			const char *nl = memchr(p, '\n', end - p);
			size_t line_len = nl ? (size_t)(nl - p + 1) : (size_t)(end - p);

			if (line_len >= 7 && memcmp(p, "crc32=", 6) == 0) {
				/* CRC everything before this line */
				crc_line = buf;
				crc_len = before_len;
				break;
			}
			before_len += line_len;
			p = nl ? nl + 1 : end;
			if (!nl)
				break;
		}
		if (p >= end)
			return 0; /* no crc32= line found */
	}

	return crc32c(0, crc_line, crc_len);
}

/* ---- Metadata write-back (kernel → file) ---- */

static DEFINE_MUTEX(tv_save_mutex);

int tv_metadata_save_kernel(struct tieredvol_ctx *ctx)
{
	struct file *f;
	char *buf;
	int off = 0;
	int ret;
	u32 crc;
	loff_t pos = 0;
	char bak_path[260];

	if (!ctx->config_path[0])
		return -ENOENT;

	mutex_lock(&tv_save_mutex);

	buf = kmalloc(65536, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	off += scnprintf(buf + off, 65536 - off, "[weighted_striping]\n");
	off += scnprintf(buf + off, 65536 - off, "version=%u\n",
			  ctx->meta.version);
	off += scnprintf(buf + off, 65536 - off, "chunk_size=%u\n",
			  ctx->meta.chunk_size);
	off += scnprintf(buf + off, 65536 - off, "segment_count=%u\n",
			  ctx->meta.segment_count);
	off += scnprintf(buf + off, 65536 - off, "disk_count=%u\n",
			  ctx->meta.disk_count);

	for (u32 i = 0; i < ctx->meta.disk_count; i++)
		off += scnprintf(buf + off, 65536 - off,
				 "disk%u_name=%s\n", i, ctx->meta.disk_names[i]);

	for (u32 i = 0; i < ctx->meta.segment_count; i++) {
		struct tieredvol_segment *seg = &ctx->meta.segments[i];

		off += scnprintf(buf + off, 65536 - off,
				 "seg%u_begin=%llu\n", i, seg->logical_begin);
		off += scnprintf(buf + off, 65536 - off,
				 "seg%u_end=%llu\n", i, seg->logical_end);
		off += scnprintf(buf + off, 65536 - off,
				 "seg%u_count=%u\n", i, seg->disk_count);

		off += scnprintf(buf + off, 65536 - off, "seg%u_disks=",
				 i);
		for (u32 j = 0; j < seg->disk_count; j++)
			off += scnprintf(buf + off, 65536 - off,
					 "%s%u", j ? "," : "", seg->disk_index[j]);
		off += scnprintf(buf + off, 65536 - off, "\n");

		off += scnprintf(buf + off, 65536 - off, "seg%u_weight=",
				 i);
		for (u32 j = 0; j < seg->disk_count; j++)
			off += scnprintf(buf + off, 65536 - off,
					 "%s%u", j ? "," : "", seg->weight[j]);
		off += scnprintf(buf + off, 65536 - off, "\n");

		off += scnprintf(buf + off, 65536 - off,
				 "seg%u_stripe=%llu\n", i, seg->stripe_size);
		if (seg->mirror_enabled)
			off += scnprintf(buf + off, 65536 - off,
					 "seg%u_mirror=%u\n", i,
					 seg->mirror_disk);
		if (seg->policy >= 0)
			off += scnprintf(buf + off, 65536 - off,
					 "seg%u_policy=%d\n", i,
					 seg->policy);
	}

	/* Save bad block bitmaps as ranges */
	for (u32 i = 0; i < ctx->meta.disk_count; i++) {
		struct tieredvol_badmap *bm = &ctx->badmaps[i];
		u64 start = 0;
		int range_count = 0;

		if (!bm->bitmap || bm->n_chunks == 0)
			continue;

		off += scnprintf(buf + off, 65536 - off, "badmap_%u=", i);

		while (start < bm->n_chunks) {
			/* Find next set bit */
			start = find_next_bit(bm->bitmap, bm->n_chunks, start);
			if (start >= bm->n_chunks)
				break;

			/* Find end of consecutive run */
			u64 end = find_next_zero_bit(bm->bitmap, bm->n_chunks, start);

			if (range_count > 0)
				off += scnprintf(buf + off, 65536 - off, ",");
			if (end == start + 1)
				off += scnprintf(buf + off, 65536 - off, "%llu", start);
			else
				off += scnprintf(buf + off, 65536 - off, "%llu-%llu", start, end - 1);
			range_count++;
			start = end;
		}
		off += scnprintf(buf + off, 65536 - off, "\n");
	}

	off += scnprintf(buf + off, 65536 - off, "[runtime]\n");
	off += scnprintf(buf + off, 65536 - off, "policy=%d\n",
			  ctx->policy);
	off += scnprintf(buf + off, 65536 - off, "borrow_enable=%d\n",
			  ctx->meta.runtime_borrow_enable);
	off += scnprintf(buf + off, 65536 - off, "borrow_watermark_kb=%u\n",
			  ctx->meta.runtime_borrow_watermark_kb);
	off += scnprintf(buf + off, 65536 - off, "borrow_area_mb=");
	for (u32 i = 0; i < ctx->meta.disk_count; i++)
		off += scnprintf(buf + off, 65536 - off,
				 "%s%u", i ? "," : "",
				 ctx->meta.runtime_borrow_area_mb[i]);
	off += scnprintf(buf + off, 65536 - off, "\n");

	crc = crc32c(0, buf, off);
	off += scnprintf(buf + off, 65536 - off, "crc32=%u\n", crc);

	scnprintf(bak_path, sizeof(bak_path), "%s.bak", ctx->config_path);

	f = filp_open(ctx->config_path, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		struct file *bak;

		bak = filp_open(bak_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (!IS_ERR(bak)) {
			char kbuf[256];
			loff_t rpos = 0, wpos = 0;
			ssize_t nrd;

			while ((nrd = kernel_read(f, kbuf, sizeof(kbuf),
						  &rpos)) > 0)
				kernel_write(bak, kbuf, nrd, &wpos);
			filp_close(bak, NULL);
		}
		filp_close(f, NULL);
	}

	f = filp_open(ctx->config_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(f)) {
		pr_err("tieredvol: save failed to open %s: %ld\n",
		       ctx->config_path, PTR_ERR(f));
		kfree(buf);
		ret = PTR_ERR(f);
		goto out_unlock;
	}

	pos = 0;
	ret = kernel_write(f, buf, off, &pos);
	filp_close(f, NULL);

	if (ret != off) {
		pr_err("tieredvol: save write error %d (wrote %lld of %d)\n",
		       ret, pos, off);
		kfree(buf);
		ret = ret < 0 ? ret : -EIO;
		goto out_unlock;
	}

	tv_log(TV_LOG_INFO, TV_LOG_CONFIG, "metadata saved crc=0x%08x",
	       crc);
	pr_info("tieredvol: metadata saved crc=0x%08x to %s\n", crc,
		ctx->config_path);
	kfree(buf);
	ret = 0;
	{
		int bs = tv_borrow_save(ctx);

		if (bs && !ret)
			ret = bs;
	}

out_unlock:
	mutex_unlock(&tv_save_mutex);
	return ret;
}

int tv_metadata_load_kernel(struct tieredvol_metadata *meta,
			    const char *path)
{
	struct file *f;
	loff_t pos = 0, file_size;
	char *buf;
	char *line, *next_line;
	ssize_t bytes_read = 0;
	int ret = 0;
	u32 expected_crc = 0;
	bool has_crc = false;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	file_size = i_size_read(file_inode(f));
	if (file_size <= 0 || file_size > TV_MAX_CONFIG_SIZE) {
		filp_close(f, NULL);
		return -EINVAL;
	}

	buf = vmalloc(file_size + 1);
	if (!buf) {
		filp_close(f, NULL);
		return -ENOMEM;
	}

	{
		bytes_read = kernel_read(f, buf, file_size, &pos);
		filp_close(f, NULL);

		if (bytes_read < 0) {
			vfree(buf);
			return (int)bytes_read;
		}
		if (bytes_read == 0)
			pr_warn("tieredvol: config file is empty\n");
		buf[bytes_read] = '\0';
	}

	ret = 0;

	memset(meta, 0, sizeof(*meta));

	/* Set default per-segment policy to -1 (inherit from ctx) */
	for (u32 si = 0; si < TV_MAX_SEGS; si++)
		meta->segments[si].policy = -1;

	/* CRC32 pre-scan: find crc32= line before main parse loop.
	 * Must not destroy the buffer — do NOT use parse_line() which writes \0.
	 */
	{
		char *scan;

		for (scan = buf; scan && *scan; ) {
			char *nl = strchr(scan, '\n');
			char *eq = strchr(scan, '=');
			char saved_nl = nl ? *nl : '\0';
			char saved_eq = '\0';

			if (nl)
				*nl = '\0';
			if (eq) {
				saved_eq = *eq;
				*eq = '\0';
			}

			if (eq && strcmp(scan, "crc32") == 0) {
				char *val = eq + 1;

				if (nl)
					*nl = '\0';
				if (kstrtou32(val, 10, &expected_crc) == 0)
					has_crc = true;
				if (nl)
					*nl = saved_nl;
			}

			if (eq)
				*eq = saved_eq;
			if (nl) {
				*nl = saved_nl;
				scan = nl + 1;
			} else {
				break;
			}
		}
	}

	/* CRC32 validation — must be computed before parsing modifies buf */
	if (has_crc) {
		u32 actual_crc = tv_compute_config_crc(buf, bytes_read);

		if (actual_crc != expected_crc) {
			pr_err("tieredvol: config CRC mismatch (expected=0x%08x actual=0x%08x) — file may be corrupted\n",
			       expected_crc, actual_crc);
			ret = -EIO;
			goto out;
		}
		pr_info("tieredvol: config CRC OK (0x%08x)\n", actual_crc);
	}

	for (line = buf; line && *line; line = next_line) {
		char *k, *v;
		unsigned long idx;
		const char *suf;

		next_line = strchr(line, '\n');
		if (next_line)
			*next_line++ = '\0';

		if (parse_line(line, &k, &v) < 0)
			continue;

		if (strcmp(k, "crc32") == 0) {
			if (kstrtou32(v, 10, &expected_crc) == 0)
				has_crc = true;
			continue;
		}

		if (strcmp(k, "version") == 0) {
			if (parse_u32(v, &meta->version) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(k, "chunk_size") == 0) {
			if (parse_u32(v, &meta->chunk_size) < 0 ||
			    meta->chunk_size == 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(k, "segment_count") == 0) {
			if (parse_u32(v, &meta->segment_count) < 0) {
				ret = -EINVAL;
				goto out;
			}
			if (meta->segment_count > TV_MAX_SEGS) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(k, "disk_count") == 0) {
			if (parse_u32(v, &meta->disk_count) < 0) {
				ret = -EINVAL;
				goto out;
			}
			if (meta->disk_count > TV_MAX_DISKS) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strncmp(k, "disk", 4) == 0 &&
			   strstr(k, "_name")) {
			if (parse_num_prefix(k + 4, &idx, &suf) == 0 &&
			    strcmp(suf, "_name") == 0 &&
			    idx < TV_MAX_DISKS &&
			    idx < meta->disk_count) {
				strncpy(meta->disk_names[idx], v, 63);
				meta->disk_names[idx][63] = '\0';
			}
		} else if (strncmp(k, "seg", 3) == 0) {
			struct tieredvol_segment *seg;

			if (parse_num_prefix(k + 3, &idx, &suf) < 0)
				continue;
			if (idx >= TV_MAX_SEGS)
				continue;

			seg = &meta->segments[idx];

		if (strcmp(suf, "_begin") == 0) {
			if (parse_u64(v, &seg->logical_begin) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_end") == 0) {
			if (parse_u64(v, &seg->logical_end) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_count") == 0) {
			if (parse_u32(v, &seg->disk_count) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_stripe") == 0) {
			if (parse_u64(v, &seg->stripe_size) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_disks") == 0) {
			int n;

			if (parse_csv_u32(v, seg->disk_index,
					   TV_MAX_DISKS, &n) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_weight") == 0) {
			int n;

			if (parse_csv_u32(v, seg->weight,
					   TV_MAX_DISKS, &n) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_mirror") == 0) {
			u32 mirror_idx;

			if (parse_u32(v, &mirror_idx) < 0) {
				ret = -EINVAL;
				goto out;
			}
			seg->mirror_enabled = true;
			seg->mirror_disk = mirror_idx;
		} else if (strcmp(suf, "_policy") == 0) {
			long pv;

			if (kstrtol(v, 10, &pv) == 0)
				seg->policy = (int)pv;
		}
		} else if (strncmp(k, "badmap_", 7) == 0) {
			unsigned long disk_idx;

			if (kstrtoul(k + 7, 10, &disk_idx) == 0 &&
			    disk_idx < TV_MAX_DISKS) {
				strncpy(meta->badmap_ranges[disk_idx], v, 255);
				meta->badmap_ranges[disk_idx][255] = '\0';
			}
		} else if (strcmp(k, "policy") == 0) {
			long v2;

			if (kstrtol(v, 10, &v2) == 0)
				meta->runtime_policy = (int)v2;
		} else if (strcmp(k, "borrow_enable") == 0) {
			long v3;

			if (kstrtol(v, 10, &v3) == 0)
				meta->runtime_borrow_enable = (int)v3;
		} else if (strcmp(k, "borrow_watermark_kb") == 0) {
			parse_u32(v, &meta->runtime_borrow_watermark_kb);
		} else if (strcmp(k, "borrow_area_mb") == 0) {
			int n;
			u32 tmp[TV_MAX_DISKS];

			if (parse_csv_u32(v, tmp, TV_MAX_DISKS, &n) == 0) {
				for (int bi = 0; bi < n && bi < TV_MAX_DISKS; bi++)
					meta->runtime_borrow_area_mb[bi] = tmp[bi];
			}
		}
	}

	/* Validate disk indices and mirror safety */
	{
		u32 si, j;

		for (si = 0; si < meta->segment_count; si++) {
			struct tieredvol_segment *seg = &meta->segments[si];

			for (j = 0; j < seg->disk_count; j++) {
				if (seg->disk_index[j] >= meta->disk_count) {
					pr_err("tieredvol: seg%u disk index %u >= disk_count %u\n",
					       si, seg->disk_index[j],
					       meta->disk_count);
					ret = -EINVAL;
					goto out;
				}
			}

			if (seg->mirror_enabled) {
				for (j = 0; j < seg->disk_count; j++) {
					if (seg->disk_index[j] ==
					    seg->mirror_disk) {
						pr_err("tieredvol: seg%u mirror_disk %u is a stripe participant\n",
						       si, seg->mirror_disk);
						ret = -EINVAL;
						goto out;
					}
				}
			}
		}
	}

	pr_info("tieredvol: loaded metadata: %u disks, %u segments\n",
		meta->disk_count, meta->segment_count);

out:
	vfree(buf);
	return ret;
}
