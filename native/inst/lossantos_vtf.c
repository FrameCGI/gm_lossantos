#include "lossantos_vtf.h"
#include "lossantos_util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// image formats
#define VTF_RGBA8888 0u
#define VTF_I8 5u
#define VTF_A8 8u
#define VTF_BGRA8888 12u
#define VTF_DXT1 13u
#define VTF_DXT3 14u
#define VTF_DXT5 15u
// header flags
#define VTF_ONEBITALPHA 0x1000u
#define VTF_EIGHTBITALPHA 0x2000u
#define CAP 512u  // max output size
#define MAX_SIZE 4096u

void lossantos_texout_free(lossantos_texout *o) { lossantos_buf_free(&o->vtf); }

static int has(const char *k, const char *w) { return strstr(k, w) != NULL; }
static int has_any(const char *k, const char *const *ws) {
	for (; *ws; ws++)
		if (strstr(k, *ws)) return 1;
	return 0;
}


typedef struct {
	uint32_t w, h;
	uint8_t *p;     /* w*h*4 */
} img;

static int img_alloc(img *m, uint32_t w, uint32_t h) {
	m->w = w;
	m->h = h;
	m->p = (uint8_t *)malloc((size_t)w * h * 4 + 4);
	return m->p != NULL;
}

// edge clamped box blur, one axis at a time
static void mean_filter(const float *src, float *dst, uint32_t w, uint32_t h, int r, float *tmp) {
	const float inv = 1.0f / (float)(2 * r + 1);
	for (uint32_t y = 0; y < h; y++) {
		const float *s = src + (size_t)y * w;
		float *t = tmp + (size_t)y * w;
		double acc = 0;
		for (int k = -r; k <= r; k++) {
			int x = k < 0 ? 0 : k >= (int)w ? (int)w - 1 : k;
			acc += s[x];
		}
		for (uint32_t x = 0; x < w; x++) {
			t[x] = (float)(acc * inv);
			int out = (int)x - r, in = (int)x + r + 1;
			acc -= s[out < 0 ? 0 : out];
			acc += s[in >= (int)w ? (int)w - 1 : in];
		}
	}
	for (uint32_t x = 0; x < w; x++) {
		double acc = 0;
		for (int k = -r; k <= r; k++) {
			int y = k < 0 ? 0 : k >= (int)h ? (int)h - 1 : k;
			acc += tmp[(size_t)y * w + x];
		}
		for (uint32_t y = 0; y < h; y++) {
			dst[(size_t)y * w + x] = (float)(acc * inv);
			int out = (int)y - r, in = (int)y + r + 1;
			acc -= tmp[(size_t)(out < 0 ? 0 : out) * w + x];
			acc += tmp[(size_t)(in >= (int)h ? (int)h - 1 : in) * w + x];
		}
	}
}

static double frac_alpha(const img *m, int (*pred)(uint8_t)) {
	size_t n = (size_t)m->w * m->h, k = 0;
	for (size_t i = 0; i < n; i++) k += pred(m->p[i * 4 + 3]);
	return n ? (double)k / (double)n : 0.0;
}
static int p_gt250(uint8_t a) { return a > 250; }
static int p_binary(uint8_t a) { return a == 0 || a == 255; }
static int p_gt127(uint8_t a) { return a > 127; }
static int p_partial(uint8_t a) { return a > 8 && a < 248; }

static int src_opaque(const img *m) { return frac_alpha(m, p_gt250) > 0.995; }

static int soft_alpha(const img *m) {
	if (frac_alpha(m, p_binary) >= 0.35) return 0;
	double cov = frac_alpha(m, p_gt127);
	return cov > 0.005 && cov < 0.995;
}

static int soft_fade_alpha(const img *m) { return frac_alpha(m, p_partial) > 0.08; }

// high frequency alpha, not mostly 0 or 255
static int is_dither_mask(const img *m) {
	uint32_t w = m->w, h = m->h;
	double s0 = 0, s1 = 0;
	size_t n0 = 0, n1 = 0;
	for (uint32_t y = 0; y + 1 < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			s0 += fabs((double)m->p[((size_t)(y + 1) * w + x) * 4 + 3] - m->p[((size_t)y * w + x) * 4 + 3]);
			n0++;
		}
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x + 1 < w; x++) {
			s1 += fabs((double)m->p[((size_t)y * w + x + 1) * 4 + 3] - m->p[((size_t)y * w + x) * 4 + 3]);
			n1++;
		}
	double ahf = 0.5 * ((n0 ? s0 / n0 : 0) + (n1 ? s1 / n1 : 0));
	if (ahf <= 11.0) return 0;
	return frac_alpha(m, p_binary) < 0.85;
}

