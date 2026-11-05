#include "lossantos_out.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE_MAGIC 0x31545647u  // "GVT1"
#define CHUNK_MAGIC 0x31435647u  // "GVC1"
#define MAX_VERTS 65535u

// weld table
#define WSLOTS (1u << 18)
#define WMASK (WSLOTS - 1)

int lossantos_tb_init(lossantos_tilebuild *t) {
	memset(t, 0, sizeof *t);
	t->single_sided = 1;
	t->wgen = (uint32_t *)calloc(WSLOTS, sizeof *t->wgen);
	t->wvert = (uint32_t *)malloc(WSLOTS * sizeof *t->wvert);
	t->whash = (uint64_t *)malloc(WSLOTS * sizeof *t->whash);
	t->gen = 1;
	return t->wgen && t->wvert && t->whash && lossantos_strtab_init(&t->names);
}

void lossantos_tb_free(lossantos_tilebuild *t) {
	lossantos_strtab_free(&t->names);
	lossantos_vec_free(t->subs);
	lossantos_buf_free(&t->verts);
	lossantos_vec_free(t->idx);
	free(t->wgen);
	free(t->wvert);
	free(t->whash);
	lossantos_vec_free(t->wkeys);
	lossantos_vec_free(t->pending);
}

static void weld_clear(lossantos_tilebuild *t) {
	if (++t->gen == 0) {             /* wrapped: really clear once */
		memset(t->wgen, 0, WSLOTS * sizeof *t->wgen);
		t->gen = 1;
	}
	t->wkeys.n = 0;
	t->block_verts = 0;
}

void lossantos_tb_reset(lossantos_tilebuild *t) {
	lossantos_strtab_free(&t->names);
	lossantos_strtab_init(&t->names);
	t->subs.n = 0;
	t->verts.n = 0;
	t->idx.n = 0;
	t->open = 0;
	weld_clear(t);
}

static int open_block(lossantos_tilebuild *t) {
	lossantos_submesh s;
	s.mat = t->cur_mat;
	s.ioff = (uint32_t)t->idx.n;
	s.icount = 0;
	s.voff = (uint32_t)(t->verts.n / 28);
	weld_clear(t);
	t->open = 1;
	return lossantos_vec_push(t->subs, s);
}

static int add_tri(lossantos_tilebuild *t, const double *pos, const double *nrm, const double *uv, int mirror);

int lossantos_tb_close(lossantos_tilebuild *t) {
	// [fronts][mirrors], same tris again, normal negated
	if (t->open && !t->single_sided) {
		size_t np = t->pending.n / 24;
		for (size_t i = 0; i < np; i++) {
			const double *d = t->pending.p + i * 24;
			double neg[9];
			for (int a = 0; a < 9; a++) neg[a] = -d[9 + a];
			if (!add_tri(t, d, neg, d + 18, 1)) return 0;
		}
	}
	t->pending.n = 0;
	if (t->open && t->subs.n && t->subs.p[t->subs.n - 1].icount == 0) t->subs.n--;  /* empty run */
	t->open = 0;
	return 1;
}

void lossantos_tb_sided(lossantos_tilebuild *t, int single_sided) {
	lossantos_tb_close(t);
	t->single_sided = single_sided;
}

int lossantos_tb_material(lossantos_tilebuild *t, const char *name) {
	if (!lossantos_tb_close(t)) return 0;
	uint32_t id = lossantos_strtab_id(&t->names, name);
	if (id == UINT32_MAX) return 0;
	t->cur_mat = id;
	return open_block(t);
}

static void corner_key(const double *p, const double *n, const double *uv, int64_t k[8]) {
	for (int a = 0; a < 3; a++) k[a] = (int64_t)nearbyint(p[a] * 100.0);
	for (int a = 0; a < 3; a++) k[3 + a] = (int64_t)nearbyint(n[a] * 127.0);
	for (int a = 0; a < 2; a++) k[6 + a] = (int64_t)nearbyint(uv[a] * 1000.0);
}

static int64_t weld_find(const lossantos_tilebuild *t, const int64_t k[8], uint64_t *slot) {
	uint64_t h = lossantos_hash_bytes(k, 64);
	uint32_t j = (uint32_t)h & WMASK;
	while (t->wgen[j] == t->gen) {
		if (t->whash[j] == h && memcmp(t->wkeys.p + (size_t)t->wvert[j] * 8, k, 64) == 0) return t->wvert[j];
		j = (j + 1) & WMASK;
	}
	*slot = ((uint64_t)j << 32) | (h >> 32);   /* slot index + hash high bits */
	return -1;
}

