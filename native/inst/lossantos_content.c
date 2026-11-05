#include "lossantos_content.h"
#include "lossantos_gma.h"
#include "lossantos_rpf.h"
#include "lossantos_rsc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lossantos_run_pool(const lossantos_map_opts *o, const char *stage, size_t n, void (*fn)(void *, size_t), void *ctx);

static const char *const ARCH[] = {"x64i.rpf", "x64j.rpf", "x64k.rpf", "x64l.rpf",
								   "x64m.rpf", "x64n.rpf", "x64o.rpf", "x64p.rpf"};
#define MAXW 64

typedef LOSSANTOS_VEC(lossantos_gma_file) gfile_vec;

typedef struct {
	const wchar_t *gta;
	const lossantos_ng *ng;
	const lossantos_map_opts *o;
	const lossantos_content_job *jobs;
	lossantos_content_result *res;
	uint32_t *order;           /* job indices sorted by source resource */
	size_t *gstart;            /* group starts, ngroups + 1 */
	size_t ngroups;
	volatile LONG nworkers;
	FILE *spill[MAXW];
	uint64_t spill_off[MAXW];
	gfile_vec files[MAXW];
	volatile LONG64 written, skipped, rebuilt;
	int failed;
	void *ws[MAXW];            /* worker states (closed at the end) */
} cctx;

typedef struct {
	const cctx *owner;
	int id;
	lossantos_rpf outer[9];  /* 8 city archives + x64g */
	int open[9];             /* 1 open, -1 failed */
	lossantos_buf buf;
	lossantos_texout tex;
} wstate;

static __declspec(thread) wstate *g_ws;

static int src_key_cmp(void *ctx, const void *a, const void *b) {
	const lossantos_content_job *J = (const lossantos_content_job *)ctx;
	const lossantos_content_job *x = &J[*(const uint32_t *)a], *y = &J[*(const uint32_t *)b];
	if (!x->info || !y->info) return (x->info != NULL) - (y->info != NULL);
	const lossantos_texloc *p = &x->info->loc, *q = &y->info->loc;
	if (p->archive != q->archive) return p->archive < q->archive ? -1 : 1;
	if (p->nested != q->nested) return p->nested < q->nested ? -1 : 1;
	if (p->entry != q->entry) return p->entry < q->entry ? -1 : 1;
	return p->tex_index < q->tex_index ? -1 : p->tex_index > q->tex_index;
}

static int same_src(const lossantos_content_job *a, const lossantos_content_job *b) {
	if (!a->info || !b->info) return 0;
	return a->info->loc.archive == b->info->loc.archive && a->info->loc.nested == b->info->loc.nested &&
		   a->info->loc.entry == b->info->loc.entry;
}

static wstate *worker(cctx *c) {
	if (g_ws && g_ws->owner == c) return g_ws;
	wstate *w = (wstate *)calloc(1, sizeof *w);
	w->owner = c;
	w->id = (int)InterlockedIncrement(&c->nworkers) - 1;
	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\c%02d.gtavspill", c->o->work_dir, w->id);
	c->ws[w->id] = w;
	c->spill[w->id] = _wfopen(p, L"wb");
	if (c->spill[w->id]) setvbuf(c->spill[w->id], NULL, _IOFBF, 1 << 20);
	g_ws = w;
	return w;
}

static int emit(cctx *c, wstate *w, const char *path, const char *ext, const void *data, size_t n) {
	lossantos_gma_file f;
	snprintf(f.path, sizeof f.path, "materials/%s.%s", path, ext);
	f.spill = (uint32_t)w->id;
	f.off = c->spill_off[w->id];
	f.size = (uint32_t)n;
	f.crc = lossantos_crc32(0, data, n);
	if (!c->spill[w->id] || fwrite(data, 1, n, c->spill[w->id]) != n) return 0;
	c->spill_off[w->id] += n;
	return lossantos_vec_push(c->files[w->id], f);
}