// threshold smoothed alpha, fill hidden texels from visible ones
static int cutout_rgba(img *m, double thresh, int smooth) {
	size_t n = (size_t)m->w * m->h;
	float *a = (float *)malloc(n * 4), *af = (float *)malloc(n * 4), *tmp = (float *)malloc(n * 4);
	float *wv = (float *)malloc(n * 4), *den = (float *)malloc(n * 4), *ch = (float *)malloc(n * 4),
		  *num = (float *)malloc(n * 4);
	if (!a || !af || !tmp || !wv || !den || !ch || !num) {
		free(a); free(af); free(tmp); free(wv); free(den); free(ch); free(num);
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		a[i] = m->p[i * 4 + 3];
		wv[i] = m->p[i * 4 + 3] > 0 ? 1.0f : 0.0f;
	}
	mean_filter(a, af, m->w, m->h, smooth, tmp);
	mean_filter(wv, den, m->w, m->h, 2, tmp);
	double gm[3] = {0, 0, 0};
	size_t nv = 0;
	for (size_t i = 0; i < n; i++)
		if (wv[i] > 0) {
			for (int c = 0; c < 3; c++) gm[c] += m->p[i * 4 + c];
			nv++;
		}
	if (!nv) {
		for (size_t i = 0; i < n; i++)
			for (int c = 0; c < 3; c++) gm[c] += m->p[i * 4 + c];
		nv = n;
	}
	for (int c = 0; c < 3; c++) gm[c] /= (double)(nv ? nv : 1);
	for (int c = 0; c < 3; c++) {
		for (size_t i = 0; i < n; i++) ch[i] = (float)m->p[i * 4 + c] * wv[i];
		mean_filter(ch, num, m->w, m->h, 2, tmp);
		for (size_t i = 0; i < n; i++) {
			if (wv[i] > 0) continue;
			double v = den[i] <= 1e-6f ? gm[c] : (double)num[i] / (den[i] > 1e-6f ? den[i] : 1e-6f);
			if (v < 0) v = 0;
			if (v > 255) v = 255;
			m->p[i * 4 + c] = (uint8_t)v;
		}
	}
	for (size_t i = 0; i < n; i++) m->p[i * 4 + 3] = af[i] > thresh ? 255 : 0;
	free(a); free(af); free(tmp); free(wv); free(den); free(ch); free(num);
	return 1;
}

// dark bluish texels = window panes
static void facade_alpha(img *m) {
	size_t n = (size_t)m->w * m->h;
	for (size_t i = 0; i < n; i++) {
		int r = m->p[i * 4], g = m->p[i * 4 + 1], b = m->p[i * 4 + 2];
		double lum = 0.299 * r + 0.587 * g + 0.114 * b;
		m->p[i * 4 + 3] = (lum < 85 && b >= r - 12) ? 100 : 255;
	}
}

static int downsample(const img *s, img *d) {
	uint32_t hw = s->w / 2 ? s->w / 2 : 1, hh = s->h / 2 ? s->h / 2 : 1;
	if (!img_alloc(d, hw, hh)) return 0;
	for (uint32_t y = 0; y < hh; y++)
		for (uint32_t x = 0; x < hw; x++)
			for (int c = 0; c < 4; c++) {
				int v = s->p[((size_t)(2 * y) * s->w + 2 * x) * 4 + c] + s->p[((size_t)(2 * y) * s->w + 2 * x + 1) * 4 + c] +
						s->p[((size_t)(2 * y + 1) * s->w + 2 * x) * 4 + c] +
						s->p[((size_t)(2 * y + 1) * s->w + 2 * x + 1) * 4 + c];
				d->p[((size_t)y * hw + x) * 4 + c] = (uint8_t)nearbyint(v / 4.0);
			}
	return 1;
}

static uint8_t *alpha_max_down(const uint8_t *a, uint32_t w, uint32_t h, uint32_t *ow, uint32_t *oh) {
	uint32_t pw = w + (w & 1), ph = h + (h & 1);
	uint32_t hw = pw / 2, hh = ph / 2;
	uint8_t *o = (uint8_t *)malloc((size_t)hw * hh + 1);
	if (!o) return NULL;
	for (uint32_t y = 0; y < hh; y++)
		for (uint32_t x = 0; x < hw; x++) {
			uint8_t m = 0;
			for (int dy = 0; dy < 2; dy++)
				for (int dx = 0; dx < 2; dx++) {
					uint32_t sx = 2 * x + dx, sy = 2 * y + dy;
					if (sx >= w) sx = w - 1;
					if (sy >= h) sy = h - 1;
					uint8_t v = a[(size_t)sy * w + sx];
					if (v > m) m = v;
				}
			o[(size_t)y * hw + x] = m;
		}
	*ow = hw;
	*oh = hh;
	return o;
}


static const float LUM[3] = {0.299f, 0.587f, 0.114f};

static uint32_t pack565(const float c[3]) {
	int v[3];
	for (int k = 0; k < 3; k++) {
		float q = nearbyintf(c[k]);
		v[k] = q < 0 ? 0 : q > 255 ? 255 : (int)q;
	}
	return ((uint32_t)(v[0] >> 3) << 11) | ((uint32_t)(v[1] >> 2) << 5) | (uint32_t)(v[2] >> 3);
}

