/*	$OpenBSD: malloc.c,v 1.285 2023/06/04 06:58:33 otto Exp $	*/
/*
 * Copyright (c) 2008, 2010, 2011, 2016, 2023 Otto Moerbeek <otto@drijf.net>
 * Copyright (c) 2012 Matthew Dempsky <matthew@openbsd.org>
 * Copyright (c) 2008 Damien Miller <djm@openbsd.org>
 * Copyright (c) 2000 Poul-Henning Kamp <phk@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * If we meet some day, and you think this stuff is worth it, you
 * can buy me a beer in return. Poul-Henning Kamp
 */

#ifndef MALLOC_SMALL
#define MALLOC_STATS
#endif

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <uvm/uvmexp.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef MALLOC_STATS
#include <sys/tree.h>
#include <sys/ktrace.h>
#include <dlfcn.h>
#endif

#include "thread_private.h"
#include <tib.h>

#define MALLOC_PAGESHIFT	_MAX_PAGE_SHIFT

#define MALLOC_MINSHIFT		4
#define MALLOC_MAXSHIFT		(MALLOC_PAGESHIFT - 1)
#define MALLOC_PAGESIZE		(1UL << MALLOC_PAGESHIFT)
#define MALLOC_MINSIZE		(1UL << MALLOC_MINSHIFT)
#define MALLOC_PAGEMASK		(MALLOC_PAGESIZE - 1)
#define MASK_POINTER(p)		((void *)(((uintptr_t)(p)) & ~MALLOC_PAGEMASK))

#define MALLOC_MAXCHUNK		(1 << MALLOC_MAXSHIFT)
#define MALLOC_MAXCACHE		256
#define MALLOC_DELAYED_CHUNK_MASK	15
#ifdef MALLOC_STATS
#define MALLOC_INITIAL_REGIONS	512
#else
#define MALLOC_INITIAL_REGIONS	(MALLOC_PAGESIZE / sizeof(struct region_info))
#endif
#define MALLOC_DEFAULT_CACHE	64
#define MALLOC_CHUNK_LISTS	4
#define CHUNK_CHECK_LENGTH	32

#define B2SIZE(b)		((b) * MALLOC_MINSIZE)
#define B2ALLOC(b)		((b) == 0 ? MALLOC_MINSIZE : \
				    (b) * MALLOC_MINSIZE)
#define BUCKETS 		(MALLOC_MAXCHUNK / MALLOC_MINSIZE)

/*
 * We move allocations between half a page and a whole page towards the end,
 * subject to alignment constraints. This is the extra headroom we allow.
 * Set to zero to be the most strict.
 */
#define MALLOC_LEEWAY		0
#define MALLOC_MOVE_COND(sz)	((sz) - mopts.malloc_guard < 		\
				    MALLOC_PAGESIZE - MALLOC_LEEWAY)
#define MALLOC_MOVE(p, sz)  	(((char *)(p)) +			\
				    ((MALLOC_PAGESIZE - MALLOC_LEEWAY -	\
			    	    ((sz) - mopts.malloc_guard)) & 	\
				    ~(MALLOC_MINSIZE - 1)))

#define PAGEROUND(x)  (((x) + (MALLOC_PAGEMASK)) & ~MALLOC_PAGEMASK)

/*
 * What to use for Junk.  This is the byte value we use to fill with
 * when the 'J' option is enabled. Use SOME_JUNK right after alloc,
 * and SOME_FREEJUNK right before free.
 */
#define SOME_JUNK		0xdb	/* deadbeef */
#define SOME_FREEJUNK		0xdf	/* dead, free */
#define SOME_FREEJUNK_ULL	0xdfdfdfdfdfdfdfdfULL

#define MMAP(sz,f)	mmap(NULL, (sz), PROT_READ | PROT_WRITE, \
    MAP_ANON | MAP_PRIVATE | (f), -1, 0)

#define MMAPNONE(sz,f)	mmap(NULL, (sz), PROT_NONE, \
    MAP_ANON | MAP_PRIVATE | (f), -1, 0)

#define MMAPA(a,sz,f)	mmap((a), (sz), PROT_READ | PROT_WRITE, \
    MAP_ANON | MAP_PRIVATE | (f), -1, 0)

struct region_info {
	void *p;		/* page; low bits used to mark chunks */
	uintptr_t size;		/* size for pages, or chunk_info pointer */
#ifdef MALLOC_STATS
	void *f;		/* where allocated from */
#endif
};

LIST_HEAD(chunk_head, chunk_info);

/*
 * Two caches, one for "small" regions, one for "big".
 * Small cache is an array per size, big cache is one array with different
 * sized regions
 */
#define MAX_SMALLCACHEABLE_SIZE	32
#define MAX_BIGCACHEABLE_SIZE	512
/* If the total # of pages is larger than this, evict before inserting */
#define BIGCACHE_FILL(sz)	(MAX_BIGCACHEABLE_SIZE * (sz) / 4)

struct smallcache {
	void **pages;
	ushort length;
	ushort max;
};

struct bigcache {
	void *page;
	size_t psize;
};

struct dir_info {
	u_int32_t canary1;
	int active;			/* status of malloc */
	struct region_info *r;		/* region slots */
	size_t regions_total;		/* number of region slots */
	size_t regions_free;		/* number of free slots */
	size_t rbytesused;		/* random bytes used */
	char *func;			/* current function */
	int malloc_junk;		/* junk fill? */
	int mmap_flag;			/* extra flag for mmap */
	int mutex;
	int malloc_mt;			/* multi-threaded mode? */
					/* lists of free chunk info structs */
	struct chunk_head chunk_info_list[BUCKETS + 1];
					/* lists of chunks with free slots */
	struct chunk_head chunk_dir[BUCKETS + 1][MALLOC_CHUNK_LISTS];
					/* delayed free chunk slots */
	void *delayed_chunks[MALLOC_DELAYED_CHUNK_MASK + 1];
	u_char rbytes[32];		/* random bytes */
					/* free pages cache */
	struct smallcache smallcache[MAX_SMALLCACHEABLE_SIZE];
	size_t bigcache_used;
	size_t bigcache_size;
	struct bigcache *bigcache;
	void *chunk_pages;
	size_t chunk_pages_used;
#ifdef MALLOC_STATS
	size_t inserts;
	size_t insert_collisions;
	size_t finds;
	size_t find_collisions;
	size_t deletes;
	size_t delete_moves;
	size_t cheap_realloc_tries;
	size_t cheap_reallocs;
	size_t malloc_used;		/* bytes allocated */
	size_t malloc_guarded;		/* bytes used for guards */
	size_t pool_searches;		/* searches for pool */
	size_t other_pool;		/* searches in other pool */
#define STATS_ADD(x,y)	((x) += (y))
#define STATS_SUB(x,y)	((x) -= (y))
#define STATS_INC(x)	((x)++)
#define STATS_ZERO(x)	((x) = 0)
#define STATS_SETF(x,y)	((x)->f = (y))
#else
#define STATS_ADD(x,y)	/* nothing */
#define STATS_SUB(x,y)	/* nothing */
#define STATS_INC(x)	/* nothing */
#define STATS_ZERO(x)	/* nothing */
#define STATS_SETF(x,y)	/* nothing */
#endif /* MALLOC_STATS */
	u_int32_t canary2;
};

static void unmap(struct dir_info *d, void *p, size_t sz, size_t clear);

/*
 * This structure describes a page worth of chunks.
 *
 * How many bits per u_short in the bitmap
 */
#define MALLOC_BITS		(NBBY * sizeof(u_short))
struct chunk_info {
	LIST_ENTRY(chunk_info) entries;
	void *page;			/* pointer to the page */
	u_short canary;
	u_short bucket;
	u_short free;			/* how many free chunks */
	u_short total;			/* how many chunks */
	u_short offset;			/* requested size table offset */
	u_short bits[1];		/* which chunks are free */
};

struct malloc_readonly {
					/* Main bookkeeping information */
	struct dir_info *malloc_pool[_MALLOC_MUTEXES];
	u_int	malloc_mutexes;		/* how much in actual use? */
	int	malloc_freecheck;	/* Extensive double free check */
	int	malloc_freeunmap;	/* mprotect free pages PROT_NONE? */
	int	def_malloc_junk;	/* junk fill? */
	int	malloc_realloc;		/* always realloc? */
	int	malloc_xmalloc;		/* xmalloc behaviour? */
	u_int	chunk_canaries;		/* use canaries after chunks? */
	int	internal_funcs;		/* use better recallocarray/freezero? */
	u_int	def_maxcache;		/* free pages we cache */
	u_int	junk_loc;		/* variation in location of junk */
	size_t	malloc_guard;		/* use guard pages after allocations? */
#ifdef MALLOC_STATS
	int	malloc_stats;		/* dump leak report at end */
	int	malloc_verbose;		/* dump verbose statistics at end */
#define	DO_STATS	mopts.malloc_stats
#else
#define	DO_STATS	0
#endif
	u_int32_t malloc_canary;	/* Matched against ones in pool */
};


/* This object is mapped PROT_READ after initialisation to prevent tampering */
static union {
	struct malloc_readonly mopts;
	u_char _pad[MALLOC_PAGESIZE];
} malloc_readonly __attribute__((aligned(MALLOC_PAGESIZE)))
		__attribute__((section(".openbsd.mutable")));
#define mopts	malloc_readonly.mopts

char		*malloc_options;	/* compile-time options */

static __dead void wrterror(struct dir_info *d, char *msg, ...)
    __attribute__((__format__ (printf, 2, 3)));

#ifdef MALLOC_STATS
void malloc_dump(void);
PROTO_NORMAL(malloc_dump);
static void malloc_exit(void);
#endif
#define CALLER	(DO_STATS ? __builtin_return_address(0) : NULL)

/* low bits of r->p determine size: 0 means >= page size and r->size holding
 * real size, otherwise low bits is the bucket + 1
 */
#define REALSIZE(sz, r)						\
	(sz) = (uintptr_t)(r)->p & MALLOC_PAGEMASK,		\
	(sz) = ((sz) == 0 ? (r)->size : B2SIZE((sz) - 1))

