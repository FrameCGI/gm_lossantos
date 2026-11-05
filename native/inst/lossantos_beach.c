#include "lossantos_beach.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CELL 256.0
#define UNITS_PER_M 39.37
#define MARGIN 250.0  // z gate
#define MARGIN_TIGHT 100.0  // z gate, ambiguous names

static const char *const SAND_ART[] = {
	"models/gtav/bh1_06/bh1_06_rsn_os_marblewall_0003", "models/gtav/vb_36/vb_36_beach1_pal",
	"models/gtav/vb_36/vb_36_beach7_pal", "models/gtav/vb_36/vb_36_beach370_pal",
	"models/gtav/vb_36/vb_36_beach371_pal", NULL};
// fill walls, sand only near a marker
static const char *const SAND_AMBIG[] = {"models/gtav/dt1_02/im_dt1_02_wall_01", "models/gtav/dt1_02/im_dt1_02_wall_02",
										 NULL};

static int in_list(const char *n, const char *const *l) {
	for (; *l; l++)
		if (strcmp(n, *l) == 0) return 1;
	return 0;
}

int lossantos_is_sand_ambig(const char *n) { return in_list(n, SAND_AMBIG); }

int lossantos_is_sand_source(const char *n) {
	if (in_list(n, SAND_ART) || in_list(n, SAND_AMBIG)) return 1;
	char low[128];
	lossantos_lower(low, n, sizeof low);
	size_t l = strlen(low);
	return strstr(low, "beach") && l >= 4 && strcmp(low + l - 4, "_pal") == 0;
}

int lossantos_is_marker_name(const char *n) {
	static const char *const W[] = {"beach", "sand", "waterstain", "seaweed", "uwrock", "erodedbank", "breaker", NULL};
	char low[128];
	lossantos_lower(low, n, sizeof low);
	for (int i = 0; W[i]; i++)
		if (strstr(low, W[i])) return 1;
	return strstr(low, "surf") && !strstr(low, "surface");
}

int lossantos_is_height_marker_name(const char *n) {
	static const char *const X[] = {"beachconcrete", "beach_concrete", "surfboard", NULL};
	if (!lossantos_is_marker_name(n)) return 0;
	char low[128];
	lossantos_lower(low, n, sizeof low);
	for (int i = 0; X[i]; i++)
		if (strstr(low, X[i])) return 0;
	return 1;
}

void lossantos_beach_free(lossantos_beach *b) {
	free(b->band);
	free(b->zmean);
	memset(b, 0, sizeof *b);
}

// window sum via prefix sums
static void box_sum(const double *a, int64_t h, int64_t w, int r, double *out) {
	double *p = (double *)calloc((size_t)(h + 1) * (w + 1), sizeof *p);
	if (!p) return;
#define P(y, x) p[(size_t)(y) * (w + 1) + (x)]
	for (int64_t y = 0; y < h; y++) {
		double row = 0;
		for (int64_t x = 0; x < w; x++) {
			row += a[(size_t)y * w + x];
			P(y + 1, x + 1) = P(y, x + 1) + row;
		}
	}
	for (int64_t y = 0; y < h; y++) {
		int64_t y0 = y - r < 0 ? 0 : y - r, y1 = y + r + 1 > h ? h : y + r + 1;
		for (int64_t x = 0; x < w; x++) {
			int64_t x0 = x - r < 0 ? 0 : x - r, x1 = x + r + 1 > w ? w : x + r + 1;
			out[(size_t)y * w + x] = P(y1, x1) - P(y0, x1) - P(y1, x0) + P(y0, x0);
		}
	}
#undef P
	free(p);
}