static void store(cctx *c, wstate *w, size_t ji) {
	const lossantos_content_job *j = &c->jobs[ji];
	lossantos_content_result *r = &c->res[ji];
	if (!emit(c, w, j->path, "vmt", w->tex.vmt, strlen(w->tex.vmt)) ||
		!emit(c, w, j->path, "vtf", w->tex.vtf.p, w->tex.vtf.n)) {
		c->failed = 1;
		return;
	}
	r->ok = 1;
	r->flags = w->tex.flags;
	memcpy(r->sha1, w->tex.sha1, 20);
	snprintf(r->canon_key, sizeof r->canon_key, "%s", w->tex.canon_key);
	InterlockedIncrement64(&c->written);
	if (w->tex.rebuilt) InterlockedIncrement64(&c->rebuilt);
}

// root+0x10 shader group, +0x08 dictionary
static int ydr_textures(lossantos_rsc *r, lossantos_tex *out, int cap) {
	uint64_t group = lossantos_rsc_u64(r, LOSSANTOS_SYS_BASE + 0x10);
	if (!group) return 0;
	uint64_t dict = lossantos_rsc_u64(r, group + 0x08);
	return dict ? lossantos_ytd_parse(r, dict, out, cap) : 0;
}

static void group_job(void *ctx, size_t gi) {
	cctx *c = (cctx *)ctx;
	wstate *w = worker(c);
	size_t s = c->gstart[gi], e = c->gstart[gi + 1];
	const lossantos_content_job *first = &c->jobs[c->order[s]];
	if (!first->info) {                       /* generated water material */
		for (size_t k = s; k < e; k++) {
			if (lossantos_water_material(&w->tex)) store(c, w, c->order[k]);
			else InterlockedIncrement64(&c->skipped);
		}
		return;
	}
	const lossantos_texloc *loc = &first->info->loc;
	int ai = loc->archive == 0xFFFF ? 8 : loc->archive;  // 0xFFFF = x64g
	if (ai > 8) {
		InterlockedAdd64(&c->skipped, (LONG64)(e - s));
		return;
	}
	if (!w->open[ai]) {
		wchar_t p[1024];
		swprintf(p, 1024, L"%ls\\%hs", c->gta, ai == 8 ? "x64g.rpf" : ARCH[ai]);
		w->open[ai] = lossantos_rpf_open(&w->outer[ai], c->ng, p) ? 1 : -1;
	}
	lossantos_rpf child;
	memset(&child, 0, sizeof child);
	lossantos_tex *tex = (lossantos_tex *)malloc(4096 * sizeof *tex);
	int ok = w->open[ai] == 1 && tex && loc->nested != UINT32_MAX &&
			 lossantos_rpf_open_child(&child, &w->outer[ai], loc->nested);
	int n = 0;
	if (ok) {
		if (loc->entry < child.count && lossantos_rpf_read(&child, loc->entry, &w->buf)) {
			lossantos_rsc r;
			if (lossantos_rsc_init(&r, w->buf.p, w->buf.n, lossantos_rpf_size_from_flags(child.e[loc->entry].sys_flags)))
				n = loc->embedded ? ydr_textures(&r, tex, 4096) : lossantos_ytd_parse(&r, LOSSANTOS_SYS_BASE, tex, 4096);
		}
	}
	for (size_t k = s; k < e; k++) {
		size_t ji = c->order[k];
		const lossantos_texinfo *ti = c->jobs[ji].info;
		const lossantos_tex *t = NULL;
		// tex_index first, then by name
		int idx = ti->loc.tex_index;
		if (idx < n) {
			char lo[128];
			lossantos_lower(lo, tex[idx].name, sizeof lo);
			if (strcmp(lo, ti->key) == 0) t = &tex[idx];
		}
		for (int q = 0; !t && q < n; q++) {
			char lo[128];
			lossantos_lower(lo, tex[q].name, sizeof lo);
			if (strcmp(lo, ti->key) == 0) t = &tex[q];
		}
		if (t && lossantos_texture_install(t, c->jobs[ji].path, &w->tex)) store(c, w, ji);
		else InterlockedIncrement64(&c->skipped);
	}
	if (ok) lossantos_rpf_close(&child);
	free(tex);
}