static inline size_t
hash(void *p)
{
	size_t sum;
	uintptr_t u;

	u = (uintptr_t)p >> MALLOC_PAGESHIFT;
	sum = u;
	sum = (sum << 7) - sum + (u >> 16);
#ifdef __LP64__
	sum = (sum << 7) - sum + (u >> 32);
	sum = (sum << 7) - sum + (u >> 48);
#endif
	return sum;
}

static inline struct dir_info *
getpool(void)
{
	if (mopts.malloc_pool[1] == NULL || !mopts.malloc_pool[1]->malloc_mt)
		return mopts.malloc_pool[1];
	else	/* first one reserved for special pool */
		return mopts.malloc_pool[1 + TIB_GET()->tib_tid %
		    (mopts.malloc_mutexes - 1)];
}

static __dead void
wrterror(struct dir_info *d, char *msg, ...)
{
	int		saved_errno = errno;
	va_list		ap;

	dprintf(STDERR_FILENO, "%s(%d) in %s(): ", __progname,
	    getpid(), (d != NULL && d->func) ? d->func : "unknown");
	va_start(ap, msg);
	vdprintf(STDERR_FILENO, msg, ap);
	va_end(ap);
	dprintf(STDERR_FILENO, "\n");

#ifdef MALLOC_STATS
	if (DO_STATS && mopts.malloc_verbose)
		malloc_dump();
#endif

	errno = saved_errno;

	abort();
}

static void
rbytes_init(struct dir_info *d)
{
	arc4random_buf(d->rbytes, sizeof(d->rbytes));
	/* add 1 to account for using d->rbytes[0] */
	d->rbytesused = 1 + d->rbytes[0] % (sizeof(d->rbytes) / 2);
}

static inline u_char
getrbyte(struct dir_info *d)
{
	u_char x;

	if (d->rbytesused >= sizeof(d->rbytes))
		rbytes_init(d);
	x = d->rbytes[d->rbytesused++];
	return x;
}

static void
omalloc_parseopt(char opt)
{
	switch (opt) {
	case '+':
		mopts.malloc_mutexes <<= 1;
		if (mopts.malloc_mutexes > _MALLOC_MUTEXES)
			mopts.malloc_mutexes = _MALLOC_MUTEXES;
		break;
	case '-':
		mopts.malloc_mutexes >>= 1;
		if (mopts.malloc_mutexes < 2)
			mopts.malloc_mutexes = 2;
		break;
	case '>':
		mopts.def_maxcache <<= 1;
		if (mopts.def_maxcache > MALLOC_MAXCACHE)
			mopts.def_maxcache = MALLOC_MAXCACHE;
		break;
	case '<':
		mopts.def_maxcache >>= 1;
		break;
	case 'c':
		mopts.chunk_canaries = 0;
		break;
	case 'C':
		mopts.chunk_canaries = 1;
		break;
#ifdef MALLOC_STATS
	case 'd':
		mopts.malloc_stats = 0;
		break;
	case 'D':
		mopts.malloc_stats = 1;
		break;
#endif /* MALLOC_STATS */
	case 'f':
		mopts.malloc_freecheck = 0;
		mopts.malloc_freeunmap = 0;
		break;
	case 'F':
		mopts.malloc_freecheck = 1;
		mopts.malloc_freeunmap = 1;
		break;
	case 'g':
		mopts.malloc_guard = 0;
		break;
	case 'G':
		mopts.malloc_guard = MALLOC_PAGESIZE;
		break;
	case 'j':
		if (mopts.def_malloc_junk > 0)
			mopts.def_malloc_junk--;
		break;
	case 'J':
		if (mopts.def_malloc_junk < 2)
			mopts.def_malloc_junk++;
		break;
	case 'r':
		mopts.malloc_realloc = 0;
		break;
	case 'R':
		mopts.malloc_realloc = 1;
		break;
	case 'u':
		mopts.malloc_freeunmap = 0;
		break;
	case 'U':
		mopts.malloc_freeunmap = 1;
		break;
#ifdef MALLOC_STATS
	case 'v':
		mopts.malloc_verbose = 0;
		break;
	case 'V':
		mopts.malloc_verbose = 1;
		break;
#endif /* MALLOC_STATS */
	case 'x':
		mopts.malloc_xmalloc = 0;
		break;
	case 'X':
		mopts.malloc_xmalloc = 1;
		break;
	default:
		dprintf(STDERR_FILENO, "malloc() warning: "
                    "unknown char in MALLOC_OPTIONS\n");
		break;
	}
}

static void
omalloc_init(void)
{
	char *p, *q, b[16];
	int i, j;
	const int mib[2] = { CTL_VM, VM_MALLOC_CONF };
	size_t sb;

	/*
	 * Default options
	 */
	mopts.malloc_mutexes = 8;
	mopts.def_malloc_junk = 1;
	mopts.def_maxcache = MALLOC_DEFAULT_CACHE;

	for (i = 0; i < 3; i++) {
		switch (i) {
		case 0:
			sb = sizeof(b);
			j = sysctl(mib, 2, b, &sb, NULL, 0);
			if (j != 0)
				continue;
			p = b;
			break;
		case 1:
			if (issetugid() == 0)
				p = getenv("MALLOC_OPTIONS");
			else
				continue;
			break;
		case 2:
			p = malloc_options;
			break;
		default:
			p = NULL;
		}

		for (; p != NULL && *p != '\0'; p++) {
			switch (*p) {
			case 'S':
				for (q = "CFGJ"; *q != '\0'; q++)
					omalloc_parseopt(*q);
				mopts.def_maxcache = 0;
				break;
			case 's':
				for (q = "cfgj"; *q != '\0'; q++)
					omalloc_parseopt(*q);
				mopts.def_maxcache = MALLOC_DEFAULT_CACHE;
				break;
			default:
				omalloc_parseopt(*p);
				break;
			}
		}
	}

#ifdef MALLOC_STATS
	if (DO_STATS && (atexit(malloc_exit) == -1)) {
		dprintf(STDERR_FILENO, "malloc() warning: atexit(2) failed."
		    " Will not be able to dump stats on exit\n");
	}
#endif

	while ((mopts.malloc_canary = arc4random()) == 0)
		;
	mopts.junk_loc = arc4random();
	if (mopts.chunk_canaries)
		do {
			mopts.chunk_canaries = arc4random();
		} while ((u_char)mopts.chunk_canaries == 0 ||
		    (u_char)mopts.chunk_canaries == SOME_FREEJUNK); 
}

static void
omalloc_poolinit(struct dir_info *d, int mmap_flag)
{
	int i, j;

	d->r = NULL;
	d->rbytesused = sizeof(d->rbytes);
	d->regions_free = d->regions_total = 0;
	for (i = 0; i <= BUCKETS; i++) {
		LIST_INIT(&d->chunk_info_list[i]);
		for (j = 0; j < MALLOC_CHUNK_LISTS; j++)
			LIST_INIT(&d->chunk_dir[i][j]);
	}
	d->mmap_flag = mmap_flag;
	d->malloc_junk = mopts.def_malloc_junk;
	d->canary1 = mopts.malloc_canary ^ (u_int32_t)(uintptr_t)d;
	d->canary2 = ~d->canary1;
}

static int
omalloc_grow(struct dir_info *d)
{
	size_t newtotal;
	size_t newsize;
	size_t mask;
	size_t i, oldpsz;
	struct region_info *p;

	if (d->regions_total > SIZE_MAX / sizeof(struct region_info) / 2)
		return 1;

	newtotal = d->regions_total == 0 ? MALLOC_INITIAL_REGIONS :
	    d->regions_total * 2;
	newsize = PAGEROUND(newtotal * sizeof(struct region_info));
	mask = newtotal - 1;

	/* Don't use cache here, we don't want user uaf touch this */
	p = MMAP(newsize, d->mmap_flag);
	if (p == MAP_FAILED)
		return 1;

	STATS_ADD(d->malloc_used, newsize);
	STATS_ZERO(d->inserts);
	STATS_ZERO(d->insert_collisions);
	for (i = 0; i < d->regions_total; i++) {
		void *q = d->r[i].p;
		if (q != NULL) {
			size_t index = hash(q) & mask;
			STATS_INC(d->inserts);
			while (p[index].p != NULL) {
				index = (index - 1) & mask;
				STATS_INC(d->insert_collisions);
			}
			p[index] = d->r[i];
		}
	}

	if (d->regions_total > 0) {
		oldpsz = PAGEROUND(d->regions_total * sizeof(struct region_info));
		/* clear to avoid meta info ending up in the cache */
		unmap(d, d->r, oldpsz, oldpsz);
	}
	d->regions_free += newtotal - d->regions_total;
	d->regions_total = newtotal;
	d->r = p;
	return 0;
}

/*
 * The hashtable uses the assumption that p is never NULL. This holds since
 * non-MAP_FIXED mappings with hint 0 start at BRKSIZ.
 */
static int
insert(struct dir_info *d, void *p, size_t sz, void *f)
{
	size_t index;
	size_t mask;
	void *q;

	if (d->regions_free * 4 < d->regions_total || d->regions_total == 0) {
		if (omalloc_grow(d))
			return 1;
	}
	mask = d->regions_total - 1;
	index = hash(p) & mask;
	q = d->r[index].p;
	STATS_INC(d->inserts);
	while (q != NULL) {
		index = (index - 1) & mask;
		q = d->r[index].p;
		STATS_INC(d->insert_collisions);
	}
	d->r[index].p = p;
	d->r[index].size = sz;
	STATS_SETF(&d->r[index], f);
	d->regions_free--;
	return 0;
}

static struct region_info *
find(struct dir_info *d, void *p)
{
	size_t index;
	size_t mask = d->regions_total - 1;
	void *q, *r;

	if (mopts.malloc_canary != (d->canary1 ^ (u_int32_t)(uintptr_t)d) ||
	    d->canary1 != ~d->canary2)
		wrterror(d, "internal struct corrupt");
	if (d->r == NULL)
		return NULL;
	p = MASK_POINTER(p);
	index = hash(p) & mask;
	r = d->r[index].p;
	q = MASK_POINTER(r);
	STATS_INC(d->finds);
	while (q != p && r != NULL) {
		index = (index - 1) & mask;
		r = d->r[index].p;
		q = MASK_POINTER(r);
		STATS_INC(d->find_collisions);
	}
	return (q == p && r != NULL) ? &d->r[index] : NULL;
}

