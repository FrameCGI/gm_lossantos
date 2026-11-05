#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gtav_decode.h"
#include "gtav_pages.h"
#include "gtav_shadow.h"

#define TILE_DIM 512
#define TILE_BASE (-256)

static float    g_texel = 16.0f;
static uint32_t g_tileTexels = 256;
static float    g_uLo, g_vLo, g_tileWorld;
static float    g_bu[3], g_bv[3], g_bd[3];
static float    g_sunAzim, g_sunElev;

static uint16_t **g_tiles;         /* TILE_DIM^2 slots, lazily allocated */
static uint32_t  g_tilesUsed;
static float     g_dMin = 3.0e38f, g_dMax = -3.0e38f;
static float     g_dLo, g_dInvScale, g_dQ;

static uint64_t g_blobs, g_tris, g_verts, g_rasterized, g_writes;
static uint64_t g_degenerate, g_oob, g_skippedW, g_skippedOther, g_decodeFail;

struct page { int px, py; };
static struct page *g_pages;
static int g_npages;

// just dies. fine for a tool, shit for a library
static void fail(const char *msg) {
	fprintf(stderr, "gtav_bake: %s\n", msg);
	exit(2);
}

// same operand order as gtav_shadow.c
static void project(float x, float y, float z,
					float *u, float *v, float *d) {
	*u = x * g_bu[0] + y * g_bu[1] + z * g_bu[2];
	*v = x * g_bv[0] + y * g_bv[1] + z * g_bv[2];
	*d = x * g_bd[0] + y * g_bd[1] + z * g_bd[2];
}

static void wr_u32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v & 0xff);         p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff); p[3] = (uint8_t)((v >> 24) & 0xff);
}
static void wr_u16(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
}
static void wr_f32(uint8_t *p, float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	wr_u32(p, u);
}


static int blob_is_occluder(const char *name) {
	const char *base = strrchr(name, '/');
	const char *b2 = strrchr(name, '\\');
	if (b2 && (!base || b2 > base)) base = b2;
	base = base ? base + 1 : name;
	// w_ water, sun goes through
	if (base[0] == 'w' && base[1] == '_') return 0;
	// lod1-4 copies, lod0 already covers them
	if (base[0] == 't' || base[0] == 's') {
		int underscores = 0;
		for (const char *p = base; *p && *p != '.'; p++) underscores += *p == '_';
		if ((base[0] == 't' && underscores >= 4) || (base[0] == 's' && underscores >= 3)) return 0;
	}
	return 1;
}

typedef void (*blob_fn)(const uint8_t *blob, size_t len);

static void walk_pages(blob_fn fn) {
	for (int i = 0; i < g_npages; i++) {
		const gtav_page *p = gtav_pages_open('v', g_pages[i].px * 8, g_pages[i].py * 8);
		if (!p) continue;
		gtav_pages_lock();
		uint32_t n = gtav_pages_count(p);
		for (uint32_t k = 0; k < n; k++) {
			const char *name = NULL;
			const uint8_t *blob = NULL;
			size_t len = 0;
			if (!gtav_pages_enum(p, k, &name, &blob, &len)) continue;
			if (!blob || len < 32 || memcmp(blob, "GVT1", 4) != 0) {
				g_skippedOther++;
				continue;
			}
			if (!blob_is_occluder(name ? name : "")) {
				g_skippedW++;
				continue;
			}
			fn(blob, len);
		}
		gtav_pages_unlock();
		if ((i % 100) == 0 || i == g_npages - 1) {
			printf("  page %d/%d  blobs %llu  tris %llu  tiles %u\r",
				   i + 1, g_npages, (unsigned long long)g_blobs,
				   (unsigned long long)g_tris, (unsigned)g_tilesUsed);
			fflush(stdout);
		}
	}
	printf("\n");
}

// pass 1, depth range
static void measure_range(const uint8_t *blob, size_t len) {
	gtav_tile t;
	char err[256];
	if (!gtav_tile_decode(blob, len, &t, err, sizeof(err))) {
		g_decodeFail++;
		return;
	}
	g_blobs++;
	g_verts += t.vertexCount;
	for (uint32_t i = 0; i < t.vertexCount; i++) {
		float u, v, d;
		project(t.vertices[i].x, t.vertices[i].y, t.vertices[i].z, &u, &v, &d);
		if (d < g_dMin) g_dMin = d;
		if (d > g_dMax) g_dMax = d;
	}
	gtav_tile_free(&t);
}

