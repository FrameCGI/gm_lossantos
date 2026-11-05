#include "lossantos_geo.h"
#include "lossantos_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *const LOSSANTOS_PALETTE_NAMES[LOSSANTOS_CAT_COUNT] = {
	"models/gtav/bh1_17/im_road_027b",       "models/gtav/bh1_17/dc_sidewalk004",  // road, sidewalk
	"models/gtav/dt1_02/im_dt1_02_windows_01", "models/gtav/bh1_06/im_grass11b_fur",  // windows, grass
	"models/gtav/dt1_02/im_dt1_02_wall_02",  "models/gtav/dt1_02/im_dt1_02_wall_01",  // metal, wall
	"models/gtav/bh1_17/dc_sidewalk004",     "models/gtav/dt1_02/im_roof_underside003",  // ground, ceiling
	"models/gtav/ch3_02/mw_median_conc_01",  // median
};
const double LOSSANTOS_PALETTE_METRES[LOSSANTOS_CAT_COUNT] = {4.0, 2.5, 6.0, 3.0, 4.0, 6.0, 2.5, 8.0, 2.5};


void lossantos_coll_init(lossantos_ybn *y) {
	memset(y, 0, sizeof *y);
	for (int k = 0; k < 3; k++) {
		y->bmin[k] = DBL_MAX;
		y->bmax[k] = -DBL_MAX;
	}
}

int lossantos_coll_push(lossantos_ybn *y, const double *a, const double *b, const double *c, uint8_t mat) {
	if (y->ntris == y->cap) {
		size_t nc = y->cap ? y->cap * 2 : 1024;
		double *t = (double *)realloc(y->tri, nc * 9 * sizeof(double));
		if (!t) return 0;
		y->tri = t;
		uint8_t *m = (uint8_t *)realloc(y->mat, nc);
		if (!m) return 0;
		y->mat = m;
		y->cap = nc;
	}
	double *d = y->tri + y->ntris * 9;
	memcpy(d, a, 24);
	memcpy(d + 3, b, 24);
	memcpy(d + 6, c, 24);
	y->mat[y->ntris++] = mat;
	return 1;
}

int lossantos_coll_append(lossantos_ybn *dst, const lossantos_ybn *src, const uint8_t *keep) {
	for (size_t i = 0; i < src->ntris; i++) {
		if (keep && !keep[i]) continue;
		const double *t = src->tri + i * 9;
		if (!lossantos_coll_push(dst, t, t + 3, t + 6, src->mat[i])) return 0;
	}
	for (int k = 0; k < 3; k++) {
		if (src->bmin[k] < dst->bmin[k]) dst->bmin[k] = src->bmin[k];
		if (src->bmax[k] > dst->bmax[k]) dst->bmax[k] = src->bmax[k];
	}
	return 1;
}