static void unpack565(uint32_t v, float o[3]) {
	o[0] = (float)((v >> 11) & 31) * 255.0f / 31.0f;
	o[1] = (float)((v >> 5) & 63) * 255.0f / 63.0f;
	o[2] = (float)(v & 31) * 255.0f / 31.0f;
}

static void encode_color(const float x[16][3], uint8_t out[8]) {
	// luma extremes, then 2 least squares refits
	int i0 = 0, i1 = 0;
	float lmin = 1e30f, lmax = -1e30f;
	for (int i = 0; i < 16; i++) {
		float l = x[i][0] * LUM[0] + x[i][1] * LUM[1] + x[i][2] * LUM[2];
		if (l < lmin) { lmin = l; i0 = i; }
		if (l > lmax) { lmax = l; i1 = i; }
	}
	float c0[3], c1[3];
	memcpy(c0, x[i0], 12);
	memcpy(c1, x[i1], 12);
	for (int it = 0; it < 2; it++) {
		float d[3] = {c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2]};
		float den = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + 1e-6f;
		float a = 1e-6f, b = 0, c = 1e-6f, r0[3] = {0, 0, 0}, r1[3] = {0, 0, 0};
		for (int i = 0; i < 16; i++) {
			float t = ((x[i][0] - c0[0]) * d[0] + (x[i][1] - c0[1]) * d[1] + (x[i][2] - c0[2]) * d[2]) / den;
			float k = nearbyintf(t * 3.0f);
			if (k < 0) k = 0;
			if (k > 3) k = 3;
			float w1 = k / 3.0f, w0 = 1.0f - w1;   /* ramp position -> weights */
			a += w0 * w0;
			b += w0 * w1;
			c += w1 * w1;
			for (int z = 0; z < 3; z++) {
				r0[z] += w0 * x[i][z];
				r1[z] += w1 * x[i][z];
			}
		}
		float det = a * c - b * b;
		float l0[3], l1[3];
		int ok = 1;
		for (int z = 0; z < 3; z++) {
			l0[z] = (c * r0[z] - b * r1[z]) / det;
			l1[z] = (a * r1[z] - b * r0[z]) / det;
			if (!isfinite(l0[z]) || !isfinite(l1[z])) ok = 0;
		}
		if (ok) {
			memcpy(c0, l0, 12);
			memcpy(c1, l1, 12);
		}
	}
	uint32_t v0 = pack565(c0), v1 = pack565(c1);
	if (v0 < v1) {
		uint32_t t = v0;
		v0 = v1;
		v1 = t;
	}
	float p[4][3];
	unpack565(v0, p[0]);
	unpack565(v1, p[1]);
	for (int z = 0; z < 3; z++) {
		p[2][z] = (2 * p[0][z] + p[1][z]) / 3;
		p[3][z] = (p[0][z] + 2 * p[1][z]) / 3;
	}
	uint32_t bits = 0;
	for (int i = 0; i < 16; i++) {
		int best = 0;
		float bd = 1e30f;
		for (int k = 0; k < 4; k++) {
			float dd = 0;
			for (int z = 0; z < 3; z++) dd += (x[i][z] - p[k][z]) * (x[i][z] - p[k][z]);
			if (dd < bd) { bd = dd; best = k; }
		}
		bits |= (uint32_t)best << (2 * i);
	}
	out[0] = (uint8_t)v0;
	out[1] = (uint8_t)(v0 >> 8);
	out[2] = (uint8_t)v1;
	out[3] = (uint8_t)(v1 >> 8);
	memcpy(out + 4, &bits, 4);
}

static void encode_alpha(const float x[16], uint8_t out[8]) {
	float w1[8] = {0, 1, 1 / 7.f, 2 / 7.f, 3 / 7.f, 4 / 7.f, 5 / 7.f, 6 / 7.f}, w0[8];
	for (int k = 0; k < 8; k++) w0[k] = 1.0f - w1[k];
	float a0 = x[0], a1 = x[0];
	for (int i = 1; i < 16; i++) {
		if (x[i] > a0) a0 = x[i];
		if (x[i] < a1) a1 = x[i];
	}
	for (int it = 0; it < 3; it++) {
		float aa = 1e-6f, bb = 0, cc = 1e-6f, r0 = 0, r1 = 0;
		for (int i = 0; i < 16; i++) {
			int best = 0;
			float bd = 1e30f;
			for (int k = 0; k < 8; k++) {
				float d = fabsf(x[i] - (a0 * w0[k] + a1 * w1[k]));
				if (d < bd) { bd = d; best = k; }
			}
			aa += w0[best] * w0[best];
			bb += w0[best] * w1[best];
			cc += w1[best] * w1[best];
			r0 += w0[best] * x[i];
			r1 += w1[best] * x[i];
		}
		float det = aa * cc - bb * bb;
		float l0 = a0, l1 = a1;
		if (fabsf(det) > 1e-6f) {
			l0 = (cc * r0 - bb * r1) / det;
			l1 = (aa * r1 - bb * r0) / det;
		}
		a0 = l0 < 0 ? 0 : l0 > 255 ? 255 : l0;
		a1 = l1 < 0 ? 0 : l1 > 255 ? 255 : l1;
	}
	int v0 = (int)nearbyintf(a0), v1 = (int)nearbyintf(a1);
	if (v0 < v1) {
		int t = v0;
		v0 = v1;
		v1 = t;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 16; i++) {
		int best = 0;
		float bd = 1e30f;
		for (int k = 0; k < 8; k++) {
			float d = fabsf(x[i] - ((float)v0 * w0[k] + (float)v1 * w1[k]));
			if (d < bd) { bd = d; best = k; }
		}
		bits |= (uint64_t)best << (3 * i);
	}
	out[0] = (uint8_t)v0;
	out[1] = (uint8_t)v1;
	for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}