int lossantos_beach_build(lossantos_beach *b, int32_t tx0, int32_t tx1, int32_t ty0, int32_t ty1, const lossantos_marker *m,
					size_t n) {
	memset(b, 0, sizeof *b);
	b->cell = CELL;
	b->gx0 = (int64_t)floor(tx0 * 1024.0 / CELL);
	b->gy0 = (int64_t)floor(ty0 * 1024.0 / CELL);
	b->nx = (int64_t)ceil((tx1 + 1) * 1024.0 / CELL) - b->gx0;
	b->ny = (int64_t)ceil((ty1 + 1) * 1024.0 / CELL) - b->gy0;
	b->rc = (int)ceil(75.0 * UNITS_PER_M / CELL);
	b->rcz = (int)ceil(25.0 * UNITS_PER_M / CELL);
	size_t cells = (size_t)b->nx * b->ny;
	double *cnt = (double *)calloc(cells, sizeof *cnt), *hcnt = (double *)calloc(cells, sizeof *hcnt);
	double *zsum = (double *)calloc(cells, sizeof *zsum), *t1 = (double *)malloc(cells * sizeof *t1);
	double *t2 = (double *)malloc(cells * sizeof *t2), *t3 = (double *)malloc(cells * sizeof *t3);
	double *t4 = (double *)malloc(cells * sizeof *t4);
	b->band = (uint8_t *)malloc(cells);
	b->zmean = (double *)malloc(cells * sizeof *b->zmean);
	if (!cnt || !hcnt || !zsum || !t1 || !t2 || !t3 || !t4 || !b->band || !b->zmean) {
		free(cnt); free(hcnt); free(zsum); free(t1); free(t2); free(t3); free(t4);
		lossantos_beach_free(b);
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		const lossantos_marker *k = &m[i];
		int64_t cx0 = (int64_t)floor(k->x0 / CELL) - b->gx0, cx1 = (int64_t)floor(k->x1 / CELL) - b->gx0;
		int64_t cy0 = (int64_t)floor(k->y0 / CELL) - b->gy0, cy1 = (int64_t)floor(k->y1 / CELL) - b->gy0;
		if (!(cx1 >= 0 && cx0 < b->nx && cy1 >= 0 && cy0 < b->ny)) continue;
		cx0 = cx0 < 0 ? 0 : cx0 > b->nx - 1 ? b->nx - 1 : cx0;
		cx1 = cx1 < 0 ? 0 : cx1 > b->nx - 1 ? b->nx - 1 : cx1;
		cy0 = cy0 < 0 ? 0 : cy0 > b->ny - 1 ? b->ny - 1 : cy0;
		cy1 = cy1 < 0 ? 0 : cy1 > b->ny - 1 ? b->ny - 1 : cy1;
		int h = k->height && k->z > 0;
		int64_t dx = cx1 - cx0, dy = cy1 - cy0;
		// huge tris only seed corners + center
		if (dx > 7 || dy > 7) {
			static const double F[5][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0.5, 0.5}};
			for (int s = 0; s < 5; s++) {
				int64_t sx = (int64_t)(cx0 + F[s][0] * (double)(cx1 - cx0));
				int64_t sy = (int64_t)(cy0 + F[s][1] * (double)(cy1 - cy0));
				sx = sx < 0 ? 0 : sx > b->nx - 1 ? b->nx - 1 : sx;
				sy = sy < 0 ? 0 : sy > b->ny - 1 ? b->ny - 1 : sy;
				size_t q = (size_t)sy * b->nx + sx;
				cnt[q] += 1;
				if (h) {
					hcnt[q] += 1;
					zsum[q] += k->z;
				}
			}
			continue;
		}
		for (int64_t y = cy0; y <= cy1; y++)
			for (int64_t x = cx0; x <= cx1; x++) {
				size_t q = (size_t)y * b->nx + x;
				cnt[q] += 1;
				if (h) {
					hcnt[q] += 1;
					zsum[q] += k->z;
				}
			}
	}
	for (size_t q = 0; q < cells; q++) cnt[q] = cnt[q] > 0 ? 1.0 : 0.0;
	box_sum(cnt, b->ny, b->nx, b->rc, t1);
	for (size_t q = 0; q < cells; q++) b->band[q] = t1[q] > 0;
	box_sum(hcnt, b->ny, b->nx, b->rcz, t1);
	box_sum(zsum, b->ny, b->nx, b->rcz, t2);
	box_sum(hcnt, b->ny, b->nx, b->rc, t3);
	box_sum(zsum, b->ny, b->nx, b->rc, t4);
	for (size_t q = 0; q < cells; q++) {
		if (t1[q] > 0) b->zmean[q] = t2[q] / (t1[q] > 1 ? t1[q] : 1.0);
		else if (t3[q] > 0) b->zmean[q] = t4[q] / (t3[q] > 1 ? t3[q] : 1.0);
		else b->zmean[q] = NAN;
	}
	free(cnt); free(hcnt); free(zsum); free(t1); free(t2); free(t3); free(t4);
	return 1;
}

typedef struct {
	int64_t k[9];
} tkey;

static void tri_key(const double p[3][3], tkey *out) {
	double c[3][3];
	for (int i = 0; i < 3; i++)
		for (int a = 0; a < 3; a++) c[i][a] = nearbyint(p[i][a] * 100.0) / 100.0;
	int o[3] = {0, 1, 2};
	for (int i = 1; i < 3; i++)
		for (int j = i; j > 0; j--) {
			const double *x = c[o[j - 1]], *y = c[o[j]];
			int gt = x[0] != y[0] ? x[0] > y[0] : x[1] != y[1] ? x[1] > y[1] : x[2] > y[2];
			if (!gt) break;
			int t = o[j];
			o[j] = o[j - 1];
			o[j - 1] = t;
		}
	for (int i = 0; i < 3; i++)
		for (int a = 0; a < 3; a++) out->k[i * 3 + a] = (int64_t)c[o[i]][a];   /* astype(int64) truncates */
}

