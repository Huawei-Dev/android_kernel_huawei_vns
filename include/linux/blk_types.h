/*
 * Block data types and constants.  Directly include this file only to
 * break include dependency loop.
 */
#ifndef __LINUX_BLK_TYPES_H
#define __LINUX_BLK_TYPES_H

#include <linux/types.h>
#include <linux/timer.h>

struct bio_set;
struct bio;
struct bio_integrity_payload;
struct page;
struct block_device;
struct io_context;
struct cgroup_subsys_state;
typedef void (bio_end_io_t) (struct bio *, int);
typedef void (bio_destructor_t) (struct bio *);
typedef void (bio_throtl_end_io_t) (struct bio *);

enum bio_process_stage_enum {
	BIO_PROC_STAGE_SUBMIT = 0,
	BIO_PROC_STAGE_GENERIC_MAKE_REQ,
	BIO_PROC_STAGE_WBT,
	BIO_PROC_STAGE_ENDBIO,
	BIO_PROC_STAGE_MAX,
};

enum req_process_stage_enum {
	REQ_PROC_STAGE_INIT_FROM_BIO = 0,
	REQ_PROC_STAGE_MQ_ADDTO_PLUGLIST,
	REQ_PROC_STAGE_MQ_FLUSH_PLUGLIST_MANUAL,
	REQ_PROC_STAGE_MQ_FLUSH_PLUGLIST_SCHEDULE,
	REQ_PROC_STAGE_MQ_IO_DECISION_IN,
	REQ_PROC_STAGE_MQ_IO_DECISION_OUT,
	REQ_PROC_STAGE_MQ_SYNC_DISPATCH,
	REQ_PROC_STAGE_MQ_ADDTO_ASYNC_LIST,
	REQ_PROC_STAGE_MQ_SYNC_DISPATCH_EXIT,
	REQ_PROC_STAGE_MQ_PLUGFLUSH_DISPATCH,
	REQ_PROC_STAGE_MQ_RUN_QUEUE_CHECK,
	REQ_PROC_STAGE_MQ_RUN_QUEUE_DISPATCH,
	REQ_PROC_STAGE_MQ_RUN_QUEUE_EXIT,
	REQ_PROC_STAGE_START,
	REQ_PROC_STAGE_COMPLETE,
	REQ_PROC_STAGE_MQ_START,
	REQ_PROC_STAGE_MQ_COMPLETE,
	REQ_PROC_STAGE_MQ_REQUEUE,
	REQ_PROC_STAGE_MAX,
};

/*
 * was unsigned short, but we might as well be ready for > 64kB I/O pages
 */
struct bio_vec {
	struct page	*bv_page;
	unsigned int	bv_len;
	unsigned int	bv_offset;
};

#ifdef CONFIG_BLOCK

struct bvec_iter {
	sector_t		bi_sector;	/* device address in 512 byte
						   sectors */
	unsigned int		bi_size;	/* residual I/O count */

	unsigned int		bi_idx;		/* current index into bvl_vec */

	unsigned int            bi_bvec_done;	/* number of bytes completed in
						   current bvec */
};

#define IO_FROM_SUBMIT_BIO_MAGIC 0x4C
#define IO_FROM_BLK_EXEC			0x4D
/*
 * main unit of I/O for the block layer and lower layers (ie drivers and
 * stacking drivers)
 */
struct bio {
	struct bio		*bi_next;	/* request queue link */
	struct block_device	*bi_bdev;
	unsigned long		bi_flags;	/* status, command, etc */
	unsigned long		bi_rw;		/* bottom bits READ/WRITE,
						 * top bits priority
						 */
	unsigned char bi_iosche_bypass;
	struct bvec_iter	bi_iter;

	/* Number of segments in this BIO after
	 * physical address coalescing is performed.
	 */
	unsigned int		bi_phys_segments;

	/*
	 * To keep track of the max segment size, we account for the
	 * sizes of the first and last mergeable segments in this bio.
	 */
	unsigned int		bi_seg_front_size;
	unsigned int		bi_seg_back_size;

