#include "lossantos_ybn.h"

#include <float.h>
#include <stdlib.h>

// bounds block types
#define T_BOX 3
#define T_GEOMETRY 4
#define T_BVH 8
#define T_COMPOSITE 10

typedef double mat4[4][4];        /* column-vector convention, m[row][col] */

static void mul4(const mat4 a, const mat4 b, mat4 out) {
	mat4 t;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			double s = 0;
			for (int k = 0; k < 4; k++) s += a[i][k] * b[k][j];
			t[i][j] = s;
		}
	memcpy(out, t, sizeof t);
}

static void xform(const mat4 m, const double v[3], double o[3]) {
	for (int j = 0; j < 3; j++) o[j] = v[0] * m[j][0] + v[1] * m[j][1] + v[2] * m[j][2] + m[j][3];
}

static int push(lossantos_ybn *y, const double *a, const double *b, const double *c, uint8_t mat) {
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
	for (int k = 0; k < 9; k++) {
		if (d[k] < y->bmin[k % 3]) y->bmin[k % 3] = d[k];
		if (d[k] > y->bmax[k % 3]) y->bmax[k % 3] = d[k];
	}
	return 1;
}

static void vec3(lossantos_rsc *r, uint64_t a, double v[3]) {
	for (int i = 0; i < 3; i++) v[i] = lossantos_rsc_f32(r, a + (uint64_t)i * 4);
}

static void read_bounds(lossantos_rsc *r, uint64_t addr, const mat4 parent, lossantos_ybn *y, int depth);

static void read_composite(lossantos_rsc *r, uint64_t addr, const mat4 parent, lossantos_ybn *y, int depth) {
	uint64_t children = lossantos_rsc_u64(r, addr + 0x70);
	uint64_t x1 = lossantos_rsc_u64(r, addr + 0x78), x2 = lossantos_rsc_u64(r, addr + 0x80);  // child transforms, either one
	uint32_t count = lossantos_rsc_u16(r, addr + 0xA0);
	if (!children || !count) return;
	uint64_t xp = x1 ? x1 : x2;
	for (uint32_t i = 0; i < count; i++) {
		uint64_t child = lossantos_rsc_u64(r, children + (uint64_t)i * 8);
		if (!child) continue;
		mat4 local = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
		if (xp) {
			uint64_t m = xp + (uint64_t)i * 64;  // 4 vec3 columns, 16 bytes apart
			for (int c = 0; c < 4; c++) {
				double col[3];
				vec3(r, m + (uint64_t)c * 16, col);
				for (int k = 0; k < 3; k++) local[k][c] = col[k];
			}
		}
		mat4 world;
		mul4(parent, local, world);
		read_bounds(r, child, world, y, depth + 1);
	}
}