static void
delete(struct dir_info *d, struct region_info *ri)
{
	/* algorithm R, Knuth Vol III section 6.4 */
	size_t mask = d->regions_total - 1;
	size_t i, j, r;

	if (d->regions_total & (d->regions_total - 1))
		wrterror(d, "regions_total not 2^x");
	d->regions_free++;
	STATS_INC(d->deletes);

	i = ri - d->r;
	for (;;) {
		d->r[i].p = NULL;
		d->r[i].size = 0;
		j = i;
		for (;;) {
			i = (i - 1) & mask;
			if (d->r[i].p == NULL)
				return;
			r = hash(d->r[i].p) & mask;
			if ((i <= r && r < j) || (r < j && j < i) ||
			    (j < i && i <= r))
				continue;
			d->r[j] = d->r[i];
			STATS_INC(d->delete_moves);
			break;
		}

	}
}

static inline void
junk_free(int junk, void *p, size_t sz)
{
	size_t i, step = 1;
	uint64_t *lp = p;

	if (junk == 0 || sz == 0)
		return;
	sz /= sizeof(uint64_t);
	if (junk == 1) {
		if (sz > MALLOC_PAGESIZE / sizeof(uint64_t))
			sz = MALLOC_PAGESIZE / sizeof(uint64_t);
		step = sz / 4;
		if (step == 0)
			step = 1;
	}
	/* Do not always put the free junk bytes in the same spot.
	   There is modulo bias here, but we ignore that. */
	for (i = mopts.junk_loc % step; i < sz; i += step)
		lp[i] = SOME_FREEJUNK_ULL;
}

static inline void
validate_junk(struct dir_info *pool, void *p, size_t sz)
{
	size_t i, step = 1;
	uint64_t *lp = p;

	if (pool->malloc_junk == 0 || sz == 0)
		return;
	sz /= sizeof(uint64_t);
	if (pool->malloc_junk == 1) {
		if (sz > MALLOC_PAGESIZE / sizeof(uint64_t))
			sz = MALLOC_PAGESIZE / sizeof(uint64_t);
		step = sz / 4;
		if (step == 0)
			step = 1;
	}
	/* see junk_free */
	for (i = mopts.junk_loc % step; i < sz; i += step) {
		if (lp[i] != SOME_FREEJUNK_ULL)
			wrterror(pool, "write after free %p", p);
	}
}


/*
 * Cache maintenance. 
 * Opposed to the regular region data structure, the sizes in the
 * cache are in MALLOC_PAGESIZE units.
 */
static void
unmap(struct dir_info *d, void *p, size_t sz, size_t clear)
{
	size_t psz = sz >> MALLOC_PAGESHIFT;
	void *r;
	u_short i;
	struct smallcache *cache;

	if (sz != PAGEROUND(sz) || psz == 0)
		wrterror(d, "munmap round");

	if (d->bigcache_size > 0 && psz > MAX_SMALLCACHEABLE_SIZE &&
	    psz <= MAX_BIGCACHEABLE_SIZE) {
		u_short base = getrbyte(d);
		u_short j;

		/* don't look through all slots */
		for (j = 0; j < d->bigcache_size / 4; j++) {
			i = (base + j) & (d->bigcache_size - 1);
			if (d->bigcache_used <
			    BIGCACHE_FILL(d->bigcache_size))  {
				if (d->bigcache[i].psize == 0)
					break;
			} else {
				if (d->bigcache[i].psize != 0)
					break;
			}
		}
		/* if we didn't find a preferred slot, use random one */
		if (d->bigcache[i].psize != 0) {
			size_t tmp;

			r = d->bigcache[i].page;
			d->bigcache_used -= d->bigcache[i].psize;
			tmp = d->bigcache[i].psize << MALLOC_PAGESHIFT;
			if (!mopts.malloc_freeunmap)
				validate_junk(d, r, tmp);
			if (munmap(r, tmp))
				 wrterror(d, "munmap %p", r);
			STATS_SUB(d->malloc_used, tmp);
		}
		
		if (clear > 0)
			explicit_bzero(p, clear);
		if (mopts.malloc_freeunmap) {
			if (mprotect(p, sz, PROT_NONE))
				wrterror(d, "mprotect %p", r);
		} else
			junk_free(d->malloc_junk, p, sz);
		d->bigcache[i].page = p;
		d->bigcache[i].psize = psz;
		d->bigcache_used += psz;
		return;
	}
	if (psz > MAX_SMALLCACHEABLE_SIZE || d->smallcache[psz - 1].max == 0) {
		if (munmap(p, sz))
			wrterror(d, "munmap %p", p);
		STATS_SUB(d->malloc_used, sz);
		return;
	}
	cache = &d->smallcache[psz - 1];
	if (cache->length == cache->max) {
		int fresh;
		/* use a random slot */
		i = getrbyte(d) & (cache->max - 1);
		r = cache->pages[i];
		fresh = (uintptr_t)r & 1;
		*(uintptr_t*)&r &= ~1ULL;
		if (!fresh && !mopts.malloc_freeunmap)
			validate_junk(d, r, sz);
		if (munmap(r, sz))
			wrterror(d, "munmap %p", r);
		STATS_SUB(d->malloc_used, sz);
		cache->length--;
	} else
		i = cache->length;

	/* fill slot */
	if (clear > 0)
		explicit_bzero(p, clear);
	if (mopts.malloc_freeunmap)
		mprotect(p, sz, PROT_NONE);
	else
		junk_free(d->malloc_junk, p, sz);
	cache->pages[i] = p;
	cache->length++;
}

static void *
map(struct dir_info *d, size_t sz, int zero_fill)
{
	size_t psz = sz >> MALLOC_PAGESHIFT;
	u_short i;
	void *p;
	struct smallcache *cache;

	if (mopts.malloc_canary != (d->canary1 ^ (u_int32_t)(uintptr_t)d) ||
	    d->canary1 != ~d->canary2)
		wrterror(d, "internal struct corrupt");
	if (sz != PAGEROUND(sz) || psz == 0)
		wrterror(d, "map round");

	
	if (d->bigcache_size > 0 && psz > MAX_SMALLCACHEABLE_SIZE &&
	    psz <= MAX_BIGCACHEABLE_SIZE) {
		size_t base = getrbyte(d);
		size_t cached = d->bigcache_used;
		ushort j;

		for (j = 0; j < d->bigcache_size && cached >= psz; j++) {
			i = (j + base) & (d->bigcache_size - 1);
			if (d->bigcache[i].psize == psz) {
				p = d->bigcache[i].page;
				d->bigcache_used -= psz;
				d->bigcache[i].page = NULL;
				d->bigcache[i].psize = 0;

				if (!mopts.malloc_freeunmap)
					validate_junk(d, p, sz);
				if (mopts.malloc_freeunmap)
					mprotect(p, sz, PROT_READ | PROT_WRITE);
				if (zero_fill)
					memset(p, 0, sz);
				else if (mopts.malloc_freeunmap)
					junk_free(d->malloc_junk, p, sz);
				return p;
			}
			cached -= d->bigcache[i].psize;
		}
	}
	if (psz <= MAX_SMALLCACHEABLE_SIZE && d->smallcache[psz - 1].max > 0) {
		cache = &d->smallcache[psz - 1];
		if (cache->length > 0) {
			int fresh;
			if (cache->length == 1)
				p = cache->pages[--cache->length];
			else {
				i = getrbyte(d) % cache->length;
				p = cache->pages[i];
				cache->pages[i] = cache->pages[--cache->length];
			}
			/* check if page was not junked, i.e. "fresh
			   we use the lsb of the pointer for that */	
			fresh = (uintptr_t)p & 1UL;
			*(uintptr_t*)&p &= ~1UL;
			if (!fresh && !mopts.malloc_freeunmap)
				validate_junk(d, p, sz);
			if (mopts.malloc_freeunmap)
				mprotect(p, sz, PROT_READ | PROT_WRITE);
			if (zero_fill)
				memset(p, 0, sz);
			else if (mopts.malloc_freeunmap)
				junk_free(d->malloc_junk, p, sz);
			return p;
		}
		if (psz <= 1) {
			p = MMAP(cache->max * sz, d->mmap_flag);
			if (p != MAP_FAILED) {
				STATS_ADD(d->malloc_used, cache->max * sz);
				cache->length = cache->max - 1;
				for (i = 0; i < cache->max - 1; i++) {
					void *q = (char*)p + i * sz;
					cache->pages[i] = q;
					/* mark pointer in slot as not junked */
					*(uintptr_t*)&cache->pages[i] |= 1UL;
				}
				if (mopts.malloc_freeunmap)
					mprotect(p, (cache->max - 1) * sz,
					    PROT_NONE);
				p = (char*)p + (cache->max - 1) * sz;
				/* zero fill not needed, freshly mmapped */
				return p;
			}
		}

	}
	p = MMAP(sz, d->mmap_flag);
	if (p != MAP_FAILED)
		STATS_ADD(d->malloc_used, sz);
	/* zero fill not needed */
	return p;
}

static void
init_chunk_info(struct dir_info *d, struct chunk_info *p, u_int bucket)
{
	u_int i;

	p->bucket = bucket;
	p->total = p->free = MALLOC_PAGESIZE / B2ALLOC(bucket);
	p->offset = bucket == 0 ? 0xdead : howmany(p->total, MALLOC_BITS);
	p->canary = (u_short)d->canary1;

	/* set all valid bits in the bitmap */
 	i = p->total - 1;	
	memset(p->bits, 0xff, sizeof(p->bits[0]) * (i / MALLOC_BITS));
	p->bits[i / MALLOC_BITS] = (2U << (i % MALLOC_BITS)) - 1;
}

