#include "lossantos_clip.h"

#include <math.h>
#include <string.h>

#define AREA_EPS 1e-9
#define MAXV 16

// flat corner: x y z u v nx ny nz
typedef double corner[8];

static void lerp(const double *a, const double *b, double t, double *o) {
	for (int k = 0; k < 8; k++) o[k] = a[k] + (b[k] - a[k]) * t;
	double ln = sqrt(o[5] * o[5] + o[6] * o[6] + o[7] * o[7]);
	if (ln <= 1e-9) {
		const double *c[2] = {a, b};
		for (int i = 0; i < 2; i++) {
			double l = sqrt(c[i][5] * c[i][5] + c[i][6] * c[i][6] + c[i][7] * c[i][7]);
			if (l > 1e-9) {
				o[5] = c[i][5] / l;
				o[6] = c[i][6] / l;
				o[7] = c[i][7] / l;
				return;
			}
		}
		return;
	}
	o[5] /= ln;
	o[6] /= ln;
	o[7] /= ln;
}

// one sutherland hodgman pass
static int clip_pass(corner *src, int n, corner *dst, int axis, double bound, int keep_lo) {
	int m = 0;
	for (int i = 0; i < n; i++) {
		const double *a = src[i], *b = src[(i + 1) % n];
		double da = a[axis] - bound, db = b[axis] - bound;
		int ina = keep_lo ? da >= 0 : da <= 0;
		int inb = keep_lo ? db >= 0 : db <= 0;
		if (ina && m < MAXV) memcpy(dst[m++], a, sizeof(corner));
		if (ina != inb && m < MAXV) lerp(a, b, da / (da - db), dst[m++]);
	}
	return m;
}

static double poly_area3(corner *p, int n) {
	double a = 0;
	for (int i = 1; i < n - 1; i++) {
		double ax = p[i][0] - p[0][0], ay = p[i][1] - p[0][1], az = p[i][2] - p[0][2];
		double bx = p[i + 1][0] - p[0][0], by = p[i + 1][1] - p[0][1], bz = p[i + 1][2] - p[0][2];
		double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
		a += sqrt(cx * cx + cy * cy + cz * cz);
	}
	return a * 0.5;
}

static void emit(lossantos_piece_fn fn, void *ctx, int64_t cx, int64_t cy, const double *a, const double *b,
				 const double *c) {
	const double *v[3] = {a, b, c};
	double pos[9], nrm[9], uv[6];
	for (int i = 0; i < 3; i++) {
		pos[i * 3] = v[i][0];
		pos[i * 3 + 1] = v[i][1];
		pos[i * 3 + 2] = v[i][2];
		uv[i * 2] = v[i][3];
		uv[i * 2 + 1] = v[i][4];
		nrm[i * 3] = v[i][5];
		nrm[i * 3 + 1] = v[i][6];
		nrm[i * 3 + 2] = v[i][7];
	}
	fn(ctx, cx, cy, pos, nrm, uv);
}

int lossantos_clip_tri(const double *pos, const double *nrm, const double *uv, double cell, lossantos_piece_fn fn,
				 void *ctx) {
	double minx = pos[0], maxx = pos[0], miny = pos[1], maxy = pos[1];
	for (int c = 1; c < 3; c++) {
		if (pos[c * 3] < minx) minx = pos[c * 3];
		if (pos[c * 3] > maxx) maxx = pos[c * 3];
		if (pos[c * 3 + 1] < miny) miny = pos[c * 3 + 1];
		if (pos[c * 3 + 1] > maxy) maxy = pos[c * 3 + 1];
	}
	int64_t x0 = (int64_t)floor(minx / cell), x1 = (int64_t)floor(maxx / cell);
	int64_t y0 = (int64_t)floor(miny / cell), y1 = (int64_t)floor(maxy / cell);
	if (x0 == x1 && y0 == y1) {
		fn(ctx, x0, y0, pos, nrm, uv);
		return 1;
	}
	corner tri[3];
	for (int c = 0; c < 3; c++) {
		tri[c][0] = pos[c * 3];
		tri[c][1] = pos[c * 3 + 1];
		tri[c][2] = pos[c * 3 + 2];
		tri[c][3] = uv[c * 2];
		tri[c][4] = uv[c * 2 + 1];
		tri[c][5] = nrm[c * 3];
		tri[c][6] = nrm[c * 3 + 1];
		tri[c][7] = nrm[c * 3 + 2];
	}
	int pieces = 0;
	for (int64_t cy = y0; cy <= y1; cy++)
		for (int64_t cx = x0; cx <= x1; cx++) {
			corner a[MAXV], b[MAXV];
			memcpy(a, tri, sizeof tri);
			int n = 3;
			n = clip_pass(a, n, b, 0, (double)cx * cell, 1);
			if (!n) continue;
			n = clip_pass(b, n, a, 0, (double)(cx + 1) * cell, 0);
			if (!n) continue;
			n = clip_pass(a, n, b, 1, (double)cy * cell, 1);
			if (!n) continue;
			n = clip_pass(b, n, a, 1, (double)(cy + 1) * cell, 0);
			if (n < 3) continue;
			// centroid cell only, a piece on a line lands twice
			double px = 0, py = 0;
			for (int i = 0; i < n; i++) {
				px += a[i][0];
				py += a[i][1];
			}
			px /= n;
			py /= n;
			if ((int64_t)floor(px / cell) != cx || (int64_t)floor(py / cell) != cy) continue;
			if (poly_area3(a, n) <= AREA_EPS) continue;
			for (int i = 1; i < n - 1; i++) emit(fn, ctx, cx, cy, a[0], a[i], a[i + 1]);
			pieces++;
		}
	return pieces;
}
