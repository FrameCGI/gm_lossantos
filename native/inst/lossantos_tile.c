#include "lossantos_tile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lossantos_tilemem_free(lossantos_tilemem *t) {
	for (size_t i = 0; i < t->runs.n; i++) {
		free(t->runs.p[i].v);
		free(t->runs.p[i].ix);
	}
	lossantos_vec_free(t->runs);
}

int lossantos_tilemem_add(lossantos_tilemem *t, uint32_t mat, const uint8_t *v, uint32_t nverts, const uint16_t *ix,
					uint32_t nidx) {
	if (!nidx) return 1;
	lossantos_trun r;
	r.mat = mat;
	r.nverts = nverts;
	r.nidx = nidx;
	r.v = (uint8_t *)malloc((size_t)nverts * 28 + 1);
	r.ix = (uint16_t *)malloc((size_t)nidx * 2 + 1);
	if (!r.v || !r.ix) {
		free(r.v);
		free(r.ix);
		return 0;
	}
	memcpy(r.v, v, (size_t)nverts * 28);
	memcpy(r.ix, ix, (size_t)nidx * 2);
	return lossantos_vec_push(t->runs, r);
}

size_t lossantos_tilemem_tris(const lossantos_tilemem *t) {
	size_t n = 0;
	for (size_t i = 0; i < t->runs.n; i++) n += t->runs.p[i].nidx / 3;
	return n;
}

void lossantos_vtx_encode(uint8_t *o, const double *p, const double *n, const double *uv) {
	float f[5] = {(float)p[0], (float)p[1], (float)p[2], (float)uv[0], (float)uv[1]};
	memcpy(o, f, 20);
	for (int a = 0; a < 2; a++) {
		double q = nearbyint(uv[a] * 65535.0);
		if (!(q > 0)) q = 0;
		if (q > 65535.0) q = 65535.0;
		uint16_t u = (uint16_t)q;
		memcpy(o + 20 + a * 2, &u, 2);
	}
	for (int a = 0; a < 3; a++) {
		double q = nearbyint(n[a] * 127.0);
		if (q < -127) q = -127;
		if (q > 127) q = 127;
		o[24 + a] = (uint8_t)(int8_t)q;
	}
	o[27] = 0;
}