static struct chunk_info *
alloc_chunk_info(struct dir_info *d, u_int bucket)
{
	struct chunk_info *p;

	if (LIST_EMPTY(&d->chunk_info_list[bucket])) {
		const size_t chunk_pages = 64;
		size_t size, count, i;
		char *q;

		count = MALLOC_PAGESIZE / B2ALLOC(bucket);

		size = howmany(count, MALLOC_BITS);
		size = sizeof(struct chunk_info) + (size - 1) * sizeof(u_short);
		if (mopts.chunk_canaries)
			size += count * sizeof(u_short);
		size = _ALIGN(size);
		count = MALLOC_PAGESIZE / size;

		/* Don't use cache here, we don't want user uaf touch this */
		if (d->chunk_pages_used == chunk_pages ||
		     d->chunk_pages == NULL) {
			q = MMAP(MALLOC_PAGESIZE * chunk_pages, d->mmap_flag);
			if (q == MAP_FAILED)
				return NULL;
			d->chunk_pages = q;
			d->chunk_pages_used = 0;
			STATS_ADD(d->malloc_used, MALLOC_PAGESIZE *
			    chunk_pages);
		}
		q = (char *)d->chunk_pages + d->chunk_pages_used *
		    MALLOC_PAGESIZE;
		d->chunk_pages_used++;

		for (i = 0; i < count; i++, q += size) {
			p = (struct chunk_info *)q;
			LIST_INSERT_HEAD(&d->chunk_info_list[bucket], p, entries);
		}
	}
	p = LIST_FIRST(&d->chunk_info_list[bucket]);
	LIST_REMOVE(p, entries);
	if (p->total == 0)
		init_chunk_info(d, p, bucket);
	return p;
}

/*
 * Allocate a page of chunks
 */
static struct chunk_info *
omalloc_make_chunks(struct dir_info *d, u_int bucket, u_int listnum)
{
	struct chunk_info *bp;
	void *pp;

	/* Allocate a new bucket */
	pp = map(d, MALLOC_PAGESIZE, 0);
	if (pp == MAP_FAILED)
		return NULL;

	/* memory protect the page allocated in the malloc(0) case */
	if (bucket == 0 && mprotect(pp, MALLOC_PAGESIZE, PROT_NONE) == -1)
		goto err;

	bp = alloc_chunk_info(d, bucket);
	if (bp == NULL)
		goto err;
	bp->page = pp;

	if (insert(d, (void *)((uintptr_t)pp | (bucket + 1)), (uintptr_t)bp,
	    NULL))
		goto err;
	LIST_INSERT_HEAD(&d->chunk_dir[bucket][listnum], bp, entries);

	if (bucket > 0 && d->malloc_junk != 0)
		memset(pp, SOME_FREEJUNK, MALLOC_PAGESIZE);

	return bp;

err:
	unmap(d, pp, MALLOC_PAGESIZE, 0);
	return NULL;
}

static inline unsigned int
lb(u_int x)
{
	/* I need an extension just for integer-length (: */
	return (sizeof(int) * CHAR_BIT - 1) - __builtin_clz(x);
}

/* https://pvk.ca/Blog/2015/06/27/linear-log-bucketing-fast-versatile-simple/
   via Tony Finch */
static inline unsigned int
bin_of(unsigned int size)
{
	const unsigned int linear = 6;
	const unsigned int subbin = 2;

	unsigned int mask, range, rounded, sub_index, rounded_size;
	unsigned int n_bits, shift;

	n_bits = lb(size | (1U << linear));
	shift = n_bits - subbin;
	mask = (1ULL << shift) - 1;
	rounded = size + mask; /* XXX: overflow. */
	sub_index = rounded >> shift;
	range = n_bits - linear;

	rounded_size = rounded & ~mask;
	return rounded_size;
}

static inline u_short
find_bucket(u_short size)
{
	/* malloc(0) is special */
	if (size == 0)
		return 0;
	if (size < MALLOC_MINSIZE)
		size = MALLOC_MINSIZE;
	if (mopts.def_maxcache != 0)
		size = bin_of(size);
	return howmany(size, MALLOC_MINSIZE);
}

static void
fill_canary(char *ptr, size_t sz, size_t allocated)
{
	size_t check_sz = allocated - sz;

	if (check_sz > CHUNK_CHECK_LENGTH)
		check_sz = CHUNK_CHECK_LENGTH;
	memset(ptr + sz, mopts.chunk_canaries, check_sz);
}

/*
 * Allocate a chunk
 */
static void *
malloc_bytes(struct dir_info *d, size_t size, void *f)
{
	u_int i, r, bucket, listnum;
	size_t k;
	u_short	*lp;
	struct chunk_info *bp;
	void *p;

	if (mopts.malloc_canary != (d->canary1 ^ (u_int32_t)(uintptr_t)d) ||
	    d->canary1 != ~d->canary2)
		wrterror(d, "internal struct corrupt");

	bucket = find_bucket(size);

	r = ((u_int)getrbyte(d) << 8) | getrbyte(d);
	listnum = r % MALLOC_CHUNK_LISTS;

	/* If it's empty, make a page more of that size chunks */
	if ((bp = LIST_FIRST(&d->chunk_dir[bucket][listnum])) == NULL) {
		bp = omalloc_make_chunks(d, bucket, listnum);
		if (bp == NULL)
			return NULL;
	}

	if (bp->canary != (u_short)d->canary1)
		wrterror(d, "chunk info corrupted");

	/* bias, as bp->total is not a power of 2 */
	i = (r / MALLOC_CHUNK_LISTS) % bp->total;

	/* potentially start somewhere in a short */
	lp = &bp->bits[i / MALLOC_BITS];
	if (*lp) {
		int j = i % MALLOC_BITS; /* j must be signed */
		k = ffs(*lp >> j);
		if (k != 0) {
			k += j - 1;
			goto found;
		}
	}
	/* no bit halfway, go to next full short */
	i /= MALLOC_BITS;
	for (;;) {
		if (++i >= howmany(bp->total, MALLOC_BITS))
			i = 0;
		lp = &bp->bits[i];
		if (*lp) {
			k = ffs(*lp) - 1;
			break;
		}
	}
found:
	if (i == 0 && k == 0 && DO_STATS) {
		struct region_info *r = find(d, bp->page);
		STATS_SETF(r, f);
	}

	*lp ^= 1 << k;

	/* If there are no more free, remove from free-list */
	if (--bp->free == 0)
		LIST_REMOVE(bp, entries);

	/* Adjust to the real offset of that chunk */
	k += (lp - bp->bits) * MALLOC_BITS;

	if (mopts.chunk_canaries && size > 0)
		bp->bits[bp->offset + k] = size;

	k *= B2ALLOC(bp->bucket);

	p = (char *)bp->page + k;
	if (bp->bucket > 0) {
		validate_junk(d, p, B2SIZE(bp->bucket));
		if (mopts.chunk_canaries)
			fill_canary(p, size, B2SIZE(bp->bucket));
	}
	return p;
}

static void
validate_canary(struct dir_info *d, u_char *ptr, size_t sz, size_t allocated)
{
	size_t check_sz = allocated - sz;
	u_char *p, *q;

	if (check_sz > CHUNK_CHECK_LENGTH)
		check_sz = CHUNK_CHECK_LENGTH;
	p = ptr + sz;
	q = p + check_sz;

	while (p < q) {
		if (*p != (u_char)mopts.chunk_canaries && *p != SOME_JUNK) {
			wrterror(d, "canary corrupted %p %#tx@%#zx%s",
			    ptr, p - ptr, sz,
			    *p == SOME_FREEJUNK ? " (double free?)" : "");
		}
		p++;
	}
}

static uint32_t
find_chunknum(struct dir_info *d, struct chunk_info *info, void *ptr, int check)
{
	uint32_t chunknum;

	if (info->canary != (u_short)d->canary1)
		wrterror(d, "chunk info corrupted");

	/* Find the chunk number on the page */
	chunknum = ((uintptr_t)ptr & MALLOC_PAGEMASK) / B2ALLOC(info->bucket);

	if ((uintptr_t)ptr & (MALLOC_MINSIZE - 1))
		wrterror(d, "modified chunk-pointer %p", ptr);
	if (info->bits[chunknum / MALLOC_BITS] &
	    (1U << (chunknum % MALLOC_BITS)))
		wrterror(d, "double free %p", ptr);
	if (check && info->bucket > 0) {
		validate_canary(d, ptr, info->bits[info->offset + chunknum],
		    B2SIZE(info->bucket));
	}
	return chunknum;
}

/*
 * Free a chunk, and possibly the page it's on, if the page becomes empty.
 */
static void
free_bytes(struct dir_info *d, struct region_info *r, void *ptr)
{
	struct chunk_head *mp;
	struct chunk_info *info;
	uint32_t chunknum;
	uint32_t listnum;

	info = (struct chunk_info *)r->size;
	chunknum = find_chunknum(d, info, ptr, 0);

	if (chunknum == 0)
		STATS_SETF(r, NULL);

	info->bits[chunknum / MALLOC_BITS] |= 1U << (chunknum % MALLOC_BITS);
	info->free++;

	if (info->free == 1) {
		/* Page became non-full */
		listnum = getrbyte(d) % MALLOC_CHUNK_LISTS;
		mp = &d->chunk_dir[info->bucket][listnum];
		LIST_INSERT_HEAD(mp, info, entries);
		return;
	}

	if (info->free != info->total)
		return;

	LIST_REMOVE(info, entries);

	if (info->bucket == 0 && !mopts.malloc_freeunmap)
		mprotect(info->page, MALLOC_PAGESIZE, PROT_READ | PROT_WRITE);
	unmap(d, info->page, MALLOC_PAGESIZE, 0);

	delete(d, r);
	mp = &d->chunk_info_list[info->bucket];
	LIST_INSERT_HEAD(mp, info, entries);
}