	atomic_t		__bi_remaining;

	bio_end_io_t		*bi_end_io;

	void			*bi_private;
#ifdef CONFIG_HISI_IO_LATENCY_TRACE
	unsigned char from_submit_bio_flag;
	unsigned long bio_stage_jiffies[BIO_PROC_STAGE_MAX];
	struct timer_list bio_latency_check_timer;
	void* io_req;
	atomic_t	bio_latency_timer_executing;
	struct block_device	*bi_bdev_part;
	struct task_struct* dispatch_task;
#endif
#ifdef CONFIG_BLK_DEV_THROTTLING
	bio_throtl_end_io_t	*bi_throtl_end_io1;
	void			*bi_throtl_private1;
	bio_throtl_end_io_t	*bi_throtl_end_io2;
	void			*bi_throtl_private2;
#endif
#ifdef CONFIG_BLK_CGROUP
	/*
	 * Optional ioc and css associated with this bio.  Put on bio
	 * release.  Read comment on top of bio_associate_current().
	 */
	struct io_context	*bi_ioc;
	struct cgroup_subsys_state *bi_css;
#endif
	union {
#if defined(CONFIG_BLK_DEV_INTEGRITY)
		struct bio_integrity_payload *bi_integrity; /* data integrity */
#endif
	};

	unsigned short		bi_vcnt;	/* how many bio_vec's */

#ifdef CONFIG_HISI_BLK_INLINE_CRYPTO
	void			*ci_key;
	int			ci_key_len;
	pgoff_t			index;
#endif

	/*
	 * Everything starting with bi_max_vecs will be preserved by bio_reset()
	 */

	unsigned short		bi_max_vecs;	/* max bvl_vecs we can hold */

	atomic_t		bi_cnt;		/* pin count */

	struct bio_vec		*bi_io_vec;	/* the actual vec list */

	struct bio_set		*bi_pool;

	/*
	 * We can inline a number of vecs at the end of the bio, to avoid
	 * double allocations for a small number of bio_vecs. This member
	 * MUST obviously be kept at the very end of the bio.
	 */
	struct bio_vec		bi_inline_vecs[0];
};

#define BIO_RESET_BYTES		offsetof(struct bio, bi_max_vecs)

/*
 * bio flags
 */
#define BIO_UPTODATE	0	/* ok after I/O completion */
#define BIO_RW_BLOCK	1	/* RW_AHEAD set, and read/write would block */
#define BIO_EOF		2	/* out-out-bounds error */
#define BIO_SEG_VALID	3	/* bi_phys_segments valid */
#define BIO_CLONED	4	/* doesn't own data */
#define BIO_BOUNCED	5	/* bio is a bounce bio */
#define BIO_USER_MAPPED 6	/* contains user pages */
#define BIO_NULL_MAPPED 8	/* contains invalid user pages */
#define BIO_QUIET	9	/* Make BIO Quiet */
#define BIO_SNAP_STABLE	10	/* bio data must be snapshotted during write */
#define BIO_CHAIN	11	/* chained bio, ->bi_remaining in effect */

/*
 * Flags starting here get preserved by bio_reset() - this includes
 * BIO_POOL_IDX()
 */
#define BIO_RESET_BITS	13
#define BIO_OWNS_VEC	13	/* bio_free() should free bvec */

#define bio_flagged(bio, flag)	((bio)->bi_flags & (1 << (flag)))

/*
 * top 4 bits of bio flags indicate the pool this bio came from
 */
#define BIO_POOL_BITS		(4)
#define BIO_POOL_NONE		((1UL << BIO_POOL_BITS) - 1)
#define BIO_POOL_OFFSET		(BITS_PER_LONG - BIO_POOL_BITS)
#define BIO_POOL_MASK		(1UL << BIO_POOL_OFFSET)
#define BIO_POOL_IDX(bio)	((bio)->bi_flags >> BIO_POOL_OFFSET)

#endif /* CONFIG_BLOCK */

/*
 * Request flags.  For use in the cmd_flags field of struct request, and in
 * bi_rw of struct bio.  Note that some flags are only valid in either one.
 */
