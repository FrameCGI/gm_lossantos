#include "gtav_decode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GTAV_TILE_MAGIC 0x31545647u   /* "GVT1" little-endian */

// sanity caps
#define GTAV_MAX_VERTS   262144u
#define GTAV_MAX_INDICES 1048576u
#define GTAV_MAX_SUBS    8192u
#define GTAV_MAX_MATS    8192u
#define GTAV_MIN_STRIDE  20u
#define GTAV_MAX_STRIDE  256u

static uint32_t rd_u32(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static uint16_t rd_u16(const uint8_t *p) {
	uint16_t v;
	memcpy(&v, p, 2);
	return v;
}

static float rd_f32(const uint8_t *p) {
	float v;
	memcpy(&v, p, 4);
	return v;
}

static void seterr(char *err, size_t errlen, const char *fmt, ...) {
	va_list ap;
	if (!err || errlen == 0) return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

void gtav_tile_free(gtav_tile *t) {
	if (!t) return;
	free(t->materials);
	free(t->vertices);
	free(t->indices);
	free(t->submeshes);
	memset(t, 0, sizeof(*t));
}

int gtav_tile_decode(const uint8_t *blob, size_t len, gtav_tile *out,
				   char *err, size_t errlen) {
	if (out) memset(out, 0, sizeof(*out));
	if (!blob || !out) {
		seterr(err, errlen, "null argument");
		return 0;
	}
	if (len < 32) {
		seterr(err, errlen, "blob is %u bytes, shorter than the 32-byte header",
			   (unsigned)len);
		return 0;
	}
	if (rd_u32(blob) != GTAV_TILE_MAGIC) {
		seterr(err, errlen, "bad magic 0x%08x", (unsigned)rd_u32(blob));
		return 0;
	}

	uint16_t version = rd_u16(blob + 4);
	uint16_t flags   = rd_u16(blob + 6);
	uint32_t vc      = rd_u32(blob + 8);
	uint32_t ic      = rd_u32(blob + 12);
	uint32_t sc      = rd_u32(blob + 16);
	uint32_t mc      = rd_u32(blob + 20);
	uint32_t stride  = rd_u32(blob + 24);

	if (stride < GTAV_MIN_STRIDE || stride > GTAV_MAX_STRIDE) {
		seterr(err, errlen, "implausible vertex stride %u", (unsigned)stride);
		return 0;
	}
	if (sc > GTAV_MAX_SUBS || mc > GTAV_MAX_MATS) {
		seterr(err, errlen, "implausible counts: %u submeshes, %u materials",
			   (unsigned)sc, (unsigned)mc);
		return 0;
	}
	if (vc > GTAV_MAX_VERTS || ic > GTAV_MAX_INDICES) {
		seterr(err, errlen, "implausible counts: %u verts, %u indices",
			   (unsigned)vc, (unsigned)ic);
		return 0;
	}

	size_t hdrVerts = vc, hdrIdx = ic;
	uint32_t clamped = 0;

	size_t pos = 32;

	// submesh table
	gtav_submesh *subs = NULL;
	if (sc > 0) {
		if (pos + (size_t)sc * 16 > len) {
			seterr(err, errlen, "submesh table runs past the blob (%u entries)",
				   (unsigned)sc);
			return 0;
		}
		subs = (gtav_submesh *)calloc(sc, sizeof(gtav_submesh));
		if (!subs) { seterr(err, errlen, "out of memory"); return 0; }
		for (uint32_t i = 0; i < sc; i++) {
			const uint8_t *e = blob + pos + (size_t)i * 16;
			subs[i].material     = rd_u32(e);
			subs[i].indexOffset  = rd_u32(e + 4);
			subs[i].indexCount   = rd_u32(e + 8);
			subs[i].vertexOffset = rd_u32(e + 12);
		}
	}
	pos += (size_t)sc * 16;

	// material names
	char (*mats)[64] = NULL;
	if (mc > 0) {
		if (pos + (size_t)mc * 64 > len) {
			free(subs);
			seterr(err, errlen, "material table runs past the blob (%u entries)",
				   (unsigned)mc);
			return 0;
		}
		mats = (char (*)[64])calloc(mc, 64);
		if (!mats) { free(subs); seterr(err, errlen, "out of memory"); return 0; }
		for (uint32_t i = 0; i < mc; i++) {
			memcpy(mats[i], blob + pos + (size_t)i * 64, 64);
			mats[i][63] = '\0';     /* names are NUL padded, never trust it */
		}
	}
	pos += (size_t)mc * 64;

	size_t vbase = pos;
	if (vc > 0 && vbase + (size_t)vc * stride - 1 > len) {
		vc = (uint32_t)((len - vbase) / stride);
		clamped = 1;
	}

	gtav_vertex *verts = NULL;
	if (vc > 0) {
		verts = (gtav_vertex *)malloc((size_t)vc * sizeof(gtav_vertex));
		if (!verts) {
			free(subs); free(mats);
			seterr(err, errlen, "out of memory");
			return 0;
		}
		// flags bit0 lightmap uv, bit1 normals
		int has_lm = (flags & 1u) != 0;
		int has_nrm = (flags & 2u) != 0;
		for (uint32_t i = 0; i < vc; i++) {
			const uint8_t *v = blob + vbase + (size_t)i * stride;
			gtav_vertex *o = &verts[i];
			o->x = rd_f32(v);
			o->y = rd_f32(v + 4);
			o->z = rd_f32(v + 8);
			o->u = rd_f32(v + 12);
			o->v = rd_f32(v + 16);
			o->nx = o->ny = o->nz = 0.0f;

			if (has_nrm) {
				const uint8_t *n = v + (has_lm ? 24 : 20);
				if (n + 2 < v + stride) {
					int8_t nx = (int8_t)n[0];
					int8_t ny = (int8_t)n[1];
					int8_t nz = (int8_t)n[2];
					o->nx = (float)nx / 127.0f;
					o->ny = (float)ny / 127.0f;
					o->nz = (float)nz / 127.0f;
				}
			}
		}
	}
	pos = vbase + (size_t)vc * stride;

	size_t usable = (len > pos) ? (len - pos) / 2 : 0;
	if (ic > usable) { ic = (uint32_t)usable; clamped = 1; }

	uint16_t *idx = NULL;
	if (ic > 0) {
		idx = (uint16_t *)malloc((size_t)ic * sizeof(uint16_t));
		if (!idx) {
			free(subs); free(mats); free(verts);
			seterr(err, errlen, "out of memory");
			return 0;
		}
		for (uint32_t i = 0; i < ic; i++) {
			idx[i] = rd_u16(blob + pos + (size_t)i * 2);
		}
	}

	for (uint32_t i = 0; i < sc; i++) {
		uint32_t first = subs[i].indexOffset;
		uint32_t count = subs[i].indexCount;
		if (first > ic) {
			count = 0;
		} else if (first + count > ic) {
			count = ic - first;
			clamped = 1;
		}
		subs[i].indexCount = count;
		if (subs[i].vertexOffset > vc) {
			subs[i].vertexOffset = 0;
			clamped = 1;
		}
	}

	out->version      = version;
	out->flags        = flags;
	out->vertexCount  = vc;
	out->indexCount   = ic;
	out->submeshCount = sc;
	out->materialCount= mc;
	out->stride       = stride;
	out->clamped      = clamped;
	out->materials    = mats;
	out->vertices     = verts;
	out->indices      = idx;
	out->submeshes    = subs;

	if (clamped && err && errlen) {
		snprintf(err, errlen,
				 "header claimed %u verts / %u indices, decoded %u / %u",
				 (unsigned)hdrVerts, (unsigned)hdrIdx,
				 (unsigned)vc, (unsigned)ic);
	}
	return 1;
}
