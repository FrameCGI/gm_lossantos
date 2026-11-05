#include "gtav_shadow.h"

#include <windows.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GVSD_MAGIC 0x44535647u   /* "GVSD" little endian */
#define GVSD_VERSION 1
#define GVSD_HEADER 88

struct shadow_map {
	HANDLE   file;
	HANDLE   mapping;
	uint8_t *view;
	size_t   size;

	float    sunAzim, sunElev;
	// sun basis, bd points at the sun
	float    bu[3], bv[3], bd[3];
	float    uLo, vLo, texelSize;
	float    dLo, dInvScale;
	uint32_t tileTexels, tilesX, tilesY;
	const uint32_t *offsets;

	uint32_t usedTiles;      /* diagnostics for gtav_shadow_info */
	char     info[192];
};

static struct shadow_map S;
static int   g_baked = 0;

#include "gtav_light_tc.h"

static float g_ambient = 2.0f;   /* scale on DIRAMB + SKY + BOUNCE */
static float g_sun = 1.0f;       /* scale on SUN */
static float g_expo = 2.5f;      /* filmic input of the sunlit reference */
static float g_k = 0.0f;         /* exposure multiplier (derived) */
static float g_norm = 1.0f;      /* 1 / filmic(g_expo)  (derived) */

static float filmic(float x) {
	const float A = TC_FILMIC[0], B = TC_FILMIC[1], C = TC_FILMIC[2],
				D = TC_FILMIC[3], E = TC_FILMIC[4], F = TC_FILMIC[5];
	if (x < 0.0f) x = 0.0f;
	return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// sunlit up facing surface comes out as 1
static void light_ref_update(void) {
	float sl = S.view ? S.bd[2] : 0.79f;   /* sin(sun elevation) */
	float ref[3], lum, f;
	int i;
	if (sl < 0.05f) sl = 0.05f;
	for (i = 0; i < 3; i++)
		ref[i] = TC_SUN[i] * g_sun * sl
			   + g_ambient * (TC_DIRAMB[i] * (0.5f + 0.5f * sl) + TC_SKY[i]);
	lum = 0.2126f * ref[0] + 0.7152f * ref[1] + 0.0722f * ref[2];
	g_k = lum > 1e-6f ? g_expo / lum : 0.0f;
	f = filmic(g_expo);
	g_norm = f > 1e-6f ? 1.0f / f : 1.0f;
}

static void seterr(char *err, size_t errlen, const char *fmt, ...) {
	va_list ap;
	if (!err || errlen == 0) return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t rd_u32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) {
	return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static float rd_f32(const uint8_t *p) {
	uint32_t u = rd_u32(p);
	float f;
	memcpy(&f, &u, sizeof(f));
	return f;
}

void gtav_shadow_close(void) {
	if (S.view) UnmapViewOfFile(S.view);
	if (S.mapping) CloseHandle(S.mapping);
	if (S.file && S.file != INVALID_HANDLE_VALUE) CloseHandle(S.file);
	memset(&S, 0, sizeof(S));
	S.file = INVALID_HANDLE_VALUE;
}

int gtav_shadow_active(void) { return S.view != NULL; }

void gtav_shadow_set_baked(int on) { g_baked = on ? 1 : 0; }
int  gtav_shadow_baked(void)       { return g_baked; }

void gtav_shadow_set_light(float ambient, float sun, float exposure) {
	g_ambient = clampf(ambient, 0.0f, 8.0f);
	g_sun = clampf(sun, 0.0f, 8.0f);
	if (exposure > 0.0f) g_expo = clampf(exposure, 0.1f, 16.0f);
	light_ref_update();
}

void gtav_shadow_get_light(float *ambient, float *sun, float *exposure) {
	if (ambient) *ambient = g_ambient;
	if (sun) *sun = g_sun;
	if (exposure) *exposure = g_expo;
}

void gtav_shadow_sun(float *azimRad, float *elevRad) {
	if (S.view) {
		if (azimRad) *azimRad = S.sunAzim;
		if (elevRad) *elevRad = S.sunElev;
	} else {
		if (azimRad) *azimRad = 0.0f;
		if (elevRad) *elevRad = 0.0f;
	}
}

int gtav_shadow_open(const char *mapRoot, char *err, size_t errlen) {
	char path[MAX_PATH];
	if (!mapRoot || !mapRoot[0]) {
		seterr(err, errlen, "no map root (gtav.pages_init first)");
		return 0;
	}
	snprintf(path, sizeof(path), "%s\\sun_shadow.gvsd", mapRoot);
	return gtav_shadow_open_file(path, err, errlen);
}

int gtav_shadow_open_file(const char *path, char *err, size_t errlen) {
	LARGE_INTEGER sz;

	gtav_shadow_close();
	if (!path || !path[0]) {
		seterr(err, errlen, "no shadow map path");
		return 0;
	}

	S.file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
						 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (S.file == INVALID_HANDLE_VALUE) {
		seterr(err, errlen, "sun_shadow.gvsd missing at %s", path);
		return 0;
	}
	if (!GetFileSizeEx(S.file, &sz) || sz.QuadPart < GVSD_HEADER + 4) {
		seterr(err, errlen, "sun_shadow.gvsd is truncated");
		gtav_shadow_close();
		return 0;
	}
	S.size = (size_t)sz.QuadPart;
	S.mapping = CreateFileMappingA(S.file, NULL, PAGE_READONLY, 0, 0, NULL);
	if (!S.mapping) {
		seterr(err, errlen, "CreateFileMapping failed (%lu)", GetLastError());
		gtav_shadow_close();
		return 0;
	}
	S.view = (uint8_t *)MapViewOfFile(S.mapping, FILE_MAP_READ, 0, 0, 0);
	if (!S.view) {
		seterr(err, errlen, "MapViewOfFile failed (%lu)", GetLastError());
		gtav_shadow_close();
		return 0;
	}

	const uint8_t *h = S.view;
	if (rd_u32(h) != GVSD_MAGIC || rd_u16(h + 4) != GVSD_VERSION) {
		seterr(err, errlen, "sun_shadow.gvsd: bad magic/version");
		gtav_shadow_close();
		return 0;
	}
	S.sunAzim  = rd_f32(h + 8);
	S.sunElev  = rd_f32(h + 12);
	for (int i = 0; i < 3; i++) {
		S.bu[i] = rd_f32(h + 16 + i * 4);
		S.bv[i] = rd_f32(h + 28 + i * 4);
		S.bd[i] = rd_f32(h + 40 + i * 4);
	}
	S.uLo        = rd_f32(h + 52);
	S.vLo        = rd_f32(h + 56);
	S.texelSize  = rd_f32(h + 60);
	S.tileTexels = rd_u32(h + 64);
	S.tilesX     = rd_u32(h + 68);
	S.tilesY     = rd_u32(h + 72);
	S.dLo        = rd_f32(h + 76);
	S.dInvScale  = rd_f32(h + 80);

	if (S.tileTexels == 0 || S.tileTexels > 4096 ||
		S.tilesX == 0 || S.tilesY == 0 ||
		S.tilesX > 65536 || S.tilesY > 65536 ||
		!(S.texelSize > 0.0f) || !(S.dInvScale > 0.0f)) {
		seterr(err, errlen, "sun_shadow.gvsd: implausible header");
		gtav_shadow_close();
		return 0;
	}
	size_t ntab = (size_t)S.tilesX * (size_t)S.tilesY;
	if (GVSD_HEADER + ntab * 4 > S.size) {
		seterr(err, errlen, "sun_shadow.gvsd: offset table runs past the file");
		gtav_shadow_close();
		return 0;
	}
	S.offsets = (const uint32_t *)(const void *)(S.view + GVSD_HEADER);

	size_t tileBytes = (size_t)S.tileTexels * S.tileTexels * 2;
	for (size_t i = 0; i < ntab; i++) {
		uint32_t off = S.offsets[i];
		if (!off) continue;
		if ((size_t)off + tileBytes > S.size) {
			seterr(err, errlen, "sun_shadow.gvsd: tile %u runs past the file",
				   (unsigned)i);
			gtav_shadow_close();
			return 0;
		}
		S.usedTiles++;
	}

	snprintf(S.info, sizeof(S.info),
			 "sun_shadow.gvsd: %u tiles, %.1f MB, %.0f u/texel, sun elev %.1f "
			 "azim %.1f",
			 (unsigned)S.usedTiles, (double)S.size / (1024.0 * 1024.0),
			 (double)S.texelSize,
			 (double)(S.sunElev * (180.0f / 3.14159265f)),
			 (double)(S.sunAzim * (180.0f / 3.14159265f)));
	light_ref_update();   /* the reference depends on the sun elevation */
	return 1;
}

const char *gtav_shadow_info(void) {
	return S.view ? S.info : "sun_shadow.gvsd: not loaded";
}

// max depth at a texel, -1e30 = nothing there
static float shadow_maxd(int32_t sx, int32_t sy) {
	uint32_t spanU = S.tilesX * S.tileTexels;
	uint32_t spanV = S.tilesY * S.tileTexels;
	if (sx < 0 || sy < 0 || (uint32_t)sx >= spanU || (uint32_t)sy >= spanV) {
		return -1e30f;
	}
	uint32_t tx = (uint32_t)sx / S.tileTexels;
	uint32_t ty = (uint32_t)sy / S.tileTexels;
	uint32_t off = S.offsets[(size_t)ty * S.tilesX + tx];
	if (!off) return -1e30f;

	const uint16_t *tile =
		(const uint16_t *)(const void *)(S.view + off);
	uint32_t cx = (uint32_t)sx - tx * S.tileTexels;
	uint32_t cy = (uint32_t)sy - ty * S.tileTexels;
	return S.dLo + (float)tile[cy * S.tileTexels + cx] * S.dInvScale;
}

float gtav_shadow_visibility(float x, float y, float z,
						   float nx, float ny, float nz) {
	if (!S.view) return 1.0f;

	float u = x * S.bu[0] + y * S.bu[1] + z * S.bu[2];
	float v = x * S.bv[0] + y * S.bv[1] + z * S.bv[2];
	float d = x * S.bd[0] + y * S.bd[1] + z * S.bd[2];

	float fu = (u - S.uLo) / S.texelSize;
	float fv = (v - S.vLo) / S.texelSize;
	float spanU = (float)(S.tilesX * S.tileTexels);
	float spanV = (float)(S.tilesY * S.tileTexels);
	if (!(fu >= 0.0f) || !(fv >= 0.0f) || fu >= spanU || fv >= spanV) {
		return 1.0f;
	}

	float len = sqrtf(nx * nx + ny * ny + nz * nz);
	float ndl = 1.0f;
	if (len > 1e-6f) {
		ndl = (nx * S.bd[0] + ny * S.bd[1] + nz * S.bd[2]) / len;
	}
	// slope scaled bias
	float cos2 = 1.0f - ndl * ndl;
	float slope = sqrtf(cos2 < 0.0f ? 0.0f : cos2) /
				  (fabsf(ndl) < 0.25f ? 0.25f : fabsf(ndl));
	float bias = S.texelSize * clampf(0.5f + slope, 0.5f, 6.0f);
	float soft = S.texelSize;

	int32_t iu0 = (int32_t)fu;
	int32_t iv0 = (int32_t)fv;

	float gap = shadow_maxd(iu0, iv0) - d;
	if (!(gap > 0.0f)) gap = 0.0f;
	// penumbra widens with occluder distance
	float radius = 0.75f + gap * 0.25f / S.texelSize;
	if (radius > 3.5f) radius = 3.5f;
	int32_t r = radius < 1.5f ? 1 : 2;

	float acc = 0.0f, wsum = 0.0f;
	for (int32_t dy = -r; dy <= r; dy++) {
		for (int32_t dx = -r; dx <= r; dx++) {
			float cx = (float)(iu0 + dx) + 0.5f - fu;
			float cy = (float)(iv0 + dy) + 0.5f - fv;
			float w = 1.0f - sqrtf(cx * cx + cy * cy) / radius;
			if (!(w > 0.0f)) continue;
			float maxd = shadow_maxd(iu0 + dx, iv0 + dy);
			float vis = 1.0f - ((maxd - d) - bias) / soft;
			if (!(vis > 0.0f)) vis = 0.0f;      /* also catches NaN */
			else if (vis > 1.0f) vis = 1.0f;
			acc += w * vis;
			wsum += w;
		}
	}
	if (!(wsum > 0.0f)) return 1.0f;
	return acc / wsum;
}

void gtav_shadow_shade_rgb(float x, float y, float z,
						 float nx, float ny, float nz, float out[3]) {
	if (!S.view) {
		out[0] = out[1] = out[2] = 1.0f;
		return;
	}

	float len = sqrtf(nx * nx + ny * ny + nz * nz);
	float ndls = 0.0f;   /* signed n.l, for the wrapped directional ambient */
	float nz1 = 0.0f;
	if (len > 1e-6f) {
		ndls = (nx * S.bd[0] + ny * S.bd[1] + nz * S.bd[2]) / len;
		nz1 = nz / len;
	}
	float ndl = ndls > 0.0f ? ndls : 0.0f;

	float up = clampf((nz1 + 1.0f) * 0.5f, 0.0f, 1.0f);
	float vis = gtav_shadow_visibility(x, y, z, nx, ny, nz);
	float sunAmt = g_sun * ndl * vis;
	float dirw = 0.5f + 0.5f * ndls;

	for (int i = 0; i < 3; i++) {
		float L = TC_SUN[i] * sunAmt
				+ g_ambient * (TC_DIRAMB[i] * dirw
							   + TC_BOUNCE[i] + (TC_SKY[i] - TC_BOUNCE[i]) * up);
		out[i] = clampf(filmic(g_k * L) * g_norm, 0.0f, 1.0f);
	}
}