static int cmp_gfile(const void *a, const void *b) {
	return strcmp(((const lossantos_gma_file *)a)->path, ((const lossantos_gma_file *)b)->path);
}

static void region_of(const char *path, char *out, size_t cap) {
	const char *p = strstr(path, "models/gtav/");
	p = p ? p + 12 : path;
	const char *s = strchr(p, '/');
	snprintf(out, cap, "%.*s", (int)(s ? s - p : (ptrdiff_t)strlen(p)), p);
}

int lossantos_build_content(const wchar_t *gta_dir, const lossantos_ng *ng, const lossantos_map_opts *o, const wchar_t *content_dir,
					  uint64_t limit, const lossantos_content_job *jobs, size_t n, lossantos_content_result *res,
					  lossantos_content_stats *st) {
	double t0 = lossantos_now();
	memset(st, 0, sizeof *st);
	st->jobs = n;
	cctx *c = (cctx *)calloc(1, sizeof *c);
	if (!c) return 0;
	c->gta = gta_dir;
	c->ng = ng;
	c->o = o;
	c->jobs = jobs;
	c->res = res;
	memset(res, 0, n * sizeof *res);
	CreateDirectoryW(o->work_dir, NULL);
	CreateDirectoryW(content_dir, NULL);
	c->order = (uint32_t *)malloc((n ? n : 1) * sizeof *c->order);
	for (size_t i = 0; i < n; i++) c->order[i] = (uint32_t)i;
	qsort_s(c->order, n, sizeof *c->order, src_key_cmp, (void *)jobs);
	c->gstart = (size_t *)malloc((n + 1) * sizeof *c->gstart);
	for (size_t i = 0; i < n; i++)
		if (i == 0 || !same_src(&jobs[c->order[i]], &jobs[c->order[i - 1]])) c->gstart[c->ngroups++] = i;
	c->gstart[c->ngroups] = n;
	lossantos_run_pool(o, "textures", c->ngroups, group_job, c);
	for (int i = 0; i < MAXW; i++) {
		if (c->spill[i]) fclose(c->spill[i]);
		wstate *w = (wstate *)c->ws[i];
		if (!w) continue;
		for (int a = 0; a < 9; a++)
			if (w->open[a] == 1) lossantos_rpf_close(&w->outer[a]);
		lossantos_buf_free(&w->buf);
		lossantos_texout_free(&w->tex);
		free(w);
	}
	st->written = (size_t)c->written;
	st->skipped = (size_t)c->skipped;
	st->rebuilt = (size_t)c->rebuilt;
	st->t_convert = lossantos_now() - t0;

	// pack, sorted by path, chunked per region
	double t1 = lossantos_now();
	gfile_vec all = {0};
	for (int i = 0; i < MAXW; i++) {
		for (size_t k = 0; k < c->files[i].n; k++) lossantos_vec_push(all, c->files[i].p[k]);
		lossantos_vec_free(c->files[i]);
	}
	qsort(all.p, all.n, sizeof *all.p, cmp_gfile);
	HANDLE sp[MAXW];
	memset(sp, 0, sizeof sp);
	for (LONG i = 0; i < c->nworkers && i < MAXW; i++) {
		wchar_t p[1024];
		swprintf(p, 1024, L"%ls\\c%02d.gtavspill", o->work_dir, (int)i);
		sp[i] = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	}
	wchar_t lst[1024];
	swprintf(lst, 1024, L"%ls\\chunks_mat.txt", content_dir);
	FILE *lf = _wfopen(lst, L"wb");
	size_t s = 0;
	int chunk = 1;
	while (s < all.n && !c->failed) {
		size_t e = s;
		uint64_t size = 0;
		while (e < all.n) {
			char reg[96];
			region_of(all.p[e].path, reg, sizeof reg);
			// whole regions while they fit
			size_t re = e;
			uint64_t rs = 0;
			while (re < all.n) {
				char r2[96];
				region_of(all.p[re].path, r2, sizeof r2);
				if (strcmp(reg, r2) != 0) break;
				rs += all.p[re].size;
				re++;
			}
			if (e > s && size + rs > limit) break;
			size += rs;
			e = re;
		}
		char name[64];
		snprintf(name, sizeof name, "lossantos_content_mat_%02d", chunk++);
		char title[128];
		snprintf(title, sizeof title, "GTAV converted content (%s)", name);
		wchar_t out[1024];
		swprintf(out, 1024, L"%ls\\%hs.gma", content_dir, name);
		uint64_t b = lossantos_gma_write(out, title, all.p + s, e - s, (void *const *)sp);
		if (!b) c->failed = 1;
		st->bytes += b;
		st->chunks++;
		if (lf) fprintf(lf, "%s.gma\n", name);
		s = e;
	}
	if (lf) fclose(lf);
	st->files = all.n;
	for (LONG i = 0; i < c->nworkers && i < MAXW; i++) {
		if (sp[i] && sp[i] != INVALID_HANDLE_VALUE) CloseHandle(sp[i]);
		wchar_t p[1024];
		swprintf(p, 1024, L"%ls\\c%02d.gtavspill", o->work_dir, (int)i);
		DeleteFileW(p);
	}
	lossantos_vec_free(all);
	st->t_pack = lossantos_now() - t1;
	int ok = !c->failed;
	free(c->order);
	free(c->gstart);
	free(c);
	return ok;
}