static void *
omalloc(struct dir_info *pool, size_t sz, int zero_fill, void *f)
{
	void *p;
	size_t psz;

	if (sz > MALLOC_MAXCHUNK) {
		if (sz >= SIZE_MAX - mopts.malloc_guard - MALLOC_PAGESIZE) {
			errno = ENOMEM;
			return NULL;
		}
		sz += mopts.malloc_guard;
		psz = PAGEROUND(sz);
		p = map(pool, psz, zero_fill);
		if (p == MAP_FAILED) {
			errno = ENOMEM;
			return NULL;
		}
		if (insert(pool, p, sz, f)) {
			unmap(pool, p, psz, 0);
			errno = ENOMEM;
			return NULL;
		}
		if (mopts.malloc_guard) {
			if (mprotect((char *)p + psz - mopts.malloc_guard,
			    mopts.malloc_guard, PROT_NONE))
				wrterror(pool, "mprotect");
			STATS_ADD(pool->malloc_guarded, mopts.malloc_guard);
		}

		if (MALLOC_MOVE_COND(sz)) {
			/* fill whole allocation */
			if (pool->malloc_junk == 2)
				memset(p, SOME_JUNK, psz - mopts.malloc_guard);
			/* shift towards the end */
			p = MALLOC_MOVE(p, sz);
			/* fill zeros if needed and overwritten above */
			if (zero_fill && pool->malloc_junk == 2)
				memset(p, 0, sz - mopts.malloc_guard);
		} else {
			if (pool->malloc_junk == 2) {
				if (zero_fill)
					memset((char *)p + sz -
					    mopts.malloc_guard, SOME_JUNK,
					    psz - sz);
				else
					memset(p, SOME_JUNK,
					    psz - mopts.malloc_guard);
			} else if (mopts.chunk_canaries)
				fill_canary(p, sz - mopts.malloc_guard,
				    psz - mopts.malloc_guard);
		}

	} else {
		/* takes care of SOME_JUNK */
		p = malloc_bytes(pool, sz, f);
		if (zero_fill && p != NULL && sz > 0)
			memset(p, 0, sz);
	}

	return p;
}

/*
 * Common function for handling recursion.  Only
 * print the error message once, to avoid making the problem
 * potentially worse.
 */
static void
malloc_recurse(struct dir_info *d)
{
	static int noprint;

	if (noprint == 0) {
		noprint = 1;
		wrterror(d, "recursive call");
	}
	d->active--;
	_MALLOC_UNLOCK(d->mutex);
	errno = EDEADLK;
}

void
_malloc_init(int from_rthreads)
{
	u_int i, j, nmutexes;
	struct dir_info *d;

	_MALLOC_LOCK(1);
	if (!from_rthreads && mopts.malloc_pool[1]) {
		_MALLOC_UNLOCK(1);
		return;
	}
	if (!mopts.malloc_canary) {
		char *p;
		size_t sz, d_avail;

		omalloc_init();
		/*
		 * Allocate dir_infos with a guard page on either side. Also
		 * randomise offset inside the page at which the dir_infos
		 * lay (subject to alignment by 1 << MALLOC_MINSHIFT)
		 */
		sz = mopts.malloc_mutexes * sizeof(*d) + 2 * MALLOC_PAGESIZE;
		if ((p = MMAPNONE(sz, 0)) == MAP_FAILED)
			wrterror(NULL, "malloc_init mmap1 failed");
		if (mprotect(p + MALLOC_PAGESIZE, mopts.malloc_mutexes * sizeof(*d),
		    PROT_READ | PROT_WRITE))
			wrterror(NULL, "malloc_init mprotect1 failed");
		if (mimmutable(p, sz))
			wrterror(NULL, "malloc_init mimmutable1 failed");
		d_avail = (((mopts.malloc_mutexes * sizeof(*d) + MALLOC_PAGEMASK) &
		    ~MALLOC_PAGEMASK) - (mopts.malloc_mutexes * sizeof(*d))) >>
		    MALLOC_MINSHIFT;
		d = (struct dir_info *)(p + MALLOC_PAGESIZE +
		    (arc4random_uniform(d_avail) << MALLOC_MINSHIFT));
		STATS_ADD(d[1].malloc_used, sz);
		for (i = 0; i < mopts.malloc_mutexes; i++)
			mopts.malloc_pool[i] = &d[i];
		mopts.internal_funcs = 1;
		if (((uintptr_t)&malloc_readonly & MALLOC_PAGEMASK) == 0) {
			if (mprotect(&malloc_readonly, sizeof(malloc_readonly),
			    PROT_READ))
				wrterror(NULL, "malloc_init mprotect r/o failed");
			if (mimmutable(&malloc_readonly, sizeof(malloc_readonly)))
				wrterror(NULL, "malloc_init mimmutable r/o failed");
		}
	}

	nmutexes = from_rthreads ? mopts.malloc_mutexes : 2;
	for (i = 0; i < nmutexes; i++) {
		d = mopts.malloc_pool[i];
		d->malloc_mt = from_rthreads;
		if (d->canary1 == ~d->canary2)
			continue;
		if (i == 0) {
			omalloc_poolinit(d, MAP_CONCEAL);
			d->malloc_junk = 2;
			d->bigcache_size = 0;
			for (j = 0; j < MAX_SMALLCACHEABLE_SIZE; j++)
				d->smallcache[j].max = 0;
		} else {
			size_t sz = 0;

			omalloc_poolinit(d, 0);
			d->malloc_junk = mopts.def_malloc_junk;
			d->bigcache_size = mopts.def_maxcache;
			for (j = 0; j < MAX_SMALLCACHEABLE_SIZE; j++) {
				d->smallcache[j].max =
				    mopts.def_maxcache >> (j / 8);
				sz += d->smallcache[j].max * sizeof(void *);
			}
			sz += d->bigcache_size * sizeof(struct bigcache);
			if (sz > 0) {
				void *p = MMAP(sz, 0);
				if (p == MAP_FAILED)
					wrterror(NULL,
					    "malloc_init mmap2 failed");
				if (mimmutable(p, sz))
					wrterror(NULL, "malloc_init mimmutable2 failed");
				for (j = 0; j < MAX_SMALLCACHEABLE_SIZE; j++) {
					d->smallcache[j].pages = p;
					p = (char *)p + d->smallcache[j].max *
					    sizeof(void *);
				}
				d->bigcache = p;
			}
		}
		d->mutex = i;
	}

	_MALLOC_UNLOCK(1);
}
DEF_STRONG(_malloc_init);

#define PROLOGUE(p, fn)			\
	d = (p); 			\
	if (d == NULL) { 		\
		_malloc_init(0);	\
		d = (p);		\
	}				\
	_MALLOC_LOCK(d->mutex);		\
	d->func = fn;			\
	if (d->active++) {		\
		malloc_recurse(d);	\
		return NULL;		\
	}				\

#define EPILOGUE()				\
	d->active--;				\
	_MALLOC_UNLOCK(d->mutex);		\
	if (r == NULL && mopts.malloc_xmalloc)	\
		wrterror(d, "out of memory");	\
	if (r != NULL)				\
		errno = saved_errno;		\
	
void *
malloc(size_t size)
{
	void *r;
	struct dir_info *d;
	int saved_errno = errno;

	PROLOGUE(getpool(), "malloc")
	r = omalloc(d, size, 0, CALLER);
	EPILOGUE()
	return r;
}
DEF_STRONG(malloc);

void *
malloc_conceal(size_t size)
{
	void *r;
	struct dir_info *d;
	int saved_errno = errno;

	PROLOGUE(mopts.malloc_pool[0], "malloc_conceal")
	r = omalloc(d, size, 0, CALLER);
	EPILOGUE()
	return r;
}
DEF_WEAK(malloc_conceal);

static struct region_info *
findpool(void *p, struct dir_info *argpool, struct dir_info **foundpool,
    char **saved_function)
{
	struct dir_info *pool = argpool;
	struct region_info *r = find(pool, p);

	STATS_INC(pool->pool_searches);
	if (r == NULL) {
		u_int i, nmutexes;

		nmutexes = mopts.malloc_pool[1]->malloc_mt ? mopts.malloc_mutexes : 2;
		STATS_INC(pool->other_pool);
		for (i = 1; i < nmutexes; i++) {
			u_int j = (argpool->mutex + i) & (nmutexes - 1);

			pool->active--;
			_MALLOC_UNLOCK(pool->mutex);
			pool = mopts.malloc_pool[j];
			_MALLOC_LOCK(pool->mutex);
			pool->active++;
			r = find(pool, p);
			if (r != NULL) {
				*saved_function = pool->func;
				pool->func = argpool->func;
				break;
			}
		}
		if (r == NULL)
			wrterror(argpool, "bogus pointer (double free?) %p", p);
	}
	*foundpool = pool;
	return r;
}