static void weld_insert(lossantos_tilebuild *t, const int64_t k[8], uint32_t vert) {
	uint64_t h = lossantos_hash_bytes(k, 64);
	uint32_t j = (uint32_t)h & WMASK;
	while (t->wgen[j] == t->gen) j = (j + 1) & WMASK;
	t->wgen[j] = t->gen;
	t->whash[j] = h;
	t->wvert[j] = vert;
}

// 0   pos f32 x3
// 12  uv f32 x2
// 20  uv u16 x2
// 24  normal s8 x3
// 27  pad
static void encode_vertex(uint8_t *o, const double *p, const double *n, const double *uv) {
	float f[5] = {(float)p[0], (float)p[1], (float)p[2], (float)uv[0], (float)uv[1]};
	memcpy(o, f, 20);
	for (int a = 0; a < 2; a++) {
		double q = uv[a] * 65535.0;
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

int lossantos_tb_tri(lossantos_tilebuild *t, const double *pos, const double *nrm, const double *uv) {
	if (!t->open && !open_block(t)) return 0;
	int64_t k[3][8];
	int64_t found[3];
	uint64_t h[3];
	int fresh = 0;
	for (int c = 0; c < 3; c++) {
		corner_key(pos + c * 3, nrm + c * 3, uv + c * 2, k[c]);
		found[c] = weld_find(t, k[c], &h[c]);
		if (found[c] < 0) {
			int dup = 0;
			for (int d = 0; d < c; d++)
				if (found[d] < 0 && memcmp(k[d], k[c], 64) == 0) dup = 1;
			if (!dup) fresh++;
		}
	}
	uint32_t limit = t->single_sided ? MAX_VERTS : MAX_VERTS / 2;
	if (t->block_verts + fresh > limit) {
		if (!lossantos_tb_close(t) || !open_block(t)) return 0;
	}
	if (!t->single_sided) {
		for (int a = 0; a < 9; a++) lossantos_vec_push(t->pending, pos[a]);
		for (int a = 0; a < 9; a++) lossantos_vec_push(t->pending, nrm[a]);
		for (int a = 0; a < 6; a++) lossantos_vec_push(t->pending, uv[a]);
	}
	return add_tri(t, pos, nrm, uv, 0);
}

static int add_tri(lossantos_tilebuild *t, const double *pos, const double *nrm, const double *uv, int mirror) {
	int64_t k[3][8];
	uint64_t h[3];
	for (int c = 0; c < 3; c++) corner_key(pos + c * 3, nrm + c * 3, uv + c * 2, k[c]);
	uint32_t local[3];
	for (int c = 0; c < 3; c++) {
		int64_t f = weld_find(t, k[c], &h[c]);   /* re-probe, earlier corner may have added it */
		if (f >= 0) {
			local[c] = (uint32_t)f;
			continue;
		}
		local[c] = t->block_verts;
		for (int a = 0; a < 8; a++)
			if (!lossantos_vec_push(t->wkeys, k[c][a])) return 0;
		weld_insert(t, k[c], t->block_verts);
		if (!lossantos_buf_reserve(&t->verts, t->verts.n + 28)) return 0;
		encode_vertex(t->verts.p + t->verts.n, pos + c * 3, nrm + c * 3, uv + c * 2);
		t->verts.n += 28;
		t->block_verts++;
	}
	// source front is clockwise, stored (0,2,1)
	uint32_t b = mirror ? local[1] : local[2], c2 = mirror ? local[2] : local[1];
	if (!lossantos_vec_push(t->idx, (uint16_t)local[0]) || !lossantos_vec_push(t->idx, (uint16_t)b) ||
		!lossantos_vec_push(t->idx, (uint16_t)c2))
		return 0;
	t->subs.p[t->subs.n - 1].icount += 3;
	return 1;
}

uint32_t lossantos_tb_vertices(const lossantos_tilebuild *t) { return (uint32_t)(t->verts.n / 28); }

int lossantos_tb_write(lossantos_tilebuild *t, const wchar_t *path) {
	lossantos_tb_close(t);
	if (!t->subs.n) return 0;
	FILE *f = _wfopen(path, L"wb");
	if (!f) {
		lossantos_set_error("cannot write %ls", path);
		return -1;
	}
	// 0   magic
	// 4   ver, flags
	// 8   vertex count
	// 12  index count
	// 16  submesh count
	// 20  material count
	// 24  stride
	// 28  reserved
	// 32  submeshes, 16 each
	//     material names, 64 each
	//     vertices, 28 each
	//     indices, u16
	struct {
		uint32_t magic;
		uint16_t ver, flags;
		uint32_t vc, ic, sc, mc, stride, res;
	} h = {TILE_MAGIC, 1, 3, (uint32_t)(t->verts.n / 28), (uint32_t)t->idx.n, (uint32_t)t->subs.n,
		   t->names.n, 28, 0};
	fwrite(&h, 32, 1, f);
	fwrite(t->subs.p, 16, t->subs.n, f);
	for (uint32_t i = 0; i < t->names.n; i++) {
		char nm[64] = {0};
		size_t l = strlen(t->names.s[i]);
		if (l > 63) {
			fclose(f);
			lossantos_set_error("material name does not fit GVT1 (%zu > 63): %s", l, t->names.s[i]);
			return -1;
		}
		memcpy(nm, t->names.s[i], l);
		fwrite(nm, 64, 1, f);
	}
	fwrite(t->verts.p, 1, t->verts.n, f);
	fwrite(t->idx.p, 2, t->idx.n, f);
	int bad = ferror(f);
	fclose(f);
	return bad ? -1 : 1;
}


typedef struct {
	double area;
	uint32_t i;
} area_ix;

static int cmp_area_desc(const void *a, const void *b) {
	const area_ix *x = (const area_ix *)a, *y = (const area_ix *)b;
	if (x->area != y->area) return x->area > y->area ? -1 : 1;
	return x->i < y->i ? -1 : x->i > y->i;
}
static int cmp_u32(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static int cmp_key3(void *ctx, const void *a, const void *b) {
	const int64_t *keys = (const int64_t *)ctx;
	const int64_t *x = keys + (size_t)*(const uint32_t *)a * 3, *y = keys + (size_t)*(const uint32_t *)b * 3;
	for (int k = 0; k < 3; k++)
		if (x[k] != y[k]) return x[k] < y[k] ? -1 : 1;
	return 0;
}

static uint64_t qkey_hash(const double *p) {
	int64_t k[3];
	for (int a = 0; a < 3; a++) k[a] = (int64_t)nearbyint(p[a] * 100.0);
	return lossantos_hash_bytes(k, sizeof k);
}

int lossantos_encode_owc1(const double *tri, size_t n, int cap_tris, size_t *dropped, lossantos_buf *out) {
	const size_t limit = MAX_VERTS / 3;
	uint32_t *sel = (uint32_t *)malloc((n ? n : 1) * sizeof *sel);
	if (!sel) return -1;
	size_t ns = n;
	for (size_t i = 0; i < n; i++) sel[i] = (uint32_t)i;
	*dropped = 0;
	if (!cap_tris && n > limit) {
		lossantos_hset hs;
		lossantos_hset_init(&hs, n * 3);
		for (size_t i = 0; i < n * 3; i++) lossantos_hset_add(&hs, qkey_hash(tri + i * 3));
		size_t nv = hs.n;
		lossantos_hset_free(&hs);
		if (nv <= MAX_VERTS) goto weld;
		area_ix *ai = (area_ix *)malloc(n * sizeof *ai);
		if (!ai) { free(sel); return -1; }
		for (size_t i = 0; i < n; i++) {
			const double *t = tri + i * 9;
			double ux = t[3] - t[0], uy = t[4] - t[1], uz = t[5] - t[2];
			double vx = t[6] - t[0], vy = t[7] - t[1], vz = t[8] - t[2];
			double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
			ai[i].area = sqrt(cx * cx + cy * cy + cz * cz) * 0.5;
			ai[i].i = (uint32_t)i;
		}
		qsort(ai, n, sizeof *ai, cmp_area_desc);
		lossantos_hset_init(&hs, MAX_VERTS);
		ns = 0;
		for (size_t i = 0; i < n; i++) {
			const double *t = tri + (size_t)ai[i].i * 9;
			uint64_t h[3] = {qkey_hash(t), qkey_hash(t + 3), qkey_hash(t + 6)};
			size_t fresh = 0;
			for (int c = 0; c < 3; c++)
				if (!lossantos_hset_has(&hs, h[c]) && (c < 1 || h[c] != h[0]) && (c < 2 || h[c] != h[1])) fresh++;
			if (hs.n + fresh > MAX_VERTS) continue;
			for (int c = 0; c < 3; c++) lossantos_hset_add(&hs, h[c]);
			sel[ns++] = ai[i].i;
		}
		lossantos_hset_free(&hs);
		free(ai);
		qsort(sel, ns, sizeof *sel, cmp_u32);
		*dropped = n - ns;
		goto weld;
	}
	// same area sort as above, copy paste
	if (n > limit) {
		area_ix *ai = (area_ix *)malloc(n * sizeof *ai);
		if (!ai) { free(sel); return -1; }
		for (size_t i = 0; i < n; i++) {
			double nn[3];
			const double *t = tri + i * 9;
			double ux = t[3] - t[0], uy = t[4] - t[1], uz = t[5] - t[2];
			double vx = t[6] - t[0], vy = t[7] - t[1], vz = t[8] - t[2];
			nn[0] = uy * vz - uz * vy;
			nn[1] = uz * vx - ux * vz;
			nn[2] = ux * vy - uy * vx;
			ai[i].area = sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]) * 0.5;
			ai[i].i = (uint32_t)i;
		}
		qsort(ai, n, sizeof *ai, cmp_area_desc);
		for (size_t i = 0; i < limit; i++) sel[i] = ai[i].i;
		ns = limit;
		qsort(sel, ns, sizeof *sel, cmp_u32);
		*dropped = n - limit;
		free(ai);
	}
weld:;
	size_t nc = ns * 3;
	int64_t *keys = (int64_t *)malloc(nc * 3 * sizeof *keys);
	uint32_t *ord = (uint32_t *)malloc(nc * sizeof *ord);
	uint32_t *inv = (uint32_t *)malloc(nc * sizeof *inv);
	float *verts = (float *)malloc(nc * 3 * sizeof *verts);
	uint16_t *idx = (uint16_t *)malloc(nc * sizeof *idx);
	if (!keys || !ord || !inv || !verts || !idx) {
		free(sel); free(keys); free(ord); free(inv); free(verts); free(idx);
		return -1;
	}
	for (size_t t = 0; t < ns; t++)
		for (int c = 0; c < 3; c++)
			for (int a = 0; a < 3; a++)
				keys[(t * 3 + c) * 3 + a] = (int64_t)nearbyint(tri[(size_t)sel[t] * 9 + c * 3 + a] * 100.0);
	for (size_t i = 0; i < nc; i++) ord[i] = (uint32_t)i;
	qsort_s(ord, nc, sizeof *ord, cmp_key3, keys);
	uint32_t nv = 0;
	for (size_t i = 0; i < nc; i++) {
		if (i == 0 || cmp_key3(keys, &ord[i - 1], &ord[i]) != 0) {
			for (int a = 0; a < 3; a++) verts[nv * 3 + a] = (float)((double)keys[(size_t)ord[i] * 3 + a] * 0.01);
			nv++;
		}
		inv[ord[i]] = nv - 1;
	}
	for (size_t t = 0; t < ns; t++) {
		idx[t * 3 + 0] = (uint16_t)inv[t * 3 + 0];
		idx[t * 3 + 1] = (uint16_t)inv[t * 3 + 2];
		idx[t * 3 + 2] = (uint16_t)inv[t * 3 + 1];
	}
	int rc = -1;
	size_t total = 20 + (size_t)nv * 12 + nc * 2;
	if (lossantos_buf_reserve(out, total)) {
		// 0   magic
		// 4   ver, flags
		// 8   tris
		// 12  verts
		// 16  reserved
		// 20  positions f32 x3
		//     indices u16
		struct {
			uint32_t magic;
			uint16_t ver, flags;
			uint32_t tris, verts, res;
		} h = {CHUNK_MAGIC, 1, 0, (uint32_t)ns, nv, 0};
		memcpy(out->p, &h, 20);
		memcpy(out->p + 20, verts, (size_t)nv * 12);
		memcpy(out->p + 20 + (size_t)nv * 12, idx, nc * 2);
		out->n = total;
		rc = (int)nv;
	}
	free(sel); free(keys); free(ord); free(inv); free(verts); free(idx);
	return rc;
}

int lossantos_write_owc1(const wchar_t *path, const double *tri, size_t n, int cap_tris, size_t *dropped) {
	lossantos_buf b = {0};
	int rc = lossantos_encode_owc1(tri, n, cap_tris, dropped, &b);
	if (rc >= 0) {
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