int lossantos_tilemem_encode(const lossantos_tilemem *t, const char *const *names, lossantos_buf *out, size_t *tris,
					   size_t *verts) {
	size_t nr = t->runs.n;
	out->n = 0;
	uint32_t *order = (uint32_t *)malloc((nr ? nr : 1) * sizeof *order);
	uint8_t *done = (uint8_t *)calloc(nr ? nr : 1, 1);
	if (!order || !done) { free(order); free(done); return -1; }
	// materials in first appearance order
	size_t no = 0;
	for (size_t i = 0; i < nr; i++) {
		if (!t->runs.p[i].nidx) done[i] = 1;   /* emptied by the beach split */
		if (done[i]) continue;
		for (size_t j = i; j < nr; j++)
			if (!done[j] && t->runs.p[j].nidx && t->runs.p[j].mat == t->runs.p[i].mat) {
				order[no++] = (uint32_t)j;
				done[j] = 1;
			}
	}
	free(done);
	if (!no) { free(order); return 0; }
	lossantos_strtab tn;
	lossantos_strtab_init(&tn);
	LOSSANTOS_VEC(uint32_t) subs = {0};   /* 4 per submesh */
	size_t vtotal = 0, itotal = 0;
	for (size_t oi = 0; oi < no; oi++) {
		vtotal += t->runs.p[order[oi]].nverts;
		itotal += t->runs.p[order[oi]].nidx;
	}
	uint32_t vbase = 0, block_start = 0, block_verts = 0, cur = UINT32_MAX;
	uint32_t ip = 0;
	for (size_t oi = 0; oi < no; oi++) {
		const lossantos_trun *r = &t->runs.p[order[oi]];
		if (r->mat != cur) {
			vbase += block_verts;
			block_start = vbase;
			block_verts = 0;
			cur = r->mat;
		}
		// a material shares one 65535 vertex window
		if (block_verts + r->nverts > 65535u) {
			vbase += block_verts;
			block_start = vbase;
			block_verts = 0;
		}
		lossantos_vec_push(subs, lossantos_strtab_id(&tn, names[r->mat]));
		lossantos_vec_push(subs, ip);
		lossantos_vec_push(subs, r->nidx);
		lossantos_vec_push(subs, block_start);
		ip += r->nidx;
		block_verts += r->nverts;
	}
	size_t total = 32 + subs.n * 4 + (size_t)tn.n * 64 + vtotal * 28 + itotal * 2;
	int rc = 1;
	if (!lossantos_buf_reserve(out, total)) {
		rc = -1;
	} else {
		uint8_t *o = out->p;
		// GVT1 header, same as tb_write
		struct {
			uint32_t magic;
			uint16_t ver, flags;
			uint32_t vc, ic, sc, mc, stride, res;
		} h = {0x31545647u, 1, 3, (uint32_t)vtotal, (uint32_t)itotal, (uint32_t)(subs.n / 4), tn.n, 28, 0};
		memcpy(o, &h, 32);
		o += 32;
		memcpy(o, subs.p, subs.n * 4);
		o += subs.n * 4;
		for (uint32_t i = 0; i < tn.n; i++) {
			memset(o, 0, 64);
			size_t l = strlen(tn.s[i]);
			memcpy(o, tn.s[i], l > 63 ? 63 : l);
			o += 64;
		}
		for (size_t oi = 0; oi < no; oi++) {
			const lossantos_trun *r = &t->runs.p[order[oi]];
			memcpy(o, r->v, (size_t)r->nverts * 28);
			o += (size_t)r->nverts * 28;
		}
		// indices local to their window
		block_verts = 0;
		cur = UINT32_MAX;
		for (size_t oi = 0; oi < no; oi++) {
			const lossantos_trun *r = &t->runs.p[order[oi]];
			if (r->mat != cur) {
				block_verts = 0;
				cur = r->mat;
			}
			if (block_verts + r->nverts > 65535u) block_verts = 0;
			uint16_t *ix = (uint16_t *)o;
			for (uint32_t k = 0; k < r->nidx; k++) ix[k] = (uint16_t)(r->ix[k] + block_verts);
			o += (size_t)r->nidx * 2;
			block_verts += r->nverts;
		}
		out->n = total;
		if (tris) *tris += itotal / 3;
		if (verts) *verts += vtotal;
	}
	free(order);
	lossantos_vec_free(subs);
	lossantos_strtab_free(&tn);
	return rc;
}

int lossantos_tilemem_write(const lossantos_tilemem *t, const char *const *names, const wchar_t *path, size_t *tris,
					  size_t *verts) {
	lossantos_buf b = {0};
	int rc = lossantos_tilemem_encode(t, names, &b, tris, verts);
	if (rc == 1) {
		FILE *f = _wfopen(path, L"wb");
		if (!f) rc = -1;
		else {
			fwrite(b.p, 1, b.n, f);
			if (ferror(f)) rc = -1;
			fclose(f);
		}
	}
	lossantos_buf_free(&b);
	return rc;
}


int lossantos_page_add(lossantos_page *pg, const char *name, const void *blob, size_t len) {
	lossantos_page_entry e;
	snprintf(e.name, sizeof e.name, "%s", name);
	e.off = pg->data.n;
	e.len = len;
	if (!lossantos_buf_reserve(&pg->data, pg->data.n + len)) return 0;
	memcpy(pg->data.p + pg->data.n, blob, len);
	pg->data.n += len;
	return lossantos_vec_push(pg->entries, e);
}

static int cmp_entry(const void *a, const void *b) {
	return strcmp(((const lossantos_page_entry *)a)->name, ((const lossantos_page_entry *)b)->name);
}