enum rq_flag_bits {
	/* common flags */
	__REQ_WRITE,		/* not set, read. set, write */
	__REQ_FAILFAST_DEV,	/* no driver retries of device errors */
	__REQ_FAILFAST_TRANSPORT, /* no driver retries of transport errors */
	__REQ_FAILFAST_DRIVER,	/* no driver retries of driver errors */

	__REQ_SYNC,		/* request is sync (sync write or read) */
	__REQ_META,		/* metadata io request */
	__REQ_PRIO,		/* boost priority in cfq */
	__REQ_DISCARD,		/* request to discard sectors */
	__REQ_SECURE,		/* secure discard (used with __REQ_DISCARD) */
	__REQ_WRITE_SAME,	/* write same block many times */

	__REQ_NOIDLE,		/* don't anticipate more IO after this one */
	__REQ_INTEGRITY,	/* I/O includes block integrity payload */
	__REQ_FUA,		/* forced unit access */
	__REQ_FLUSH,		/* request for cache flush */
	__REQ_BG,		/* background activity */
	__REQ_FG,		/* foreground activity */

	/* bio only flags */
	__REQ_RAHEAD,		/* read ahead, can fail anytime */
	__REQ_THROTTLED,	/* This bio has already been subjected to
				 * throttling rules. Don't do it again. */

	/* request only flags */
	__REQ_SORTED,		/* elevator knows about this request */
	__REQ_SOFTBARRIER,	/* may not be passed by ioscheduler */
	__REQ_NOMERGE,		/* don't touch this for merging */
	__REQ_STARTED,		/* drive already may have started this one */
	__REQ_DONTPREP,		/* don't call prep for this one */
	__REQ_QUEUED,		/* uses queueing */
	__REQ_ELVPRIV,		/* elevator private data attached */
	__REQ_FAILED,		/* set if the request failed */
	__REQ_QUIET,		/* don't worry about errors */
	__REQ_PREEMPT,		/* set for "ide_preempt" requests and also
				   for requests for which the SCSI "quiesce"
				   state must be ignored. */
	__REQ_ALLOCED,		/* request came from our alloc pool */
	__REQ_COPY_USER,	/* contains copies of user pages */
	__REQ_FLUSH_SEQ,	/* request for flush sequence */
	__REQ_IO_STAT,		/* account I/O stat */
	__REQ_MIXED_MERGE,	/* merge of different types, fail separately */
	__REQ_PM,		/* runtime pm request */
	__REQ_HASHED,		/* on IO scheduler merge hash */
	__REQ_MQ_INFLIGHT,	/* track inflight for MQ */
	__REQ_NO_TIMEOUT,	/* requests may never expire */
	__REQ_URGENT,		/* urgent request */
	__REQ_NR_BITS,		/* stops here */
};

#define REQ_WRITE		(1ULL << __REQ_WRITE)
#define REQ_FAILFAST_DEV	(1ULL << __REQ_FAILFAST_DEV)
#define REQ_FAILFAST_TRANSPORT	(1ULL << __REQ_FAILFAST_TRANSPORT)
#define REQ_FAILFAST_DRIVER	(1ULL << __REQ_FAILFAST_DRIVER)
#define REQ_SYNC		(1ULL << __REQ_SYNC)
#define REQ_META		(1ULL << __REQ_META)
#define REQ_PRIO		(1ULL << __REQ_PRIO)
#define REQ_DISCARD		(1ULL << __REQ_DISCARD)
#define REQ_WRITE_SAME		(1ULL << __REQ_WRITE_SAME)
#define REQ_NOIDLE		(1ULL << __REQ_NOIDLE)
#define REQ_INTEGRITY		(1ULL << __REQ_INTEGRITY)
#define REQ_URGENT		(1ULL << __REQ_URGENT)

#define REQ_FAILFAST_MASK \
	(REQ_FAILFAST_DEV | REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER)