static int encode_level(const img *m, int dxt5, lossantos_buf *o) {
	uint32_t bw = (m->w + 3) / 4, bh = (m->h + 3) / 4;
	size_t bs = dxt5 ? 16 : 8;
	if (!lossantos_buf_reserve(o, o->n + (size_t)bw * bh * bs)) return 0;
	// only fully transparent texels get the visible mean
	double fill[3] = {128, 128, 128};
	if (dxt5) {
		double s[3] = {0, 0, 0};
		size_t k = 0, n = (size_t)m->w * m->h;
		for (size_t i = 0; i < n; i++)
			if (m->p[i * 4 + 3]) {
				for (int c = 0; c < 3; c++) s[c] += m->p[i * 4 + c];
				k++;
			}
		if (k)
			for (int c = 0; c < 3; c++) fill[c] = (double)(uint8_t)(s[c] / k);
	}
	for (uint32_t by = 0; by < bh; by++)
		for (uint32_t bx = 0; bx < bw; bx++) {
			float rgb[16][3], al[16];
			for (int i = 0; i < 16; i++) {
				uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
				if (x >= m->w) x = m->w - 1;
				if (y >= m->h) y = m->h - 1;
				const uint8_t *px = m->p + ((size_t)y * m->w + x) * 4;
				al[i] = px[3];
				for (int c = 0; c < 3; c++) rgb[i][c] = (dxt5 && px[3] == 0) ? (float)fill[c] : (float)px[c];
			}
			uint8_t *d = o->p + o->n;
			if (dxt5) {
				encode_alpha(al, d);
				encode_color((const float(*)[3])rgb, d + 8);
			} else {
				encode_color((const float(*)[3])rgb, d);
			}
			o->n += bs;
		}
	return 1;
}


static size_t mip_size(uint32_t fmt, uint32_t w, uint32_t h) {
	if (fmt == VTF_BGRA8888) return (size_t)w * h * 4;
	if (fmt == VTF_DXT1) return (size_t)((w + 3) / 4 ? (w + 3) / 4 : 1) * ((h + 3) / 4 ? (h + 3) / 4 : 1) * 8;
	if (fmt == VTF_DXT3 || fmt == VTF_DXT5) return (size_t)((w + 3) / 4 ? (w + 3) / 4 : 1) * ((h + 3) / 4 ? (h + 3) / 4 : 1) * 16;
	return 0;
}

// 0   "VTF\0"
// 4   version 7.2
// 12  header size
// 16  width, height
// 20  flags
// 24  frames
// 32  reflectivity
// 48  bump scale
// 52  format
// 56  mip count
// 57  lowres format, none
// 63  depth
static void vtf_header(uint8_t *h, uint32_t w, uint32_t hgt, uint32_t flags, uint32_t fmt, uint32_t mips, uint32_t depth) {
	memset(h, 0, 80);
	memcpy(h, "VTF\0", 4);
	uint32_t u;
	u = 7; memcpy(h + 4, &u, 4);
	u = 2; memcpy(h + 8, &u, 4);
	u = 80; memcpy(h + 12, &u, 4);
	uint16_t s;
	s = (uint16_t)w; memcpy(h + 16, &s, 2);
	s = (uint16_t)hgt; memcpy(h + 18, &s, 2);
	memcpy(h + 20, &flags, 4);
	s = 1; memcpy(h + 24, &s, 2);
	float f = 0.5f;
	memcpy(h + 32, &f, 4);
	memcpy(h + 36, &f, 4);
	memcpy(h + 40, &f, 4);
	f = 1.0f;
	memcpy(h + 48, &f, 4);
	memcpy(h + 52, &fmt, 4);
	h[56] = (uint8_t)mips;
	u = 0xFFFFFFFFu;
	memcpy(h + 57, &u, 4);
	s = (uint16_t)depth;
	memcpy(h + 63, &s, 2);
}