// 0   magic "GVPG"
// 4   ver u16
// 6   flags u16
// 8   entry count
// 12  reserved
// 16  entries, 16 each (name off, blob off, len, 0)
//     names, nul terminated
//     blobs
int lossantos_page_write(lossantos_page *pg, const wchar_t *path) {
	size_t n = pg->entries.n;
	if (!n) return 0;
	qsort(pg->entries.p, n, sizeof *pg->entries.p, cmp_entry);
	size_t names = 0;
	for (size_t i = 0; i < n; i++) names += strlen(pg->entries.p[i].name) + 1;
	size_t head = 16 + n * 16;
	if (head + names + pg->data.n > 0xFFFFFFFFull) return -1;   /* u32 offsets */
	uint8_t *h = (uint8_t *)calloc(head + names, 1);
	if (!h) return -1;
	memcpy(h, "GVPG", 4);
	uint16_t ver = 1, flags = 0;
	uint32_t cnt = (uint32_t)n, res = 0;
	memcpy(h + 4, &ver, 2);
	memcpy(h + 6, &flags, 2);
	memcpy(h + 8, &cnt, 4);
	memcpy(h + 12, &res, 4);
	size_t no = head, bo = head + names;
	for (size_t i = 0; i < n; i++) {
		const lossantos_page_entry *e = &pg->entries.p[i];
		uint32_t w4[4] = {(uint32_t)no, (uint32_t)(bo), (uint32_t)e->len, 0};
		memcpy(h + 16 + i * 16, w4, 16);
		size_t l = strlen(e->name) + 1;
		memcpy(h + no, e->name, l);
		no += l;
		bo += e->len;
	}
	FILE *f = _wfopen(path, L"wb");
	int rc = 1;
	if (!f) rc = -1;
	else {
		fwrite(h, 1, head + names, f);
		for (size_t i = 0; i < n; i++) fwrite(pg->data.p + pg->entries.p[i].off, 1, pg->entries.p[i].len, f);
		if (ferror(f)) rc = -1;
		fclose(f);
	}
	free(h);
	return rc;
}

void lossantos_page_free(lossantos_page *pg) {
	lossantos_vec_free(pg->entries);
	lossantos_buf_free(&pg->data);
}


typedef struct {
	double p[3], uv[2], n[3], na[3];
	double cnt, cn, cna;
	uint32_t out;      /* output vertex, UINT32_MAX = unused */
	int snap_axis;     /* -1, or the axis pinned to snap_val (border line) */
	double snap_val;
} cluster;

static void decode(const uint8_t *v, double p[3], double uv[2], double n[3]) {
	float f[5];
	memcpy(f, v, 20);
	p[0] = f[0]; p[1] = f[1]; p[2] = f[2];
	uv[0] = f[3]; uv[1] = f[4];
	for (int a = 0; a < 2; a++)
		if (!isfinite(uv[a]) || fabs(uv[a]) > 1e6) uv[a] = 0.0;
	for (int a = 0; a < 3; a++) n[a] = (double)(int8_t)v[24 + a] / 127.0;
}

// first half of a [fronts][mirrors] run
static void authored(const lossantos_trun *r, uint8_t *keep) {
	uint32_t count = r->nidx;
	memset(keep, 1, r->nverts);
	if (count <= 3 || count % 6 != 0) return;
	uint32_t half = count / 2;
	for (uint32_t t = 0; t < half / 3; t++) {
		const uint16_t *a = r->ix + t * 3, *b = r->ix + half + t * 3;
		float pa[3][3], pb[3][3];
		for (int c = 0; c < 3; c++) {
			memcpy(pa[c], r->v + (size_t)a[c] * 28, 12);
			memcpy(pb[c], r->v + (size_t)b[c] * 28, 12);
		}
		int fwd = memcmp(pa[0], pb[0], 12) == 0 && memcmp(pa[1], pb[2], 12) == 0 && memcmp(pa[2], pb[1], 12) == 0;
		int same = memcmp(pa[0], pb[0], 12) == 0 && memcmp(pa[1], pb[1], 12) == 0 && memcmp(pa[2], pb[2], 12) == 0;
		if (!fwd && !same) return;          /* not a doubled run */
	}
	memset(keep, 0, r->nverts);
	for (uint32_t k = 0; k < half; k++) keep[r->ix[k]] = 1;
}