static uint16_t *tile_at(int32_t tu, int32_t tv) {
	if (tu < 0 || tv < 0 || tu >= TILE_DIM || tv >= TILE_DIM) return NULL;
	size_t slot = (size_t)tv * TILE_DIM + (size_t)tu;
	uint16_t *t = g_tiles[slot];
	if (!t) {
		t = (uint16_t *)calloc((size_t)g_tileTexels * g_tileTexels,
							   sizeof(uint16_t));
		if (!t) fail("out of memory allocating a shadow tile");
		g_tiles[slot] = t;
		g_tilesUsed++;
	}
	return t;
}

static void raster_tri(const gtav_vertex *a, const gtav_vertex *b,
					   const gtav_vertex *c) {
	float u0, v0, d0, u1, v1, d1, u2, v2, d2;
	project(a->x, a->y, a->z, &u0, &v0, &d0);
	project(b->x, b->y, b->z, &u1, &v1, &d1);
	project(c->x, c->y, c->z, &u2, &v2, &d2);

	float umin = u0 < u1 ? (u0 < u2 ? u0 : u2) : (u1 < u2 ? u1 : u2);
	float umax = u0 > u1 ? (u0 > u2 ? u0 : u2) : (u1 > u2 ? u1 : u2);
	float vmin = v0 < v1 ? (v0 < v2 ? v0 : v2) : (v1 < v2 ? v1 : v2);
	float vmax = v0 > v1 ? (v0 > v2 ? v0 : v2) : (v1 > v2 ? v1 : v2);

	float span = (float)(TILE_DIM * (int32_t)g_tileTexels);
	float f0 = (umin - g_uLo) / g_texel;
	float f1 = (umax - g_uLo) / g_texel;
	float f2 = (vmin - g_vLo) / g_texel;
	float f3 = (vmax - g_vLo) / g_texel;
	if (!(f1 >= 0.0f) || !(f3 >= 0.0f) || f0 >= span || f2 >= span) return;

	int32_t iu0 = (int32_t)floorf(f0 < 0.0f ? 0.0f : f0);
	int32_t iu1 = (int32_t)floorf(f1 > span - 1.0f ? span - 1.0f : f1);
	int32_t iv0 = (int32_t)floorf(f2 < 0.0f ? 0.0f : f2);
	int32_t iv1 = (int32_t)floorf(f3 > span - 1.0f ? span - 1.0f : f3);
	if (iu0 > iu1 || iv0 > iv1) return;

	float area = (u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0);
	if (area == 0.0f) { g_degenerate++; return; }
	float inv = 1.0f / area;

	int32_t tt = (int32_t)g_tileTexels;
	for (int32_t iv = iv0; iv <= iv1; iv++) {
		float pv = g_vLo + ((float)iv + 0.5f) * g_texel;
		uint16_t *tile = NULL;
		int32_t tileRow = -1;
		for (int32_t iu = iu0; iu <= iu1; iu++) {
			float pu = g_uLo + ((float)iu + 0.5f) * g_texel;

			float w0 = ((u1 - pu) * (v2 - pv) - (v1 - pv) * (u2 - pu)) * inv;
			float w1 = ((u2 - pu) * (v0 - pv) - (v2 - pv) * (u0 - pu)) * inv;
			float w2 = 1.0f - w0 - w1;
			if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

			float d = w0 * d0 + w1 * d1 + w2 * d2;
			uint32_t code = (uint32_t)((d - g_dLo) * g_dQ + 0.5f);
			if (code > 65535u) code = 65535u;

			int32_t tv = iv / tt;
			if (!tile || tileRow != tv) {
				tile = tile_at(iu / tt, tv);
				tileRow = tv;
			}
			if (!tile) { g_oob++; continue; }

			int32_t cx = iu - (iu / tt) * tt;
			int32_t cy = iv - tv * tt;
			uint16_t *slot = &tile[(size_t)cy * g_tileTexels + (size_t)cx];
			// max d wins
			if ((uint16_t)code > *slot) {
				*slot = (uint16_t)code;
				g_writes++;
			}
		}
	}
	g_rasterized++;
}

// pass 2, rasterize
static void raster_blob(const uint8_t *blob, size_t len) {
	gtav_tile t;
	char err[256];
	if (!gtav_tile_decode(blob, len, &t, err, sizeof(err))) {
		g_decodeFail++;
		return;
	}
	g_blobs++;
	for (uint32_t s = 0; s < t.submeshCount; s++) {
		const gtav_submesh *sm = &t.submeshes[s];
		if (sm->material < t.materialCount &&
			strncmp(t.materials[sm->material], "models/gtav/water/", 18) == 0)
			continue;
		uint64_t end = (uint64_t)sm->indexOffset + sm->indexCount;
		if (end > t.indexCount) continue;
		g_tris += sm->indexCount / 3;
		for (uint32_t i = sm->indexOffset; i + 2 < (uint32_t)end; i += 3) {
			uint32_t a = t.indices[i] + sm->vertexOffset, b = t.indices[i + 1] + sm->vertexOffset,
					 c = t.indices[i + 2] + sm->vertexOffset;
			if (a >= t.vertexCount || b >= t.vertexCount || c >= t.vertexCount) continue;
			raster_tri(&t.vertices[a], &t.vertices[b], &t.vertices[c]);
		}
	}
	gtav_tile_free(&t);
}