int lossantos_build_water_chunk(const wchar_t *gta_dir, const lossantos_ng *ng, const wchar_t *work_dir, const wchar_t *content_dir) {
	static const char *const WANT[4] = {"oceanwaves", "shorewaves", "waterbump2", "waternoise"};
	wchar_t p[1024], sp[1024], out[1024];
	swprintf(p, 1024, L"%ls\\update\\update.rpf", gta_dir);
	swprintf(sp, 1024, L"%ls\\water.gtavspill", work_dir);
	swprintf(out, 1024, L"%ls\\lossantos_content_mat_00.gma", content_dir);
	lossantos_rpf up;
	if (!lossantos_rpf_open(&up, ng, p)) return 0;
	lossantos_buf b = {0}, v = {0};
	lossantos_tex *tex = (lossantos_tex *)malloc(4096 * sizeof *tex);
	lossantos_gma_file files[4];
	memset(files, 0, sizeof files);
	size_t n = 0;
	uint64_t off = 0;
	int32_t idx = lossantos_rpf_find(&up, "x64/textures/graphics.ytd");
	FILE *f = _wfopen(sp, L"wb");
	int ok = 0;
	if (idx >= 0 && tex && f && lossantos_rpf_read(&up, (uint32_t)idx, &b)) {
		lossantos_rsc r;
		int nt = 0;
		if (lossantos_rsc_init(&r, b.p, b.n, lossantos_rpf_size_from_flags(up.e[idx].sys_flags)))
			nt = lossantos_ytd_parse(&r, LOSSANTOS_SYS_BASE, tex, 4096);
		ok = 1;
		for (int w = 0; w < 4 && ok; w++) {
			ok = 0;
			for (int i = 0; i < nt && !ok; i++) {
				char lo[128];
				lossantos_lower(lo, tex[i].name, sizeof lo);
				if (strcmp(lo, WANT[w]) || !lossantos_texture_raw(&tex[i], &v)) continue;
				snprintf(files[n].path, sizeof files[n].path, "materials/gtav/water/%s.vtf", WANT[w]);
				files[n].off = off;
				files[n].size = (uint32_t)v.n;
				files[n].crc = lossantos_crc32(0, v.p, v.n);
				ok = fwrite(v.p, 1, v.n, f) == v.n;
				off += v.n;
				n++;
			}
		}
	}
	if (f) fclose(f);
	if (ok) {
		void *h = CreateFileW(sp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		ok = h != INVALID_HANDLE_VALUE &&
			 lossantos_gma_write(out, "GTAV converted content (lossantos_content_mat_00)", files, n, &h) != 0;
		if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
	} else {
		lossantos_set_error("water textures not found in update.rpf");
	}
	DeleteFileW(sp);
	lossantos_rpf_close(&up);
	lossantos_buf_free(&b);
	lossantos_buf_free(&v);
	free(tex);
	return ok;
}