// kept stays in place, sand gets a new run
static int split_run(lossantos_tilemem *t, size_t ri, const uint8_t *sand, uint32_t sand_mat) {
	lossantos_trun r = t->runs.p[ri];
	uint32_t ntri = r.nidx / 3;
	uint32_t *map = (uint32_t *)malloc((size_t)r.nverts * 2 * sizeof *map);
	uint8_t *vk = (uint8_t *)malloc((size_t)r.nverts * 28), *vs = (uint8_t *)malloc((size_t)r.nverts * 28);
	uint16_t *ik = (uint16_t *)malloc((size_t)r.nidx * 2), *is = (uint16_t *)malloc((size_t)r.nidx * 2);
	if (!map || !vk || !vs || !ik || !is) {
		free(map); free(vk); free(vs); free(ik); free(is);
		return 0;
	}
	for (uint32_t v = 0; v < r.nverts * 2; v++) map[v] = UINT32_MAX;
	uint32_t nk = 0, ns = 0, ik_n = 0, is_n = 0;
	for (uint32_t tr = 0; tr < ntri; tr++) {
		int s = sand[tr];
		for (int c = 0; c < 3; c++) {
			uint32_t v = r.ix[tr * 3 + c];
			uint32_t *slot = &map[v * 2 + s];
			if (*slot == UINT32_MAX) {
				*slot = s ? ns : nk;
				memcpy((s ? vs : vk) + (size_t)*slot * 28, r.v + (size_t)v * 28, 28);
				if (s) ns++;
				else nk++;
			}
			if (s) is[is_n++] = (uint16_t)*slot;
			else ik[ik_n++] = (uint16_t)*slot;
		}
	}
	int ok = 1;
	if (is_n) ok = lossantos_tilemem_add(t, sand_mat, vs, ns, is, is_n);
	lossantos_trun *dst = &t->runs.p[ri];     /* add may have moved the array */
	free(dst->v);
	free(dst->ix);
	dst->v = vk;
	dst->ix = ik;
	dst->nverts = nk;
	dst->nidx = ik_n;
	free(map); free(vs); free(is);
	return ok;
}

size_t lossantos_beach_apply(const lossantos_beach *b, lossantos_tilemem *t, const char *const *names, uint32_t sand_mat) {
	if (!b->band) return 0;
	size_t nr0 = t->runs.n;
	size_t ncand = 0;
	for (size_t ri = 0; ri < nr0; ri++)
		if (lossantos_is_sand_source(names[t->runs.p[ri].mat])) ncand += t->runs.p[ri].nidx / 3;
	if (!ncand) return 0;
	uint8_t *sand = (uint8_t *)calloc(ncand, 1);
	tkey *keys = (tkey *)malloc(ncand * sizeof *keys);
	if (!sand || !keys) { free(sand); free(keys); return 0; }
	size_t at = 0, nsand = 0;
	for (size_t ri = 0; ri < nr0; ri++) {
		const lossantos_trun *r = &t->runs.p[ri];
		const char *nm = names[r->mat];
		if (!lossantos_is_sand_source(nm)) continue;
		double margin = lossantos_is_sand_ambig(nm) ? MARGIN_TIGHT : MARGIN;
		for (uint32_t tr = 0; tr < r->nidx / 3; tr++, at++) {
			double p[3][3], nz = 0, cen[3] = {0, 0, 0};
			for (int c = 0; c < 3; c++) {
				const uint8_t *v = r->v + (size_t)r->ix[tr * 3 + c] * 28;
				lossantos_vtx_pos(v, p[c]);
				nz += (double)(int8_t)v[26] / 127.0;
				for (int a = 0; a < 3; a++) cen[a] += p[c][a];
			}
			for (int a = 0; a < 3; a++) cen[a] /= 3.0;
			tri_key((const double(*)[3])p, &keys[at]);
			// facing up, in band, near the marker z
			if (!(nz / 3.0 > 0.5)) continue;
			int64_t cx = (int64_t)floor(cen[0] / b->cell) - b->gx0, cy = (int64_t)floor(cen[1] / b->cell) - b->gy0;
			if (cx < 0 || cy < 0 || cx >= b->nx || cy >= b->ny) continue;
			size_t q = (size_t)cy * b->nx + cx;
			if (!b->band[q]) continue;
			double zm = b->zmean[q];
			if (!(fabs(cen[2] - zm) <= margin)) continue;
			sand[at] = 1;
			nsand++;
		}
	}
	if (nsand) {
		// twins, same rounded corners as a sanded tri
		lossantos_hset hit;
		lossantos_hset_init(&hit, nsand);
		for (size_t i = 0; i < ncand; i++)
			if (sand[i]) lossantos_hset_add(&hit, lossantos_hash_bytes(&keys[i], sizeof(tkey)));
		nsand = 0;
		for (size_t i = 0; i < ncand; i++) {
			if (!sand[i] && lossantos_hset_has(&hit, lossantos_hash_bytes(&keys[i], sizeof(tkey)))) sand[i] = 1;
			nsand += sand[i];
		}
		lossantos_hset_free(&hit);
		at = 0;
		for (size_t ri = 0; ri < nr0; ri++) {
			if (!lossantos_is_sand_source(names[t->runs.p[ri].mat])) continue;
			uint32_t nt = t->runs.p[ri].nidx / 3;
			int any = 0;
			for (uint32_t k = 0; k < nt; k++) any |= sand[at + k];
			if (any) split_run(t, ri, sand + at, sand_mat);
			at += nt;
		}
	}
	free(sand);
	free(keys);
	return nsand;
}