int lossantos_merge_hi(const lossantos_ybn *base, const lossantos_ybn *hi, lossantos_ybn *out) {
	lossantos_coll_init(out);
	if (!base || !base->ntris) return hi ? lossantos_coll_append(out, hi, NULL) : 1;
	if (!hi || !hi->ntris) return lossantos_coll_append(out, base, NULL);
	// hi footprint on a grid, base tris under it get dropped
	const double margin = 2.0;
	double lo[2] = {hi->bmin[0] - margin, hi->bmin[1] - margin};
	double up[2] = {hi->bmax[0] + margin, hi->bmax[1] + margin};
	double span[2] = {up[0] - lo[0], up[1] - lo[1]};
	if (!isfinite(span[0]) || !isfinite(span[1]) || span[0] <= 0 || span[1] <= 0)
		return lossantos_coll_append(out, base, NULL);
	double step = 1.0;
	double smax = span[0] > span[1] ? span[0] : span[1];
	if (smax / step > 2048.0) step = smax / 2048.0;
	long nx = (long)ceil(span[0] / step) + 1, ny = (long)ceil(span[1] / step) + 1;
	int32_t *diff = (int32_t *)calloc((size_t)(nx + 1) * (ny + 1), sizeof *diff);
	uint8_t *occ = (uint8_t *)calloc((size_t)nx * ny, 1);
	uint8_t *grown = (uint8_t *)calloc((size_t)nx * ny, 1);
	uint8_t *keep = (uint8_t *)malloc(base->ntris);
	if (!diff || !occ || !grown || !keep) {
		free(diff); free(occ); free(grown); free(keep);
		return 0;
	}
#define D(i, j) diff[(size_t)(i) * (ny + 1) + (j)]
	for (size_t t = 0; t < hi->ntris; t++) {
		const double *v = hi->tri + t * 9;
		double mnx = v[0], mxx = v[0], mny = v[1], mxy = v[1];
		for (int c = 1; c < 3; c++) {
			if (v[c * 3] < mnx) mnx = v[c * 3];
			if (v[c * 3] > mxx) mxx = v[c * 3];
			if (v[c * 3 + 1] < mny) mny = v[c * 3 + 1];
			if (v[c * 3 + 1] > mxy) mxy = v[c * 3 + 1];
		}
		long a0 = (long)floor((mnx - lo[0]) / step), a1 = (long)floor((mny - lo[1]) / step);
		long b0 = (long)floor((mxx - lo[0]) / step), b1 = (long)floor((mxy - lo[1]) / step);
		a0 = a0 < 0 ? 0 : a0 >= nx ? nx - 1 : a0;
		b0 = b0 < 0 ? 0 : b0 >= nx ? nx - 1 : b0;
		a1 = a1 < 0 ? 0 : a1 >= ny ? ny - 1 : a1;
		b1 = b1 < 0 ? 0 : b1 >= ny ? ny - 1 : b1;
		D(a0, a1) += 1;
		D(b0 + 1, a1) -= 1;
		D(a0, b1 + 1) -= 1;
		D(b0 + 1, b1 + 1) += 1;
	}
	for (long i = 1; i <= nx; i++)
		for (long j = 0; j <= ny; j++) D(i, j) += D(i - 1, j);
	for (long i = 0; i <= nx; i++)
		for (long j = 1; j <= ny; j++) D(i, j) += D(i, j - 1);
	for (long i = 0; i < nx; i++)
		for (long j = 0; j < ny; j++) occ[(size_t)i * ny + j] = D(i, j) > 0;
#undef D
	for (long i = 0; i < nx; i++)
		for (long j = 0; j < ny; j++) {
			size_t q = (size_t)i * ny + j;
			uint8_t g = occ[q];
			if (i + 1 < nx) g |= occ[q + ny];
			if (i > 0) g |= occ[q - ny];
			if (j + 1 < ny) g |= occ[q + 1];
			if (j > 0) g |= occ[q - 1];
			grown[q] = g;
		}
	double w[45][3];
	int nw = 0;
	for (int i = 0; i < 9; i++)
		for (int j = 0; j < 9 - i; j++) {
			double r0 = 1.0 - (double)(i + j) / 9.0, r1 = (double)i / 9.0, r2 = (double)j / 9.0;
			double s = r0 + r1 + r2;
			w[nw][0] = r0 / s;
			w[nw][1] = r1 / s;
			w[nw][2] = r2 / s;
			nw++;
		}
	for (size_t t = 0; t < base->ntris; t++) {
		const double *v = base->tri + t * 9;
		double cx = (v[0] + v[3] + v[6]) / 3.0, cy = (v[1] + v[4] + v[7]) / 3.0;
		int inside = cx >= lo[0] && cx <= up[0] && cy >= lo[1] && cy <= up[1];
		keep[t] = 1;
		if (!inside) continue;
		int hits = 0;
		for (int s = 0; s < nw; s++) {
			double px = v[0] * w[s][0] + v[3] * w[s][1] + v[6] * w[s][2];
			double py = v[1] * w[s][0] + v[4] * w[s][1] + v[7] * w[s][2];
			double fx = floor((px - lo[0]) / step), fy = floor((py - lo[1]) / step);
			if (fx < 0 || fy < 0 || fx >= nx || fy >= ny) continue;
			hits += grown[(size_t)fx * ny + (size_t)fy];
		}
		if ((double)hits / (double)nw >= 0.9) keep[t] = 0;  // 90% of samples under hi
	}
	int ok = lossantos_coll_append(out, hi, NULL) && lossantos_coll_append(out, base, keep);
	for (int k = 0; k < 3; k++) {
		out->bmin[k] = base->bmin[k] < hi->bmin[k] ? base->bmin[k] : hi->bmin[k];
		out->bmax[k] = base->bmax[k] > hi->bmax[k] ? base->bmax[k] : hi->bmax[k];
	}
	free(diff); free(occ); free(grown); free(keep);
	return ok;
}


