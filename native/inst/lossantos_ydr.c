#include "lossantos_ydr.h"
#include "lossantos_common.h"

#include <stdio.h>
#include <stdlib.h>

#define ROOT 0x50000000ull

static float half_to_float(uint16_t h) {
	uint32_t sign = (uint32_t)(h & 0x8000) << 16;
	uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
	if (exp == 0) {
		if (man == 0) {
			bits = sign;
		} else {                       /* subnormal -> normalise */
			exp = 127 - 15 + 1;
			while (!(man & 0x400)) {
				man <<= 1;
				exp--;
			}
			man &= 0x3FF;
			bits = sign | (exp << 23) | (man << 13);
		}
	} else if (exp == 31) {
		bits = sign | 0x7F800000u | (man << 13);
	} else {
		bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
	}
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

static int component(uint32_t type, int *kind, int *comps) {
	switch (type) {
	case 1: *kind = 0; *comps = 2; return 4;  // f16 x2
	case 2: *kind = 1; *comps = 1; return 4;  // f32
	case 3: *kind = 0; *comps = 4; return 8;  // f16 x4
	case 5: *kind = 1; *comps = 2; return 8;  // f32 x2
	case 6: *kind = 1; *comps = 3; return 12;  // f32 x3
	case 7: *kind = 1; *comps = 4; return 16;  // f32 x4
	case 8:
	case 9: *kind = 2; *comps = 4; return 4;  // u8 x4
	case 10: *kind = 3; *comps = 4; return 4;  // s8 x4
	default: *kind = 0; *comps = 0; return 0;
	}
}

typedef struct {
	int present, offset, kind, comps;
} slot;

static void read_comp(const uint8_t *raw, uint32_t stride, uint32_t count, const slot *s, int want, float *out) {
	for (uint32_t v = 0; v < count; v++) {
		const uint8_t *p = raw + (size_t)v * stride + s->offset;
		for (int c = 0; c < want; c++) {
			float f = 0.0f;
			if (c < s->comps) {
				switch (s->kind) {
				case 0: { uint16_t hv; memcpy(&hv, p + c * 2, 2); f = half_to_float(hv); break; }
				case 1: memcpy(&f, p + c * 4, 4); break;
				case 2: f = (float)p[c]; break;
				default: f = (float)(int8_t)p[c]; break;
				}
			}
			out[(size_t)v * want + c] = f;
		}
	}
}

static int read_vertices(lossantos_rsc *r, uint64_t vb, lossantos_geom *g) {
	uint32_t stride = lossantos_rsc_u16(r, vb + 0x08);
	uint64_t data = lossantos_rsc_u64(r, vb + 0x10);
	uint32_t count = lossantos_rsc_u32(r, vb + 0x18);
	uint64_t decl = lossantos_rsc_u64(r, vb + 0x30);
	if (!data || !count || !stride || !decl) return 0;
	uint32_t flags = lossantos_rsc_u32(r, decl);  // semantic bits
	uint64_t types = lossantos_rsc_u64(r, decl + 8);  // 4 bits per semantic
	slot sl[16];
	memset(sl, 0, sizeof sl);
	int off = 0;
	for (int i = 0; i < 16; i++) {
		if (!((flags >> i) & 1)) continue;
		int kind, comps;
		int size = component((uint32_t)((types >> (i * 4)) & 0xF), &kind, &comps);
		if (size) {
			sl[i].present = 1;
			sl[i].offset = off;
			sl[i].kind = kind;
			sl[i].comps = comps;
		}
		off += size;
	}
	if (!sl[0].present) return 0;
	const uint8_t *raw = lossantos_rsc_ptr(r, data, (size_t)count * stride);
	if (!raw) return 0;
	g->nverts = count;
	g->pos = (float *)malloc((size_t)count * 12);
	g->uv = (float *)calloc((size_t)count * 2, 4);
	if (!g->pos || !g->uv) return 0;
	read_comp(raw, stride, count, &sl[0], 3, g->pos);  // position
	if (sl[3].present) {  // normal
		g->nrm = (float *)malloc((size_t)count * 12);
		if (!g->nrm) return 0;
		read_comp(raw, stride, count, &sl[3], 3, g->nrm);
	}
	if (sl[6].present) read_comp(raw, stride, count, &sl[6], 2, g->uv);  // uv0
	return 1;
}

static void geom_free(lossantos_geom *g) {
	free(g->pos);
	free(g->uv);
	free(g->nrm);
	free(g->idx);
	memset(g, 0, sizeof *g);
}

static void shader_texture(lossantos_rsc *r, uint64_t group, uint32_t si, char *out, size_t cap) {
	out[0] = 0;
	uint64_t shaders = lossantos_rsc_u64(r, group + 0x10);
	uint32_t count = lossantos_rsc_u16(r, group + 0x18);
	if (!shaders || si >= count) return;
	uint64_t sp = lossantos_rsc_u64(r, shaders + (uint64_t)si * 8);
	if (!sp) return;
	uint64_t params = lossantos_rsc_u64(r, sp);
	uint32_t pcount = lossantos_rsc_u8(r, sp + 0x10);
	for (uint32_t pi = 0; pi < pcount; pi++) {
		uint64_t o = params + (uint64_t)pi * 16;
		if (lossantos_rsc_u8(r, o) != 0) continue;  // 0 = texture param
		uint64_t dp = lossantos_rsc_u64(r, o + 8);
		if (!dp) continue;
		lossantos_rsc tmp = *r;                         /* ydr.py: exception -> '' */
		const char *nm = lossantos_rsc_str(&tmp, lossantos_rsc_u64(&tmp, dp + 0x28));
		if (*nm) {
			snprintf(out, cap, "%s", nm);
			return;
		}
	}
}

static int model_list(lossantos_rsc *r, uint64_t ptr, uint64_t *out, int cap) {
	if (!ptr) return 0;
	uint64_t header = lossantos_rsc_u64(r, ptr);
	uint32_t capacity = lossantos_rsc_u16(r, ptr + 0x08);
	if (!header || !capacity) return 0;
	int n = 0;
	for (uint32_t i = 0; i < capacity && n < cap; i++) {
		uint64_t p = lossantos_rsc_u64(r, header + (uint64_t)i * 8);
		if (p) out[n++] = p;
	}
	return n;
}

int lossantos_ydr_parse(const uint8_t *data, size_t n, size_t system_size, lossantos_drawable *d) {
	memset(d, 0, sizeof *d);
	lossantos_rsc r;
	if (!lossantos_rsc_init(&r, data, n, system_size)) return 0;
	for (int k = 0; k < 3; k++) {
		d->bmin[k] = lossantos_rsc_f32(&r, ROOT + 0x30 + k * 4);
		d->bmax[k] = lossantos_rsc_f32(&r, ROOT + 0x40 + k * 4);
	}
	uint64_t group = lossantos_rsc_u64(&r, ROOT + 0x10);
	if (group) {
		uint64_t dict = lossantos_rsc_u64(&r, group + 0x08);
		if (dict) {
			d->tex = (lossantos_tex *)calloc(512, sizeof(lossantos_tex));
			if (d->tex) d->ntex = lossantos_ytd_parse(&r, dict, d->tex, 512);
		}
	}
	uint64_t models[64];
	int nm = 0;
	// lod model lists, high first
	for (int k = 0; k < 4 && !nm; k++) nm = model_list(&r, lossantos_rsc_u64(&r, ROOT + 0x50 + k * 8), models, 64);

	int cap = 16;
	d->geoms = (lossantos_geom *)calloc(cap, sizeof(lossantos_geom));
	if (!d->geoms) return 0;
	for (int m = 0; m < nm; m++) {
		uint64_t mp = models[m];
		uint64_t gptr = lossantos_rsc_u64(&r, mp + 0x08);
		uint32_t gcount = lossantos_rsc_u16(&r, mp + 0x10);
		uint64_t smap = lossantos_rsc_u64(&r, mp + 0x20);
		for (uint32_t gi = 0; gi < gcount; gi++) {
			uint64_t gp = lossantos_rsc_u64(&r, gptr + (uint64_t)gi * 8);
			if (!gp) continue;
			uint64_t vb = lossantos_rsc_u64(&r, gp + 0x18), ib = lossantos_rsc_u64(&r, gp + 0x38);
			if (!vb || !ib) continue;
			lossantos_geom g;
			memset(&g, 0, sizeof g);
			if (!read_vertices(&r, vb, &g)) {
				geom_free(&g);
				continue;
			}
			uint32_t icount = lossantos_rsc_u32(&r, ib + 0x08);
			uint64_t iptr = lossantos_rsc_u64(&r, ib + 0x10);
			const uint8_t *ip = (icount && iptr) ? lossantos_rsc_ptr(&r, iptr, (size_t)icount * 2) : NULL;
			uint32_t ntris = ip ? icount / 3 : 0;
			if (!ntris) {
				geom_free(&g);
				continue;
			}
			g.ntris = ntris;
			g.idx = (uint32_t *)malloc((size_t)ntris * 12);
			if (!g.idx) {
				geom_free(&g);
				return 0;
			}
			for (uint32_t i = 0; i < ntris * 3; i++) {
				uint16_t v;
				memcpy(&v, ip + i * 2, 2);
				g.idx[i] = v;
			}
			g.shader = smap ? lossantos_rsc_u16(&r, smap + (uint64_t)gi * 2) : 0;
			if (group) shader_texture(&r, group, g.shader, g.texture, sizeof g.texture);
			if (d->ngeoms == cap) {
				cap *= 2;
				lossantos_geom *ng = (lossantos_geom *)realloc(d->geoms, cap * sizeof(lossantos_geom));
				if (!ng) return 0;
				d->geoms = ng;
			}
			d->geoms[d->ngeoms++] = g;
		}
	}
	return 1;
}

void lossantos_ydr_free(lossantos_drawable *d) {
	for (int i = 0; i < d->ngeoms; i++) geom_free(&d->geoms[i]);
	free(d->geoms);
	free(d->tex);
	memset(d, 0, sizeof *d);
}