static void
ofree(struct dir_info **argpool, void *p, int clear, int check, size_t argsz)
{
	struct region_info *r;
	struct dir_info *pool;
	char *saved_function;
	size_t sz;

	r = findpool(p, *argpool, &pool, &saved_function);

	REALSIZE(sz, r);
	if (pool->mmap_flag) {
		clear = 1;
		if (!check) {
			argsz = sz;
			if (sz > MALLOC_MAXCHUNK)
				argsz -= mopts.malloc_guard;
		}
	}
	if (check) {
		if (sz <= MALLOC_MAXCHUNK) {
			if (mopts.chunk_canaries && sz > 0) {
				struct chunk_info *info =
				    (struct chunk_info *)r->size;
				uint32_t chunknum =
				    find_chunknum(pool, info, p, 0);

				if (info->bits[info->offset + chunknum] < argsz)
					wrterror(pool, "recorded size %hu"
					    " < %zu",
					    info->bits[info->offset + chunknum],
					    argsz);
			} else {
				if (sz < argsz)
					wrterror(pool, "chunk size %zu < %zu",
					    sz, argsz);
			}
		} else if (sz - mopts.malloc_guard < argsz) {
			wrterror(pool, "recorded size %zu < %zu",
			    sz - mopts.malloc_guard, argsz);
		}
	}
	if (sz > MALLOC_MAXCHUNK) {
		if (!MALLOC_MOVE_COND(sz)) {
			if (r->p != p)
				wrterror(pool, "bogus pointer %p", p);
			if (mopts.chunk_canaries)
				validate_canary(pool, p,
				    sz - mopts.malloc_guard,
				    PAGEROUND(sz - mopts.malloc_guard));
		} else {
			/* shifted towards the end */
			if (p != MALLOC_MOVE(r->p, sz))
				wrterror(pool, "bogus moved pointer %p", p);
			p = r->p;
		}
		if (mopts.malloc_guard) {
			if (sz < mopts.malloc_guard)
				wrterror(pool, "guard size");
			if (!mopts.malloc_freeunmap) {
				if (mprotect((char *)p + PAGEROUND(sz) -
				    mopts.malloc_guard, mopts.malloc_guard,
				    PROT_READ | PROT_WRITE))
					wrterror(pool, "mprotect");
			}
			STATS_SUB(pool->malloc_guarded, mopts.malloc_guard);
		}
		unmap(pool, p, PAGEROUND(sz), clear ? argsz : 0);
		delete(pool, r);
	} else {
		void *tmp;
		u_int i;

		/* Validate and optionally canary check */
		struct chunk_info *info = (struct chunk_info *)r->size;
		if (B2SIZE(info->bucket) != sz)
			wrterror(pool, "internal struct corrupt");
		find_chunknum(pool, info, p, mopts.chunk_canaries);

		if (mopts.malloc_freecheck) {
			for (i = 0; i <= MALLOC_DELAYED_CHUNK_MASK; i++) {
				tmp = pool->delayed_chunks[i];
				if (tmp == p)
					wrterror(pool,
					    "double free %p", p);
				if (tmp != NULL) {
					size_t tmpsz;

					r = find(pool, tmp);
					if (r == NULL)
						wrterror(pool,
						    "bogus pointer ("
						    "double free?) %p", tmp);
					REALSIZE(tmpsz, r);
					validate_junk(pool, tmp, tmpsz);
				}
			}
		}

		if (clear && argsz > 0)
			explicit_bzero(p, argsz);
		junk_free(pool->malloc_junk, p, sz);

		i = getrbyte(pool) & MALLOC_DELAYED_CHUNK_MASK;
		tmp = p;
		p = pool->delayed_chunks[i];
		if (tmp == p)
			wrterror(pool, "double free %p", p);
		pool->delayed_chunks[i] = tmp;
		if (p != NULL) {
			r = find(pool, p);
			if (r == NULL)
				wrterror(pool,
				    "bogus pointer (double free?) %p", p);
			if (!mopts.malloc_freecheck) {
				REALSIZE(sz, r);
				validate_junk(pool, p, sz);
			}
			free_bytes(pool, r, p);
		}
	}

	if (*argpool != pool) {
		pool->func = saved_function;
		*argpool = pool;
	}
}

void
free(void *ptr)
{
	struct dir_info *d;
	int saved_errno = errno;

	/* This is legal. */
	if (ptr == NULL)
		return;

	d = getpool();
	if (d == NULL)
		wrterror(d, "free() called before allocation");
	_MALLOC_LOCK(d->mutex);
	d->func = "free";
	if (d->active++) {
		malloc_recurse(d);
		return;
	}
	ofree(&d, ptr, 0, 0, 0);
	d->active--;
	_MALLOC_UNLOCK(d->mutex);
	errno = saved_errno;
}
DEF_STRONG(free);

static void
freezero_p(void *ptr, size_t sz)
{
	explicit_bzero(ptr, sz);
	free(ptr);
}

void
freezero(void *ptr, size_t sz)
{
	struct dir_info *d;
	int saved_errno = errno;

	/* This is legal. */
	if (ptr == NULL)
		return;

	if (!mopts.internal_funcs) {
		freezero_p(ptr, sz);
		return;
	}

	d = getpool();
	if (d == NULL)
		wrterror(d, "freezero() called before allocation");
	_MALLOC_LOCK(d->mutex);
	d->func = "freezero";
	if (d->active++) {
		malloc_recurse(d);
		return;
	}
	ofree(&d, ptr, 1, 1, sz);
	d->active--;
	_MALLOC_UNLOCK(d->mutex);
	errno = saved_errno;
}
DEF_WEAK(freezero);

static void *
orealloc(struct dir_info **argpool, void *p, size_t newsz, void *f)
{
	struct region_info *r;
	struct dir_info *pool;
	char *saved_function;
	struct chunk_info *info;
	size_t oldsz, goldsz, gnewsz;
	void *q, *ret;
	uint32_t chunknum;
	int forced;

	if (p == NULL)
		return omalloc(*argpool, newsz, 0, f);

	if (newsz >= SIZE_MAX - mopts.malloc_guard - MALLOC_PAGESIZE) {
		errno = ENOMEM;
		return  NULL;
	}

	r = findpool(p, *argpool, &pool, &saved_function);

	REALSIZE(oldsz, r);
	if (oldsz <= MALLOC_MAXCHUNK) {
		if (DO_STATS || mopts.chunk_canaries) {
			info = (struct chunk_info *)r->size;
			chunknum = find_chunknum(pool, info, p, 0);
		}
	}

	goldsz = oldsz;
	if (oldsz > MALLOC_MAXCHUNK) {
		if (oldsz < mopts.malloc_guard)
			wrterror(pool, "guard size");
		oldsz -= mopts.malloc_guard;
	}

	gnewsz = newsz;
	if (gnewsz > MALLOC_MAXCHUNK)
		gnewsz += mopts.malloc_guard;

	forced = mopts.malloc_realloc || pool->mmap_flag;
	if (newsz > MALLOC_MAXCHUNK && oldsz > MALLOC_MAXCHUNK && !forced) {
		/* First case: from n pages sized allocation to m pages sized
		   allocation, m > n */
		size_t roldsz = PAGEROUND(goldsz);
		size_t rnewsz = PAGEROUND(gnewsz);

		if (rnewsz < roldsz && rnewsz > roldsz / 2 &&
		    roldsz - rnewsz < mopts.def_maxcache * MALLOC_PAGESIZE &&
		    !mopts.malloc_guard) {

			ret = p;
			goto done;
		}

		if (rnewsz > roldsz) {
			/* try to extend existing region */
			if (!mopts.malloc_guard) {
				void *hint = (char *)r->p + roldsz;
				size_t needed = rnewsz - roldsz;

				STATS_INC(pool->cheap_realloc_tries);
				q = MMAPA(hint, needed, MAP_FIXED | __MAP_NOREPLACE | pool->mmap_flag);
				if (q == hint) {
					STATS_ADD(pool->malloc_used, needed);
					if (pool->malloc_junk == 2)
						memset(q, SOME_JUNK, needed);
					r->size = gnewsz;
					if (r->p != p) {
						/* old pointer is moved */
						memmove(r->p, p, oldsz);
						p = r->p;
					}
					if (mopts.chunk_canaries)
						fill_canary(p, newsz,
						    PAGEROUND(newsz));
					STATS_SETF(r, f);
					STATS_INC(pool->cheap_reallocs);
					ret = p;
					goto done;
				}
			}
		} else if (rnewsz < roldsz) {
			/* shrink number of pages */
			if (mopts.malloc_guard) {
				if (mprotect((char *)r->p + rnewsz -
				    mopts.malloc_guard, mopts.malloc_guard,
				    PROT_NONE))
					wrterror(pool, "mprotect");
			}
			if (munmap((char *)r->p + rnewsz, roldsz - rnewsz))
				wrterror(pool, "munmap %p", (char *)r->p +
				    rnewsz);
			STATS_SUB(pool->malloc_used, roldsz - rnewsz);
			r->size = gnewsz;
			if (MALLOC_MOVE_COND(gnewsz)) {
				void *pp = MALLOC_MOVE(r->p, gnewsz);
				memmove(pp, p, newsz);
				p = pp;
			} else if (mopts.chunk_canaries)
				fill_canary(p, newsz, PAGEROUND(newsz));
			STATS_SETF(r, f);
			ret = p;
			goto done;
		} else {
			/* number of pages remains the same */
			void *pp = r->p;

			r->size = gnewsz;
			if (MALLOC_MOVE_COND(gnewsz))
				pp = MALLOC_MOVE(r->p, gnewsz);
			if (p != pp) {
				memmove(pp, p, oldsz < newsz ? oldsz : newsz);
				p = pp;
			}
			if (p == r->p) {
				if (newsz > oldsz && pool->malloc_junk == 2)
					memset((char *)p + newsz, SOME_JUNK,
					    rnewsz - mopts.malloc_guard -
					    newsz);
				if (mopts.chunk_canaries)
					fill_canary(p, newsz, PAGEROUND(newsz));
			}
			STATS_SETF(r, f);
			ret = p;
			goto done;
		}
	}
	if (oldsz <= MALLOC_MAXCHUNK && oldsz > 0 &&
	    newsz <= MALLOC_MAXCHUNK && newsz > 0 &&
	    !forced && find_bucket(newsz) == find_bucket(oldsz)) {
		/* do not reallocate if new size fits good in existing chunk */
		if (pool->malloc_junk == 2)
			memset((char *)p + newsz, SOME_JUNK, oldsz - newsz);
		if (mopts.chunk_canaries) {
			info->bits[info->offset + chunknum] = newsz;
			fill_canary(p, newsz, B2SIZE(info->bucket));
		}
		if (DO_STATS && chunknum == 0)
			STATS_SETF(r, f);
		ret = p;
	} else if (newsz != oldsz || forced) {
		/* create new allocation */
		q = omalloc(pool, newsz, 0, f);
		if (q == NULL) {
			ret = NULL;
			goto done;
		}
		if (newsz != 0 && oldsz != 0)
			memcpy(q, p, oldsz < newsz ? oldsz : newsz);
		ofree(&pool, p, 0, 0, 0);
		ret = q;
	} else {
		/* oldsz == newsz */
		if (newsz != 0)
			wrterror(pool, "realloc internal inconsistency");
		if (DO_STATS && chunknum == 0)
			STATS_SETF(r, f);
		ret = p;
	}
done:
	if (*argpool != pool) {
		pool->func = saved_function;
		*argpool = pool;
	}
	return ret;
}

void *
realloc(void *ptr, size_t size)
{
	struct dir_info *d;
	void *r;
	int saved_errno = errno;

	PROLOGUE(getpool(), "realloc")
	r = orealloc(&d, ptr, size, CALLER);
	EPILOGUE()
	return r;
}
DEF_STRONG(realloc);

/*
 * This is sqrt(SIZE_MAX+1), as s1*s2 <= SIZE_MAX
 * if both s1 < MUL_NO_OVERFLOW and s2 < MUL_NO_OVERFLOW
 */
#define MUL_NO_OVERFLOW	(1UL << (sizeof(size_t) * 4))