// mips are smallest first so just chop the tail
static void shrink(lossantos_buf *v) {
	if (v->n < 80) return;
	uint16_t w, h;
	uint32_t fmt;
	memcpy(&w, v->p + 16, 2);
	memcpy(&h, v->p + 18, 2);
	memcpy(&fmt, v->p + 52, 4);
	uint32_t mips = v->p[56];
	size_t sizes[16], total = 0;
	uint32_t mw = w, mh = h;
	if (!mips || mips > 16) return;
	for (uint32_t i = 0; i < mips; i++) {
		sizes[i] = mip_size(fmt, mw, mh);
		if (!sizes[i]) return;
		total += sizes[i];
		mw = mw > 1 ? mw >> 1 : 1;
		mh = mh > 1 ? mh >> 1 : 1;
	}
	if (total != v->n - 80) return;
	uint32_t drops = 0, rw = w, rh = h;
	while ((rw > rh ? rw : rh) > CAP && mips - drops > 1) {
		rw = rw > 1 ? rw >> 1 : 1;
		rh = rh > 1 ? rh >> 1 : 1;
		drops++;
	}
	if (!drops) return;
	size_t keep = 0;
	for (uint32_t i = drops; i < mips; i++) keep += sizes[i];
	uint16_t s = (uint16_t)rw;
	memcpy(v->p + 16, &s, 2);
	s = (uint16_t)rh;
	memcpy(v->p + 18, &s, 2);
	v->p[56] = (uint8_t)(mips - drops);
	v->n = 80 + keep;
}

static int vtf_format(uint32_t gfmt, uint32_t *flags) {
	*flags = 0;
	switch (gfmt) {
	case LOSSANTOS_FMT_DXT1: *flags = VTF_ONEBITALPHA; return VTF_DXT1;
	case LOSSANTOS_FMT_DXT3: *flags = VTF_EIGHTBITALPHA; return VTF_DXT3;
	case LOSSANTOS_FMT_DXT5: *flags = VTF_EIGHTBITALPHA; return VTF_DXT5;
	case LOSSANTOS_FMT_A8R8G8B8:
	case LOSSANTOS_FMT_X8R8G8B8: return VTF_BGRA8888;
	case LOSSANTOS_FMT_A8B8G8R8: return VTF_RGBA8888;
	case LOSSANTOS_FMT_L8: return VTF_I8;
	case LOSSANTOS_FMT_A8: return VTF_A8;
	default: return -1;
	}
}

static int decode0(const lossantos_tex *t, img *m) {
	if (!img_alloc(m, t->width, t->height)) return 0;
	uint32_t w, h;
	if (!lossantos_tex_decode(t, 0, m->p, (size_t)t->width * t->height * 4, &w, &h)) {
		free(m->p);
		m->p = NULL;
		return 0;
	}
	return 1;
}

int lossantos_texture_raw(const lossantos_tex *t, lossantos_buf *v) {
	uint32_t vflags;
	int vfmt = vtf_format(t->format, &vflags);
	if (vfmt < 0 || !t->levels || !lossantos_buf_reserve(v, 80 + t->data_n)) return 0;
	v->n = 80;
	vtf_header(v->p, t->width, t->height, vflags, (uint32_t)vfmt, t->levels, t->depth ? t->depth : 1);
	// smallest mip first
	size_t offs[16], lens[16], off = 0;
	uint32_t nl = t->levels > 16 ? 16 : t->levels;
	for (uint32_t l = 0; l < nl; l++) {
		lens[l] = lossantos_tex_level_size(t->format, t->width, t->height, t->stride, l);
		offs[l] = off;
		off += lens[l];
	}
	for (int l = (int)nl - 1; l >= 0; l--) {
		memcpy(v->p + v->n, t->data + offs[l], lens[l]);
		v->n += lens[l];
	}
	return 1;
}