static void write_map(const char *outPath) {
	size_t tileBytes = (size_t)g_tileTexels * g_tileTexels * 2;
	size_t ntab = (size_t)TILE_DIM * TILE_DIM;
	uint32_t off = (uint32_t)(GVSD_HEADER + ntab * 4);

	uint8_t hdr[GVSD_HEADER];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, "GVSD", 4);
	wr_u16(hdr + 4, GVSD_VERSION);
	wr_u16(hdr + 6, 0);
	wr_f32(hdr + 8, g_sunAzim);
	wr_f32(hdr + 12, g_sunElev);
	for (int i = 0; i < 3; i++) {
		wr_f32(hdr + 16 + i * 4, g_bu[i]);
		wr_f32(hdr + 28 + i * 4, g_bv[i]);
		wr_f32(hdr + 40 + i * 4, g_bd[i]);
	}
	wr_f32(hdr + 52, g_uLo);
	wr_f32(hdr + 56, g_vLo);
	wr_f32(hdr + 60, g_texel);
	wr_u32(hdr + 64, g_tileTexels);
	wr_u32(hdr + 68, TILE_DIM);
	wr_u32(hdr + 72, TILE_DIM);
	wr_f32(hdr + 76, g_dLo);
	wr_f32(hdr + 80, g_dInvScale);
	wr_u32(hdr + 84, 0);

	uint8_t *tab = (uint8_t *)malloc(ntab * 4);
	if (!tab) fail("out of memory for the offset table");
	for (size_t i = 0; i < ntab; i++) {
		if (g_tiles[i]) {
			wr_u32(tab + i * 4, off);
			off += (uint32_t)tileBytes;
		} else {
			wr_u32(tab + i * 4, 0);
		}
	}

	FILE *f = fopen(outPath, "wb");
	if (!f) fail("cannot open the output file for writing");
	fwrite(hdr, 1, sizeof(hdr), f);
	fwrite(tab, 1, ntab * 4, f);
	for (size_t i = 0; i < ntab; i++) {
		if (g_tiles[i]) fwrite(g_tiles[i], 1, tileBytes, f);
	}
	fclose(f);
	free(tab);
}

static void verify_page(void) {
	if (g_npages == 0) return;
	const gtav_page *p = gtav_pages_open('v', g_pages[0].px * 8, g_pages[0].py * 8);
	if (!p) return;
	gtav_pages_lock();
	uint32_t n = gtav_pages_count(p);
	uint64_t nv = 0, ns = 0, nblobs = 0;
	float vmin = 1.0f, vmax = 0.0f, vsum = 0.0f;
	for (uint32_t k = 0; k < n; k++) {
		const char *name = NULL;
		const uint8_t *blob = NULL;
		size_t len = 0;
		gtav_tile t;
		char err[256];
		if (!gtav_pages_enum(p, k, &name, &blob, &len)) continue;
		if (!blob || len < 32 || memcmp(blob, "GVT1", 4) != 0) continue;
		if (!blob_is_occluder(name ? name : "")) continue;
		if (!gtav_tile_decode(blob, len, &t, err, sizeof(err))) continue;
		nblobs++;
		for (uint32_t i = 0; i < t.vertexCount; i++) {
			const gtav_vertex *v = &t.vertices[i];
			float vis = gtav_shadow_visibility(v->x, v->y, v->z,
											 v->nx, v->ny, v->nz);
			nv++;
			if (vis < 0.5f) ns++;
			if (vis < vmin) vmin = vis;
			if (vis > vmax) vmax = vis;
			vsum += vis;
		}
		gtav_tile_free(&t);
	}
	gtav_pages_unlock();
	if (nv > 0) {
		printf("  verify page %d %d: %llu blobs, %llu verts, vis min %.2f "
			   "mean %.2f max %.2f, %.1f%% shadowed\n",
			   g_pages[0].px, g_pages[0].py, (unsigned long long)nblobs,
			   (unsigned long long)nv,
			   (double)vmin, (double)(vsum / (float)nv), (double)vmax,
			   100.0 * (double)ns / (double)nv);
	} else {
		printf("  verify: no occluder vertices found in page %d %d\n",
			   g_pages[0].px, g_pages[0].py);
	}
}