double lossantos_tri_normal(const double *t, double n[3]) {
	double ux = t[3] - t[0], uy = t[4] - t[1], uz = t[5] - t[2];
	double vx = t[6] - t[0], vy = t[7] - t[1], vz = t[8] - t[2];
	n[0] = uy * vz - uz * vy;
	n[1] = uz * vx - ux * vz;
	n[2] = ux * vy - uy * vx;
	double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len > 1e-12) {
		n[0] /= len;
		n[1] /= len;
		n[2] /= len;
	} else {
		n[0] = n[1] = 0.0;
		n[2] = 1.0;
	}
	return len * 0.5;
}

size_t lossantos_dedupe_tris(const double *tri, size_t n, const lossantos_compat *cx, uint8_t *keep) {
	int64_t *keys = (int64_t *)malloc(n * 9 * sizeof *keys);
	lossantos_hmap m;
	if (!keys || !lossantos_hmap_init(&m, n)) {
		free(keys);
		memset(keep, 1, n);
		return n;
	}
	size_t kept = 0;
	for (size_t i = 0; i < n; i++) {
		int64_t k[3][3];
		for (int c = 0; c < 3; c++)
			for (int a = 0; a < 3; a++) k[c][a] = (int64_t)nearbyint(tri[i * 9 + c * 3 + a] / 0.1);
		int ord[3] = {0, 1, 2};
		for (int a = 1; a < 3; a++) {
			int v = ord[a], b = a;
			while (b > 0) {
				int u = ord[b - 1];
				int gt;
				if (cx->dedupe_x_only) gt = k[u][0] > k[v][0];
				else gt = k[u][0] != k[v][0] ? k[u][0] > k[v][0]
						: k[u][1] != k[v][1] ? k[u][1] > k[v][1] : k[u][2] > k[v][2];
				if (!gt) break;
				ord[b] = u;
				b--;
			}
			ord[b] = v;
		}
		int64_t *row = keys + i * 9;
		for (int c = 0; c < 3; c++)
			for (int a = 0; a < 3; a++) row[c * 3 + a] = k[ord[c]][a];
		uint64_t h = lossantos_hash_bytes(row, 72);
		for (;;) {
			int found;
			uint32_t *v = lossantos_hmap_put(&m, h, &found);
			if (!found) {
				*v = (uint32_t)i;
				keep[i] = 1;
				kept++;
				break;
			}
			if (memcmp(keys + (size_t)*v * 9, row, 72) == 0) {
				keep[i] = 0;
				break;
			}
			h = lossantos_mix64(h + 1);
		}
	}
	lossantos_hmap_free(&m);
	free(keys);
	return kept;
}


static int is_in(uint8_t m, const uint8_t *set, int n) {
	for (int i = 0; i < n; i++)
		if (set[i] == m) return 1;
	return 0;
}
// ybn material types
static const uint8_t ROAD_M[] = {4};
static const uint8_t SIDEWALK_M[] = {15, 68, 181};
static const uint8_t WINDOW_M[] = {57, 59};
static const uint8_t GRASS_M[] = {174};
static const uint8_t METAL_M[] = {14, 50, 64, 66, 96, 108, 113};