static int write_rebuilt(img *base, int alpha_max, int force_rgba, lossantos_buf *v) {
	img lv[16];
	int n = 0;
	lv[n++] = *base;
	while ((lv[n - 1].w < lv[n - 1].h ? lv[n - 1].w : lv[n - 1].h) > 1 && n < 16) {
		if (!downsample(&lv[n - 1], &lv[n])) break;
		n++;
	}
	if (alpha_max) {
		uint32_t w = base->w, h = base->h;
		uint8_t *a = (uint8_t *)malloc((size_t)w * h);
		for (size_t i = 0; i < (size_t)w * h; i++) a[i] = base->p[i * 4 + 3];
		for (int l = 1; l < n; l++) {
			uint32_t nw, nh;
			uint8_t *na = alpha_max_down(a, w, h, &nw, &nh);
			free(a);
			a = na;
			w = nw;
			h = nh;
			for (uint32_t y = 0; y < lv[l].h && y < h; y++)
				for (uint32_t x = 0; x < lv[l].w && x < w; x++) lv[l].p[((size_t)y * lv[l].w + x) * 4 + 3] = a[(size_t)y * w + x];
		}
		free(a);
	}
	int dxt1 = 1;
	for (size_t i = 0; i < (size_t)base->w * base->h; i++)
		if (base->p[i * 4 + 3] != 255) {
			dxt1 = 0;
			break;
		}
	uint32_t fmt = force_rgba ? VTF_BGRA8888 : dxt1 ? VTF_DXT1 : VTF_DXT5;
	uint32_t flags = force_rgba ? 0 : dxt1 ? VTF_ONEBITALPHA : VTF_EIGHTBITALPHA;
	int first = 0;  // skip levels over CAP
	{
		uint32_t rw = base->w, rh = base->h;
		while ((rw > rh ? rw : rh) > CAP && n - first > 1) {
			rw = rw > 1 ? rw >> 1 : 1;
			rh = rh > 1 ? rh >> 1 : 1;
			first++;
		}
	}
	lossantos_buf_reserve(v, 80);
	v->n = 80;
	vtf_header(v->p, lv[first].w, lv[first].h, flags, fmt, (uint32_t)(n - first), 1);
	int ok = 1;
	for (int l = n - 1; l >= first && ok; l--) {
		if (force_rgba) {
			size_t bytes = (size_t)lv[l].w * lv[l].h * 4;
			if (!lossantos_buf_reserve(v, v->n + bytes)) { ok = 0; break; }
			for (size_t i = 0; i < (size_t)lv[l].w * lv[l].h; i++) {
				v->p[v->n + i * 4] = lv[l].p[i * 4 + 2];
				v->p[v->n + i * 4 + 1] = lv[l].p[i * 4 + 1];
				v->p[v->n + i * 4 + 2] = lv[l].p[i * 4];
				v->p[v->n + i * 4 + 3] = lv[l].p[i * 4 + 3];
			}
			v->n += bytes;
		} else {
			ok = encode_level(&lv[l], !dxt1, v);
		}
	}
	for (int l = 0; l < n; l++) free(lv[l].p);
	base->p = NULL;
	return ok;
}


static void vmt_text(char *o, size_t cap, const char *path, const char *surface, int alpha, int translucent,
					 double opacity, int nocull, int selfillum, int additive, int polygonoffset, int no_va) {
	int n = snprintf(o, cap,
					 "\"UnlitGeneric\"\n{\n    \"$model\" \"1\"\n    \"$vertexcolor\" \"1\"\n"
					 "    \"$basetexture\" \"%s\"\n    \"$surfaceprop\" \"%s\"\n",
					 path, surface);
	if ((translucent || additive) && !no_va) n += snprintf(o + n, cap - n, "    \"$vertexalpha\" \"1\"\n");
	if (translucent) {
		char op[32];
		snprintf(op, sizeof op, "%g", opacity);
		if (!strchr(op, '.')) strcat(op, ".0");
		n += snprintf(o + n, cap - n, "    \"$translucent\" \"1\"\n    \"$alpha\" \"%s\"\n", op);
	} else if (alpha) {
		n += snprintf(o + n, cap - n, "    \"$alphatest\" \"1\"\n");
	}
	if (selfillum) n += snprintf(o + n, cap - n, "    \"$selfillum\" \"1\"\n");
	if (additive) n += snprintf(o + n, cap - n, "    \"$additive\" \"1\"\n");
	if (nocull) n += snprintf(o + n, cap - n, "    \"$nocull\" \"1\"\n");
	if (polygonoffset) n += snprintf(o + n, cap - n, "    \"$polygonoffset\" \"2\"\n");
	snprintf(o + n, cap - n, "}\n");
}

uint32_t lossantos_name_flags(const char *name) {
	char l[256];
	lossantos_lower(l, name, sizeof l);
	int glass = has(l, "glass") || has(l, "window") || has(l, "shop");
	int nocull = glass || has(l, "railing") || has(l, "fence") || has(l, "branch") || has(l, "leaf") ||
				 has(l, "hedge") || has(l, "ivy");
	return (uint32_t)(glass ? 1 : 0) | (nocull ? 2 : 0);
}

static void finish(lossantos_texout *out, const char *path, int nocull, int translucent, int additive, const char *vmt_params) {
	lossantos_sha1(out->vtf.p, out->vtf.n, out->sha1);
	uint32_t f = lossantos_name_flags(path);
	if (nocull) f |= 2;
	if (translucent) f |= 1;
	if (additive) f |= 4;
	out->flags = f;
	snprintf(out->canon_key, sizeof out->canon_key, "%s", vmt_params);
}