void *
calloc(size_t nmemb, size_t size)
{
	struct dir_info *d;
	void *r;
	int saved_errno = errno;

	PROLOGUE(getpool(), "calloc")
	if ((nmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    nmemb > 0 && SIZE_MAX / nmemb < size) {
		d->active--;
		_MALLOC_UNLOCK(d->mutex);
		if (mopts.malloc_xmalloc)
			wrterror(d, "out of memory");
		errno = ENOMEM;
		return NULL;
	}

	size *= nmemb;
	r = omalloc(d, size, 1, CALLER);
	EPILOGUE()
	return r;
}
DEF_STRONG(calloc);

void *
calloc_conceal(size_t nmemb, size_t size)
{
	struct dir_info *d;
	void *r;
	int saved_errno = errno;

	PROLOGUE(mopts.malloc_pool[0], "calloc_conceal")
	if ((nmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    nmemb > 0 && SIZE_MAX / nmemb < size) {
		d->active--;
		_MALLOC_UNLOCK(d->mutex);
		if (mopts.malloc_xmalloc)
			wrterror(d, "out of memory");
		errno = ENOMEM;
		return NULL;
	}

	size *= nmemb;
	r = omalloc(d, size, 1, CALLER);
	EPILOGUE()
	return r;
}
DEF_WEAK(calloc_conceal);

static void *
orecallocarray(struct dir_info **argpool, void *p, size_t oldsize,
    size_t newsize, void *f)
{
	struct region_info *r;
	struct dir_info *pool;
	char *saved_function;
	void *newptr;
	size_t sz;

	if (p == NULL)
		return omalloc(*argpool, newsize, 1, f);

	if (oldsize == newsize)
		return p;

	r = findpool(p, *argpool, &pool, &saved_function);

	REALSIZE(sz, r);
	if (sz <= MALLOC_MAXCHUNK) {
		if (mopts.chunk_canaries && sz > 0) {
			struct chunk_info *info = (struct chunk_info *)r->size;
			uint32_t chunknum = find_chunknum(pool, info, p, 0);

			if (info->bits[info->offset + chunknum] != oldsize)
				wrterror(pool, "recorded size %hu != %zu",
				    info->bits[info->offset + chunknum],
				    oldsize);
		} else {
			if (sz < oldsize)
				wrterror(pool, "chunk size %zu < %zu",
				    sz, oldsize);
		}
	} else {
		if (sz - mopts.malloc_guard < oldsize)
			wrterror(pool, "recorded size %zu < %zu",
			    sz - mopts.malloc_guard, oldsize);
		if (oldsize < (sz - mopts.malloc_guard) / 2)
			wrterror(pool, "recorded size %zu inconsistent with %zu",
			    sz - mopts.malloc_guard, oldsize);
	}

	newptr = omalloc(pool, newsize, 0, f);
	if (newptr == NULL)
		goto done;

	if (newsize > oldsize) {
		memcpy(newptr, p, oldsize);
		memset((char *)newptr + oldsize, 0, newsize - oldsize);
	} else
		memcpy(newptr, p, newsize);

	ofree(&pool, p, 1, 0, oldsize);

done:
	if (*argpool != pool) {
		pool->func = saved_function;
		*argpool = pool;
	}

	return newptr;
}

static void *
recallocarray_p(void *ptr, size_t oldnmemb, size_t newnmemb, size_t size)
{
	size_t oldsize, newsize;
	void *newptr;

	if (ptr == NULL)
		return calloc(newnmemb, size);

	if ((newnmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    newnmemb > 0 && SIZE_MAX / newnmemb < size) {
		errno = ENOMEM;
		return NULL;
	}
	newsize = newnmemb * size;

	if ((oldnmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    oldnmemb > 0 && SIZE_MAX / oldnmemb < size) {
		errno = EINVAL;
		return NULL;
	}
	oldsize = oldnmemb * size;

	/*
	 * Don't bother too much if we're shrinking just a bit,
	 * we do not shrink for series of small steps, oh well.
	 */
	if (newsize <= oldsize) {
		size_t d = oldsize - newsize;

		if (d < oldsize / 2 && d < MALLOC_PAGESIZE) {
			memset((char *)ptr + newsize, 0, d);
			return ptr;
		}
	}

	newptr = malloc(newsize);
	if (newptr == NULL)
		return NULL;

	if (newsize > oldsize) {
		memcpy(newptr, ptr, oldsize);
		memset((char *)newptr + oldsize, 0, newsize - oldsize);
	} else
		memcpy(newptr, ptr, newsize);

	explicit_bzero(ptr, oldsize);
	free(ptr);

	return newptr;
}

void *
recallocarray(void *ptr, size_t oldnmemb, size_t newnmemb, size_t size)
{
	struct dir_info *d;
	size_t oldsize = 0, newsize;
	void *r;
	int saved_errno = errno;

	if (!mopts.internal_funcs)
		return recallocarray_p(ptr, oldnmemb, newnmemb, size);

	PROLOGUE(getpool(), "recallocarray")

	if ((newnmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    newnmemb > 0 && SIZE_MAX / newnmemb < size) {
		d->active--;
		_MALLOC_UNLOCK(d->mutex);
		if (mopts.malloc_xmalloc)
			wrterror(d, "out of memory");
		errno = ENOMEM;
		return NULL;
	}
	newsize = newnmemb * size;

	if (ptr != NULL) {
		if ((oldnmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
		    oldnmemb > 0 && SIZE_MAX / oldnmemb < size) {
			d->active--;
			_MALLOC_UNLOCK(d->mutex);
			errno = EINVAL;
			return NULL;
		}
		oldsize = oldnmemb * size;
	}

	r = orecallocarray(&d, ptr, oldsize, newsize, CALLER);
	EPILOGUE()
	return r;
}
DEF_WEAK(recallocarray);

static void *
mapalign(struct dir_info *d, size_t alignment, size_t sz, int zero_fill)
{
	char *p, *q;

	if (alignment < MALLOC_PAGESIZE || ((alignment - 1) & alignment) != 0)
		wrterror(d, "mapalign bad alignment");
	if (sz != PAGEROUND(sz))
		wrterror(d, "mapalign round");

	/* Allocate sz + alignment bytes of memory, which must include a
	 * subrange of size bytes that is properly aligned.  Unmap the
	 * other bytes, and then return that subrange.
	 */

	/* We need sz + alignment to fit into a size_t. */
	if (alignment > SIZE_MAX - sz)
		return MAP_FAILED;

	p = map(d, sz + alignment, zero_fill);
	if (p == MAP_FAILED)
		return MAP_FAILED;
	q = (char *)(((uintptr_t)p + alignment - 1) & ~(alignment - 1));
	if (q != p) {
		if (munmap(p, q - p))
			wrterror(d, "munmap %p", p);
	}
	if (munmap(q + sz, alignment - (q - p)))
		wrterror(d, "munmap %p", q + sz);
	STATS_SUB(d->malloc_used, alignment);

	return q;
}

static void *
omemalign(struct dir_info *pool, size_t alignment, size_t sz, int zero_fill,
    void *f)
{
	size_t psz;
	void *p;

	/* If between half a page and a page, avoid MALLOC_MOVE. */
	if (sz > MALLOC_MAXCHUNK && sz < MALLOC_PAGESIZE)
		sz = MALLOC_PAGESIZE;
	if (alignment <= MALLOC_PAGESIZE) {
		size_t pof2;
		/*
		 * max(size, alignment) rounded up to power of 2 is enough
		 * to assure the requested alignment. Large regions are
		 * always page aligned.
		 */
		if (sz < alignment)
			sz = alignment;
		if (sz < MALLOC_PAGESIZE) {
			pof2 = MALLOC_MINSIZE;
			while (pof2 < sz)
				pof2 <<= 1;
		} else
			pof2 = sz;
		return omalloc(pool, pof2, zero_fill, f);
	}

	if (sz >= SIZE_MAX - mopts.malloc_guard - MALLOC_PAGESIZE) {
		errno = ENOMEM;
		return NULL;
	}

	if (sz < MALLOC_PAGESIZE)
		sz = MALLOC_PAGESIZE;
	sz += mopts.malloc_guard;
	psz = PAGEROUND(sz);

	p = mapalign(pool, alignment, psz, zero_fill);
	if (p == MAP_FAILED) {
		errno = ENOMEM;
		return NULL;
	}

	if (insert(pool, p, sz, f)) {
		unmap(pool, p, psz, 0);
		errno = ENOMEM;
		return NULL;
	}

	if (mopts.malloc_guard) {
		if (mprotect((char *)p + psz - mopts.malloc_guard,
		    mopts.malloc_guard, PROT_NONE))
			wrterror(pool, "mprotect");
		STATS_ADD(pool->malloc_guarded, mopts.malloc_guard);
	}

	if (pool->malloc_junk == 2) {
		if (zero_fill)
			memset((char *)p + sz - mopts.malloc_guard,
			    SOME_JUNK, psz - sz);
		else
			memset(p, SOME_JUNK, psz - mopts.malloc_guard);
	} else if (mopts.chunk_canaries)
		fill_canary(p, sz - mopts.malloc_guard,
		    psz - mopts.malloc_guard);

	return p;
}

int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
	struct dir_info *d;
	int res, saved_errno = errno;
	void *r;

	/* Make sure that alignment is a large enough power of 2. */
	if (((alignment - 1) & alignment) != 0 || alignment < sizeof(void *))
		return EINVAL;

	d = getpool();
	if (d == NULL) {
		_malloc_init(0);
		d = getpool();
	}
	_MALLOC_LOCK(d->mutex);
	d->func = "posix_memalign";
	if (d->active++) {
		malloc_recurse(d);
		goto err;
	}
	r = omemalign(d, alignment, size, 0, CALLER);
	d->active--;
	_MALLOC_UNLOCK(d->mutex);
	if (r == NULL) {
		if (mopts.malloc_xmalloc)
			wrterror(d, "out of memory");
		goto err;
	}
	errno = saved_errno;
	*memptr = r;
	return 0;

err:
	res = errno;
	errno = saved_errno;
	return res;
}
DEF_STRONG(posix_memalign);

void *
aligned_alloc(size_t alignment, size_t size)
{
	struct dir_info *d;
	int saved_errno = errno;
	void *r;

	/* Make sure that alignment is a positive power of 2. */
	if (((alignment - 1) & alignment) != 0 || alignment == 0) {
		errno = EINVAL;
		return NULL;
	};
	/* Per spec, size should be a multiple of alignment */
	if ((size & (alignment - 1)) != 0) {
		errno = EINVAL;
		return NULL;
	}

	PROLOGUE(getpool(), "aligned_alloc")
	r = omemalign(d, alignment, size, 0, CALLER);
	EPILOGUE()
	return r;
}
DEF_STRONG(aligned_alloc);

#ifdef MALLOC_STATS

static void
ulog(const char *format, ...)
{
	va_list ap;
	static char* buf;
	static size_t filled;
	int len;

	if (buf == NULL)
		buf = MMAP(KTR_USER_MAXLEN, 0);
	if (buf == MAP_FAILED)
		return;

	va_start(ap, format);
	len = vsnprintf(buf + filled, KTR_USER_MAXLEN - filled, format, ap);
	va_end(ap);
	if (len < 0)
		return;
	if (len > KTR_USER_MAXLEN - filled)
		len = KTR_USER_MAXLEN - filled;
	filled += len;
	if (filled > 0) {
		if (filled == KTR_USER_MAXLEN || buf[filled - 1] == '\n') {
			utrace("malloc", buf, filled);
			filled = 0;
		}
	}
}

struct malloc_leak {
	void *f;
	size_t total_size;
	int count;
};

struct leaknode {
	RBT_ENTRY(leaknode) entry;
	struct malloc_leak d;
};

static inline int
leakcmp(const struct leaknode *e1, const struct leaknode *e2)
{
	return e1->d.f < e2->d.f ? -1 : e1->d.f > e2->d.f;
}

RBT_HEAD(leaktree, leaknode);
RBT_PROTOTYPE(leaktree, leaknode, entry, leakcmp);
RBT_GENERATE(leaktree, leaknode, entry, leakcmp);

static void
putleakinfo(struct leaktree *leaks, void *f, size_t sz, int cnt)
{
	struct leaknode key, *p;
	static struct leaknode *page;
	static unsigned int used;

	if (cnt == 0 || page == MAP_FAILED)
		return;

	key.d.f = f;
	p = RBT_FIND(leaktree, leaks, &key);
	if (p == NULL) {
		if (page == NULL ||
		    used >= MALLOC_PAGESIZE / sizeof(struct leaknode)) {
			page = MMAP(MALLOC_PAGESIZE, 0);
			if (page == MAP_FAILED)
				return;
			used = 0;
		}
		p = &page[used++];
		p->d.f = f;
		p->d.total_size = sz * cnt;
		p->d.count = cnt;
		RBT_INSERT(leaktree, leaks, p);
	} else {
		p->d.total_size += sz * cnt;
		p->d.count += cnt;
	}
}

static void
dump_leaks(struct leaktree *leaks)
{
	struct leaknode *p;

	ulog("Leak report:\n");
	ulog("                 f     sum      #    avg\n");

	RBT_FOREACH(p, leaktree, leaks) {
		Dl_info info;
		const char *caller = p->d.f;
		const char *object = ".";

		if (caller != NULL) {
			if (dladdr(p->d.f, &info) != 0) {
				caller -= (uintptr_t)info.dli_fbase;
				object = info.dli_fname;
			}
		}
		ulog("%18p %7zu %6u %6zu addr2line -e %s %p\n",
		    p->d.f, p->d.total_size, p->d.count,
		    p->d.total_size / p->d.count,
		    object, caller);
	}
}

static void
dump_chunk(struct leaktree* leaks, struct chunk_info *p, void *f,
    int fromfreelist)
{
	while (p != NULL) {
		if (mopts.malloc_verbose)
			ulog("chunk %18p %18p %4zu %d/%d\n",
			    p->page, ((p->bits[0] & 1) ? NULL : f),
			    B2SIZE(p->bucket), p->free, p->total);
		if (!fromfreelist) {
			size_t sz =  B2SIZE(p->bucket);
			if (p->bits[0] & 1)
				putleakinfo(leaks, NULL, sz, p->total -
				    p->free);
			else {
				putleakinfo(leaks, f, sz, 1);
				putleakinfo(leaks, NULL, sz,
				    p->total - p->free - 1);
			}
			break;
		}
		p = LIST_NEXT(p, entries);
		if (mopts.malloc_verbose && p != NULL)
			ulog("       ->");
	}
}

static void
dump_free_chunk_info(struct dir_info *d, struct leaktree *leaks)
{
	int i, j, count;
	struct chunk_info *p;

	ulog("Free chunk structs:\n");
	ulog("Bkt) #CI                     page"
	    "                  f size free/n\n");
	for (i = 0; i <= BUCKETS; i++) {
		count = 0;
		LIST_FOREACH(p, &d->chunk_info_list[i], entries)
			count++;
		for (j = 0; j < MALLOC_CHUNK_LISTS; j++) {
			p = LIST_FIRST(&d->chunk_dir[i][j]);
			if (p == NULL && count == 0)
				continue;
			if (j == 0)
				ulog("%3d) %3d ", i, count);
			else
				ulog("         ");
			if (p != NULL)
				dump_chunk(leaks, p, NULL, 1);
			else
				ulog(".\n");
		}
	}

}

static void
dump_free_page_info(struct dir_info *d)
{
	struct smallcache *cache;
	size_t i, total = 0;

	ulog("Cached in small cache:\n");
	for (i = 0; i < MAX_SMALLCACHEABLE_SIZE; i++) {
		cache = &d->smallcache[i];
		if (cache->length != 0)
			ulog("%zu(%u): %u = %zu\n", i + 1, cache->max,
			    cache->length, cache->length * (i + 1));
		total += cache->length * (i + 1);
	}

	ulog("Cached in big cache: %zu/%zu\n", d->bigcache_used,
	    d->bigcache_size);
	for (i = 0; i < d->bigcache_size; i++) {
		if (d->bigcache[i].psize != 0)
			ulog("%zu: %zu\n", i, d->bigcache[i].psize);
		total += d->bigcache[i].psize;
	}
	ulog("Free pages cached: %zu\n", total);
}

static void
malloc_dump1(int poolno, struct dir_info *d, struct leaktree *leaks)
{
	size_t i, realsize;

	if (mopts.malloc_verbose) {
		ulog("Malloc dir of %s pool %d at %p\n", __progname, poolno, d);
		ulog("MT=%d J=%d Fl=%x\n", d->malloc_mt, d->malloc_junk,
		    d->mmap_flag);
		ulog("Region slots free %zu/%zu\n",
			d->regions_free, d->regions_total);
		ulog("Finds %zu/%zu\n", d->finds, d->find_collisions);
		ulog("Inserts %zu/%zu\n", d->inserts, d->insert_collisions);
		ulog("Deletes %zu/%zu\n", d->deletes, d->delete_moves);
		ulog("Cheap reallocs %zu/%zu\n",
		    d->cheap_reallocs, d->cheap_realloc_tries);
		ulog("Other pool searches %zu/%zu\n",
		    d->other_pool, d->pool_searches);
		ulog("In use %zu\n", d->malloc_used);
		ulog("Guarded %zu\n", d->malloc_guarded);
		dump_free_chunk_info(d, leaks);
		dump_free_page_info(d);
		ulog("Hash table:\n");
		ulog("slot)  hash d  type               page                  "
		    "f size [free/n]\n");
	}
	for (i = 0; i < d->regions_total; i++) {
		if (d->r[i].p != NULL) {
			size_t h = hash(d->r[i].p) &
			    (d->regions_total - 1);
			if (mopts.malloc_verbose)
				ulog("%4zx) #%4zx %zd ",
			        i, h, h - i);
			REALSIZE(realsize, &d->r[i]);
			if (realsize > MALLOC_MAXCHUNK) {
				putleakinfo(leaks, d->r[i].f, realsize, 1);
				if (mopts.malloc_verbose)
					ulog("pages %18p %18p %zu\n", d->r[i].p,
				        d->r[i].f, realsize);
			} else
				dump_chunk(leaks,
				    (struct chunk_info *)d->r[i].size,
				    d->r[i].f, 0);
		}
	}
	if (mopts.malloc_verbose)
		ulog("\n");
}

static void
malloc_dump0(int poolno, struct dir_info *pool, struct leaktree *leaks)
{
	int i;
	void *p;
	struct region_info *r;

	if (pool == NULL || pool->r == NULL)
		return;
	for (i = 0; i < MALLOC_DELAYED_CHUNK_MASK + 1; i++) {
		p = pool->delayed_chunks[i];
		if (p == NULL)
			continue;
		r = find(pool, p);
		if (r == NULL)
			wrterror(pool, "bogus pointer in malloc_dump %p", p);
		free_bytes(pool, r, p);
		pool->delayed_chunks[i] = NULL;
	}
	malloc_dump1(poolno, pool, leaks);
}

void
malloc_dump(void)
{
	int i;
	int saved_errno = errno;

	/* XXX leak when run multiple times */
	struct leaktree leaks = RBT_INITIALIZER(&leaks);

	for (i = 0; i < mopts.malloc_mutexes; i++)
		malloc_dump0(i, mopts.malloc_pool[i], &leaks);

	dump_leaks(&leaks);
	ulog("\n");
	errno = saved_errno;
}
DEF_WEAK(malloc_dump);

static void
malloc_exit(void)
{
	int save_errno = errno;

	ulog("******** Start dump %s *******\n", __progname);
	ulog("M=%u I=%d F=%d U=%d J=%d R=%d X=%d C=%d cache=%u "
	    "G=%zu\n",
	    mopts.malloc_mutexes,
	    mopts.internal_funcs, mopts.malloc_freecheck,
	    mopts.malloc_freeunmap, mopts.def_malloc_junk,
	    mopts.malloc_realloc, mopts.malloc_xmalloc,
	    mopts.chunk_canaries, mopts.def_maxcache,
	    mopts.malloc_guard);

	malloc_dump();
	ulog("******** End dump %s *******\n", __progname);
	errno = save_errno;
}

#endif /* MALLOC_STATS */