// sidewalk with road on both sides = median
static int median_strips(const double *tri, const uint8_t *mat, size_t n, const lossantos_compat *cx, uint8_t *out) {
	memset(out, 0, n);
	int anyp = 0, anyt = 0;
	for (size_t i = 0; i < n; i++) {
		anyp |= is_in(mat[i], SIDEWALK_M, 3);
		anyt |= is_in(mat[i], ROAD_M, 1);
	}
	if (!anyp || !anyt) return 1;
	double lox = DBL_MAX, loy = DBL_MAX, hix = -DBL_MAX, hiy = -DBL_MAX;
	for (size_t i = 0; i < n; i++)
		for (int c = 0; c < 3; c++) {
			double x = tri[i * 9 + c * 3], y = tri[i * 9 + c * 3 + 1];
			if (x < lox) lox = x;
			if (x > hix) hix = x;
			if (y < loy) loy = y;
			if (y > hiy) hiy = y;
		}
	const double scale = LOSSANTOS_SCALE;
	double area = (hix - lox) * (hiy - loy);
	double cell;
	if (cx->median_mixed_units) {
		double r = sqrt(area / 4000000.0);
		cell = (r > 1.0 ? r : 1.0) * scale;
	} else {
		double r = sqrt(area / (scale * scale) / 4000000.0);   /* metres */
		cell = (r > 1.0 ? r : 1.0) * scale;
	}
	long steps = (long)nearbyint(3.0 * scale / cell);   /* python round(): half-even */
	if (steps < 1) steps = 1;
	double ox = lox - cell, oy = loy - cell;
	long nx = (long)((hix - ox) / cell) + 2, ny = (long)((hiy - oy) / cell) + 2;
	size_t cells = (size_t)nx * ny;
	int32_t *diff = (int32_t *)calloc((size_t)(nx + 1) * (ny + 1), sizeof *diff);
	uint8_t *cov = (uint8_t *)calloc(cells, 1), *ok = (uint8_t *)calloc(cells, 1);
	uint8_t *pos = (uint8_t *)calloc(cells, 1), *neg = (uint8_t *)calloc(cells, 1);
	if (!diff || !cov || !ok || !pos || !neg) {
		free(diff); free(cov); free(ok); free(pos); free(neg);
		return 0;
	}
#define DI(i, j) diff[(size_t)(i) * (nx + 1) + (j)]
	for (size_t t = 0; t < n; t++) {
		if (!is_in(mat[t], ROAD_M, 1)) continue;
		const double *v = tri + t * 9;
		double mnx = v[0], mxx = v[0], mny = v[1], mxy = v[1];
		for (int c = 1; c < 3; c++) {
			if (v[c * 3] < mnx) mnx = v[c * 3];
			if (v[c * 3] > mxx) mxx = v[c * 3];
			if (v[c * 3 + 1] < mny) mny = v[c * 3 + 1];
			if (v[c * 3 + 1] > mxy) mxy = v[c * 3 + 1];
		}
		long j0 = (long)floor((mnx - ox) / cell), j1 = (long)floor((mxx - ox) / cell);
		long i0 = (long)floor((mny - oy) / cell), i1 = (long)floor((mxy - oy) / cell);
		if (!(j1 >= 0 && i1 >= 0 && j0 < nx && i0 < ny)) continue;
		j0 = j0 < 0 ? 0 : j0 > nx - 1 ? nx - 1 : j0;
		j1 = j1 < 0 ? 0 : j1 > nx - 1 ? nx - 1 : j1;
		i0 = i0 < 0 ? 0 : i0 > ny - 1 ? ny - 1 : i0;
		i1 = i1 < 0 ? 0 : i1 > ny - 1 ? ny - 1 : i1;
		DI(i0, j0) += 1;
		DI(i1 + 1, j0) -= 1;
		DI(i0, j1 + 1) -= 1;
		DI(i1 + 1, j1 + 1) += 1;
	}
	for (long i = 1; i <= ny; i++)
		for (long j = 0; j <= nx; j++) DI(i, j) += DI(i - 1, j);
	for (long i = 0; i <= ny; i++)
		for (long j = 1; j <= nx; j++) DI(i, j) += DI(i, j - 1);
	for (long i = 0; i < ny; i++)
		for (long j = 0; j < nx; j++) cov[(size_t)i * nx + j] = DI(i, j) > 0;
#undef DI
	static const int DIRS[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
	for (int d = 0; d < 4; d++) {
		for (int sgn = 0; sgn < 2; sgn++) {
			int di = sgn ? -DIRS[d][0] : DIRS[d][0], dj = sgn ? -DIRS[d][1] : DIRS[d][1];
			uint8_t *acc = sgn ? neg : pos;
			memset(acc, 0, cells);
			for (long i = 0; i < ny; i++)
				for (long j = 0; j < nx; j++) {
					uint8_t a = 0;
					for (long s = 1; s <= steps && !a; s++) {
						long si = i + di * s, sj = j + dj * s;
						if (si >= 0 && si < ny && sj >= 0 && sj < nx) a = cov[(size_t)si * nx + sj];
					}
					acc[(size_t)i * nx + j] = a;
				}
		}
		for (size_t q = 0; q < cells; q++) ok[q] |= pos[q] & neg[q];
	}
	for (size_t t = 0; t < n; t++) {
		if (!is_in(mat[t], SIDEWALK_M, 3)) continue;
		const double *v = tri + t * 9;
		double cxv = (v[0] + v[3] + v[6]) / 3.0, cyv = (v[1] + v[4] + v[7]) / 3.0;
		long ci = (long)((cyv - oy) / cell), cj = (long)((cxv - ox) / cell);
		ci = ci < 0 ? 0 : ci > ny - 1 ? ny - 1 : ci;
		cj = cj < 0 ? 0 : cj > nx - 1 ? nx - 1 : cj;
		out[t] = ok[(size_t)ci * nx + cj];
	}
	free(diff); free(cov); free(ok); free(pos); free(neg);
	return 1;
}

int lossantos_categories(const double *tri, const double *nrm, const uint8_t *mat, size_t n,
				   const lossantos_compat *cx, uint8_t *cat) {
	for (size_t i = 0; i < n; i++) {
		double nz = nrm[i * 3 + 2];
		int up = nz > 0.7, down = nz < -0.7, vert = fabs(nz) < 0.4;
		// last match wins
		uint8_t c = LOSSANTOS_CAT_GROUND;
		if (vert) c = LOSSANTOS_CAT_WALL;
		if (down) c = LOSSANTOS_CAT_CEILING;
		uint8_t m = mat[i];
		if (is_in(m, WINDOW_M, 2)) c = LOSSANTOS_CAT_WINDOWS;
		if (is_in(m, METAL_M, 7) && vert) c = LOSSANTOS_CAT_METAL;
		if (is_in(m, ROAD_M, 1) && up) c = LOSSANTOS_CAT_ROAD;
		if (is_in(m, SIDEWALK_M, 3)) c = LOSSANTOS_CAT_SIDEWALK;
		if (is_in(m, GRASS_M, 1)) c = LOSSANTOS_CAT_GRASS;
		cat[i] = c;
	}
	uint8_t *med = (uint8_t *)malloc(n ? n : 1);
	if (!med || !median_strips(tri, mat, n, cx, med)) {
		free(med);
		return 0;
	}
	for (size_t i = 0; i < n; i++)
		if (med[i]) cat[i] = LOSSANTOS_CAT_MEDIAN;
	free(med);
	return 1;
}

void lossantos_planar_uv(const double *t, const double *n, double repeat, double *uv) {
	double ax = fabs(n[0]), ay = fabs(n[1]), az = fabs(n[2]);
	int pick = 0;
	if (ay > ax) pick = 1;
	if (az > (pick ? ay : ax)) pick = 2;
	for (int c = 0; c < 3; c++) {
		double x = t[c * 3], y = t[c * 3 + 1], z = t[c * 3 + 2];
		uv[c * 2] = (pick == 0 ? y : x) / repeat;
		uv[c * 2 + 1] = (pick == 2 ? y : z) / repeat;
	}
}


typedef LOSSANTOS_VEC(uint64_t) u64vec;

static double max_edge(const double *t) {
	double m = 0;
	for (int e = 0; e < 3; e++) {
		const double *a = t + e * 3, *b = t + ((e + 2) % 3) * 3;  /* t - roll(t, 1) */
		double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
		double l = sqrt(dx * dx + dy * dy + dz * dz);
		if (l > m) m = l;
	}
	return m;
}

// 21 bits per axis
static inline uint64_t pack_shell(int64_t x, int64_t y, int64_t z) {
	x += 1 << 20;
	y += 1 << 20;
	z += 1 << 20;
	return ((uint64_t)x << 42) | ((uint64_t)y << 21) | (uint64_t)z;
}

// centroid + 3 points 2/3 of the way to the corners
static void samples4(const double *t, double s[4][3]) {
	double c[3];
	for (int a = 0; a < 3; a++) c[a] = (t[a] + t[3 + a] + t[6 + a]) / 3.0;
	for (int a = 0; a < 3; a++) s[0][a] = c[a];
	for (int i = 0; i < 3; i++)
		for (int a = 0; a < 3; a++) s[i + 1][a] = c[a] + (t[i * 3 + a] - c[a]) * (4.0 / 6.0);
}

int lossantos_shell_covered(const double *coll, size_t ncoll, const double *art, size_t nart, uint8_t *covered) {
	memset(covered, 0, ncoll);
	if (!nart || !ncoll) return 1;
	const double cell = 1.5 * LOSSANTOS_SCALE;  // 1.5m voxels
	u64vec keys = {0};
	for (size_t t = 0; t < nart; t++) {
		const double *v = art + t * 9;
		double sub = ceil(max_edge(v) / cell);
		int n = sub < 1 ? 1 : sub > 48 ? 48 : (int)sub;
		for (int i = 0; i <= n; i++)
			for (int j = 0; j <= n - i; j++) {
				double a = (double)i / n, c = (double)j / n, w0 = 1.0 - a - c;
				int64_t q[3];
				for (int k = 0; k < 3; k++) q[k] = (int64_t)floor((v[k] * w0 + v[3 + k] * a + v[6 + k] * c) / cell);
				if (!lossantos_vec_push(keys, pack_shell(q[0], q[1], q[2]))) { lossantos_vec_free(keys); return 0; }
			}
	}
	uint64_t *tmp = (uint64_t *)malloc(keys.n * sizeof *tmp);
	if (!tmp) { lossantos_vec_free(keys); return 0; }
	size_t nu = lossantos_sort_unique_u64(keys.p, keys.n, tmp);
	free(tmp);
	for (size_t t = 0; t < ncoll; t++) {
		double s[4][3];
		samples4(coll + t * 9, s);
		int hits = 0;
		for (int k = 0; k < 4; k++) {
			int64_t b[3];
			for (int a = 0; a < 3; a++) b[a] = (int64_t)floor(s[k][a] / cell);
			int hit = 0;
			for (int dx = -1; dx <= 1 && !hit; dx++)
				for (int dy = -1; dy <= 1 && !hit; dy++)
					for (int dz = -1; dz <= 1 && !hit; dz++) {
						uint64_t q = pack_shell(b[0] + dx, b[1] + dy, b[2] + dz);
						size_t p = lossantos_lower_bound_u64(keys.p, nu, q);
						if (p < nu && keys.p[p] == q) hit = 1;
					}
			hits += hit;
		}
		covered[t] = hits >= 2;  // 2 of 4 samples
	}
	lossantos_vec_free(keys);
	return 1;
}

#define NQ 8  // normal steps, -8..8 per axis
#define NQW 17  // values per axis
#define NQ_BITS 13  // 17^3 codes fit

static double g_dirs[NQW * NQW * NQW][3];
static INIT_ONCE g_dirs_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_dirs(PINIT_ONCE once, PVOID param, PVOID *ctx) {
	(void)once; (void)param; (void)ctx;
	for (int q = 0; q < NQW * NQW * NQW; q++) {
		double d[3] = {(double)(q / (NQW * NQW)) - NQ, (double)((q / NQW) % NQW) - NQ, (double)(q % NQW) - NQ};
		double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (l == 0) l = 1.0;
		for (int a = 0; a < 3; a++) g_dirs[q][a] = d[a] / l;
	}
	return TRUE;
}

int lossantos_filler_covered(const double *coll, size_t ncoll, const double *art, size_t nart, uint8_t *covered) {
	memset(covered, 0, ncoll);
	if (!ncoll || !nart) return 1;
	const double scale = LOSSANTOS_SCALE, cell = 0.5 * scale, step = 0.25 * scale;
	// good art + its normal codes
	uint8_t *good = (uint8_t *)malloc(nart);
	uint16_t *code = (uint16_t *)malloc(nart * sizeof *code);
	if (!good || !code) { free(good); free(code); return 0; }
	size_t ngood = 0;
	double mn[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, mx[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
	for (size_t t = 0; t < nart; t++) {
		double nn[3];
		double area = lossantos_tri_normal(art + t * 9, nn);
		good[t] = area > 1e-9;
		if (!good[t]) continue;
		ngood++;
		int q[3];
		for (int a = 0; a < 3; a++) q[a] = (int)nearbyint(nn[a] * NQ) + NQ;
		code[t] = (uint16_t)((q[0] * NQW + q[1]) * NQW + q[2]);
		for (int c = 0; c < 9; c++) {
			double v = art[t * 9 + c];
			if (v < mn[c % 3]) mn[c % 3] = v;
			if (v > mx[c % 3]) mx[c % 3] = v;
		}
	}
	if (!ngood) { free(good); free(code); return 1; }
	for (size_t c = 0; c < ncoll * 9; c++) {
		double v = coll[c];
		if (v < mn[c % 3]) mn[c % 3] = v;
		if (v > mx[c % 3]) mx[c % 3] = v;
	}
	int64_t lo[3], hi[3], span = 0;
	for (int a = 0; a < 3; a++) {
		lo[a] = (int64_t)floor(mn[a] / cell) - 2;
		hi[a] = (int64_t)floor(mx[a] / cell) + 2;
		if (hi[a] - lo[a] > span) span = hi[a] - lo[a];
	}
	int bits = 1;
	while ((span >> bits) != 0) bits++;     /* int.bit_length(), min 1 */
	// too big for the key, give up
	if (3 * bits + NQ_BITS > 62) { free(good); free(code); return 1; }
// voxel xyz then normal code
#define PACK(x, y, z) (((((uint64_t)(x) << bits) | (uint64_t)(y)) << bits | (uint64_t)(z)) << NQ_BITS)
	u64vec keys = {0};
	for (size_t t = 0; t < nart; t++) {
		if (!good[t]) continue;
		const double *v = art + t * 9;
		double sub = ceil(max_edge(v) / step);
		int n = sub < 1 ? 1 : sub > 256 ? 256 : (int)sub;
		size_t start = keys.n;
		for (int i = 0; i <= n; i++)
			for (int j = 0; j <= n - i; j++) {
				double a = (double)i / n, c = (double)j / n, w0 = 1.0 - a - c;
				int64_t q[3];
				for (int k = 0; k < 3; k++)
					q[k] = (int64_t)floor((v[k] * w0 + v[3 + k] * a + v[6 + k] * c) / cell) - lo[k];
				uint64_t key = PACK(q[0], q[1], q[2]) | code[t];
				if (keys.n > start && keys.p[keys.n - 1] == key) continue;
				if (!lossantos_vec_push(keys, key)) { lossantos_vec_free(keys); free(good); free(code); return 0; }
			}
	}
	uint64_t *tmp = (uint64_t *)malloc((keys.n ? keys.n : 1) * sizeof *tmp);
	if (!tmp) { lossantos_vec_free(keys); free(good); free(code); return 0; }
	size_t nu = lossantos_sort_unique_u64(keys.p, keys.n, tmp);
	free(tmp);
	InitOnceExecuteOnce(&g_dirs_once, init_dirs, NULL, NULL);
	const double (*dirs)[3] = g_dirs;
	const double cos_max = cos(25.0 * 3.14159265358979323846 / 180.0);
	const uint64_t cmask = (1u << NQ_BITS) - 1;
	for (size_t t = 0; t < ncoll; t++) {
		const double *v = coll + t * 9;
		double cn[3];
		lossantos_tri_normal(v, cn);
		double s[4][3];
		samples4(v, s);
		int hits = 0;
		for (int k = 0; k < 4; k++) {
			int64_t b[3];
			for (int a = 0; a < 3; a++) b[a] = (int64_t)floor(s[k][a] / cell) - lo[a];
			int hit = 0;
			for (int dx = -1; dx <= 1 && !hit; dx++)
				for (int dy = -1; dy <= 1 && !hit; dy++)
					for (int dz = -1; dz <= 1 && !hit; dz++) {
						int64_t x = b[0] + dx, y = b[1] + dy, z = b[2] + dz;
						if (x < 0 || y < 0 || z < 0) continue;
						uint64_t first = PACK(x, y, z);
						size_t p = lossantos_lower_bound_u64(keys.p, nu, first);
						for (; p < nu && keys.p[p] < first + (1u << NQ_BITS); p++) {
							const double *d = dirs[keys.p[p] & cmask];
							if (d[0] * cn[0] + d[1] * cn[1] + d[2] * cn[2] >= cos_max) {
								hit = 1;
								break;
							}
						}
					}
			hits += hit;
		}
		covered[t] = hits >= 2;
	}
#undef PACK
	lossantos_vec_free(keys);
	free(good);
	free(code);
	return 1;
}