static void read_geometry(lossantos_rsc *r, uint64_t addr, const mat4 parent, lossantos_ybn *y) {
	// 0x88   polys, 16 bytes each
	// 0x90   quantum
	// 0xA0   center
	// 0xB0   verts, s16 xyz
	// 0xD0   vert count
	// 0xD4   poly count
	// 0xF0   material table
	// 0x118  material index per poly
	// 0x120  material count
	uint64_t polys = lossantos_rsc_u64(r, addr + 0x88);
	double q[3], c[3];
	vec3(r, addr + 0x90, q);
	vec3(r, addr + 0xA0, c);
	uint64_t vptr = lossantos_rsc_u64(r, addr + 0xB0);
	uint32_t vcount = lossantos_rsc_u32(r, addr + 0xD0), pcount = lossantos_rsc_u32(r, addr + 0xD4);
	uint64_t mats = lossantos_rsc_u64(r, addr + 0xF0);
	uint32_t mcount = lossantos_rsc_u8(r, addr + 0x120);
	uint64_t pmat = lossantos_rsc_u64(r, addr + 0x118);
	if (!polys || !vptr || !vcount || !pcount) return;
	const uint8_t *vraw = lossantos_rsc_ptr(r, vptr, (size_t)vcount * 6);
	const uint8_t *praw = lossantos_rsc_ptr(r, polys, (size_t)pcount * 16);
	if (!vraw || !praw) return;
	double *v = (double *)malloc((size_t)vcount * 3 * sizeof(double));
	if (!v) return;
	for (uint32_t i = 0; i < vcount; i++) {
		int16_t s[3];
		memcpy(s, vraw + (size_t)i * 6, 6);
		double l[3] = {(double)s[0] * q[0] + c[0], (double)s[1] * q[1] + c[1], (double)s[2] * q[2] + c[2]};
		xform(parent, l, v + (size_t)i * 3);
	}
	// 8 byte entries, type byte first, max(4, count) of them
	uint32_t ntypes = mcount ? (mcount > 4 ? mcount : 4) : 0;
	uint8_t types[256] = {0};
	const uint8_t *mraw = (mats && ntypes) ? lossantos_rsc_ptr(r, mats, (size_t)ntypes * 8) : NULL;
	if (mraw)
		for (uint32_t i = 0; i < ntypes; i++) types[i] = mraw[(size_t)i * 8];
	const uint8_t *pm = (mraw && pmat) ? lossantos_rsc_ptr(r, pmat, pcount) : NULL;

	// tris first then boxes, same order as ybn.py
	uint32_t handled = 0;
	for (uint32_t i = 0; i < pcount; i++) {
		const uint8_t *p = praw + (size_t)i * 16;
		if ((p[0] & 7) != 0) continue;  // 0 = triangle
		handled++;
		uint16_t w[3];
		memcpy(w, p + 4, 6);
		uint32_t a = w[0] & 0x7FFF, b = w[1] & 0x7FFF, cc = w[2] & 0x7FFF;
		if (a >= vcount || b >= vcount || cc >= vcount) continue;
		uint8_t mt = (pm && pm[i] < ntypes) ? types[pm[i]] : 0;
		push(y, v + a * 3, v + b * 3, v + cc * 3, mt);
	}
	for (uint32_t i = 0; i < pcount; i++) {
		const uint8_t *p = praw + (size_t)i * 16;
		if ((p[0] & 7) != 3) continue;  // 3 = box, ends up a quad
		handled++;
		uint16_t w[4];
		memcpy(w, p + 4, 8);
		if (w[0] >= vcount || w[1] >= vcount || w[2] >= vcount || w[3] >= vcount) continue;
		uint8_t mt = (pm && pm[i] < ntypes) ? types[pm[i]] : 0;
		push(y, v + w[0] * 3, v + w[1] * 3, v + w[2] * 3, mt);
		push(y, v + w[0] * 3, v + w[2] * 3, v + w[3] * 3, mt);
	}
	y->skipped_polys += pcount - handled;
	free(v);
}

static void read_box(lossantos_rsc *r, uint64_t addr, const mat4 parent, lossantos_ybn *y) {
	double a[3], b[3], lo[3], hi[3], corners[8][3];
	vec3(r, addr + 0x30, a);
	vec3(r, addr + 0x20, b);
	for (int k = 0; k < 3; k++) {
		lo[k] = a[k] < b[k] ? a[k] : b[k];
		hi[k] = a[k] > b[k] ? a[k] : b[k];
	}
	int n = 0;
	for (int xi = 0; xi < 2; xi++)
		for (int yi = 0; yi < 2; yi++)
			for (int zi = 0; zi < 2; zi++, n++) {
				double l[3] = {xi ? hi[0] : lo[0], yi ? hi[1] : lo[1], zi ? hi[2] : lo[2]};
				xform(parent, l, corners[n]);
			}
	static const int F[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};  // 6 quad faces
	for (int f = 0; f < 6; f++) {
		push(y, corners[F[f][0]], corners[F[f][1]], corners[F[f][2]], 0);
		push(y, corners[F[f][0]], corners[F[f][2]], corners[F[f][3]], 0);
	}
}

static void read_bounds(lossantos_rsc *r, uint64_t addr, const mat4 parent, lossantos_ybn *y, int depth) {
	if (depth > 32) return;
	uint32_t type = lossantos_rsc_u8(r, addr + 0x10);
	y->blocks++;
	if (type == T_COMPOSITE) {
		y->composites++;
		read_composite(r, addr, parent, y, depth);
	} else if (type == T_GEOMETRY || type == T_BVH) {
		if (type == T_BVH) y->bvhs++;
		else y->geometries++;
		read_geometry(r, addr, parent, y);
	} else if (type == T_BOX) {
		y->boxes++;
		read_box(r, addr, parent, y);
	} else {
		y->skipped_blocks++;
	}
}

int lossantos_ybn_parse(const uint8_t *data, size_t n, size_t system_size, lossantos_ybn *y) {
	memset(y, 0, sizeof *y);
	for (int k = 0; k < 3; k++) {
		y->bmin[k] = DBL_MAX;
		y->bmax[k] = -DBL_MAX;
	}
	lossantos_rsc r;
	if (!lossantos_rsc_init(&r, data, n, system_size)) return 0;
	mat4 id = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
	read_bounds(&r, LOSSANTOS_SYS_BASE, id, y, 0);
	return 1;
}

void lossantos_ybn_free(lossantos_ybn *y) {
	free(y->tri);
	free(y->mat);
	memset(y, 0, sizeof *y);
}