#define REQ_COMMON_MASK \
	(REQ_WRITE | REQ_FAILFAST_MASK | REQ_SYNC | REQ_META | REQ_PRIO | \
	 REQ_DISCARD | REQ_WRITE_SAME | REQ_NOIDLE | REQ_FLUSH | REQ_FUA | \
	 REQ_SECURE | REQ_INTEGRITY | REQ_BG | REQ_FG)
#define REQ_CLONE_MASK		REQ_COMMON_MASK

#define BIO_NO_ADVANCE_ITER_MASK	(REQ_DISCARD|REQ_WRITE_SAME)

/* This mask is used for both bio and request merge checking */
#define REQ_NOMERGE_FLAGS \
	(REQ_NOMERGE | REQ_STARTED | REQ_SOFTBARRIER | REQ_FLUSH | REQ_FUA | REQ_FLUSH_SEQ)

#define REQ_RAHEAD		(1ULL << __REQ_RAHEAD)
#define REQ_THROTTLED		(1ULL << __REQ_THROTTLED)

#define REQ_SORTED		(1ULL << __REQ_SORTED)
#define REQ_SOFTBARRIER		(1ULL << __REQ_SOFTBARRIER)
#define REQ_FUA			(1ULL << __REQ_FUA)
#define REQ_NOMERGE		(1ULL << __REQ_NOMERGE)
#define REQ_STARTED		(1ULL << __REQ_STARTED)
#define REQ_DONTPREP		(1ULL << __REQ_DONTPREP)
#define REQ_QUEUED		(1ULL << __REQ_QUEUED)
#define REQ_ELVPRIV		(1ULL << __REQ_ELVPRIV)
#define REQ_FAILED		(1ULL << __REQ_FAILED)
#define REQ_QUIET		(1ULL << __REQ_QUIET)
#define REQ_PREEMPT		(1ULL << __REQ_PREEMPT)
#define REQ_ALLOCED		(1ULL << __REQ_ALLOCED)
#define REQ_COPY_USER		(1ULL << __REQ_COPY_USER)
#define REQ_FLUSH		(1ULL << __REQ_FLUSH)
#define REQ_FLUSH_SEQ		(1ULL << __REQ_FLUSH_SEQ)
#define REQ_BG			(1ULL << __REQ_BG)
#define REQ_FG			(1ULL << __REQ_FG)
#define REQ_IO_STAT		(1ULL << __REQ_IO_STAT)
#define REQ_MIXED_MERGE		(1ULL << __REQ_MIXED_MERGE)
#define REQ_SECURE		(1ULL << __REQ_SECURE)
#define REQ_PM			(1ULL << __REQ_PM)
#define REQ_HASHED		(1ULL << __REQ_HASHED)
#define REQ_MQ_INFLIGHT		(1ULL << __REQ_MQ_INFLIGHT)
#define REQ_NO_TIMEOUT		(1ULL << __REQ_NO_TIMEOUT)

struct blk_rq_stat {
	s64 mean;
	u64 min;
	u64 max;
	s64 nr_samples;
	s64 time;
};

#define BIO_DELAY_WARNING_GENERIC_MAKE_REQ			(10)
#define BIO_DELAY_WARNING_MERGED					(10)
#define BIO_DELAY_WARNING_MQ_MAKE					(10)
#define BIO_DELAY_WARNING_ENDBIO						(500)

#define REQ_DELAY_WARNING_MQ_REQ_MAPPED			(20)
#define REQ_DELAY_WARNING_MQ_REQ_DECISION			(100)
#define REQ_DELAY_WARNING_MQ_REQ_DISPATCH			(100)
#define REQ_DELAY_WARNING_MQ_REQ_INT_BACK			(50)
#define REQ_DELAY_WARNING_MQ_REQ_FREE				(500)

#ifdef CONFIG_HISI_IO_LATENCY_TRACE
void bio_latency_check(struct bio *bio,enum bio_process_stage_enum bio_stage);
#else
static inline void bio_latency_check(struct bio *bio,enum bio_process_stage_enum bio_stage){}
#endif

#endif /* __LINUX_BLK_TYPES_H */
