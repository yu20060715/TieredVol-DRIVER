/* Minimal mock of <linux/bio.h> for the feasibility prototype.
 * bio_endio invokes bi_end_io synchronously so parallel-completion
 * handoff logic is exercised deterministically. */
#ifndef _MOCK_LINUX_BIO_H
#define _MOCK_LINUX_BIO_H

#include <stdlib.h>
#include "types.h"
#include "kernel.h"

#define BLK_STS_OK 0

struct bio {
	void *bi_private;
	void (*bi_end_io)(struct bio *);
	blk_status_t bi_status;
	unsigned int bi_op;
	struct {
		sector_t bi_sector;
		unsigned int bi_size;
	} bi_iter;
};

struct bio_set { int dummy; };
extern struct bio_set fs_bio_set;
extern struct bio *mock_last_submitted;

static inline struct bio *bio_alloc_clone(void *bdev, struct bio *orig,
					  gfp_t gfp, struct bio_set *bs)
{
	struct bio *b = calloc(1, sizeof(struct bio));

	(void)bdev; (void)gfp; (void)bs;
	if (b)
		b->bi_iter.bi_size = orig->bi_iter.bi_size;
	return b;
}

static inline void bio_put(struct bio *b)
{
	free(b);
}

static inline void bio_advance(struct bio *b, unsigned int bytes)
{
	b->bi_iter.bi_size -= bytes;
	b->bi_iter.bi_sector += bytes >> 9;
}

static inline void bio_set_dev(struct bio *b, void *bdev)
{
	(void)bdev;
}

static inline void bio_endio(struct bio *b)
{
	if (b->bi_end_io)
		b->bi_end_io(b);
}

static inline void bio_io_error(struct bio *b)
{
	b->bi_status = 5;
	bio_endio(b);
}

static inline void submit_bio(struct bio *b)
{
	/* Real kernels submit asynchronously; tests drive completion
	 * explicitly by calling tv_parallel_end_io. The last submitted
	 * clone is kept so tests can reach its bi_private (the parallel
	 * sub / block) deterministically. */
	mock_last_submitted = b;
}

#endif