// this sucks but it works
int lossantos_texture_install(const lossantos_tex *tex, const char *path, lossantos_texout *out) {
	out->vtf.n = 0;
	out->vmt[0] = 0;
	out->flags = 0;
	out->canon_key[0] = 0;
	out->rebuilt = 0;
	char lower[160];
	lossantos_lower(lower, tex->name, sizeof lower);
	const char *stem = strrchr(path, '/');
	stem = stem ? stem + 1 : path;

	// same lists as lossantos_tex.c
	static const char *const MARK[] = {"roadmark", "road_mark", "marking", "genallmark", "linemark", NULL};
	static const char *const OVL[] = {"overlay", "ovly", "splat", "blend", "glue", "road_damage", "roadblend",
									  "ground_dirt", "dirt_overlay", "stain", "crack", "skid", "tyremark",
									  "tyreskid", "litter", NULL};
	static const char *const STIP[] = {"railing", "fence", "mesh", "net", "grill", "grid", "trellis", "chainlink",
									   "wicker", "shutter", "gate", "doorgate", "canopy", NULL};
	size_t ll = strlen(lower);
	int ends_a = ll >= 2 && strcmp(lower + ll - 2, "_a") == 0;
	int marking = has_any(lower, MARK);
	int overlay = has_any(lower, OVL) && !has(lower, "stainless");
	int stipple_name = has_any(lower, STIP) || (has(lower, "canopy") && ends_a);
	int glass = has(lower, "glass") && !has(lower, "shop") && !has(lower, "brand") && !has(lower, "interior");
	int facade = has(lower, "bld01_rsn_ql_window") || has(lower, "bld01_rsn_ql_shop");
	int foliage = has(lower, "branches") || has(lower, "leaf");
	int stipple = has(lower, "railing") || has(lower, "fence") || (has(lower, "canopy") && ends_a);

	img rgba = {0, 0, NULL};
	int have_rgba = 0, rgba_map = 0, alpha_map = 0, soft = 0, opaque = 0;
	if (marking || overlay || stipple_name) {
		if (!decode0(tex, &rgba)) {
			lossantos_set_error("unsupported format 0x%x %ux%u", tex->format, tex->width, tex->height);
			return 0;
		}
		have_rgba = 1;
		opaque = src_opaque(&rgba);
		if (opaque) {
		} else if (marking) {
			rgba_map = cutout_rgba(&rgba, 110, 2);
		} else if (stipple_name) {
			rgba_map = cutout_rgba(&rgba, 128, 1);
		} else if (overlay) {
			if (soft_alpha(&rgba)) soft = 1;
			else rgba_map = cutout_rgba(&rgba, 128, 1);
		}
	} else if (facade) {
		if (!decode0(tex, &rgba)) {
			lossantos_set_error("unsupported format 0x%x %ux%u", tex->format, tex->width, tex->height);
			return 0;
		}
		have_rgba = 1;
		facade_alpha(&rgba);
		alpha_map = 1;
	} else if (!glass) {
		if (decode0(tex, &rgba)) {
			have_rgba = 1;
			opaque = src_opaque(&rgba);
			if (is_dither_mask(&rgba)) rgba_map = cutout_rgba(&rgba, 128, 1);
			else if (soft_alpha(&rgba)) soft = 1;
		}
	}
	int fade = !rgba_map && !alpha_map && have_rgba && soft_fade_alpha(&rgba);
	int alpha_max = rgba_map || alpha_map;

	uint32_t vflags;
	int vfmt = vtf_format(tex->format, &vflags);
	int force_rgba = (vfmt == (int)VTF_DXT1 || vfmt == (int)VTF_DXT3 || vfmt == (int)VTF_DXT5) &&
					 (tex->width % 4 || tex->height % 4);
	int ok;
	if (glass || alpha_map || rgba_map || alpha_max || force_rgba) {
		img base = {0, 0, NULL};
		if (rgba_map || alpha_map) {
			base = rgba;
			rgba.p = NULL;
			have_rgba = 0;
		} else {
			if (tex->width > MAX_SIZE || tex->height > MAX_SIZE || !decode0(tex, &base)) {
				free(rgba.p);
				lossantos_set_error("unsupported format 0x%x %ux%u", tex->format, tex->width, tex->height);
				return 0;
			}
			if (glass)
				for (size_t i = 0; i < (size_t)base.w * base.h; i++) {
					double a = base.p[i * 4 + 3] * 0.35f;
					base.p[i * 4 + 3] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
				}
		}
		ok = write_rebuilt(&base, alpha_max, force_rgba, &out->vtf);
		out->rebuilt = 1;
	} else {
		if (vfmt < 0 || tex->width > MAX_SIZE || tex->height > MAX_SIZE || !tex->levels) {
			free(rgba.p);
			lossantos_set_error("unsupported format 0x%x %ux%u", tex->format, tex->width, tex->height);
			return 0;
		}
		lossantos_buf_reserve(&out->vtf, 80 + tex->data_n);
		out->vtf.n = 80;
		vtf_header(out->vtf.p, tex->width, tex->height, vflags, (uint32_t)vfmt, tex->levels, tex->depth ? tex->depth : 1);
		// smallest mip first
		size_t offs[16], lens[16], off = 0;
		uint32_t nl = tex->levels > 16 ? 16 : tex->levels;
		for (uint32_t l = 0; l < nl; l++) {
			lens[l] = lossantos_tex_level_size(tex->format, tex->width, tex->height, tex->stride, l);
			offs[l] = off;
			off += lens[l];
		}
		for (int l = (int)nl - 1; l >= 0; l--) {
			memcpy(out->vtf.p + out->vtf.n, tex->data + offs[l], lens[l]);
			out->vtf.n += lens[l];
		}
		ok = 1;
	}
	free(rgba.p);
	if (!ok) {
		lossantos_set_error("vtf write failed");
		return 0;
	}
	shrink(&out->vtf);

	const char *surface = (glass || facade) ? "glass" : "concrete";
	if (foliage) surface = "grass";
	else if (stipple) surface = "metal";
	else if (!glass) {
		if (has(lower, "road") || has(lower, "tarmac") || has(lower, "asphalt")) surface = "asphalt";
		else if (has(lower, "sidewalk") || has(lower, "pavement")) surface = "concrete";
		else if (has(lower, "dirt") || has(lower, "mud") || has(lower, "gravel") || has(lower, "sand")) surface = "dirt";
		else if (has(lower, "grass") || has(lower, "hedge") || has(lower, "ivy")) surface = "grass";
		else if (has(lower, "metal")) surface = "metal";
	}
	int glow = has(lower, "glow") || has(lower, "neon");
	int alpha, translucent, selfillum = 0, additive = 0;
	double opacity = 1.0;
	if (glow) {
		alpha = 0; translucent = 0; selfillum = 1; additive = 1;
	} else if (soft) {
		alpha = 0; translucent = 1;
	} else if (glass || facade) {
		alpha = 0; translucent = 1; opacity = facade ? 1.0 : 0.35;
	} else if (rgba_map || alpha_map) {
		alpha = 1; translucent = 0;
	} else if (opaque) {
		alpha = 0; translucent = 0;
	} else if (fade) {
		alpha = 0; translucent = 1;
	} else {
		alpha = 1; translucent = 0;
	}
	int nocull = foliage || stipple || glass || facade;

	char sl[160];
	lossantos_lower(sl, stem, sizeof sl);
	// beach rules from fix_beach_vmt.py
	int foam = has(sl, "beachfoam") && !has(sl, "insulation");
	static const char *const DECAL[] = {"waterstain", "beach_decal", "beachdecal", "beachraking", "beach_raking",
										"sandmoss", "waterdamage", "im_beach_decals", "beachrake", NULL};
	int decal = !foam && has_any(sl, DECAL);
	int foam_patched = 0;
	if (foam && !additive) {
		foam_patched = !translucent;
		alpha = 0;
		translucent = 0;
		additive = 1;
	}
	vmt_text(out->vmt, sizeof out->vmt, path, surface, alpha, translucent, opacity, nocull, selfillum, additive, decal,
			 foam_patched);

	char params[256];
	snprintf(params, sizeof params, "unlitgeneric|a%d|t%d|o%g|n%d|s%d|ad%d|p%d", alpha, translucent,
			 translucent ? opacity : 0.0, nocull, selfillum, additive, decal);
	finish(out, path, nocull, translucent, additive, params);
	return 1;
}