// per thread, cleared by bumping the generation
#define CSLOTS (1u << 17)
typedef struct {
	uint32_t *gen, *id, g;
	uint64_t *hash;
} ctable;
static __declspec(thread) ctable g_ct;

static int ct_ready(void) {
	if (g_ct.gen) return 1;
	g_ct.gen = (uint32_t *)calloc(CSLOTS, sizeof *g_ct.gen);
	g_ct.id = (uint32_t *)malloc(CSLOTS * sizeof *g_ct.id);
	g_ct.hash = (uint64_t *)malloc(CSLOTS * sizeof *g_ct.hash);
	g_ct.g = 1;
	return g_ct.gen && g_ct.id && g_ct.hash;
}

static void ct_clear(void) {
	if (++g_ct.g == 0) {
		memset(g_ct.gen, 0, CSLOTS * sizeof *g_ct.gen);
		g_ct.g = 1;
	}
}

int lossantos_lod_cluster(const lossantos_tilemem *in, double cell, int coarse, int nrm_step, const double *box,
					lossantos_tilemem *out) {
	int step = coarse ? nrm_step : 4;
	memset(out, 0, sizeof *out);
	if (!ct_ready()) return 0;
	LOSSANTOS_VEC(int64_t) keys = {0};
	LOSSANTOS_VEC(cluster) cl = {0};
	LOSSANTOS_VEC(uint8_t) vb = {0};
	LOSSANTOS_VEC(uint16_t) ib = {0};
	uint32_t *vmap = NULL;
	uint8_t *keep = NULL;
	size_t cap = 0;
	int ok = 1;
	for (size_t ri = 0; ri < in->runs.n && ok; ri++) {
		const lossantos_trun *r = &in->runs.p[ri];
		if (r->nverts > cap) {
			free(vmap);
			free(keep);
			cap = r->nverts;
			vmap = (uint32_t *)malloc(cap * sizeof *vmap);
			keep = (uint8_t *)malloc(cap);
			if (!vmap || !keep) { ok = 0; break; }
		}
		authored(r, keep);
		// clusters never cross runs
		ct_clear();
		keys.n = 0;
		cl.n = 0;
		for (uint32_t v = 0; v < r->nverts; v++) vmap[v] = UINT32_MAX;
		for (uint32_t k = 0; k < r->nidx; k++) {
			uint32_t v = r->ix[k];
			if (vmap[v] != UINT32_MAX) continue;
			double p[3], uv[2], n[3];
			decode(r->v + (size_t)v * 28, p, uv, n);
			int64_t key[8] = {0};
			int nk = 3;
			for (int a = 0; a < 3; a++) key[a] = (int64_t)floor(p[a] / cell);
			// border verts stay put so neighbour lods still meet
			int side = -1, nside = 0;
			if (box) {
				static const int AX[4] = {0, 1, 0, 1};
				for (int b4 = 0; b4 < 4; b4++)
					if (fabs(p[AX[b4]] - box[b4]) < 1e-3) {
						side = b4;
						nside++;
					}
			}
			int border = nside > 0;
			if (nside > 1) {
				// cell corner: kept exact
				for (int a = 0; a < 3; a++) key[a] = (int64_t)nearbyint(p[a] * 1000.0);
				key[7] = INT64_MIN;
			} else if (border) {
				int ax = side & 1;
				key[ax] = INT64_MIN + side;
				key[7] = INT64_MIN + 1;
			}
			if (!coarse || step) {
				double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (l < 1e-9) l = 1e-9;
				for (int a = 0; a < 3; a++) key[3 + a] = (int64_t)nearbyint(n[a] / l * step);
				nk = 6;
			}
			if (!coarse || nside > 1) {
				key[6] = (int64_t)nearbyint(uv[0] * 1024.0);
				if (!border) key[7] = (int64_t)nearbyint(uv[1] * 1024.0);
				nk = 8;
			}
			if (nside > 1 && coarse && !step) {
				double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (l < 1e-9) l = 1e-9;
				for (int a = 0; a < 3; a++) key[3 + a] = (int64_t)nearbyint(n[a] / l * 4);
			}
			(void)nk;  // nk isnt used
			uint64_t h = lossantos_hash_bytes(key, 64);
			uint32_t j = (uint32_t)h & (CSLOTS - 1), id = UINT32_MAX;
			while (g_ct.gen[j] == g_ct.g) {
				if (g_ct.hash[j] == h && memcmp(keys.p + (size_t)g_ct.id[j] * 8, key, 64) == 0) {
					id = g_ct.id[j];
					break;
				}
				j = (j + 1) & (CSLOTS - 1);
			}
			if (id == UINT32_MAX) {
				id = (uint32_t)cl.n;
				g_ct.gen[j] = g_ct.g;
				g_ct.hash[j] = h;
				g_ct.id[j] = id;
				cluster c;
				memset(&c, 0, sizeof c);
				c.out = UINT32_MAX;
				c.snap_axis = -1;
				if (border && nside == 1) {
					c.snap_axis = side & 1;
					c.snap_val = box[side];
				}
				if (!lossantos_vec_push(cl, c)) { ok = 0; break; }
				for (int a = 0; a < 8; a++) lossantos_vec_push(keys, key[a]);
			}
			vmap[v] = id;
			cluster *c = &cl.p[id];
			for (int a = 0; a < 3; a++) c->p[a] += p[a];
			for (int a = 0; a < 2; a++) c->uv[a] += uv[a];
			c->cnt += 1;
			for (int a = 0; a < 3; a++) c->na[a] += n[a];
			c->cna += 1;
			if (keep[v]) {
				for (int a = 0; a < 3; a++) c->n[a] += n[a];
				c->cn += 1;
			}
		}
		if (!ok) break;
		vb.n = 0;
		ib.n = 0;
		uint32_t nout = 0;
		for (uint32_t k = 0; k + 2 < r->nidx; k += 3) {
			uint32_t a = vmap[r->ix[k]], b = vmap[r->ix[k + 1]], c2 = vmap[r->ix[k + 2]];
			if (a == b || b == c2 || a == c2) continue;
			uint32_t t3[3] = {a, b, c2};
			for (int q = 0; q < 3; q++) {
				cluster *c = &cl.p[t3[q]];
				if (c->out == UINT32_MAX) {
					double p[3], uv[2], n[3];
					for (int z = 0; z < 3; z++) p[z] = c->p[z] / c->cnt;
					if (c->snap_axis >= 0) p[c->snap_axis] = c->snap_val;
					for (int z = 0; z < 2; z++) uv[z] = c->uv[z] / c->cnt;
					const double *ns = c->cn > 0 ? c->n : c->na;
					double cnt = c->cn > 0 ? c->cn : c->cna;
					for (int z = 0; z < 3; z++) n[z] = ns[z] / (cnt > 1 ? cnt : 1.0);
					double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
					if (l < 1e-6) {
						n[0] = n[1] = 0;
						n[2] = 1;
					} else {
						double d = l > 1e-9 ? l : 1e-9;
						for (int z = 0; z < 3; z++) n[z] /= d;
					}
					size_t at = vb.n;
					for (int z = 0; z < 28; z++) lossantos_vec_push(vb, 0);
					lossantos_vtx_encode(vb.p + at, p, n, uv);
					c->out = nout++;
				}
				lossantos_vec_push(ib, (uint16_t)c->out);
			}
		}
		if (ib.n) ok = lossantos_tilemem_add(out, r->mat, vb.p, nout, ib.p, (uint32_t)ib.n);
	}
	// nothing survived, write the input
	if (ok && !out->runs.n) {
		for (size_t ri = 0; ri < in->runs.n && ok; ri++) {
			const lossantos_trun *r = &in->runs.p[ri];
			ok = lossantos_tilemem_add(out, r->mat, r->v, r->nverts, r->ix, r->nidx);
		}
	}
	free(vmap);
	free(keep);
	lossantos_vec_free(keys);
	lossantos_vec_free(cl);
	lossantos_vec_free(vb);
	lossantos_vec_free(ib);
	return ok;
}
