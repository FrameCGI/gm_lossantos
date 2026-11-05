#include "lossantos_map.h"
#include "lossantos_beach.h"
#include "lossantos_content.h"
#include "lossantos_clip.h"
#include "lossantos_out.h"
#include "lossantos_slod.h"
#include "lossantos_tile.h"
#include "lossantos_water.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE 1024.0
#define SCALE LOSSANTOS_SCALE


// thread pool
typedef void (*job_fn)(void *ctx, size_t i);
typedef struct {
	job_fn fn;
	void *ctx;
	size_t n;
	volatile LONG64 next;
	int background;
	volatile long *cancel;
	lossantos_progress_fn progress;
	void *pctx;
	const char *stage;
	volatile LONG64 done;
} pool;

static DWORD WINAPI pool_worker(LPVOID arg) {
	pool *p = (pool *)arg;
	if (p->background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
	for (;;) {
		if (p->cancel && *p->cancel) break;
		LONG64 i = InterlockedIncrement64(&p->next) - 1;
		if ((size_t)i >= p->n) break;
		p->fn(p->ctx, (size_t)i);
		LONG64 d = InterlockedIncrement64(&p->done);
		if (p->progress) p->progress(p->pctx, p->stage, (size_t)d, p->n);
	}
	if (p->background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
	return 0;
}

static void run_pool(const lossantos_map_opts *o, const char *stage, size_t n, job_fn fn, void *ctx);
void lossantos_run_pool(const lossantos_map_opts *o, const char *stage, size_t n, void (*fn)(void *, size_t), void *ctx) {
	run_pool(o, stage, n, fn, ctx);
}
static void run_pool(const lossantos_map_opts *o, const char *stage, size_t n, job_fn fn, void *ctx) {
	pool p;
	memset(&p, 0, sizeof p);
	p.fn = fn;
	p.ctx = ctx;
	p.n = n;
	p.background = o->background;
	p.cancel = o->cancel;
	p.progress = o->progress;
	p.pctx = o->progress_ctx;
	p.stage = stage;
	int jobs = o->jobs;
	if (jobs <= 0) {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		jobs = (int)si.dwNumberOfProcessors;
	}
	if ((size_t)jobs > n) jobs = (int)(n ? n : 1);
	HANDLE th[64];
	if (jobs > 64) jobs = 64;
	for (int i = 0; i < jobs; i++)
		th[i] = CreateThread(NULL, 16u << 20, pool_worker, &p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
	WaitForMultipleObjects((DWORD)jobs, th, TRUE, INFINITE);
	for (int i = 0; i < jobs; i++) CloseHandle(th[i]);
}


// one (tile, material) run in a block's spill
typedef struct {
	int32_t tx, ty;
	uint32_t block;           /* rank, nrefs = SLOD */
	uint32_t seq;             /* order inside the block */
	uint32_t mat;             /* global material id */
	uint32_t nverts, nidx;
	uint64_t off;
	uint32_t fam;             /* 0 detail (t), 1 far-land SLOD (s) */
} run_rec;

typedef struct {
	int32_t cx, cy;
	uint32_t block, seq;
	uint32_t ntris;
	uint64_t off;
} phys_rec;

typedef struct cell_job cell_job;
typedef struct {
	int32_t px, py;
	size_t first, last;
} page_job;

typedef struct {
	lossantos_world *w;
	const lossantos_map_opts *o;
	const lossantos_blockref *refs;
	size_t nrefs;
	uint32_t *rank;           /* block index -> merge rank */
	// phase 0 results
	lossantos_install_vec *installs;
	lossantos_texinfo_vec *atlas, *items, *local, *extra;
	lossantos_strvec *missing;
	size_t borrowed;
	int *status;              /* 0 ok, 1 empty, 2 fallback-needed, -1 failed */
	// phase 1
	SRWLOCK lock;
	lossantos_strtab mats;          /* global material names (under lock) */
	LOSSANTOS_VEC(run_rec) runs;
	LOSSANTOS_VEC(phys_rec) phys;
	HANDLE *spill;            /* per block, read handles after phase 1 */
	uint64_t *spill_size;
	int *kind;                /* 0 detail, 1 fallback, 2 empty, -1 failed */
	FILE *log;
	SRWLOCK loglock;
	lossantos_marker_vec markers;   /* beach markers (under lock) */
	lossantos_hset slod_cells;      /* cells holding an SLOD triangle centroid */
	int with_slod;
	lossantos_slod_out slod;
	lossantos_beach beach;
	lossantos_water water;
	int have_water;
	uint32_t sand_mat, water_mat;
	volatile LONG64 sanded, files_written;
	// final pass
	run_rec *tr;              /* sorted runs */
	cell_job *cells;          /* output cells, numeric (x, y) order */
	size_t ntkeys;
	page_job *tpages, *ppages;
	size_t ntpages, nppages;
	size_t *tpage_cells, *ppage_keys;
	phys_rec *pr;
	size_t *pstart;
	size_t npkeys;
	volatile LONG64 tile_tris, tile_verts, chunk_tris, chunk_dropped;
	int failed;
} map_ctx;

static void spill_path(const map_ctx *c, size_t bi, wchar_t *p, size_t cap) {
	swprintf(p, cap, L"%ls\\b%04zu.gtavspill", c->o->work_dir, bi);
}

static void logf_(map_ctx *c, const char *fmt, ...) {
	if (!c->log) return;
	va_list ap;
	va_start(ap, fmt);
	AcquireSRWLockExclusive(&c->loglock);
	vfprintf(c->log, fmt, ap);
	fflush(c->log);
	ReleaseSRWLockExclusive(&c->loglock);
	va_end(ap);
}


static void phase0_job(void *ctx, size_t i) {
	map_ctx *c = (map_ctx *)ctx;
	lossantos_block_out out;
	memset(&out, 0, sizeof out);
	if (lossantos_block_run(c->w, &c->refs[i], 0, &out)) {
		c->status[i] = 0;
		c->atlas[i].p = out.atlas_items.p;
		c->atlas[i].n = out.atlas_items.n;
		c->atlas[i].cap = out.atlas_items.cap;
		memset(&out.atlas_items, 0, sizeof out.atlas_items);
		c->items[i] = out.items;
		memset(&out.items, 0, sizeof out.items);
		c->local[i] = out.local;
		memset(&out.local, 0, sizeof out.local);
		c->missing[i] = out.missing;
		memset(&out.missing, 0, sizeof out.missing);
	} else {
		c->status[i] = out.empty ? 1 : 2;
		logf_(c, "phase0 %s: %s\n", c->refs[i].stem, out.error);
	}
	lossantos_block_out_free(&out);
}


typedef struct {
	lossantos_soup s;                 /* pieces */
	LOSSANTOS_VEC(uint64_t) key;      /* (tile, material) sort key per piece */
} pieces;

static uint64_t tile_key(int64_t tx, int64_t ty) {
	return ((uint64_t)(uint32_t)(tx + 32768) << 16) | (uint32_t)(ty + 32768);
}

typedef struct {
	pieces *pc;
	uint32_t mat;
} piece_ctx;

static void on_piece(void *ctx, int64_t cx, int64_t cy, const double *pos, const double *nrm, const double *uv) {
	piece_ctx *p = (piece_ctx *)ctx;
	pieces *pc = p->pc;
	if (pc->s.n == pc->s.cap && !lossantos_soup_reserve(&pc->s, pc->s.n + 1)) return;
	size_t o = pc->s.n++;
	memcpy(pc->s.pos + o * 9, pos, 72);
	memcpy(pc->s.nrm + o * 9, nrm, 72);
	memcpy(pc->s.uv + o * 6, uv, 48);
	pc->s.mat[o] = p->mat;
	lossantos_vec_push(pc->key, (tile_key(cx, cy) << 32) | p->mat);
}

// stable lsd radix on (key, idx)
static int sort_pairs(uint64_t *key, uint32_t *idx, size_t n) {
	uint64_t *k2 = (uint64_t *)malloc((n ? n : 1) * sizeof *k2);
	uint32_t *i2 = (uint32_t *)malloc((n ? n : 1) * sizeof *i2);
	size_t *cnt = (size_t *)malloc(65536 * sizeof *cnt);
	if (!k2 || !i2 || !cnt) { free(k2); free(i2); free(cnt); return 0; }
	uint64_t lo = ~0ull, hi = 0;
	for (size_t i = 0; i < n; i++) { lo &= key[i]; hi |= key[i]; }
	uint64_t vary = lo ^ hi;
	uint64_t *ks = key, *kd = k2;
	uint32_t *is = idx, *id = i2;
	for (int pass = 0; pass < 4; pass++) {
		int sh = pass * 16;
		if (((vary >> sh) & 0xFFFF) == 0) continue;
		memset(cnt, 0, 65536 * sizeof *cnt);
		for (size_t i = 0; i < n; i++) cnt[(ks[i] >> sh) & 0xFFFF]++;
		size_t s = 0;
		for (size_t d = 0; d < 65536; d++) { size_t t = cnt[d]; cnt[d] = s; s += t; }
		for (size_t i = 0; i < n; i++) {
			size_t q = cnt[(ks[i] >> sh) & 0xFFFF]++;
			kd[q] = ks[i];
			id[q] = is[i];
		}
		uint64_t *tk = ks; ks = kd; kd = tk;
		uint32_t *ti = is; is = id; id = ti;
	}
	if (ks != key) {
		memcpy(key, ks, n * sizeof *key);
		memcpy(idx, is, n * sizeof *idx);
	}
	free(k2); free(i2); free(cnt);
	return 1;
}

typedef LOSSANTOS_VEC(run_rec) run_vec;

static int spill_soup(map_ctx *c, size_t bi, const lossantos_soup *vis, const lossantos_strtab *mats, int double_sided,
					  uint32_t fam, FILE *f, uint64_t *off, uint32_t *seq, run_vec *runs,
					  lossantos_marker_vec *markers, lossantos_hset *cent_cells) {
	uint32_t *gmat = (uint32_t *)malloc((mats->n ? mats->n : 1) * sizeof *gmat);
	uint8_t *mark = (uint8_t *)malloc(mats->n ? mats->n : 1);
	if (!gmat || !mark) { free(gmat); free(mark); return 0; }
	AcquireSRWLockExclusive(&c->lock);
	for (uint32_t m = 0; m < mats->n; m++) gmat[m] = lossantos_strtab_id(&c->mats, mats->s[m]);
	ReleaseSRWLockExclusive(&c->lock);
	// 0 no, 1 marker, 2 height marker
	for (uint32_t m = 0; m < mats->n; m++)
		mark[m] = (uint8_t)(lossantos_is_marker_name(mats->s[m]) ? 1 + lossantos_is_height_marker_name(mats->s[m]) : 0);

	pieces pc;
	memset(&pc, 0, sizeof pc);
	lossantos_soup_reserve(&pc.s, vis->n + vis->n / 8 + 16);
	lossantos_vec_reserve(pc.key, vis->n + vis->n / 8 + 16);
	for (size_t t = 0; t < vis->n; t++) {
		const double *p = vis->pos + t * 9;
		if (cent_cells) {
			int64_t cx = (int64_t)floor((p[0] + p[3] + p[6]) / 3.0 / TILE);
			int64_t cy = (int64_t)floor((p[1] + p[4] + p[7]) / 3.0 / TILE);
			lossantos_hset_add(cent_cells, tile_key(cx, cy) + 1);
		}
		piece_ctx pcx = {&pc, vis->mat[t]};
		lossantos_clip_tri(p, vis->nrm + t * 9, vis->uv + t * 6, TILE, on_piece, &pcx);
	}
	for (size_t i = 0; i < pc.s.n; i++) {
		uint8_t mk = mark[pc.s.mat[i]];
		if (!mk) continue;
		const double *p = pc.s.pos + i * 9;
		lossantos_marker m;
		m.x0 = (float)fmin(fmin(p[0], p[3]), p[6]);
		m.x1 = (float)fmax(fmax(p[0], p[3]), p[6]);
		m.y0 = (float)fmin(fmin(p[1], p[4]), p[7]);
		m.y1 = (float)fmax(fmax(p[1], p[4]), p[7]);
		m.z = (float)((p[2] + p[5] + p[8]) / 3.0);
		m.height = mk == 2;
		lossantos_vec_push(*markers, m);
	}
	size_t n = pc.s.n;
	uint32_t *ord = (uint32_t *)malloc((n ? n : 1) * sizeof *ord);
	for (size_t i = 0; i < n; i++) ord[i] = (uint32_t)i;
	sort_pairs(pc.key.p, ord, n);
	lossantos_tilebuild tb;
	lossantos_tb_init(&tb);
	for (size_t s = 0; s < n;) {
		uint64_t tk = pc.key.p[s] >> 32;
		size_t e = s;
		while (e < n && (pc.key.p[e] >> 32) == tk) e++;
		lossantos_tb_reset(&tb);
		lossantos_tb_sided(&tb, !double_sided);
		for (size_t i = s; i < e;) {
			uint32_t m = (uint32_t)pc.key.p[i];
			lossantos_tb_material(&tb, mats->s[m]);
			for (; i < e && (uint32_t)pc.key.p[i] == m; i++) {
				uint32_t q = ord[i];
				lossantos_tb_tri(&tb, pc.s.pos + (size_t)q * 9, pc.s.nrm + (size_t)q * 9, pc.s.uv + (size_t)q * 6);
			}
		}
		lossantos_tb_close(&tb);
		int32_t tx = (int32_t)((tk >> 16) & 0xFFFF) - 32768, ty = (int32_t)(tk & 0xFFFF) - 32768;
		for (size_t k = 0; k < tb.subs.n; k++) {
			const lossantos_submesh *sm = &tb.subs.p[k];
			uint32_t vend = k + 1 < tb.subs.n ? tb.subs.p[k + 1].voff : (uint32_t)(tb.verts.n / 28);
			run_rec r;
			r.tx = tx;
			r.ty = ty;
			r.block = c->rank[bi];
			r.seq = (*seq)++;
			r.mat = gmat[(uint32_t)lossantos_strtab_find(mats, tb.names.s[sm->mat])];
			r.nverts = vend - sm->voff;
			r.nidx = sm->icount;
			r.off = *off;
			r.fam = fam;
			fwrite(tb.verts.p + (size_t)sm->voff * 28, 28, r.nverts, f);
			fwrite(tb.idx.p + sm->ioff, 2, r.nidx, f);
			*off += (uint64_t)r.nverts * 28 + (uint64_t)r.nidx * 2;
			lossantos_vec_push(*runs, r);
		}
		s = e;
	}
	lossantos_tb_free(&tb);
	free(ord);
	lossantos_soup_free(&pc.s);
	lossantos_vec_free(pc.key);
	free(gmat);
	free(mark);
	return 1;
}

// bi == nrefs is the SLOD pseudo block
static void phase1_job(void *ctx, size_t bi) {
	map_ctx *c = (map_ctx *)ctx;
	int is_slod = bi == c->nrefs;
	lossantos_block_out out;
	memset(&out, 0, sizeof out);
	const char *label;
	if (is_slod) {
		label = "SLOD";
		if (!lossantos_slod_build(c->w, &c->slod)) {
			logf_(c, "SLOD FAILED: %s\n", c->slod.error);
			c->kind[bi] = -1;
			return;
		}
		logf_(c, "SLOD: %u packs, %u placed, %zu tris, %u materials, %zu installs, %u fallback tris\n",
			  c->slod.packs, c->slod.placed, c->slod.vis.n, c->slod.mats.n, c->slod.installs.n,
			  c->slod.fallback_geoms);
		c->kind[bi] = 0;
	} else {
		label = c->refs[bi].stem;
		if (c->status[bi] == 1) {
			c->kind[bi] = 2;
			return;
		}
		int phase = c->status[bi] == 0 ? 1 : 2;
		lossantos_vec_reserve(out.installs, c->installs[bi].n + 1);
		for (size_t k = 0; k < c->installs[bi].n; k++) lossantos_vec_push(out.installs, c->installs[bi].p[k]);
		out.installs_given = 1;
		int ok = lossantos_block_run(c->w, &c->refs[bi], phase, &out);
		if (!ok && phase == 1 && !out.empty) {
			logf_(c, "detail %s failed (%s): collision-only fallback\n", label, out.error);
			lossantos_block_out_free(&out);
			memset(&out, 0, sizeof out);
			phase = 2;
			ok = lossantos_block_run(c->w, &c->refs[bi], 2, &out);
		}
		if (!ok) {
			c->kind[bi] = out.empty ? 2 : -1;
			if (!out.empty) logf_(c, "block %s FAILED: %s\n", label, out.error);
			lossantos_block_out_free(&out);
			return;
		}
		c->kind[bi] = phase == 2 ? 1 : 0;
		logf_(c, "%s %s: placed %u, %zu visual tris, %u materials, coll %zu, %.2f s\n",
			  phase == 2 ? "fallback" : "detail", label, out.placed, out.vis.n, out.mats.n, out.coll.ntris,
			  out.t_total);
	}

	wchar_t sp[1024];
	spill_path(c, bi, sp, 1024);
	FILE *f = _wfopen(sp, L"wb");
	if (!f) {
		c->kind[bi] = -1;
		logf_(c, "%s: cannot write spill\n", label);
		lossantos_block_out_free(&out);
		return;
	}
	setvbuf(f, NULL, _IOFBF, 1 << 20);
	uint64_t off = 0;
	uint32_t seq = 0;
	run_vec runs = {0};
	LOSSANTOS_VEC(phys_rec) phys = {0};
	lossantos_marker_vec markers = {0};
	lossantos_hset cells;
	lossantos_hset_init(&cells, 1024);
	if (is_slod) {
		spill_soup(c, bi, &c->slod.vis, &c->slod.mats, 0, 1, f, &off, &seq, &runs, &markers, &cells);
		lossantos_soup_free(&c->slod.vis);
	} else {
		spill_soup(c, bi, &out.vis, &out.mats, out.double_sided, 0, f, &off, &seq, &runs, &markers, NULL);
		lossantos_soup_free(&out.vis);
	}

	// physics, centroid binned, ints in 0.01 units
	size_t m = is_slod ? 0 : out.coll.ntris;
	uint64_t *pk = (uint64_t *)malloc((m ? m : 1) * sizeof *pk);
	uint32_t *po = (uint32_t *)malloc((m ? m : 1) * sizeof *po);
	for (size_t i = 0; i < m; i++) {
		const double *t = out.coll.tri + i * 9;
		int64_t cx = (int64_t)floor((t[0] + t[3] + t[6]) / 3.0 / TILE);
		int64_t cy = (int64_t)floor((t[1] + t[4] + t[7]) / 3.0 / TILE);
		pk[i] = tile_key(cx, cy);
		po[i] = (uint32_t)i;
	}
	sort_pairs(pk, po, m);
	int32_t q[9];
	for (size_t s = 0; s < m;) {
		size_t e = s;
		while (e < m && pk[e] == pk[s]) e++;
		phys_rec r;
		r.cx = (int32_t)((pk[s] >> 16) & 0xFFFF) - 32768;
		r.cy = (int32_t)(pk[s] & 0xFFFF) - 32768;
		r.block = c->rank[bi];
		r.seq = seq++;
		r.ntris = (uint32_t)(e - s);
		r.off = off;
		for (size_t i = s; i < e; i++) {
			const double *t = out.coll.tri + (size_t)po[i] * 9;
			for (int a = 0; a < 9; a++) q[a] = (int32_t)nearbyint(t[a] * 100.0);
			fwrite(q, 4, 9, f);
		}
		off += (uint64_t)r.ntris * 36;
		lossantos_vec_push(phys, r);
		s = e;
	}
	free(pk);
	free(po);
	int bad = ferror(f);
	fclose(f);
	if (bad) {
		c->kind[bi] = -1;
		logf_(c, "%s: spill write failed (disk full?)\n", label);
	}
	c->spill_size[bi] = off;

	AcquireSRWLockExclusive(&c->lock);
	for (size_t i = 0; i < runs.n; i++) lossantos_vec_push(c->runs, runs.p[i]);
	for (size_t i = 0; i < phys.n; i++) lossantos_vec_push(c->phys, phys.p[i]);
	for (size_t i = 0; i < markers.n; i++) lossantos_vec_push(c->markers, markers.p[i]);
	if (is_slod)
		for (size_t i = 0; i < cells.cap; i++)
			if (cells.k[i]) lossantos_hset_add(&c->slod_cells, cells.k[i]);
	ReleaseSRWLockExclusive(&c->lock);
	lossantos_hset_free(&cells);
	lossantos_vec_free(runs);
	lossantos_vec_free(phys);
	lossantos_vec_free(markers);
	lossantos_block_out_free(&out);
}


static int cmp_run(const void *a, const void *b) {
	const run_rec *x = (const run_rec *)a, *y = (const run_rec *)b;
	if (x->tx != y->tx) return x->tx < y->tx ? -1 : 1;
	if (x->ty != y->ty) return x->ty < y->ty ? -1 : 1;
	if (x->block != y->block) return x->block < y->block ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int cmp_phys(const void *a, const void *b) {
	const phys_rec *x = (const phys_rec *)a, *y = (const phys_rec *)b;
	if (x->cx != y->cx) return x->cx < y->cx ? -1 : 1;
	if (x->cy != y->cy) return x->cy < y->cy ? -1 : 1;
	if (x->block != y->block) return x->block < y->block ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int read_at(HANDLE h, uint64_t off, void *dst, size_t len) {
	uint8_t *d = (uint8_t *)dst;
	while (len) {
		DWORD want = len > (1u << 30) ? (1u << 30) : (DWORD)len, got = 0;
		OVERLAPPED ov;
		memset(&ov, 0, sizeof ov);
		ov.Offset = (DWORD)off;
		ov.OffsetHigh = (DWORD)(off >> 32);
		if (!ReadFile(h, d, want, &got, &ov) || got != want) return 0;
		d += got;
		off += got;
		len -= got;
	}
	return 1;
}

struct cell_job {
	int32_t tx, ty;
	size_t r0, r1;
	char files[5][40];
	float zlo, zhi;           /* tile_bounds.json: union over every file */
	int has_z;
};

static int load_runs(map_ctx *c, const run_rec *r, size_t n, uint32_t fam, lossantos_tilemem *t) {
	uint8_t *tmp = NULL;
	size_t cap = 0;
	for (size_t i = 0; i < n; i++) {
		if (r[i].fam != fam) continue;
		size_t bytes = (size_t)r[i].nverts * 28 + (size_t)r[i].nidx * 2;
		if (bytes > cap) {
			free(tmp);
			cap = bytes * 2;
			tmp = (uint8_t *)malloc(cap);
			if (!tmp) return 0;
		}
		if (!read_at(c->spill[r[i].block], r[i].off, tmp, bytes)) { free(tmp); return 0; }
		if (!lossantos_tilemem_add(t, r[i].mat, tmp, r[i].nverts, (const uint16_t *)(tmp + (size_t)r[i].nverts * 28),
							 r[i].nidx)) {
			free(tmp);
			return 0;
		}
	}
	free(tmp);
	return 1;
}

static int put_file(map_ctx *c, lossantos_tilemem *t, const lossantos_water_cell *wc, const char *name, char *slot,
					lossantos_page *pg, cell_job *zj) {
	if (wc && !lossantos_water_append(&c->water, wc, c->water_mat, t)) return 0;
	size_t tris = 0, verts = 0;
	lossantos_buf b = {0};
	int rc = lossantos_tilemem_encode(t, (const char *const *)c->mats.s, &b, &tris, &verts);
	if (rc == 1) {
		uint32_t vc, sc, mc;
		memcpy(&vc, b.p + 8, 4);
		memcpy(&sc, b.p + 16, 4);
		memcpy(&mc, b.p + 20, 4);
		// 32 header, 16 per submesh, 64 per material
		const uint8_t *vp = b.p + 32 + (size_t)sc * 16 + (size_t)mc * 64;
		for (uint32_t i = 0; i < vc; i++) {
			float z;
			memcpy(&z, vp + (size_t)i * 28 + 8, 4);
			if (!zj->has_z || z < zj->zlo) zj->zlo = z;
			if (!zj->has_z || z > zj->zhi) zj->zhi = z;
			zj->has_z = 1;
		}
		if (!lossantos_page_add(pg, name, b.p, b.n)) rc = -1;
		if (c->o->loose) {
			wchar_t p[1024];
			swprintf(p, 1024, L"%ls\\tiles\\%hs", c->o->out_dir, name);
			FILE *f = _wfopen(p, L"wb");
			if (f) {
				fwrite(b.p, 1, b.n, f);
				fclose(f);
			}
		}
	}
	lossantos_buf_free(&b);
	// water run comes off again, lods dont get it
	if (wc) {
		lossantos_trun *last = &t->runs.p[t->runs.n - 1];
		free(last->v);
		free(last->ix);
		t->runs.n--;
	}
	if (rc < 0) return 0;
	if (rc == 1) {
		snprintf(slot, 40, "%s", name);
		InterlockedAdd64(&c->tile_tris, (LONG64)tris);
		InterlockedAdd64(&c->tile_verts, (LONG64)verts);
		InterlockedIncrement64(&c->files_written);
	}
	return 1;
}

static const double LOD_CELL[5] = {0, 10.0, 50.0, 75.0, 150.0};  // cluster cell, source units
static const int LOD_COARSE[5] = {0, 0, 1, 1, 1};  // 0 fine, 1 coarse
static const int LOD_NRM[5] = {0, 4, 2, 0, 0};  // normal step, 0 = position only

static int put_lods(map_ctx *c, const lossantos_tilemem *src, const lossantos_water_cell *wc, const char *stem, int first,
					int last, cell_job *job, lossantos_page *pg) {
	for (int l = first; l <= last; l++) {
		lossantos_tilemem lt;
		double box[4] = {job->tx * TILE, job->ty * TILE, (job->tx + 1) * TILE, (job->ty + 1) * TILE};
		if (!lossantos_lod_cluster(src, LOD_CELL[l], LOD_COARSE[l], LOD_NRM[l], box, &lt)) return 0;
		char name[40];
		snprintf(name, sizeof name, "%s_%d.bin", stem, l);
		int ok = put_file(c, &lt, wc, name, job->files[l], pg, job);
		lossantos_tilemem_free(&lt);
		if (!ok) return 0;
	}
	return 1;
}

static void cell_work(map_ctx *c, size_t k, lossantos_page *pg) {
	cell_job *job = &c->cells[k];
	const run_rec *r = c->tr + job->r0;
	size_t nr = job->r1 - job->r0;
	lossantos_tilemem T, S;
	memset(&T, 0, sizeof T);
	memset(&S, 0, sizeof S);
	int ok = load_runs(c, r, nr, 0, &T) && load_runs(c, r, nr, 1, &S);
	const lossantos_water_cell *wc = c->have_water ? lossantos_water_find(&c->water, job->tx, job->ty) : NULL;
	int slod_cell = lossantos_hset_has(&c->slod_cells, tile_key(job->tx, job->ty) + 1);
	if (ok && c->beach.band) {
		size_t s1 = lossantos_beach_apply(&c->beach, &T, (const char *const *)c->mats.s, c->sand_mat);
		size_t s2 = lossantos_beach_apply(&c->beach, &S, (const char *const *)c->mats.s, c->sand_mat);
		if (s1 + s2) InterlockedAdd64(&c->sanded, (LONG64)(s1 + s2));
	}
	char stem[40];
	if (ok && T.runs.n) {
		snprintf(stem, sizeof stem, "t_%d_%d_0", job->tx, job->ty);
		char name[40];
		snprintf(name, sizeof name, "%s.bin", stem);
		ok = put_file(c, &T, wc, name, job->files[0], pg, job);
		if (ok && slod_cell && S.runs.n) {
			// ttsss, slod owns the far band
			ok = put_lods(c, &T, wc, stem, 1, 1, job, pg);
			char sn[40];
			snprintf(sn, sizeof sn, "s_%d_%d.bin", job->tx, job->ty);
			char slot[40] = "";
			ok = ok && put_file(c, &S, wc, sn, slot, pg, job);
			for (int l = 2; l <= 4; l++) snprintf(job->files[l], 40, "%s", slot);
		} else if (ok) {
			ok = put_lods(c, &T, wc, stem, 1, 4, job, pg);
		}
	} else if (ok && S.runs.n) {
		// sssss: far-land only
		snprintf(stem, sizeof stem, "s_%d_%d", job->tx, job->ty);
		char name[40];
		snprintf(name, sizeof name, "%s.bin", stem);
		ok = put_file(c, &S, wc, name, job->files[0], pg, job) && put_lods(c, &S, wc, stem, 1, 4, job, pg);
	} else if (ok && wc) {  // wwwww: water only
		lossantos_tilemem W;
		memset(&W, 0, sizeof W);
		char name[40];
		snprintf(name, sizeof name, "w_%d_%d.bin", job->tx, job->ty);
		ok = put_file(c, &W, wc, name, job->files[0], pg, job);
		for (int l = 1; l <= 4; l++) snprintf(job->files[l], 40, "%s", job->files[0]);
		lossantos_tilemem_free(&W);
	}
	if (!ok) c->failed = 1;
	lossantos_tilemem_free(&T);
	lossantos_tilemem_free(&S);
}

// one 8x8 sector page
static void tile_page_job(void *ctx, size_t pi) {
	map_ctx *c = (map_ctx *)ctx;
	const page_job *pj = &c->tpages[pi];
	lossantos_page pg;
	memset(&pg, 0, sizeof pg);
	for (size_t i = pj->first; i < pj->last; i++) cell_work(c, c->tpage_cells[i], &pg);
	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\pages\\v_%d_%d.bin", c->o->out_dir, pj->px, pj->py);
	if (lossantos_page_write(&pg, p) < 0) c->failed = 1;
	lossantos_page_free(&pg);
}

static void phys_chunk(map_ctx *c, size_t k, lossantos_page *pg) {
	const phys_rec *r = c->pr + c->pstart[k];
	size_t nr = c->pstart[k + 1] - c->pstart[k];
	size_t nt = 0;
	for (size_t i = 0; i < nr; i++) nt += r[i].ntris;
	int32_t *q = (int32_t *)malloc(nt * 36);
	double *tri = (double *)malloc(nt * 72);
	if (!q || !tri) { free(q); free(tri); c->failed = 1; return; }
	size_t at = 0;
	for (size_t i = 0; i < nr; i++) {
		if (!read_at(c->spill[r[i].block], r[i].off, q + at * 9, (size_t)r[i].ntris * 36)) c->failed = 1;
		at += r[i].ntris;
	}
	for (size_t i = 0; i < nt * 9; i++) tri[i] = (double)q[i] * 0.01;
	char name[40];
	snprintf(name, sizeof name, "g_%d_%d.bin", r[0].cx, r[0].cy);
	size_t dropped = 0;
	lossantos_buf b = {0};
	if (lossantos_encode_owc1(tri, nt, 0, &dropped, &b) < 0 || !lossantos_page_add(pg, name, b.p, b.n)) c->failed = 1;
	if (c->o->loose) {
		wchar_t p[1024];
		swprintf(p, 1024, L"%ls\\phys\\%hs", c->o->out_dir, name);
		FILE *f = _wfopen(p, L"wb");
		if (f) {
			fwrite(b.p, 1, b.n, f);
			fclose(f);
		}
	}
	lossantos_buf_free(&b);
	InterlockedAdd64(&c->chunk_tris, (LONG64)(nt - dropped));
	InterlockedAdd64(&c->chunk_dropped, (LONG64)dropped);
	free(q);
	free(tri);
}

static void phys_page_job(void *ctx, size_t pi) {
	map_ctx *c = (map_ctx *)ctx;
	const page_job *pj = &c->ppages[pi];
	lossantos_page pg;
	memset(&pg, 0, sizeof pg);
	for (size_t i = pj->first; i < pj->last; i++) phys_chunk(c, c->ppage_keys[i], &pg);
	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\pages\\p_%d_%d.bin", c->o->out_dir, pj->px, pj->py);
	if (lossantos_page_write(&pg, p) < 0) c->failed = 1;
	lossantos_page_free(&pg);
}

// unused, dont ask
typedef struct {
	int32_t px, py;
	size_t first, last;
} page_job_unused;
static int cmp_pk(const void *a, const void *b) {
	const int64_t *x = (const int64_t *)a, *y = (const int64_t *)b;
	for (int i = 0; i < 3; i++)
		if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
	return 0;
}
static size_t make_pages(const int32_t *xy, size_t n, page_job **pages, size_t **order) {
	int64_t *k = (int64_t *)malloc((n ? n : 1) * 3 * sizeof *k);
	for (size_t i = 0; i < n; i++) {
		k[i * 3] = xy[i * 2] >> 3;
		k[i * 3 + 1] = xy[i * 2 + 1] >> 3;
		k[i * 3 + 2] = (int64_t)i;
	}
	qsort(k, n, 24, cmp_pk);
	*order = (size_t *)malloc((n ? n : 1) * sizeof **order);
	*pages = (page_job *)malloc((n ? n : 1) * sizeof **pages);
	size_t np = 0;
	for (size_t i = 0; i < n; i++) {
		(*order)[i] = (size_t)k[i * 3 + 2];
		if (i == 0 || k[i * 3] != k[(i - 1) * 3] || k[i * 3 + 1] != k[(i - 1) * 3 + 1]) {
			if (np) (*pages)[np - 1].last = i;
			(*pages)[np].px = (int32_t)k[i * 3];
			(*pages)[np].py = (int32_t)k[i * 3 + 1];
			(*pages)[np].first = i;
			np++;
		}
	}
	if (np) (*pages)[np - 1].last = n;
	free(k);
	return np;
}


static void json_str(FILE *f, const char *s) {
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') fputc('\\', f);
		fputc(*s, f);
	}
	fputc('"', f);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int write_index(map_ctx *c, lossantos_map_stats *st) {
	const double spawn[3] = {-516.1 * SCALE, -372.0 * SCALE, 37.5 * SCALE};
	// materials: sorted names
	char **names = (char **)malloc((c->mats.n ? c->mats.n : 1) * sizeof *names);
	for (uint32_t i = 0; i < c->mats.n; i++) names[i] = c->mats.s[i];
	qsort(names, c->mats.n, sizeof *names, cmp_str);
	size_t blocks_used = 0;
	for (size_t i = 0; i < c->nrefs; i++) blocks_used += c->kind[i] == 0 || c->kind[i] == 1;
	char source[256];
	snprintf(source, sizeof source, "{\"blocks\":%zu,\"tiles\":%zu,\"chunks\":%zu,\"chunk_triangles\":%lld}",
			 blocks_used, c->ntkeys, c->npkeys, (long long)c->chunk_tris);

	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\index.json", c->o->out_dir);
	FILE *f = _wfopen(p, L"wb");
	if (!f) return 0;
	fprintf(f, "{\"map\":\"" LOSSANTOS_MAP_NAME "\",\"tile_size\":1024,\"cell_size\":8192,\"phys_chunk_size\":1024,"
			   "\"stream_radius\":90000,\"max_tiles\":24000,\"merge_grid\":8,"
			   "\"fog\":{\"enabled\":true,\"color\":[170,181,194],\"near\":30000,\"far\":88000},"
			   "\"spawn\":[%.17g,%.17g,%.17g],\"spawn_angles\":[0.0,0.0,0.0],\"materials\":{",
			spawn[0], spawn[1], spawn[2]);
	for (uint32_t i = 0; i < c->mats.n; i++) {
		fprintf(f, "%s\"%u\":", i ? "," : "", i);
		json_str(f, names[i]);
	}
	fprintf(f, "},\"tiles\":{");
	int firstk = 1;
	for (size_t k = 0; k < c->ntkeys; k++) {
		const cell_job *j = &c->cells[k];
		if (!j->files[0][0]) continue;
		fprintf(f, "%s\"%d %d\":{", firstk ? "" : ",", j->tx, j->ty);
		firstk = 0;
		int fl = 1;
		for (int l = 0; l < 5; l++) {
			if (!j->files[l][0]) continue;
			fprintf(f, "%s\"lod%d\":{\"file\":\"%s\"}", fl ? "" : ",", l, j->files[l]);
			fl = 0;
		}
		fputc('}', f);
	}
	fprintf(f, "},\"phys_chunks\":{");
	for (size_t k = 0; k < c->npkeys; k++) {
		const phys_rec *r = c->pr + c->pstart[k];
		fprintf(f, "%s\"%d %d\":{\"file\":\"g_%d_%d.bin\"}", k ? "," : "", r->cx, r->cy, r->cx, r->cy);
	}
	fprintf(f, "},\"source\":%s}", source);
	int bad = ferror(f);
	fclose(f);

	// same as lines, JSONToTable stops at 15000
	// K  key, value
	// F  fog on, near, far, rgb
	// S  spawn xyz, angles
	// J  source json
	// M  material id, name
	// T  tile key, lod0 to lod4 files
	swprintf(p, 1024, L"%ls\\index_tiles.dat", c->o->out_dir);
	f = _wfopen(p, L"wb");
	if (!f) return 0;
	fprintf(f, "GVIDX1\nK\tmap\t" LOSSANTOS_MAP_NAME "\nK\ttile_size\t1024\nK\tcell_size\t8192\nK\tphys_chunk_size\t1024\n"
			   "K\tstream_radius\t90000\nK\tmax_tiles\t24000\nK\tmerge_grid\t8\nF\t1\t30000\t88000\t170\t181\t194\n"
			   "S\t%.17g\t%.17g\t%.17g\t0.0\t0.0\t0.0\nJ\t%s\n",
			spawn[0], spawn[1], spawn[2], source);
	for (uint32_t i = 0; i < c->mats.n; i++) fprintf(f, "M\t%u\t%s\n", i, names[i]);
	for (size_t k = 0; k < c->ntkeys; k++) {
		const cell_job *j = &c->cells[k];
		if (!j->files[0][0]) continue;
		fprintf(f, "T\t%d %d", j->tx, j->ty);
		for (int l = 0; l < 5; l++) fprintf(f, "\t%s", j->files[l][0] ? j->files[l] : "-");
		fputc('\n', f);
	}
	bad |= ferror(f);
	fclose(f);
	swprintf(p, 1024, L"%ls\\tile_bounds.json", c->o->out_dir);
	f = _wfopen(p, L"wb");
	if (!f) return 0;
	fprintf(f, "{\"schema\":1,\"bounds\":{");
	int fb = 1;
	for (size_t k = 0; k < c->ntkeys; k++) {
		const cell_job *j = &c->cells[k];
		if (!j->files[0][0] || !j->has_z) continue;
		fprintf(f, "%s\"%d %d\":[%.2f,%.2f]", fb ? "" : ",", j->tx, j->ty, floor(j->zlo * 100.0) / 100.0,
				ceil(j->zhi * 100.0) / 100.0);
		fb = 0;
	}
	fprintf(f, "}}");
	bad |= ferror(f);
	fclose(f);
	swprintf(p, 1024, L"%ls\\tile_bounds.dat", c->o->out_dir);
	f = _wfopen(p, L"wb");
	if (!f) return 0;
	fprintf(f, "GVTB1\n");
	for (size_t k = 0; k < c->ntkeys; k++) {
		const cell_job *j = &c->cells[k];
		if (!j->files[0][0] || !j->has_z) continue;
		fprintf(f, "%d %d\t%.2f\t%.2f\n", j->tx, j->ty, floor(j->zlo * 100.0) / 100.0, ceil(j->zhi * 100.0) / 100.0);
	}
	bad |= ferror(f);
	fclose(f);
	swprintf(p, 1024, L"%ls\\index_phys.dat", c->o->out_dir);
	f = _wfopen(p, L"wb");
	if (!f) return 0;
	fprintf(f, "GVPHY1\n");
	for (size_t k = 0; k < c->npkeys; k++) {
		const phys_rec *r = c->pr + c->pstart[k];
		fprintf(f, "P\t%d %d\tg_%d_%d.bin\n", r->cx, r->cy, r->cx, r->cy);
	}
	bad |= ferror(f);
	fclose(f);
	st->materials = c->mats.n;
	free(names);
	return !bad;
}

typedef struct {
	wchar_t (*paths)[512];
	lossantos_buf *out;              /* per page: "file\tname\tname...\n" lines */
} mp_ctx;

static void mp_page_job(void *ctx, size_t i) {
	mp_ctx *m = (mp_ctx *)ctx;
	lossantos_buf b = {0};
	FILE *f = _wfopen(m->paths[i], L"rb");
	if (!f) return;
	_fseeki64(f, 0, SEEK_END);
	long long n = _ftelli64(f);
	_fseeki64(f, 0, SEEK_SET);
	if (n > 16 && lossantos_buf_reserve(&b, (size_t)n + 1) && fread(b.p, 1, (size_t)n, f) == (size_t)n) b.n = (size_t)n;
	fclose(f);
	if (b.n < 16 || memcmp(b.p, "GVPG", 4)) {
		lossantos_buf_free(&b);
		return;
	}
	b.p[b.n] = 0;
	uint32_t count;
	memcpy(&count, b.p + 8, 4);
	lossantos_buf *o = &m->out[i];
	for (uint32_t k = 0; k < count && count <= 65536 && 16 + (size_t)(k + 1) * 16 <= b.n; k++) {
		uint32_t no, bo, bl;
		memcpy(&no, b.p + 16 + k * 16, 4);
		memcpy(&bo, b.p + 20 + k * 16, 4);
		memcpy(&bl, b.p + 24 + k * 16, 4);
		(void)bl;
		if (no >= b.n || (size_t)bo + 32 > b.n || memcmp(b.p + bo, "GVT1", 4)) continue;
		uint32_t sub, mc;
		memcpy(&sub, b.p + bo + 16, 4);
		memcpy(&mc, b.p + bo + 20, 4);
		size_t pos = (size_t)bo + 32 + 16 * (size_t)sub;
		if (mc > 4096 || sub > 8192 || pos + 64 * (size_t)mc > b.n) continue;
		const char *fn = (const char *)b.p + no;
		size_t fl = strnlen(fn, b.n - no);
		lossantos_buf_reserve(o, o->n + fl + 66 * (size_t)mc + 4);
		memcpy(o->p + o->n, fn, fl);
		o->n += fl;
		for (uint32_t q = 0; q < mc; q++) {
			const char *nm = (const char *)b.p + pos + 64 * (size_t)q;
			size_t nl = strnlen(nm, 64);
			if (!nl) continue;
			o->p[o->n++] = '\t';
			memcpy(o->p + o->n, nm, nl);
			o->n += nl;
		}
		o->p[o->n++] = '\n';
	}
	lossantos_buf_free(&b);
}

static int cmp_u32_str(void *ctx, const void *a, const void *b) {
	char **s = (char **)ctx;
	return strcmp(s[*(const uint32_t *)a], s[*(const uint32_t *)b]);
}

// GVMAT1, what the client reads per sector + lod
// K  key, value
// N  material name
// T  set id, name ids, 250 per line
// S  sector x, sector y, lod, set id
static int write_tile_materials(map_ctx *c) {
	wchar_t pat[1024];
	swprintf(pat, 1024, L"%ls\\pages\\v_*.bin", c->o->out_dir);
	LOSSANTOS_VEC(wchar_t *) pages = {0};
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW(pat, &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			wchar_t *p = (wchar_t *)malloc(512 * sizeof(wchar_t));
			swprintf(p, 512, L"%ls\\pages\\%ls", c->o->out_dir, d.cFileName);
			lossantos_vec_push(pages, p);
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
	mp_ctx m;
	m.paths = (wchar_t(*)[512])calloc(pages.n + 1, sizeof *m.paths);
	m.out = (lossantos_buf *)calloc(pages.n + 1, sizeof *m.out);
	for (size_t i = 0; i < pages.n; i++) {
		wcscpy(m.paths[i], pages.p[i]);
		free(pages.p[i]);
	}
	run_pool(c->o, "tile materials", pages.n, mp_page_job, &m);

	// file -> material ids
	lossantos_strtab files, mats;
	lossantos_strtab_init(&files);
	lossantos_strtab_init(&mats);
	LOSSANTOS_VEC(uint32_t) ids = {0};
	LOSSANTOS_VEC(uint64_t) span = {0};      /* per file id: start << 32 | count */
	for (size_t i = 0; i < pages.n; i++) {
		lossantos_buf *o = &m.out[i];
		for (size_t s = 0; s < o->n;) {
			char *ln = (char *)o->p + s;
			char *e = memchr(ln, '\n', o->n - s);
			*e = 0;
			s = (size_t)(e - (char *)o->p) + 1;
			char *tab = strchr(ln, '\t');
			if (tab) *tab = 0;
			uint32_t fid = lossantos_strtab_id(&files, ln);
			while (span.n <= fid) lossantos_vec_push(span, 0ull);
			uint64_t start = ids.n;
			uint32_t cnt = 0;
			for (char *q = tab; q;) {
				char *nx = strchr(q + 1, '\t');
				if (nx) *nx = 0;
				lossantos_vec_push(ids, lossantos_strtab_id(&mats, q + 1));
				cnt++;
				q = nx;
			}
			span.p[fid] = (start << 32) | cnt;   /* a later duplicate wins, like the dict */
		}
		lossantos_buf_free(o);
	}
	free(m.out);
	free(m.paths);
	lossantos_vec_free(pages);
	uint32_t *ord = (uint32_t *)malloc((mats.n + 1) * sizeof *ord), *rank = (uint32_t *)malloc((mats.n + 1) * sizeof *rank);
	for (uint32_t i = 0; i < mats.n; i++) ord[i] = i;
	qsort_s(ord, mats.n, sizeof *ord, cmp_u32_str, mats.s);
	for (uint32_t i = 0; i < mats.n; i++) rank[ord[i]] = i;
	// pr bits
	// 0   name rank
	// 24  lod
	// 28  sector y + 32768
	// 44  sector x + 32768
	LOSSANTOS_VEC(uint64_t) pr = {0};
	for (size_t k = 0; k < c->ntkeys; k++) {
		const cell_job *j = &c->cells[k];
		if (!j->files[0][0]) continue;
		int32_t sx = j->tx >> 3, sy = j->ty >> 3;
		for (int lod = 0; lod < 5; lod++) {
			// wanted lod, then down, then up
			int r = -1;
			if (j->files[lod][0]) r = lod;
			for (int a = lod - 1; r < 0 && a >= 0; a--)
				if (j->files[a][0]) r = a;
			for (int a = lod + 1; r < 0 && a < 5; a++)
				if (j->files[a][0]) r = a;
			if (r < 0) continue;
			int64_t fid = lossantos_strtab_find(&files, j->files[r]);
			if (fid < 0 || (size_t)fid >= span.n || !span.p[fid]) continue;
			uint64_t sp = span.p[fid];
			uint64_t slot = ((uint64_t)(uint16_t)(sx + 32768) << 20) | ((uint64_t)(uint16_t)(sy + 32768) << 4) | (uint64_t)lod;
			for (uint32_t q = 0; q < (uint32_t)sp; q++) lossantos_vec_push(pr, (slot << 24) | rank[ids.p[(sp >> 32) + q]]);
		}
	}
	uint64_t *tmp = (uint64_t *)malloc((pr.n + 1) * sizeof *tmp);
	size_t np = lossantos_sort_unique_u64(pr.p, pr.n, tmp);
	free(tmp);
	uint32_t *nid = (uint32_t *)malloc((mats.n + 1) * sizeof *nid);
	for (uint32_t i = 0; i < mats.n; i++) nid[i] = UINT32_MAX;
	LOSSANTOS_VEC(uint32_t) names = {0};       /* name id -> rank */
	LOSSANTOS_VEC(uint32_t) setdata = {0};
	LOSSANTOS_VEC(uint64_t) setspan = {0};
	LOSSANTOS_VEC(uint64_t) slots = {0};       /* slot << 24 | set id */
	lossantos_hmap sh;
	lossantos_hmap_init(&sh, 4096);
	LOSSANTOS_VEC(uint32_t) cur = {0};
	for (size_t s0 = 0; s0 < np;) {
		uint64_t slot = pr.p[s0] >> 24;
		size_t e0 = s0;
		cur.n = 0;
		while (e0 < np && (pr.p[e0] >> 24) == slot) {
			uint32_t rk = (uint32_t)(pr.p[e0] & 0xFFFFFF);
			if (nid[rk] == UINT32_MAX) {
				nid[rk] = (uint32_t)names.n;
				lossantos_vec_push(names, rk);
			}
			lossantos_vec_push(cur, nid[rk]);
			e0++;
		}
		uint64_t hk = lossantos_hash_bytes(cur.p, cur.n * 4) | 1;
		uint32_t sid = UINT32_MAX;
		for (uint64_t probe = hk;; probe = lossantos_mix64(probe) | 1) {
			int found;
			uint32_t *v = lossantos_hmap_put(&sh, probe, &found);
			if (!found) {
				*v = (uint32_t)setspan.n;
				sid = *v;
				lossantos_vec_push(setspan, ((uint64_t)setdata.n << 32) | cur.n);
				for (size_t q = 0; q < cur.n; q++) lossantos_vec_push(setdata, cur.p[q]);
				break;
			}
			uint64_t sp = setspan.p[*v];
			if ((uint32_t)sp == cur.n && !memcmp(setdata.p + (sp >> 32), cur.p, cur.n * 4)) {
				sid = *v;
				break;
			}
		}
		lossantos_vec_push(slots, (slot << 24) | sid);
		s0 = e0;
	}
	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\tile_materials.dat", c->o->out_dir);
	FILE *f = _wfopen(p, L"wb");
	int ok = f != NULL;
	if (f) {
		setvbuf(f, NULL, _IOFBF, 1 << 20);
		fprintf(f, "GVMAT1\nK\tsector\t8\nK\tnames\t%zu\nK\tsets\t%zu\nK\tslots\t%zu\n", names.n, setspan.n, slots.n);
		for (size_t i = 0; i < names.n; i++) fprintf(f, "N\t%s\n", mats.s[ord[names.p[i]]]);
		for (size_t s = 0; s < setspan.n; s++) {
			uint64_t sp = setspan.p[s];
			uint32_t n = (uint32_t)sp;
			for (uint32_t st = 0; st < n; st += 250) {
				fprintf(f, "T\t%zu\t", s);
				for (uint32_t q = st; q < n && q < st + 250; q++) fprintf(f, q > st ? ",%u" : "%u", setdata.p[(sp >> 32) + q]);
				fputc('\n', f);
			}
		}
		for (size_t i = 0; i < slots.n; i++) {
			uint64_t slot = slots.p[i] >> 24;
			int sx = (int)((slot >> 20) & 0xFFFF) - 32768, sy = (int)((slot >> 4) & 0xFFFF) - 32768, lod = (int)(slot & 15);
			fprintf(f, "S\t%d\t%d\t%d\t%u\n", sx, sy, lod, (uint32_t)(slots.p[i] & 0xFFFFFF));
		}
		ok = !ferror(f);
		fclose(f);
	}
	if (c->o->log)
		fprintf(c->o->log, "tile_materials: %zu names, %zu sets, %zu slots\n", names.n, setspan.n, slots.n);
	lossantos_hmap_free(&sh);
	lossantos_vec_free(cur);
	lossantos_vec_free(slots);
	lossantos_vec_free(setspan);
	lossantos_vec_free(setdata);
	lossantos_vec_free(names);
	free(nid);
	free(ord);
	free(rank);
	lossantos_vec_free(pr);
	lossantos_vec_free(span);
	lossantos_vec_free(ids);
	lossantos_strtab_free(&files);
	lossantos_strtab_free(&mats);
	return ok;
}

// globals for qsort. i know. it works ig
static const lossantos_content_result *g_canon_res;
static const lossantos_content_job *g_canon_jobs;
// vtf sha1, parameters, path
static int cmp_canon(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	int r = memcmp(g_canon_res[x].sha1, g_canon_res[y].sha1, 20);
	if (r) return r;
	r = strcmp(g_canon_res[x].canon_key, g_canon_res[y].canon_key);
	if (r) return r;
	return strcmp(g_canon_jobs[x].path, g_canon_jobs[y].path);
}


typedef struct {
	const lossantos_blockref *refs;
} rank_ctx;

static int cmp_rank(void *ctx, const void *a, const void *b) {
	const lossantos_blockref *r = (const lossantos_blockref *)ctx;
	char x[128], y[128];
	snprintf(x, sizeof x, "%s_detail", r[*(const uint32_t *)a].stem);
	snprintf(y, sizeof y, "%s_detail", r[*(const uint32_t *)b].stem);
	return strcmp(x, y);
}

int lossantos_build_map(lossantos_world *w, const lossantos_blockref_vec *all, const lossantos_map_opts *o, lossantos_map_stats *st) {
	double t0 = lossantos_now();
	memset(st, 0, sizeof *st);
	lossantos_blockref_vec refs = {0};
	for (size_t i = 0; i < all->n; i++)
		if (!o->only || strstr(all->p[i].inner, o->only)) lossantos_vec_push(refs, all->p[i]);
	uint32_t *perm = (uint32_t *)malloc((refs.n ? refs.n : 1) * sizeof *perm);
	for (size_t i = 0; i < refs.n; i++) perm[i] = (uint32_t)i;
	qsort_s(perm, refs.n, sizeof *perm, cmp_rank, refs.p);
	lossantos_blockref_vec sorted = {0};
	for (size_t i = 0; i < refs.n; i++) lossantos_vec_push(sorted, refs.p[perm[i]]);
	lossantos_vec_free(refs);
	free(perm);

	map_ctx c;
	memset(&c, 0, sizeof c);
	c.w = w;
	c.o = o;
	c.refs = sorted.p;
	c.nrefs = sorted.n;
	c.log = o->log;
	InitializeSRWLock(&c.lock);
	InitializeSRWLock(&c.loglock);
	c.rank = (uint32_t *)malloc((c.nrefs + 1) * sizeof *c.rank);
	for (size_t i = 0; i <= c.nrefs; i++) c.rank[i] = (uint32_t)i;   /* already in merge order */
	c.installs = (lossantos_install_vec *)calloc(c.nrefs, sizeof *c.installs);
	c.atlas = (lossantos_texinfo_vec *)calloc(c.nrefs, sizeof *c.atlas);
	c.items = (lossantos_texinfo_vec *)calloc(c.nrefs, sizeof *c.items);
	c.local = (lossantos_texinfo_vec *)calloc(c.nrefs, sizeof *c.local);
	c.extra = (lossantos_texinfo_vec *)calloc(c.nrefs, sizeof *c.extra);
	c.missing = (lossantos_strvec *)calloc(c.nrefs, sizeof *c.missing);
	c.status = (int *)calloc(c.nrefs + 1, sizeof *c.status);
	c.kind = (int *)calloc(c.nrefs + 1, sizeof *c.kind);
	c.spill = (HANDLE *)calloc(c.nrefs + 1, sizeof *c.spill);
	c.spill_size = (uint64_t *)calloc(c.nrefs + 1, sizeof *c.spill_size);
	lossantos_strtab_init(&c.mats);
	lossantos_hset_init(&c.slod_cells, 65536);
	st->blocks = c.nrefs;
	CreateDirectoryW(o->out_dir, NULL);
	CreateDirectoryW(o->work_dir, NULL);
	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\tiles", o->out_dir);
	CreateDirectoryW(p, NULL);
	swprintf(p, 1024, L"%ls\\phys", o->out_dir);
	CreateDirectoryW(p, NULL);

	// phase 0: installs of every block
	double t = lossantos_now();
	run_pool(o, "textures", c.nrefs, phase0_job, &c);
	{
		// a block that lacks a texture borrows it
		lossantos_texset g;
		lossantos_texset_init(&g);
		for (size_t i = 0; i < c.nrefs; i++)
			for (size_t k = 0; k < c.local[i].n; k++) {
				lossantos_texinfo ti = c.local[i].p[k];
				ti.owner = (int32_t)i;
				lossantos_texset_offer_info(&g, &ti);
			}
		lossantos_strtab inst;
		lossantos_strtab_init(&inst);
		for (size_t i = 0; i < c.nrefs; i++)
			for (size_t k = 0; k < c.items[i].n; k++) lossantos_strtab_id(&inst, c.items[i].p[k].key);
		size_t reuse = 0;
		for (size_t i = 0; i < c.nrefs; i++)
			for (size_t k = 0; k < c.missing[i].n; k++) {
				const char *key = c.missing[i].p[k];
				if (lossantos_strtab_find(&inst, key) >= 0) {
					reuse++;
					continue;
				}
				int64_t gi = lossantos_texset_find(&g, key);
				if (gi < 0 || g.v.p[gi].owner == (int32_t)i) continue;
				lossantos_vec_push(c.extra[g.v.p[gi].owner], g.v.p[gi]);
				lossantos_strtab_id(&inst, key);   /* one extra copy per key */
				c.borrowed++;
			}
		lossantos_strtab_free(&inst);
		lossantos_texset_free(&g);
		logf_(&c, "textures: %zu missing keys resolve to another block's install\n", reuse);
		for (size_t i = 0; i < c.nrefs; i++) {
			if (c.status[i] != 0) continue;
			lossantos_texset s;
			lossantos_texset_init(&s);
			for (size_t k = 0; k < c.items[i].n; k++) lossantos_texset_offer_info(&s, &c.items[i].p[k]);
			for (size_t k = 0; k < c.extra[i].n; k++) lossantos_texset_offer_info(&s, &c.extra[i].p[k]);
			int *ix = (int *)malloc((s.v.n ? s.v.n : 1) * sizeof *ix);
			for (size_t k = 0; k < s.v.n; k++) ix[k] = (int)k;
			char prefix[160];
			snprintf(prefix, sizeof prefix, "models/gtav/%s", c.refs[i].stem);
			if (!lossantos_install_names(&s, ix, (int)s.v.n, prefix, &c.installs[i]))
				logf_(&c, "install names %s: %s\n", c.refs[i].stem, lossantos_error());
			free(ix);
			lossantos_texset_free(&s);
			for (size_t k = 0; k < c.installs[i].n; k++) lossantos_texreg_add(&w->reg, c.installs[i].p[k].path);
		}
		LOSSANTOS_VEC(lossantos_texinfo) items = {0};
		for (size_t i = 0; i < c.nrefs; i++)
			for (size_t k = 0; k < c.atlas[i].n; k++) lossantos_vec_push(items, c.atlas[i].p[k]);
		lossantos_world_finish_installs(w, items.p, items.n);
		lossantos_vec_free(items);
		lossantos_texreg_sort(&w->reg);
		st->borrowed = c.borrowed;
		logf_(&c, "textures: %zu keys borrowed from other blocks' dictionaries\n", c.borrowed);
	}
	st->t_phase0 = lossantos_now() - t;
	if (o->cancel && *o->cancel) goto out;

	// phase 1: build + clip + spill
	t = lossantos_now();
	run_pool(o, "blocks", c.nrefs + 1, phase1_job, &c);
	st->t_phase1 = lossantos_now() - t;
	for (size_t i = 0; i <= c.nrefs; i++) {
		if (i < c.nrefs) switch (c.kind[i]) {
			case 0: st->detail++; break;
			case 1: st->fallback++; break;
			case 2: st->empty++; break;
			default: st->failed++; break;
			}
		st->spill_bytes += c.spill_size[i];
		if (c.kind[i] == 0 || c.kind[i] == 1) {
			spill_path(&c, i, p, 1024);
			c.spill[i] = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
									 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
		}
	}
	if (o->cancel && *o->cancel) goto out;

	t = lossantos_now();
	qsort(c.runs.p, c.runs.n, sizeof *c.runs.p, cmp_run);
	c.tr = c.runs.p;
	{
		// beach band + water cells
		int32_t tx0 = INT32_MAX, tx1 = INT32_MIN, ty0 = INT32_MAX, ty1 = INT32_MIN;
		for (size_t i = 0; i < c.runs.n; i++) {
			if (c.tr[i].tx < tx0) tx0 = c.tr[i].tx;
			if (c.tr[i].tx > tx1) tx1 = c.tr[i].tx;
			if (c.tr[i].ty < ty0) ty0 = c.tr[i].ty;
			if (c.tr[i].ty > ty1) ty1 = c.tr[i].ty;
		}
		if (c.runs.n) {
			if (!lossantos_beach_build(&c.beach, tx0, tx1, ty0, ty1, c.markers.p, c.markers.n))
				logf_(&c, "beach grid failed\n");
			c.sand_mat = lossantos_strtab_id(&c.mats, LOSSANTOS_SAND_MATERIAL);
			c.water_mat = lossantos_strtab_id(&c.mats, LOSSANTOS_WATER_MATERIAL);
			int32_t margin = (int32_t)ceil(90000.0 / TILE);
			if (lossantos_water_load(&c.water, w->gta_dir, w->ng) &&
				lossantos_water_cells(&c.water, tx0 - margin, tx1 + margin, ty0 - margin, ty1 + margin))
				c.have_water = 1;
			else
				logf_(&c, "water.xml not loaded: no water\n");
		}
		logf_(&c, "beach: %zu marker tris; water: %zu quads -> %zu cells\n", c.markers.n, c.water.quads.n,
			  c.water.cells.n);
	}
	{
		size_t cap = 0, i = 0, wi = 0, nw = c.have_water ? c.water.cells.n : 0;
		size_t est = c.runs.n + nw + 1;
		c.cells = (cell_job *)calloc(est, sizeof *c.cells);
		while (i < c.runs.n || wi < nw) {
			int32_t rx = i < c.runs.n ? c.tr[i].tx : INT32_MAX, ry = i < c.runs.n ? c.tr[i].ty : INT32_MAX;
			int32_t wx = wi < nw ? c.water.cells.p[wi].gx : INT32_MAX, wy = wi < nw ? c.water.cells.p[wi].gy : INT32_MAX;
			int take_r = i < c.runs.n && (rx < wx || (rx == wx && ry <= wy));
			int take_w = wi < nw && (wx < rx || (wx == rx && wy <= ry));
			cell_job *j = &c.cells[c.ntkeys++];
			j->tx = take_r ? rx : wx;
			j->ty = take_r ? ry : wy;
			j->r0 = j->r1 = i;
			if (take_r) {
				while (i < c.runs.n && c.tr[i].tx == rx && c.tr[i].ty == ry) i++;
				j->r1 = i;
			}
			if (take_w) wi++;
		}
		(void)cap;
	}
	{
		int32_t *xy = (int32_t *)malloc((c.ntkeys ? c.ntkeys : 1) * 2 * sizeof *xy);
		for (size_t k = 0; k < c.ntkeys; k++) {
			xy[k * 2] = c.cells[k].tx;
			xy[k * 2 + 1] = c.cells[k].ty;
		}
		c.ntpages = make_pages(xy, c.ntkeys, &c.tpages, &c.tpage_cells);
		free(xy);
		swprintf(p, 1024, L"%ls\\pages", o->out_dir);
		CreateDirectoryW(p, NULL);
	}
	run_pool(o, "tiles", c.ntpages, tile_page_job, &c);
	st->t_tiles = lossantos_now() - t;
	st->sanded = (size_t)c.sanded;
	st->files = (size_t)c.files_written;

	t = lossantos_now();
	qsort(c.phys.p, c.phys.n, sizeof *c.phys.p, cmp_phys);
	c.pr = c.phys.p;
	c.pstart = (size_t *)malloc((c.phys.n + 1) * sizeof *c.pstart);
	for (size_t i = 0; i < c.phys.n; i++)
		if (i == 0 || c.pr[i].cx != c.pr[i - 1].cx || c.pr[i].cy != c.pr[i - 1].cy) c.pstart[c.npkeys++] = i;
	c.pstart[c.npkeys] = c.phys.n;
	{
		int32_t *xy = (int32_t *)malloc((c.npkeys ? c.npkeys : 1) * 2 * sizeof *xy);
		for (size_t k = 0; k < c.npkeys; k++) {
			xy[k * 2] = c.pr[c.pstart[k]].cx;
			xy[k * 2 + 1] = c.pr[c.pstart[k]].cy;
		}
		c.nppages = make_pages(xy, c.npkeys, &c.ppages, &c.ppage_keys);
		free(xy);
	}
	run_pool(o, "physics", c.nppages, phys_page_job, &c);
	st->t_phys = lossantos_now() - t;

	// textures -> gma chunks, then flags + canon
	if (o->content_dir && !(o->cancel && *o->cancel)) {
		t = lossantos_now();
		LOSSANTOS_VEC(lossantos_content_job) cj = {0};
		lossantos_strtab seen;
		lossantos_strtab_init(&seen);
#define ADDJOB(P, I)                                                            \
	do {                                                                        \
		if (lossantos_strtab_find(&seen, (P)) < 0) {                                  \
			lossantos_strtab_id(&seen, (P));                                          \
			lossantos_content_job j_;                                                 \
			snprintf(j_.path, sizeof j_.path, "%s", (P));                       \
			j_.info = (I);                                                      \
			lossantos_vec_push(cj, j_);                                               \
		}                                                                       \
	} while (0)
		for (size_t i = 0; i < c.nrefs; i++)
			for (size_t k = 0; k < c.installs[i].n; k++) ADDJOB(c.installs[i].p[k].path, &c.installs[i].p[k].info);
		for (size_t k = 0; k < w->roads.n; k++) ADDJOB(w->roads.p[k].path, &w->roads.p[k].info);
		for (size_t k = 0; k < c.slod.installs.n; k++) ADDJOB(c.slod.installs.p[k].path, &c.slod.installs.p[k].info);
		ADDJOB(LOSSANTOS_WATER_MATERIAL, NULL);
#undef ADDJOB
		lossantos_content_result *cr = (lossantos_content_result *)calloc(cj.n ? cj.n : 1, sizeof *cr);
		lossantos_content_stats cs;
		if (!lossantos_build_content(w->gta_dir, w->ng, o, o->content_dir, o->content_limit ? o->content_limit : 1500ull << 20,
							   cj.p, cj.n, cr, &cs))
			c.failed = 1;
		if (!c.failed && !lossantos_build_water_chunk(w->gta_dir, w->ng, o->work_dir, o->content_dir)) c.failed = 1;
		st->textures = cs.written;
		st->tex_skipped = cs.skipped;
		st->tex_rebuilt = cs.rebuilt;
		st->content_bytes = cs.bytes;
		st->content_chunks = cs.chunks;
		logf_(&c, "content: %zu jobs, %zu written (%zu rebuilt), %zu skipped, %zu files in %zu chunks (%.2f GB), "
				  "convert %.1f s, pack %.1f s\n",
			  cs.jobs, cs.written, cs.rebuilt, cs.skipped, cs.files, cs.chunks, cs.bytes / 1073741824.0, cs.t_convert,
			  cs.t_pack);
		swprintf(p, 1024, L"%ls\\material_flags.dat", o->out_dir);
		FILE *fd = _wfopen(p, L"wb");
		swprintf(p, 1024, L"%ls\\material_flags.json", o->out_dir);
		FILE *fj = _wfopen(p, L"wb");
		if (fd && fj) {
			char **nm = (char **)malloc((c.mats.n ? c.mats.n : 1) * sizeof *nm);
			for (uint32_t i = 0; i < c.mats.n; i++) nm[i] = c.mats.s[i];
			qsort(nm, c.mats.n, sizeof *nm, cmp_str);
			fprintf(fd, "GVMF1\n");
			fprintf(fj, "{\"flags\":{");
			for (uint32_t i = 0; i < c.mats.n; i++) {
				int64_t ji = lossantos_strtab_find(&seen, nm[i]);
				uint32_t fl = (ji >= 0 && cr[ji].ok) ? cr[ji].flags : lossantos_name_flags(nm[i]);
				fprintf(fd, "%s\t%u\n", nm[i], fl);
				fprintf(fj, "%s\"%s\":%u", i ? "," : "", nm[i], fl);
			}
			fprintf(fj, "},\"schema\":1}");
			free(nm);
		}
		if (fd) fclose(fd);
		if (fj) fclose(fj);
		{
			// mat_canon, same vtf + params is one material
			uint32_t *ord = (uint32_t *)malloc((cj.n ? cj.n : 1) * sizeof *ord);
			size_t no = 0;
			for (size_t i = 0; i < cj.n; i++)
				if (cr[i].ok) ord[no++] = (uint32_t)i;
			g_canon_res = cr;
			g_canon_jobs = cj.p;
			qsort(ord, no, sizeof *ord, cmp_canon);
			LOSSANTOS_VEC(char *) lines = {0};
			for (size_t s0 = 0; s0 < no;) {
				size_t e0 = s0 + 1;
				while (e0 < no && memcmp(cr[ord[e0]].sha1, cr[ord[s0]].sha1, 20) == 0 &&
					   strcmp(cr[ord[e0]].canon_key, cr[ord[s0]].canon_key) == 0)
					e0++;
				for (size_t k = s0 + 1; k < e0; k++) {
					char *ln = (char *)malloc(200);
					snprintf(ln, 200, "%s\t%s", cj.p[ord[k]].path, cj.p[ord[s0]].path);
					lossantos_vec_push(lines, ln);
				}
				s0 = e0;
			}
			qsort(lines.p, lines.n, sizeof *lines.p, cmp_str);
			swprintf(p, 1024, L"%ls\\mat_canon.txt", o->out_dir);
			FILE *fc = _wfopen(p, L"wb");
			if (fc) {
				for (size_t k = 0; k < lines.n; k++) fprintf(fc, "%s\n", lines.p[k]);
				fclose(fc);
			}
			st->canon_aliases = lines.n;
			for (size_t k = 0; k < lines.n; k++) free(lines.p[k]);
			lossantos_vec_free(lines);
			free(ord);
		}
		free(cr);
		lossantos_strtab_free(&seen);
		lossantos_vec_free(cj);
		st->t_content = lossantos_now() - t;
	}

	t = lossantos_now();
	if (!write_index(&c, st)) c.failed = 1;
	if (!c.failed && !(o->cancel && *o->cancel) && !write_tile_materials(&c)) c.failed = 1;
	st->t_index = lossantos_now() - t;
	st->tiles = 0;
	for (size_t k = 0; k < c.ntkeys; k++) st->tiles += c.cells[k].files[0][0] != 0;
	st->chunks = c.npkeys;
	st->tile_tris = (size_t)c.tile_tris;
	st->tile_verts = (size_t)c.tile_verts;
	st->chunk_tris = (size_t)c.chunk_tris;
	st->chunk_dropped = (size_t)c.chunk_dropped;

out:
	for (size_t i = 0; i <= c.nrefs; i++) {
		if (c.spill[i] && c.spill[i] != INVALID_HANDLE_VALUE) CloseHandle(c.spill[i]);
		spill_path(&c, i, p, 1024);
		DeleteFileW(p);
		if (i == c.nrefs) break;
		lossantos_vec_free(c.installs[i]);
		lossantos_vec_free(c.atlas[i]);
		lossantos_vec_free(c.items[i]);
		lossantos_vec_free(c.local[i]);
		lossantos_vec_free(c.extra[i]);
		for (size_t k = 0; k < c.missing[i].n; k++) free(c.missing[i].p[k]);
		lossantos_vec_free(c.missing[i]);
	}
	RemoveDirectoryW(o->work_dir);
	free(c.installs);
	free(c.atlas);
	free(c.items);
	free(c.local);
	free(c.extra);
	free(c.missing);
	free(c.status);
	free(c.kind);
	free(c.spill);
	free(c.spill_size);
	free(c.rank);
	free(c.cells);
	free(c.tpages);
	free(c.ppages);
	free(c.tpage_cells);
	free(c.ppage_keys);
	lossantos_slod_free(&c.slod);
	lossantos_beach_free(&c.beach);
	lossantos_water_free(&c.water);
	lossantos_vec_free(c.markers);
	lossantos_hset_free(&c.slod_cells);
	free(c.pstart);
	lossantos_vec_free(c.runs);
	lossantos_vec_free(c.phys);
	lossantos_strtab_free(&c.mats);
	lossantos_vec_free(sorted);
	st->t_total = lossantos_now() - t0;
	return !c.failed && !(o->cancel && *o->cancel);
}