int main(int argc, char **argv) {
	char err[256];

	if (argc < 3) {
		fprintf(stderr,
				"usage: gtav_bake.exe <mapdir> <out.gvsd> [texelU] [tileTexels]"
				" [sunAzimDeg] [sunElevDeg]\n");
		return 2;
	}
	const char *mapdir = argv[1];
	const char *outPath = argv[2];
	if (argc > 3) g_texel = (float)atof(argv[3]);
	if (argc > 4) g_tileTexels = (uint32_t)atoi(argv[4]);
	float azimDeg = 225.0f, elevDeg = 52.0f;
	if (argc > 5) azimDeg = (float)atof(argv[5]);
	if (argc > 6) elevDeg = (float)atof(argv[6]);
	if (!(g_texel > 0.0f) || g_tileTexels == 0 || g_tileTexels > 4096) {
		fail("implausible texel/tile sizes");
	}

	g_sunAzim = azimDeg * (3.14159265f / 180.0f);
	g_sunElev = elevDeg * (3.14159265f / 180.0f);
	float ce = cosf(g_sunElev), se = sinf(g_sunElev);
	float ca = cosf(g_sunAzim), sa = sinf(g_sunAzim);
	g_bu[0] = -sa;  g_bu[1] = ca;  g_bu[2] = 0.0f;
	g_bd[0] = ce * ca; g_bd[1] = ce * sa; g_bd[2] = se;
	g_bv[0] = -se * ca; g_bv[1] = -se * sa; g_bv[2] = ce;

	g_tileWorld = g_texel * (float)g_tileTexels;
	g_uLo = (float)TILE_BASE * g_tileWorld;
	g_vLo = (float)TILE_BASE * g_tileWorld;

	if (!gtav_pages_set_root(mapdir)) fail("cannot use the map directory");
	{
		char pattern[MAX_PATH];
		snprintf(pattern, sizeof(pattern), "%s\\pages\\v_*.bin", mapdir);
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(pattern, &fd);
		if (h == INVALID_HANDLE_VALUE) fail("no visual pages found");
		int cap = 256;
		g_pages = (struct page *)malloc((size_t)cap * sizeof(struct page));
		if (!g_pages) fail("out of memory");
		do {
			int px, py;
			if (sscanf(fd.cFileName, "v_%d_%d.bin", &px, &py) != 2) continue;
			if (g_npages == cap) {
				cap *= 2;
				g_pages = (struct page *)realloc(
					g_pages, (size_t)cap * sizeof(struct page));
				if (!g_pages) fail("out of memory");
			}
			g_pages[g_npages].px = px;
			g_pages[g_npages].py = py;
			g_npages++;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}

	printf("gtav_bake: %d pages, sun elev %.1f azim %.1f, %.0f u/texel, "
		   "tile %u, grid %d x %d\n",
		   g_npages, (double)elevDeg, (double)azimDeg, (double)g_texel,
		   (unsigned)g_tileTexels, TILE_DIM, TILE_DIM);

	printf("  pass 1/2: depth range\n");
	walk_pages(measure_range);
	if (g_dMax < g_dMin) fail("no geometry found");
	g_dLo = g_dMin - 64.0f;
	g_dInvScale = (g_dMax + 64.0f - g_dLo) / 65535.0f;
	g_dQ = 1.0f / g_dInvScale;
	printf("  depth range %.1f .. %.1f  ->  %.3f u/code\n",
		   (double)g_dMin, (double)g_dMax, (double)g_dInvScale);

	g_tiles = (uint16_t **)calloc((size_t)TILE_DIM * TILE_DIM,
								  sizeof(uint16_t *));
	if (!g_tiles) fail("out of memory for the tile grid");

	printf("  pass 2/2: rasterize\n");
	g_blobs = 0;
	walk_pages(raster_blob);
	printf("  rasterized %llu tris (%llu degenerate, %llu out of grid), "
		   "%llu texel writes, %llu blobs (%llu w_ skipped, %llu decode "
		   "failures)\n",
		   (unsigned long long)g_rasterized,
		   (unsigned long long)g_degenerate,
		   (unsigned long long)g_oob,
		   (unsigned long long)g_writes,
		   (unsigned long long)g_blobs,
		   (unsigned long long)g_skippedW,
		   (unsigned long long)g_decodeFail);

	write_map(outPath);
	{
		FILE *f = fopen(outPath, "rb");
		long sz = 0;
		if (f) {
			fseek(f, 0, SEEK_END);
			sz = ftell(f);
			fclose(f);
		}
		printf("  wrote %s: %u tiles, %.1f MB\n",
			   outPath, (unsigned)g_tilesUsed,
			   (double)sz / (1024.0 * 1024.0));
	}

	if (!gtav_shadow_open_file(outPath, err, sizeof(err))) {
		fprintf(stderr, "  verify FAILED: %s\n", err);
		return 1;
	}
	printf("  verify: %s\n", gtav_shadow_info());
	verify_page();
	gtav_shadow_close();
	return 0;
}