int lossantos_water_material(lossantos_texout *out) {
	memset(out, 0, sizeof *out);
	const int size = 256;
	img m;
	if (!img_alloc(&m, size, size)) return 0;
	const double PI = 3.14159265358979323846;
	static const int RGB[3] = {115, 140, 160};
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++) {
			double xx = (double)x / size, yy = (double)y / size;
			double swell = (sin(2 * PI * (xx * 2 + yy) + 0.7) + 0.7 * sin(2 * PI * (xx - yy * 3) + 2.1) +
							0.5 * sin(2 * PI * (xx * 4 + yy * 2) + 4.0) + 0.35 * sin(2 * PI * (xx * 7 - yy * 5) + 1.2)) /
						   2.55;
			double ripple = (sin(2 * PI * (xx * 13 + yy * 9) + 5.5) + sin(2 * PI * (xx * 17 - yy * 11) + 3.3)) / 2.0;
			double value = 13.0 * swell + 4.0 * ripple;
			uint8_t *p = m.p + ((size_t)y * size + x) * 4;
			for (int c = 0; c < 3; c++) {
				double v = RGB[c] + value;
				p[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
			}
			p[3] = 217;
		}
	if (!write_rebuilt(&m, 0, 0, &out->vtf)) return 0;
	vmt_text(out->vmt, sizeof out->vmt, "models/gtav/water/ocean", "water", 0, 1, 0.95, 0, 0, 0, 0, 0);
	finish(out, "models/gtav/water/ocean", 0, 1, 0, "water");
	return 1;
}
