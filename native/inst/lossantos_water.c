#include "lossantos_water.h"
#include "lossantos_geo.h"
#include "lossantos_rpf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UV_TILE 2048.0  // uv = world / 2048
#define TS 1024.0  // tile size

void lossantos_water_free(lossantos_water *w) {
	lossantos_vec_free(w->quads);
	lossantos_vec_free(w->rects);
	lossantos_vec_free(w->cells);
}

// first tag followed by value="..."
static int field(const char *b, const char *e, const char *tag, double *out) {
	size_t tl = strlen(tag);
	for (const char *p = b; p + tl < e; p++) {
		if (strncmp(p, tag, tl) != 0) continue;
		const char *q = p + tl;
		if (!(*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) continue;
		while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
		if (strncmp(q, "value=\"", 7) != 0) continue;
		*out = strtod(q + 7, NULL);
		return 1;
	}
	return 0;
}

static int invisible(const char *b, const char *e) {
	for (const char *p = b; p + 11 < e; p++)
		if (strncmp(p, "IsInvisible", 11) == 0) {
			const char *q = p + 11;
			while (q < e && (*q == ' ' || *q == '\t')) q++;
			return strncmp(q, "value=\"true\"", 12) == 0;
		}
	return 0;
}

int lossantos_water_load(lossantos_water *w, const wchar_t *gta, const lossantos_ng *ng) {
	memset(w, 0, sizeof *w);
	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\common.rpf", gta);
	lossantos_rpf a;
	if (!lossantos_rpf_open(&a, ng, p)) return 0;
	int32_t want = -1;
	char path[1024];
	for (uint32_t i = 1; i < a.count; i++) {
		if (a.e[i].kind == LOSSANTOS_DIR || _stricmp(a.e[i].name, "water.xml") != 0) continue;
		lossantos_rpf_path(&a, i, path, sizeof path);
		char low[1024];
		lossantos_lower(low, path, sizeof low);
		if (want < 0 || strstr(low, "levels")) {
			want = (int32_t)i;
			if (strstr(low, "levels")) break;
		}
	}
	lossantos_buf b = {0};
	int ok = want >= 0 && lossantos_rpf_read(&a, (uint32_t)want, &b);
	lossantos_rpf_close(&a);
	if (!ok) {
		lossantos_buf_free(&b);
		return 0;
	}
	lossantos_buf_reserve(&b, b.n + 1);
	b.p[b.n] = 0;
	const char *t = (const char *)b.p;
	// strstr xml parser, this sucks but it works
	const char *s0 = strstr(t, "<WaterQuads>"), *s1 = s0 ? strstr(s0, "</WaterQuads>") : NULL;
	if (s0 && s1) {
		for (const char *it = strstr(s0, "<Item>"); it && it < s1; it = strstr(it + 6, "<Item>")) {
			const char *end = strstr(it, "</Item>");
			if (!end || end > s1) break;
			if (invisible(it, end)) continue;
			lossantos_rect r;
			double z = 0;
			if (!field(it, end, "minX", &r.x0) || !field(it, end, "maxX", &r.x1) || !field(it, end, "minY", &r.y0) ||
				!field(it, end, "maxY", &r.y1))
				continue;
			field(it, end, "z", &z);
			if (!(r.x1 > r.x0 && r.y1 > r.y0)) continue;
			r.x0 *= LOSSANTOS_SCALE;
			r.x1 *= LOSSANTOS_SCALE;
			r.y0 *= LOSSANTOS_SCALE;
			r.y1 *= LOSSANTOS_SCALE;
			r.z = z * LOSSANTOS_SCALE;
			lossantos_vec_push(w->quads, r);
		}
	}
	lossantos_buf_free(&b);
	return w->quads.n > 0;
}

static double r3(double v) { return nearbyint(v * 1000.0) / 1000.0; }

typedef struct {
	int32_t gx, gy;
	lossantos_rect r;
	uint32_t seq;
} crect;

static int cmp_crect(const void *a, const void *b) {
	const crect *x = (const crect *)a, *y = (const crect *)b;
	if (x->gx != y->gx) return x->gx < y->gx ? -1 : 1;
	if (x->gy != y->gy) return x->gy < y->gy ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

// pieces of b outside a
static int subtract(const lossantos_rect *b, const lossantos_rect *a, lossantos_rect *out) {
	if (a->x0 <= b->x0 && b->x1 <= a->x1 && a->y0 <= b->y0 && b->y1 <= a->y1) return 0;
	lossantos_rect c[4];
	int n = 0;
	if (b->y0 < a->y0) c[n++] = (lossantos_rect){b->x0, b->x1, b->y0, fmin(b->y1, a->y0), b->z};
	if (a->y1 < b->y1) c[n++] = (lossantos_rect){b->x0, b->x1, fmax(b->y0, a->y1), b->y1, b->z};
	if (b->x0 < a->x0) c[n++] = (lossantos_rect){b->x0, fmin(b->x1, a->x0), fmax(b->y0, a->y0), fmin(b->y1, a->y1), b->z};
	if (a->x1 < b->x1) c[n++] = (lossantos_rect){fmax(b->x0, a->x1), b->x1, fmax(b->y0, a->y0), fmin(b->y1, a->y1), b->z};
	int k = 0;
	for (int i = 0; i < n; i++)
		if (c[i].x1 > c[i].x0 + 1e-6 && c[i].y1 > c[i].y0 + 1e-6) out[k++] = c[i];
	return k;
}

int lossantos_water_cells(lossantos_water *w, int32_t gx0, int32_t gx1, int32_t gy0, int32_t gy1) {
	LOSSANTOS_VEC(crect) all = {0};
	uint32_t seq = 0;
	for (size_t q = 0; q < w->quads.n; q++) {
		const lossantos_rect *Q = &w->quads.p[q];
		int32_t cx0 = (int32_t)floor(Q->x0 / TS), cx1 = (int32_t)floor((Q->x1 - 1e-6) / TS);
		int32_t cy0 = (int32_t)floor(Q->y0 / TS), cy1 = (int32_t)floor((Q->y1 - 1e-6) / TS);
		for (int32_t gx = cx0 > gx0 ? cx0 : gx0; gx <= (cx1 < gx1 ? cx1 : gx1); gx++) {
			double wx0 = gx * TS;
			for (int32_t gy = cy0 > gy0 ? cy0 : gy0; gy <= (cy1 < gy1 ? cy1 : gy1); gy++) {
				double wy0 = gy * TS;
				crect c;
				c.gx = gx;
				c.gy = gy;
				c.r.x0 = r3(fmax(Q->x0, wx0));
				c.r.x1 = r3(fmin(Q->x1, wx0 + TS));
				c.r.y0 = r3(fmax(Q->y0, wy0));
				c.r.y1 = r3(fmin(Q->y1, wy0 + TS));
				c.r.z = r3(Q->z);
				c.seq = seq++;
				lossantos_vec_push(all, c);
			}
		}
	}
	qsort(all.p, all.n, sizeof *all.p, cmp_crect);
	w->rects.n = 0;
	w->cells.n = 0;
	lossantos_rect *pieces = NULL, *next = NULL;
	size_t pcap = 0;
	for (size_t s = 0; s < all.n;) {
		size_t e = s;
		while (e < all.n && all.p[e].gx == all.p[s].gx && all.p[e].gy == all.p[s].gy) e++;
		lossantos_water_cell cell = {all.p[s].gx, all.p[s].gy, (uint32_t)w->rects.n, 0};
		// unique rects, then same z overlaps cut out
		size_t ustart = s;
		LOSSANTOS_VEC(lossantos_rect) uniq = {0};
		for (size_t i = ustart; i < e; i++) {
			int dup = 0;
			for (size_t j = 0; j < uniq.n && !dup; j++) dup = memcmp(&uniq.p[j], &all.p[i].r, sizeof(lossantos_rect)) == 0;
			if (!dup) lossantos_vec_push(uniq, all.p[i].r);
		}
		for (size_t i = 0; i < uniq.n; i++) {
			size_t need = 64;
			if (need > pcap) {
				free(pieces);
				free(next);
				pcap = need * 64;
				pieces = (lossantos_rect *)malloc(pcap * sizeof *pieces);
				next = (lossantos_rect *)malloc(pcap * sizeof *next);
			}
			size_t np = 1;
			pieces[0] = uniq.p[i];
			for (size_t k = 0; k < i && np; k++) {
				if (uniq.p[k].z != uniq.p[i].z) continue;
				size_t nn = 0;
				for (size_t q = 0; q < np && nn + 4 <= pcap; q++) nn += (size_t)subtract(&pieces[q], &uniq.p[k], next + nn);
				memcpy(pieces, next, nn * sizeof *pieces);
				np = nn;
			}
			for (size_t q = 0; q < np; q++) lossantos_vec_push(w->rects, pieces[q]);
		}
		lossantos_vec_free(uniq);
		cell.count = (uint32_t)(w->rects.n - cell.first);
		if (cell.count) lossantos_vec_push(w->cells, cell);
		s = e;
	}
	free(pieces);
	free(next);
	lossantos_vec_free(all);
	return 1;
}

const lossantos_water_cell *lossantos_water_find(const lossantos_water *w, int32_t gx, int32_t gy) {
	size_t lo = 0, hi = w->cells.n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		const lossantos_water_cell *c = &w->cells.p[mid];
		if (c->gx < gx || (c->gx == gx && c->gy < gy)) lo = mid + 1;
		else hi = mid;
	}
	if (lo < w->cells.n && w->cells.p[lo].gx == gx && w->cells.p[lo].gy == gy) return &w->cells.p[lo];
	return NULL;
}

int lossantos_water_append(const lossantos_water *w, const lossantos_water_cell *c, uint32_t mat, lossantos_tilemem *t) {
	uint32_t nr = c->count;
	uint8_t *v = (uint8_t *)malloc((size_t)nr * 4 * 28);
	uint16_t *ix = (uint16_t *)malloc((size_t)nr * 6 * 2);
	if (!v || !ix) { free(v); free(ix); return 0; }
	const double n[3] = {0, 0, 1};
	for (uint32_t i = 0; i < nr; i++) {
		const lossantos_rect *r = &w->rects.p[c->first + i];
		double cs[4][3] = {{r->x0, r->y0, r->z}, {r->x1, r->y0, r->z}, {r->x1, r->y1, r->z}, {r->x0, r->y1, r->z}};
		for (int k = 0; k < 4; k++) {
			double uv[2] = {cs[k][0] / UV_TILE, cs[k][1] / UV_TILE};
			lossantos_vtx_encode(v + ((size_t)i * 4 + k) * 28, cs[k], n, uv);
		}
		uint16_t b = (uint16_t)(i * 4);
		// (0,1,2) (0,2,3) reversed for source
		uint16_t tri[6] = {b, (uint16_t)(b + 2), (uint16_t)(b + 1), b, (uint16_t)(b + 3), (uint16_t)(b + 2)};
		memcpy(ix + (size_t)i * 6, tri, 12);
	}
	int ok = lossantos_tilemem_add(t, mat, v, nr * 4, ix, nr * 6);
	free(v);
	free(ix);
	return ok;
}
