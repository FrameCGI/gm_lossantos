#include "gtav_decode.h"
#include "gtav_luaapi.h"
#include "gtav_mesh.h"
#include "gtav_pages.h"
#include "gtav_shadow.h"
#include "gtav_prefetch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GTAV_MODULE_NAME "gtav"
#define GTAV_VERSION     "gtav-native-2026-11-04.11-nonormal"

static void seterr(char *err, size_t errlen, const char *fmt, ...) {
	va_list ap;
	if (!err || errlen == 0) return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static double now_ms(void) {
	static LARGE_INTEGER freq;
	LARGE_INTEGER t;
	if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// stats for the last build
static struct {
	double readMs, decodeMs, accMs, meshMs, totalMs;
	int    tiles;   /* tiles that merged successfully */
	int    draws;   /* meshes (draw calls) emitted */
	int    tris;    /* triangles handed to the mesh builder */
	int    meshRetry;
	int    meshSplit;     /* meshes emitted smaller than the full chunk */
	int    meshFail;      /* materials dropped because nothing would build */
	int    meshFailTris;  /* triangles lost to those refusals */
	int    flagNames;
	double flagMs;
	int    handshakes;
	int    flagCacheN;
} BG;

// per material accumulator
struct acc {
	char  name[64];
	uint32_t hash;          /* namehash(name); the lookup compares this first */
	int   glass;
	int   nocull;
	int   flagsDone;
	gtav_vertex *verts;
	uint32_t  nverts, capV;
	float    *shade;
	uint32_t  capS;         /* vertex capacity of `shade` (3 * capS floats) */
	uint32_t  *idx;
	uint32_t  nidx, capI;
};

struct group {
	struct acc *items;
	uint32_t n, cap;

	uint32_t *lut;
	uint32_t  lutCap;       /* always a power of two, 0 until first insert */

	uint32_t *remap;
	uint32_t *touched;
	uint32_t  scratchCap;   /* entries in each, >= tile vertexCount */
};

static void group_free(struct group *g) {
	for (uint32_t i = 0; i < g->cap; i++) {
		free(g->items[i].verts);
		free(g->items[i].shade);
		free(g->items[i].idx);
	}
	free(g->items);
	free(g->lut);
	free(g->remap);
	free(g->touched);
	g->items = NULL;
	g->lut = NULL;
	g->remap = NULL;
	g->touched = NULL;
	g->n = g->cap = 0;
	g->lutCap = 0;
	g->scratchCap = 0;
}

static void group_reset(struct group *g) {
	g->n = 0;
	if (g->lut && g->lutCap) {
		memset(g->lut, 0, (size_t)g->lutCap * sizeof(uint32_t));
	}
}

// scratch, GTILE for build_tile, GBUILD for build_group
static struct group GTILE;
static struct group GBUILD;

// fnv1a
static uint32_t namehash(const char *s) {
	uint32_t h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		h ^= (uint32_t)*p;
		h *= 16777619u;
	}
	return h;
}

static int group_rehash(struct group *g, uint32_t want) {
	uint32_t ncap = 64;
	while (ncap < want) ncap *= 2;

	uint32_t *nl = (uint32_t *)malloc((size_t)ncap * sizeof(uint32_t));
	if (!nl) return 0;
	memset(nl, 0, (size_t)ncap * sizeof(uint32_t));

	uint32_t mask = ncap - 1;
	for (uint32_t i = 0; i < g->n; i++) {
		uint32_t j = g->items[i].hash & mask;
		while (nl[j]) j = (j + 1) & mask;
		nl[j] = i + 1;
	}
	free(g->lut);
	g->lut = nl;
	g->lutCap = ncap;
	return 1;
}

static struct acc *group_get(struct group *g, const char *name) {
	uint32_t h = namehash(name);

	if (g->lutCap == 0 && !group_rehash(g, 64)) return NULL;

	uint32_t mask = g->lutCap - 1;
	uint32_t j = h & mask;
	while (g->lut[j]) {
		struct acc *a = &g->items[g->lut[j] - 1];
		if (a->hash == h &&
			strncmp(a->name, name, sizeof(a->name)) == 0) {
			return a;
		}
		j = (j + 1) & mask;
	}
	if (g->n + 1 > g->lutCap - (g->lutCap >> 2)) {
		if (!group_rehash(g, g->lutCap * 2)) return NULL;
		mask = g->lutCap - 1;
		j = h & mask;
		while (g->lut[j]) j = (j + 1) & mask;
	}

	if (g->n == g->cap) {
		uint32_t ncap = g->cap ? g->cap * 2 : 8;
		struct acc *ni = (struct acc *)realloc(g->items,
											   (size_t)ncap * sizeof(struct acc));
		if (!ni) return NULL;
		for (uint32_t k = g->cap; k < ncap; k++) {
			ni[k].verts = NULL;
			ni[k].capV = 0;
			ni[k].shade = NULL;
			ni[k].capS = 0;
			ni[k].idx = NULL;
			ni[k].capI = 0;
		}
		g->items = ni;
		g->cap = ncap;
	}
	struct acc *a = &g->items[g->n++];
	{
		gtav_vertex *keepV = a->verts;
		uint32_t    keepCapV = a->capV;
		float      *keepS = a->shade;
		uint32_t    keepCapS = a->capS;
		uint32_t   *keepI = a->idx;
		uint32_t    keepCapI = a->capI;
		memset(a, 0, sizeof(*a));
		a->verts = keepV;
		a->capV = keepCapV;
		a->shade = keepS;
		a->capS = keepCapS;
		a->idx = keepI;
		a->capI = keepCapI;
	}
	snprintf(a->name, sizeof(a->name), "%s", name);
	a->hash = h;
	g->lut[j] = g->n;
	return a;
}

static int acc_reserve(struct acc *a, uint32_t addV, uint32_t addI) {
	if (a->nverts + addV > a->capV) {
		uint32_t ncap = a->capV ? a->capV : 256;
		while (ncap < a->nverts + addV) ncap *= 2;
		gtav_vertex *nv = (gtav_vertex *)realloc(a->verts,
											(size_t)ncap * sizeof(gtav_vertex));
		if (!nv) return 0;
		a->verts = nv;
		a->capV = ncap;
	}
	if (a->nverts + addV > a->capS) {
		uint32_t ncap = a->capS ? a->capS : 256;
		while (ncap < a->nverts + addV) ncap *= 2;
		float *ns = (float *)realloc(a->shade,
									 (size_t)ncap * 3 * sizeof(float));
		if (!ns) return 0;
		a->shade = ns;
		a->capS = ncap;
	}
	if (a->nidx + addI > a->capI) {
		uint32_t ncap = a->capI ? a->capI : 256;
		while (ncap < a->nidx + addI) ncap *= 2;
		uint32_t *ni = (uint32_t *)realloc(a->idx, (size_t)ncap * sizeof(uint32_t));
		if (!ni) return 0;
		a->idx = ni;
		a->capI = ncap;
	}
	return 1;
}

static void read_flags(lua_State *L, int tableIdx, const char *name,
					   int *glass, int *nocull) {
	*glass = 0;
	*nocull = 0;
	if (tableIdx == 0) return;

	OW.getfield(L, tableIdx, name);
	if (OW.type(L, -1) != GTAV_LUA_TTABLE) {
		OW.remove(L, -1);
		return;
	}
	OW.getfield(L, -1, "glass");
	if (OW.type(L, -1) == GTAV_LUA_TBOOLEAN) *glass = OW.toboolean(L, -1);
	OW.remove(L, -1);

	OW.getfield(L, -1, "nocull");
	if (OW.type(L, -1) == GTAV_LUA_TBOOLEAN) *nocull = OW.toboolean(L, -1);
	OW.remove(L, -1);

	OW.remove(L, -1);
}

static void (*flags_for)(lua_State *L, int tableIdx, const char *name,
						 int *glass, int *nocull) = read_flags;

struct job_entry { char file[64]; int tx, ty; };
struct job_flag  { char name[64]; int glass, nocull; };

struct emit_cursor { uint32_t item, tri, lostRun; };

#define JOB_IDLE      0
#define JOB_QUEUED    1
#define JOB_WORKING   2
#define JOB_NEEDFLAGS 3   /* worker blocked on the handshake */
#define JOB_DONE      4
#define JOB_FAILED    5

// the one job slot
static struct {
	struct job_entry *tiles;
	int ntiles, capTiles;
	struct job_entry *tilesPre;
	int ntilesPre, capTilesPre;
	float originX, originY;

	struct job_flag *flags;
	int nflags, capFlags;
	int *flagHash;
	int capHash;

	// past wantCur is prefetch for the next group
	struct job_flag *want;
	int nwant, wantDone, capWant;
	int wantCur;
	unsigned wantGen;

	struct group g;
	int merged, tris;
	double readMs, decodeMs, accMs, prepMs;
	int flagNames;
	double flagMs;
	int flagNamesTot;
	double flagMsTot, prepTot;
	int handRtts;
	char err[256];

	volatile LONG state;
	volatile LONG cancel;
	volatile LONG shutdown;

	CRITICAL_SECTION lock;
	HANDLE evWake;    /* main -> worker: job queued OR handshake answered */
	HANDLE evDone;    /* worker -> main: NEEDFLAGS / DONE / FAILED */
	HANDLE thread;
	int lockInit, threadUp;
} J;

// commit side, group gets swapped out of J
static struct {
	struct group g;
	struct emit_cursor cur;
	int active;
	int merged, tris;
	double readMs, decodeMs, accMs, prepMs;
	int flagNames;
	double flagMs;
	int handRtts;
} JC;

static void commit_reset(void) {
	group_reset(&JC.g);
	JC.active = 0;
	JC.cur.item = 0;
	JC.cur.tri = 0;
	JC.cur.lostRun = 0;
}

static int job_flag_grow(struct job_flag **arr, int *cap, int n) {
	if (n < *cap) return 1;
	int ncap = *cap ? *cap * 2 : 32;
	struct job_flag *p = (struct job_flag *)realloc(*arr, (size_t)ncap * sizeof(struct job_flag));
	if (!p) return 0;
	*arr = p;
	*cap = ncap;
	return 1;
}

static unsigned name_hash(const char *name) {
	unsigned h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++)
		h = (h ^ *p) * 16777619u;
	return h;
}

// caller holds the lock
static void flag_hash_rehash(int ncap) {
	int *nh = (int *)calloc((size_t)ncap, sizeof(int));
	if (!nh) return;
	size_t mask = (size_t)ncap - 1;
	for (int i = 0; i < J.nflags; i++) {
		size_t s = name_hash(J.flags[i].name) & mask;
		while (nh[s]) s = (s + 1) & mask;
		nh[s] = i + 1;
	}
	free(J.flagHash);
	J.flagHash = nh;
	J.capHash = ncap;
}

static int flag_hash_find(const char *name) {
	if (!J.flagHash || J.capHash == 0) return -1;
	size_t mask = (size_t)J.capHash - 1;
	size_t s = name_hash(name) & mask;
	for (;;) {
		int v = J.flagHash[s];
		if (v == 0) return -1;
		if (v - 1 < J.nflags &&
			strncmp(J.flags[v - 1].name, name, sizeof(J.flags[0].name)) == 0)
			return v - 1;
		s = (s + 1) & mask;
	}
}

static void flag_hash_insert(int idx) {
	if (!J.flagHash || (J.nflags + 1) * 2 > J.capHash) {
		int ncap = J.capHash ? J.capHash * 2 : 256;
		while (ncap < (J.nflags + 1) * 2) ncap *= 2;
		flag_hash_rehash(ncap);
		if (!J.flagHash) return;
	}
	size_t mask = (size_t)J.capHash - 1;
	size_t s = name_hash(J.flags[idx].name) & mask;
	while (J.flagHash[s]) s = (s + 1) & mask;
	J.flagHash[s] = idx + 1;
}

static void job_cache_clear(void) {
	if (J.lockInit) EnterCriticalSection(&J.lock);
	J.nflags = 0;
	if (J.flagHash) memset(J.flagHash, 0, (size_t)J.capHash * sizeof(int));
	if (J.lockInit) LeaveCriticalSection(&J.lock);
}

static int job_cache_get(const char *name, int *glass, int *nocull) {
	int hit = 0;
	EnterCriticalSection(&J.lock);
	int i = flag_hash_find(name);
	if (i >= 0) {
		*glass = J.flags[i].glass;
		*nocull = J.flags[i].nocull;
		hit = 1;
	}
	LeaveCriticalSection(&J.lock);
	return hit;
}

static void job_cache_put(const char *name, int glass, int nocull) {
	EnterCriticalSection(&J.lock);
	int i = flag_hash_find(name);
	if (i >= 0) {
		J.flags[i].glass = glass;
		J.flags[i].nocull = nocull;
		LeaveCriticalSection(&J.lock);
		return;
	}
	if (job_flag_grow(&J.flags, &J.capFlags, J.nflags)) {
		snprintf(J.flags[J.nflags].name, sizeof(J.flags[J.nflags].name), "%s", name);
		J.flags[J.nflags].glass = glass;
		J.flags[J.nflags].nocull = nocull;
		flag_hash_insert(J.nflags);
		J.nflags++;
	}
	LeaveCriticalSection(&J.lock);
}

static int job_handshake(void) {
	EnterCriticalSection(&J.lock);
	if (J.wantCur == 0 || J.wantDone >= J.wantCur) {
		LeaveCriticalSection(&J.lock);
		return 1;
	}
	InterlockedExchange(&J.state, JOB_NEEDFLAGS);
	J.handRtts++;
	LeaveCriticalSection(&J.lock);

	SetEvent(J.evDone);
	if (WaitForSingleObject(J.evWake, 60000) != WAIT_OBJECT_0) {
		int n = J.nwant;
		snprintf(J.err, sizeof(J.err),
				 "material flags handshake timed out (%d names)", n);
		InterlockedExchange(&J.state, JOB_FAILED);
		SetEvent(J.evDone);
		return 0;
	}
	if (J.shutdown || J.cancel) {
		snprintf(J.err, sizeof(J.err),
				 J.shutdown ? "shut down while waiting for flags"
							: "cancelled while waiting for flags");
		InterlockedExchange(&J.state, JOB_FAILED);
		SetEvent(J.evDone);
		return 0;
	}
	EnterCriticalSection(&J.lock);
	int ok = (J.state == JOB_WORKING);
	LeaveCriticalSection(&J.lock);
	return ok;
}

static int job_resolve(const char *name, int *glass, int *nocull) {
	if (job_cache_get(name, glass, nocull)) return 1;

	EnterCriticalSection(&J.lock);
	J.nwant = 0;
	J.wantDone = 0;
	J.wantCur = 0;
	J.wantGen++;
	LeaveCriticalSection(&J.lock);
	if (!job_flag_grow(&J.want, &J.capWant, 0)) return 0;
	EnterCriticalSection(&J.lock);
	snprintf(J.want[0].name, sizeof(J.want[0].name), "%s", name);
	J.nwant = 1;
	J.wantDone = 0;
	J.wantCur = 1;
	LeaveCriticalSection(&J.lock);

	if (!job_handshake()) return 0;
	return job_cache_get(name, glass, nocull);
}

static int job_answer(lua_State *L, int flagsIdx, double budgetMs) {
	char (*names)[64] = NULL;
	int n = 0, start = 0;
	unsigned gen = 0;

	EnterCriticalSection(&J.lock);
	if (J.nwant > J.wantDone) {
		start = J.wantDone;
		n = J.nwant - start;
		gen = J.wantGen;
		names = (char (*)[64])malloc((size_t)n * 64);
		if (names) {
			for (int i = 0; i < n; i++)
				snprintf(names[i], 64, "%s", J.want[start + i].name);
		}
	}
	LeaveCriticalSection(&J.lock);

	int answered = 0;
	if (names) {
		struct job_flag *got = (struct job_flag *)calloc((size_t)n, sizeof(struct job_flag));
		if (got) {
			double t0 = now_ms();
			int resolved = 0;
			for (int i = 0; i < n; i++) {
				int glass = 0, nocull = 0;
				snprintf(got[i].name, sizeof(got[i].name), "%s", names[i]);
				flags_for(L, flagsIdx, names[i], &glass, &nocull);
				got[i].glass = glass;
				got[i].nocull = nocull;
				resolved = i + 1;
				if (budgetMs > 0 && n > 3 && now_ms() - t0 >= budgetMs) break;
			}
			double dt = now_ms() - t0;

			EnterCriticalSection(&J.lock);
			if (J.wantGen == gen && J.wantDone == start &&
				start + resolved <= J.nwant) {
				J.wantDone += resolved;
				J.flagNames += resolved;
				J.flagMs += dt;
				answered = resolved;
			}
			J.flagNamesTot += resolved;
			J.flagMsTot += dt;
			LeaveCriticalSection(&J.lock);

			for (int i = 0; i < resolved; i++)
				job_cache_put(got[i].name, got[i].glass, got[i].nocull);
			free(got);
		}
		free(names);
	}

	int wake = 0;
	EnterCriticalSection(&J.lock);
	if (J.state == JOB_NEEDFLAGS && J.wantDone >= J.wantCur) {
		InterlockedExchange(&J.state, JOB_WORKING);
		wake = 1;
	}
	LeaveCriticalSection(&J.lock);

	if (wake) SetEvent(J.evWake);
	return answered;
}

static int accumulate_tile(lua_State *L, struct group *g, const gtav_tile *tile,
						   float ox, float oy, int flagsIdx, int asyncFlags,
						   char *err, size_t errlen);

// drop the mirrored half of double sided surfaces
static uint32_t drop_mirror(const gtav_vertex *verts, uint32_t nverts,
							const uint16_t *idx, uint32_t count,
							uint32_t vertBase) {
	if (count <= 3 || (count % 6) != 0) return count;

	uint32_t half = count / 2;
	for (uint32_t t = 0; t < half / 3; t++) {
		uint32_t i0 = t * 3;
		uint32_t j0 = i0 + half;

		uint32_t a[3], b[3];
		for (int k = 0; k < 3; k++) {
			uint32_t ia = (uint32_t)idx[i0 + k] + vertBase;
			uint32_t ib = (uint32_t)idx[j0 + k] + vertBase;
			if (ia >= nverts || ib >= nverts) return count;
			a[k] = ia;
			b[k] = ib;
		}

		const gtav_vertex *p1 = &verts[a[0]], *p2 = &verts[a[1]], *p3 = &verts[a[2]];
		const gtav_vertex *q1 = &verts[b[0]], *q2 = &verts[b[1]], *q3 = &verts[b[2]];

		int forward =
			p1->x == q1->x && p1->y == q1->y && p1->z == q1->z &&
			p2->x == q3->x && p2->y == q3->y && p2->z == q3->z &&
			p3->x == q2->x && p3->y == q2->y && p3->z == q2->z;

		int same =
			p1->x == q1->x && p1->y == q1->y && p1->z == q1->z &&
			p2->x == q2->x && p2->y == q2->y && p2->z == q2->z &&
			p3->x == q3->x && p3->y == q3->y && p3->z == q3->z;

		if (!forward && !same) return count;
	}
	return half;
}

static int accumulate_tile(lua_State *L, struct group *g, const gtav_tile *tile,
						   float ox, float oy, int flagsIdx, int asyncFlags,
						   char *err, size_t errlen) {
	if (tile->vertexCount > 0) {
		if (tile->vertexCount > g->scratchCap) {
			uint32_t ncap = g->scratchCap ? g->scratchCap : 1024;
			while (ncap < tile->vertexCount) ncap *= 2;

			uint32_t *nr = (uint32_t *)realloc(g->remap, (size_t)ncap * sizeof(uint32_t));
			if (!nr) {
				seterr(err, errlen, "out of memory");
				return 0;
			}
			g->remap = nr;
			uint32_t *nt = (uint32_t *)realloc(g->touched, (size_t)ncap * sizeof(uint32_t));
			if (!nt) {
				seterr(err, errlen, "out of memory");
				return 0;
			}
			g->touched = nt;
			memset(g->remap + g->scratchCap, 0,
				   (size_t)(ncap - g->scratchCap) * sizeof(uint32_t));
			g->scratchCap = ncap;
		}
	}

	for (uint32_t s = 0; s < tile->submeshCount; s++) {
		const gtav_submesh *sub = &tile->submeshes[s];
		const char *matName = (sub->material < tile->materialCount)
			? tile->materials[sub->material] : "error";

		struct acc *a = group_get(g, matName);
		if (!a) continue;
		if (!a->flagsDone) {
			if (asyncFlags) {
				if (!job_resolve(matName, &a->glass, &a->nocull)) return 0;
			} else {
				flags_for(L, flagsIdx, matName, &a->glass, &a->nocull);
			}
			a->flagsDone = 1;
		}

		uint32_t ioff = sub->indexOffset;
		uint32_t icnt = sub->indexCount;
		if (ioff > tile->indexCount) continue;
		if (ioff + icnt > tile->indexCount) icnt = tile->indexCount - ioff;

		if (a->nocull) {
			icnt = drop_mirror(tile->vertices, tile->vertexCount,
							   tile->indices + ioff, icnt, sub->vertexOffset);
		}
		if (icnt == 0) continue;

		if (!acc_reserve(a, icnt, icnt)) {
			seterr(err, errlen, "out of memory");
			return 0;
		}

		uint32_t vbase = a->nverts;
		uint32_t nnew = 0;      /* vertices added by THIS submesh */
		uint32_t ntouched = 0;  /* remap entries to clear afterwards */
		gtav_vertex *vdst = a->verts + vbase;
		uint32_t  *idst = a->idx + a->nidx;
		uint32_t   iw = 0;

		for (uint32_t k = 0; k < icnt; k++) {
			uint32_t src = (uint32_t)tile->indices[ioff + k] + sub->vertexOffset;
			if (src >= tile->vertexCount) continue;

			uint32_t slot = g->remap[src];
			if (slot == 0) {
				gtav_vertex v = tile->vertices[src];
				v.x -= ox;
				v.y -= oy;
				vdst[nnew] = v;
				{
					const gtav_vertex *w = &tile->vertices[src];
					float *dst = &a->shade[(size_t)(vbase + nnew) * 3];
					if (gtav_shadow_baked()) {
						gtav_shadow_shade_rgb(w->x, w->y, w->z,
											w->nx, w->ny, w->nz, dst);
					} else {
						dst[0] = dst[1] = dst[2] = 1.0f;
					}
				}
				slot = vbase + nnew + 1;    /* remap stores slot + 1 */
				nnew++;
				g->touched[ntouched++] = src;
				g->remap[src] = slot;
			}
			idst[iw++] = slot - 1;
		}

		a->nverts = vbase + nnew;
		a->nidx += iw;

		while (ntouched > 0) {
			g->remap[g->touched[--ntouched]] = 0;
		}
	}
	return 1;
}

#define MESH_MAX_TRIS GTAV_MESH_MAX_TRIS

// give up on a material after this many dead tris
#define MESH_MAX_LOST_RUN 32u

static uint32_t emit_meshes_slice(lua_State *L, struct group *g,
								  struct emit_cursor *cur, double budgetMs,
								  int *done, char *err, size_t errlen) {
	OW.createtable(L, (int)g->n, 0);
	uint32_t out = 0;
	double tSlice = now_ms();
	*done = 0;

	while (cur->item < g->n) {
		struct acc *a = &g->items[cur->item];
		if (a->nidx < 3) { cur->item++; cur->tri = 0; cur->lostRun = 0; continue; }

		uint32_t ntris = a->nidx / 3;
		uint32_t t0 = cur->tri;
		if (t0 >= ntris) { cur->item++; cur->tri = 0; cur->lostRun = 0; continue; }

		uint32_t want = ntris - t0;
		if (want > MESH_MAX_TRIS) want = MESH_MAX_TRIS;

		uint32_t got = 0;
		char lastErr[256];
		lastErr[0] = '\0';
		while (want >= 1) {
			err[0] = '\0';
			if (gtav_mesh_build(L, a->verts, a->nverts,
							  a->idx + (size_t)t0 * 3, want * 3,
							  a->shade,
							  err, errlen)) {
				got = want;
				break;
			}
			snprintf(lastErr, sizeof(lastErr), "%s", err);
			if (want == 1) break;
			want /= 2;
			BG.meshRetry++;
		}

		if (got == 0) {
			snprintf(err, errlen, "mesh build refused: %s",
					 lastErr[0] ? lastErr : "unknown");
			BG.meshFail++;
			BG.meshFailTris++;
			t0++;
			if (++cur->lostRun >= MESH_MAX_LOST_RUN) {
				BG.meshFailTris += (int)(ntris - t0);
				cur->item++;
				cur->tri = 0;
				cur->lostRun = 0;
				continue;
			}
			cur->tri = t0;
			continue;
		}
		cur->lostRun = 0;
		if (got < ntris - t0) BG.meshSplit++;

		// chunk box for culling
		float mn[3], mx[3];
		int hasBox = 0;
		{
			const uint32_t *ci = a->idx + (size_t)t0 * 3;
			mn[0] = mn[1] = mn[2] = 3.0e38f;
			mx[0] = mx[1] = mx[2] = -3.0e38f;
			for (uint32_t k = 0; k < got * 3; k++) {
				uint32_t vi = ci[k];
				if (vi >= a->nverts) continue;
				const gtav_vertex *v = &a->verts[vi];
				if (v->x < mn[0]) mn[0] = v->x;
				if (v->y < mn[1]) mn[1] = v->y;
				if (v->z < mn[2]) mn[2] = v->z;
				if (v->x > mx[0]) mx[0] = v->x;
				if (v->y > mx[1]) mx[1] = v->y;
				if (v->z > mx[2]) mx[2] = v->z;
				hasBox = 1;
			}
		}

		// stack: result, mesh

		OW.createtable(L, 0, 6);          /* stack: result, mesh, entry */
		OW.pushvalue(L, -2);              /* copy the Mesh */
		OW.setfield(L, -2, "mesh");
		OW.pushstring(L, a->name);
		OW.setfield(L, -2, "material");
		OW.pushboolean(L, a->glass);
		OW.setfield(L, -2, "glass");
		OW.pushinteger(L, (lua_Integer)got);
		OW.setfield(L, -2, "tris");
		if (hasBox) {
			gtav_mesh_push_vector(L, mn[0], mn[1], mn[2]);
			OW.setfield(L, -2, "mins");
			gtav_mesh_push_vector(L, mx[0], mx[1], mx[2]);
			OW.setfield(L, -2, "maxs");
		}

		OW.remove(L, -2);                 /* drop the loose Mesh copy */
		OW.rawseti(L, -2, (int)++out);    /* entry -> result[out] */
		t0 += got;
		cur->tri = t0;
		if (t0 >= ntris) {
			cur->item++;
			cur->tri = 0;
			cur->lostRun = 0;
		}

		if (budgetMs > 0 && now_ms() - tSlice >= budgetMs) break;
	}

	if (cur->item >= g->n) *done = 1;
	return out;
}

static uint32_t emit_meshes(lua_State *L, struct group *g, char *err, size_t errlen) {
	struct emit_cursor cur;
	cur.item = 0;
	cur.tri = 0;
	cur.lostRun = 0;
	int done = 0;
	return emit_meshes_slice(L, g, &cur, 0, &done, err, errlen);
}

// page first, then the loose file
static const uint8_t *read_payload(const char *file, int tx, int ty,
								   size_t *len, uint8_t **owned) {
	*owned = NULL;
	const gtav_page *pg = gtav_pages_open('v', tx, ty);
	if (pg) {
		const uint8_t *blob = gtav_pages_blob(pg, file, len);
		if (blob) return blob;
	}
	*owned = gtav_pages_read_loose("tiles", file, len);
	return *owned;
}

static int f_build_tile(lua_State *L) {
	const char *file = OW.checklstring(L, 1, NULL);
	int tx = (int)OW.checkinteger(L, 2);
	int ty = (int)OW.checkinteger(L, 3);
	double tileSize = OW.checknumber(L, 4);
	int flagsIdx = 0;
	if (OW.gettop(L) >= 5 && OW.type(L, 5) == GTAV_LUA_TTABLE) flagsIdx = 5;

	char err[256];
	err[0] = '\0';

	size_t len = 0;
	uint8_t *owned = NULL;
	const uint8_t *blob = NULL;
	gtav_tile tile;
	memset(&tile, 0, sizeof(tile));
	int decoded = 0;
	{
		char derr[256];
		derr[0] = '\0';
		gtav_pages_lock();
		blob = read_payload(file, tx, ty, &len, &owned);
		if (blob) decoded = gtav_tile_decode(blob, len, &tile, derr, sizeof(derr));
		free(owned);
		gtav_pages_unlock();
		if (!decoded && blob) snprintf(err, sizeof(err), "%s", derr);
	}
	if (!blob) {
		OW.pushnil(L);
		OW.pushstring(L, "no tile payload (no page and no loose file)");
		return 2;
	}
	if (!decoded) {
		OW.pushnil(L);
		OW.pushstring(L, err);
		return 2;
	}

	struct group *g = &GTILE;
	group_reset(g);

	float ox = (float)(tx * tileSize);
	float oy = (float)(ty * tileSize);

	if (!accumulate_tile(L, g, &tile, ox, oy, flagsIdx, 0, err, sizeof(err))) {
		group_reset(g);
		gtav_tile_free(&tile);
		OW.pushnil(L);
		OW.pushstring(L, err);
		return 2;
	}

	emit_meshes(L, g, err, sizeof(err));
	group_reset(g);
	gtav_tile_free(&tile);
	return 1;
}

static int f_build_group(lua_State *L) {
	OW.checktype(L, 1, GTAV_LUA_TTABLE);
	int originTx = (int)OW.checkinteger(L, 2);
	int originTy = (int)OW.checkinteger(L, 3);
	double tileSize = OW.checknumber(L, 4);
	int flagsIdx = 0;
	if (OW.gettop(L) >= 5 && OW.type(L, 5) == GTAV_LUA_TTABLE) flagsIdx = 5;

	float originX = (float)(originTx * tileSize);
	float originY = (float)(originTy * tileSize);

	char err[256];
	err[0] = '\0';

	memset(&BG, 0, sizeof(BG));
	double tStart = now_ms();

	int ntiles = (int)OW.objlen(L, 1);
	if (ntiles <= 0) {
		BG.totalMs = now_ms() - tStart;
		OW.pushnil(L);
		OW.pushstring(L, "empty tile list");
		return 2;
	}

	struct group *g = &GBUILD;
	group_reset(g);

	if (!OW.checkstack(L, 16)) {
		OW.pushnil(L);
		OW.pushstring(L, "Lua stack overflow");
		return 2;
	}

	int merged = 0;
	for (int i = 1; i <= ntiles; i++) {
		char file[64];

		OW.rawgeti(L, 1, i);                  /* entry */
		if (OW.type(L, -1) != GTAV_LUA_TTABLE) {
			OW.settop(L, -2);                 /* pop entry */
			continue;
		}

		OW.getfield(L, -1, "file");           /* entry, file */
		OW.getfield(L, -2, "tx");             /* entry, file, tx */
		OW.getfield(L, -3, "ty");             /* entry, file, tx, ty */

		int okEntry = OW.type(L, -3) == GTAV_LUA_TSTRING &&
					  OW.type(L, -2) == GTAV_LUA_TNUMBER &&
					  OW.type(L, -1) == GTAV_LUA_TNUMBER;

		int tx = 0, ty = 0;
		if (okEntry) {
			const char *s = OW.tolstring(L, -3, NULL);
			snprintf(file, sizeof(file), "%s", s);
			tx = (int)OW.tointeger(L, -2);
			ty = (int)OW.tointeger(L, -1);
		}

		OW.settop(L, -5);
		if (!okEntry) continue;

		size_t len = 0;
		uint8_t *owned = NULL;
		gtav_tile tile;
		memset(&tile, 0, sizeof(tile));
		int decoded = 0;
		err[0] = '\0';
		{
			double tRead = now_ms();
			gtav_pages_lock();
			const uint8_t *blob = read_payload(file, tx, ty, &len, &owned);
			double tDec = now_ms();
			if (blob) decoded = gtav_tile_decode(blob, len, &tile, err, sizeof(err));
			free(owned);
			gtav_pages_unlock();
			BG.readMs += tDec - tRead;
			BG.decodeMs += now_ms() - tDec;
		}
		if (!decoded) continue;
		double tPhase = now_ms();
		if (accumulate_tile(L, g, &tile, (float)originX, (float)originY,
							flagsIdx, 0, err, sizeof(err))) {
			merged++;
		}
		BG.accMs += now_ms() - tPhase;
		gtav_tile_free(&tile);
	}

	if (merged == 0) {
		BG.totalMs = now_ms() - tStart;
		group_reset(g);
		OW.pushnil(L);
		OW.pushstring(L, "no decodable tile in the group");
		return 2;
	}

	for (uint32_t i = 0; i < g->n; i++) {
		if (g->items[i].nidx < 3) continue;
		BG.tris += (int)(g->items[i].nidx / 3);
	}
	BG.tiles = merged;

	double tPhase2 = now_ms();
	BG.draws = (int)emit_meshes(L, g, err, sizeof(err));
	BG.meshMs += now_ms() - tPhase2;
	BG.totalMs = now_ms() - tStart;

	OW.pushinteger(L, merged);
	group_reset(g);
	return 2;
}

static const char *job_mat_name(const gtav_tile *tile, const gtav_submesh *sub) {
	return (sub->material < tile->materialCount)
		? tile->materials[sub->material] : "error";
}

static void job_fail(const char *why) {
	snprintf(J.err, sizeof(J.err), "%s", why);
	InterlockedExchange(&J.state, JOB_FAILED);
	SetEvent(J.evDone);
}

static int job_collect_names(const struct job_entry *tiles, int n) {
	for (int i = 0; i < n; i++) {
		if (J.cancel || J.shutdown) return 0;

		char derr[256];
		derr[0] = '\0';
		size_t len = 0;
		uint8_t *owned = NULL;
		gtav_tile tile;
		memset(&tile, 0, sizeof(tile));
		int decoded = 0;
		{
			gtav_pages_lock();
			const uint8_t *blob = read_payload(tiles[i].file, tiles[i].tx,
											   tiles[i].ty, &len, &owned);
			if (blob) decoded = gtav_tile_decode(blob, len, &tile, derr, sizeof(derr));
			free(owned);
			gtav_pages_unlock();
		}
		if (!decoded) continue;

		for (uint32_t s = 0; s < tile.submeshCount; s++) {
			const char *name = job_mat_name(&tile, &tile.submeshes[s]);
			int glass = 0, nocull = 0;
			if (job_cache_get(name, &glass, &nocull)) continue;

			int known = 0;
			EnterCriticalSection(&J.lock);
			for (int w = 0; w < J.nwant; w++) {
				if (strncmp(J.want[w].name, name, sizeof(J.want[w].name)) == 0) {
					known = 1;
					break;
				}
			}
			if (!known && job_flag_grow(&J.want, &J.capWant, J.nwant)) {
				snprintf(J.want[J.nwant].name, sizeof(J.want[J.nwant].name), "%s", name);
				J.nwant++;
			}
			LeaveCriticalSection(&J.lock);
		}
		gtav_tile_free(&tile);
	}
	return 1;
}

static void run_job(void) {
	struct group *g = &J.g;
	group_reset(g);
	J.readMs = J.decodeMs = J.accMs = J.prepMs = 0;
	J.merged = 0;
	J.tris = 0;
	J.err[0] = '\0';
	double tStart = now_ms();

	EnterCriticalSection(&J.lock);
	J.nwant = 0;
	J.wantDone = 0;
	J.wantCur = 0;
	J.wantGen++;
	LeaveCriticalSection(&J.lock);

	// phase 1, material names
	if (!job_collect_names(J.tiles, J.ntiles)) {
		group_reset(g);
		job_fail("cancelled");
		return;
	}

	EnterCriticalSection(&J.lock);
	J.wantCur = J.nwant;
	LeaveCriticalSection(&J.lock);

	if (!job_collect_names(J.tilesPre, J.ntilesPre)) {
		group_reset(g);
		job_fail("cancelled");
		return;
	}

	if (!job_handshake()) { group_reset(g); return; }

	// phase 2, yes it decodes everything again
	for (int i = 0; i < J.ntiles; i++) {
		if (J.cancel || J.shutdown) {
			group_reset(g);
			job_fail("cancelled");
			return;
		}

		char derr[256];
		derr[0] = '\0';
		size_t len = 0;
		uint8_t *owned = NULL;
		gtav_tile tile;
		memset(&tile, 0, sizeof(tile));
		int decoded = 0;
		double tRead = now_ms();
		{
			gtav_pages_lock();
			const uint8_t *blob = read_payload(J.tiles[i].file, J.tiles[i].tx,
											   J.tiles[i].ty, &len, &owned);
			if (blob) decoded = gtav_tile_decode(blob, len, &tile, derr, sizeof(derr));
			free(owned);
			gtav_pages_unlock();
		}
		double tDec = now_ms();
		J.readMs += tDec - tRead;
		J.decodeMs += now_ms() - tDec;
		if (!decoded) continue;

		double tAcc = now_ms();
		char aerr[256];
		aerr[0] = '\0';
		int ok = accumulate_tile(NULL, g, &tile, J.originX, J.originY,
								 0, 1, aerr, sizeof(aerr));
		J.accMs += now_ms() - tAcc;
		gtav_tile_free(&tile);
		if (!ok) {
			group_reset(g);
			if (J.state != JOB_FAILED)
				job_fail(aerr[0] ? aerr : "accumulate failed");
			return;
		}
		J.merged++;
	}

	if (J.merged == 0) {
		group_reset(g);
		job_fail("no decodable tile in the group");
		return;
	}

	for (uint32_t k = 0; k < g->n; k++) {
		if (g->items[k].nidx >= 3) J.tris += (int)(g->items[k].nidx / 3);
	}
	J.prepMs = now_ms() - tStart;
	J.prepTot += J.prepMs;
	InterlockedExchange(&J.state, JOB_DONE);
	SetEvent(J.evDone);
}

static DWORD WINAPI job_worker(LPVOID unused) {
	(void)unused;
	for (;;) {
		WaitForSingleObject(J.evWake, INFINITE);
		if (J.shutdown) break;
		if (InterlockedCompareExchange(&J.state, JOB_WORKING, JOB_QUEUED) == JOB_QUEUED)
			run_job();
	}
	return 0;
}

static int job_ensure(void) {
	if (!J.lockInit) {
		InitializeCriticalSection(&J.lock);
		J.lockInit = 1;
	}
	if (!J.evWake) J.evWake = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!J.evDone) J.evDone = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!J.threadUp && J.evWake && J.evDone) {
		J.thread = CreateThread(NULL, 0, job_worker, NULL, 0, NULL);
		J.threadUp = (J.thread != NULL);
	}
	return J.threadUp;
}

static int job_begin(const struct job_entry *tiles, int n, float ox, float oy,
					 const struct job_entry *pre, int npre) {
	if (!job_ensure()) return 0;

	LONG st = InterlockedCompareExchange(&J.state, JOB_IDLE, JOB_IDLE);
	if (st == JOB_QUEUED || st == JOB_WORKING || st == JOB_NEEDFLAGS) return 0;
	group_reset(&J.g);

	if (n > J.capTiles) {
		struct job_entry *p = (struct job_entry *)realloc(J.tiles,
								(size_t)n * sizeof(struct job_entry));
		if (!p) return 0;
		J.tiles = p;
		J.capTiles = n;
	}
	memcpy(J.tiles, tiles, (size_t)n * sizeof(struct job_entry));
	J.ntiles = n;
	J.ntilesPre = 0;
	if (pre && npre > 0) {
		if (npre > J.capTilesPre) {
			struct job_entry *p = (struct job_entry *)realloc(J.tilesPre,
									(size_t)npre * sizeof(struct job_entry));
			if (p) {
				J.tilesPre = p;
				J.capTilesPre = npre;
			}
		}
		if (npre <= J.capTilesPre) {
			memcpy(J.tilesPre, pre, (size_t)npre * sizeof(struct job_entry));
			J.ntilesPre = npre;
		}
	}
	J.originX = ox;
	J.originY = oy;
	J.err[0] = '\0';
	J.flagNames = 0;
	J.flagMs = 0;
	J.handRtts = 0;
	InterlockedExchange(&J.cancel, 0);
	InterlockedExchange(&J.state, JOB_QUEUED);
	SetEvent(J.evWake);
	return 1;
}

static void job_shutdown(void) {
	if (J.threadUp) {
		InterlockedExchange(&J.shutdown, 1);
		SetEvent(J.evWake);
		if (WaitForSingleObject(J.thread, 5000) != WAIT_OBJECT_0)
			TerminateThread(J.thread, 1);   /* last resort */
		CloseHandle(J.thread);
		J.thread = NULL;
		J.threadUp = 0;
	}
	group_free(&J.g);
	commit_reset();
	group_free(&JC.g);
	free(J.tiles);  J.tiles = NULL;  J.ntiles = J.capTiles = 0;
	free(J.tilesPre); J.tilesPre = NULL; J.ntilesPre = J.capTilesPre = 0;
	free(J.flags);  J.flags = NULL;  J.nflags = J.capFlags = 0;
	free(J.flagHash); J.flagHash = NULL; J.capHash = 0;
	free(J.want);   J.want = NULL;   J.nwant = J.wantDone = J.capWant = 0;
	J.wantCur = 0;
	if (J.evWake) { CloseHandle(J.evWake); J.evWake = NULL; }
	if (J.evDone) { CloseHandle(J.evDone); J.evDone = NULL; }
	if (J.lockInit) {
		DeleteCriticalSection(&J.lock);
		J.lockInit = 0;
	}
}

static int parse_job_entries(lua_State *L, int tblIdx,
							 struct job_entry *out, int cap) {
	int ntiles = (int)OW.objlen(L, tblIdx);
	int base = OW.gettop(L);
	int n = 0;
	for (int i = 1; i <= ntiles && n < cap; i++) {
		OW.rawgeti(L, tblIdx, i);
		if (OW.type(L, -1) != GTAV_LUA_TTABLE) {
			OW.settop(L, base);
			continue;
		}
		OW.getfield(L, -1, "file");
		OW.getfield(L, -2, "tx");
		OW.getfield(L, -3, "ty");
		int okEntry = OW.type(L, -3) == GTAV_LUA_TSTRING &&
					  OW.type(L, -2) == GTAV_LUA_TNUMBER &&
					  OW.type(L, -1) == GTAV_LUA_TNUMBER;
		if (okEntry) {
			snprintf(out[n].file, sizeof(out[n].file), "%s",
					 OW.tolstring(L, -3, NULL));
			out[n].tx = (int)OW.tointeger(L, -2);
			out[n].ty = (int)OW.tointeger(L, -1);
			n++;
		}
		OW.settop(L, base);
	}
	return n;
}

static int f_build_async_start(lua_State *L) {
	OW.checktype(L, 1, GTAV_LUA_TTABLE);
	int originTx = (int)OW.checkinteger(L, 2);
	int originTy = (int)OW.checkinteger(L, 3);
	double tileSize = OW.checknumber(L, 4);

	int ntiles = (int)OW.objlen(L, 1);
	if (ntiles <= 0) {
		OW.pushnil(L);
		OW.pushstring(L, "empty tile list");
		return 2;
	}

	struct job_entry *tmp = (struct job_entry *)malloc(
		(size_t)ntiles * sizeof(struct job_entry));
	if (!tmp) {
		OW.pushnil(L);
		OW.pushstring(L, "out of memory");
		return 2;
	}
	if (!OW.checkstack(L, 16)) {
		free(tmp);
		OW.pushnil(L);
		OW.pushstring(L, "Lua stack overflow");
		return 2;
	}

	int n = parse_job_entries(L, 1, tmp, ntiles);

	struct job_entry *tmpPre = NULL;
	int nPre = 0;
	if (OW.gettop(L) >= 5 && OW.type(L, 5) == GTAV_LUA_TTABLE) {
		int nPreMax = (int)OW.objlen(L, 5);
		if (nPreMax > 0) {
			tmpPre = (struct job_entry *)malloc(
				(size_t)nPreMax * sizeof(struct job_entry));
			if (tmpPre) nPre = parse_job_entries(L, 5, tmpPre, nPreMax);
		}
	}

	int ok = 0;
	if (n > 0) {
		ok = job_begin(tmp, n, (float)(originTx * tileSize),
					   (float)(originTy * tileSize), tmpPre, nPre);
	}
	free(tmp);
	free(tmpPre);

	if (!ok) {
		OW.pushnil(L);
		OW.pushstring(L, "job busy or thread refused");
		return 2;
	}
	OW.pushboolean(L, 1);
	return 1;
}

static int f_build_async_poll(lua_State *L) {
	int flagsIdx = 0;
	if (OW.gettop(L) >= 1 && OW.type(L, 1) == GTAV_LUA_TTABLE) flagsIdx = 1;
	double budgetMs = 0;
	if (OW.gettop(L) >= 2 && OW.type(L, 2) == GTAV_LUA_TNUMBER)
		budgetMs = OW.tonumber(L, 2);

	job_answer(L, flagsIdx, budgetMs);

	LONG st = InterlockedCompareExchange(&J.state, JOB_IDLE, JOB_IDLE);
	const char *s = "idle";
	switch (st) {
		case JOB_QUEUED:    s = "queued";  break;
		case JOB_WORKING:   s = "working"; break;
		case JOB_NEEDFLAGS: s = "working"; break;
		case JOB_DONE:      s = "ready";   break;
		case JOB_FAILED:    s = "failed";  break;
		default: break;
	}
	OW.pushstring(L, s);
	if (st == JOB_FAILED) {
		OW.pushstring(L, J.err[0] ? J.err : "build failed");
		return 2;
	}
	return 1;
}

static int f_build_async_commit(lua_State *L) {
	double budgetMs = 0;
	if (OW.gettop(L) >= 1 && OW.type(L, 1) == GTAV_LUA_TNUMBER)
		budgetMs = OW.tonumber(L, 1);

	char err[256];
	err[0] = '\0';

	if (!JC.active) {
		LONG st = InterlockedCompareExchange(&J.state, JOB_IDLE, JOB_IDLE);
		if (st == JOB_FAILED) {
			snprintf(err, sizeof(err), "%s", J.err[0] ? J.err : "build failed");
			group_reset(&J.g);
			InterlockedExchange(&J.state, JOB_IDLE);
			OW.pushnil(L);
			OW.pushstring(L, err);
			return 2;
		}
		if (st != JOB_DONE) {
			OW.pushnil(L);
			OW.pushstring(L, "not ready");
			return 2;
		}

		// swap, not copy
		struct group tmp = JC.g;
		JC.g = J.g;
		J.g = tmp;
		group_reset(&J.g);
		JC.cur.item = 0;
		JC.cur.tri = 0;
		JC.cur.lostRun = 0;
		JC.merged = J.merged;
		JC.tris = J.tris;
		JC.readMs = J.readMs;
		JC.decodeMs = J.decodeMs;
		JC.accMs = J.accMs;
		JC.prepMs = J.prepMs;
		JC.flagNames = J.flagNames;
		JC.flagMs = J.flagMs;
		JC.handRtts = J.handRtts;
		JC.active = 1;
		InterlockedExchange(&J.state, JOB_IDLE);

		memset(&BG, 0, sizeof(BG));
		BG.readMs = JC.readMs;
		BG.decodeMs = JC.decodeMs;
		BG.accMs = JC.accMs;
		BG.totalMs = JC.prepMs;
		BG.tiles = JC.merged;
		BG.tris = JC.tris;
		BG.flagNames = JC.flagNames;
		BG.flagMs = JC.flagMs;
		BG.handshakes = JC.handRtts;
		BG.flagCacheN = J.nflags;
	}

	double t0 = now_ms();
	int done = 0;
	uint32_t draws = emit_meshes_slice(L, &JC.g, &JC.cur, budgetMs, &done,
									  err, sizeof(err));
	double meshMs = now_ms() - t0;
	BG.meshMs += meshMs;
	BG.totalMs += meshMs;
	BG.draws += (int)draws;

	if (done) commit_reset();

	OW.pushboolean(L, done);
	return 2;
}

static int f_build_async_cancel(lua_State *L) {
	(void)L;
	InterlockedExchange(&J.cancel, 1);
	SetEvent(J.evWake);
	commit_reset();
	OW.pushboolean(L, 1);
	return 1;
}

static int f_tile_info(lua_State *L) {
	const char *file = OW.checklstring(L, 1, NULL);
	int tx = (int)OW.checkinteger(L, 2);
	int ty = (int)OW.checkinteger(L, 3);

	size_t len = 0;
	uint8_t *owned = NULL;
	gtav_tile t;
	memset(&t, 0, sizeof(t));
	char err[256];
	err[0] = '\0';
	int decoded = 0;
	{
		gtav_pages_lock();
		const uint8_t *blob = read_payload(file, tx, ty, &len, &owned);
		if (blob) decoded = gtav_tile_decode(blob, len, &t, err, sizeof(err));
		free(owned);
		gtav_pages_unlock();
	}
	if (!decoded) {
		OW.pushnil(L);
		OW.pushstring(L, err[0] ? err : "no tile payload (no page and no loose file)");
		return 2;
	}

	OW.createtable(L, 0, 9);
	OW.pushinteger(L, len);                OW.setfield(L, -2, "bytes");
	OW.pushinteger(L, t.vertexCount);      OW.setfield(L, -2, "verts");
	OW.pushinteger(L, t.indexCount);       OW.setfield(L, -2, "indices");
	OW.pushinteger(L, t.submeshCount);     OW.setfield(L, -2, "submeshes");
	OW.pushinteger(L, t.materialCount);    OW.setfield(L, -2, "materials");
	OW.pushinteger(L, t.stride);           OW.setfield(L, -2, "stride");
	OW.pushinteger(L, t.clamped);          OW.setfield(L, -2, "clamped");
	OW.pushinteger(L, t.flags);            OW.setfield(L, -2, "flags");
	OW.pushinteger(L, gtav_pages_mapped());  OW.setfield(L, -2, "pages");

	gtav_tile_free(&t);
	return 1;
}

static int f_pages_init(lua_State *L) {
	const char *name = OW.checklstring(L, 1, NULL);
	commit_reset();
	job_cache_clear();
	J.flagNamesTot = 0;
	J.flagMsTot = 0;
	J.prepTot = 0;
	OW.pushboolean(L, gtav_pages_init(name));
	return 1;
}

static int f_pages_mapped(lua_State *L) {
	OW.pushinteger(L, (lua_Integer)gtav_pages_mapped());
	return 1;
}

static int f_pages_budget(lua_State *L) {
	gtav_pages_set_budget((size_t)OW.checkinteger(L, 1));
	return 0;
}


static int f_shadow_init(lua_State *L) {
	char err[256];
	err[0] = '\0';
	int ok = gtav_shadow_open(gtav_pages_root(), err, sizeof(err));
	OW.pushboolean(L, ok);
	OW.pushstring(L, ok ? gtav_shadow_info() : err);
	return 2;
}

static int f_shadow_info(lua_State *L) {
	OW.pushstring(L, gtav_shadow_info());
	return 1;
}

static int f_baked_lighting(lua_State *L) {
	if (OW.gettop(L) >= 1) {
		gtav_shadow_set_baked(OW.toboolean(L, 1));
	}
	OW.pushboolean(L, gtav_shadow_baked());
	return 1;
}

static int f_light_config(lua_State *L) {
	float amb, sun, expo;
	gtav_shadow_get_light(&amb, &sun, &expo);
	if (OW.gettop(L) >= 1 && OW.type(L, 1) == GTAV_LUA_TNUMBER) {
		amb = (float)OW.tonumber(L, 1);
	}
	if (OW.gettop(L) >= 2 && OW.type(L, 2) == GTAV_LUA_TNUMBER) {
		sun = (float)OW.tonumber(L, 2);
	}
	if (OW.gettop(L) >= 3 && OW.type(L, 3) == GTAV_LUA_TNUMBER) {
		expo = (float)OW.tonumber(L, 3);
	}
	gtav_shadow_set_light(amb, sun, expo);
	gtav_shadow_get_light(&amb, &sun, &expo);
	OW.pushnumber(L, amb);
	OW.pushnumber(L, sun);
	OW.pushnumber(L, expo);
	return 3;
}

static int f_sun_info(lua_State *L) {
	float azim = 0.0f, elev = 0.0f, amb = 0.0f, sun = 0.0f;
	gtav_shadow_sun(&azim, &elev);
	gtav_shadow_get_light(&amb, &sun, NULL);
	OW.pushnumber(L, azim);
	OW.pushnumber(L, elev);
	OW.pushnumber(L, amb);
	OW.pushnumber(L, sun);
	return 4;
}

static int f_fast_vectors(lua_State *L) {
	OW.pushboolean(L, gtav_mesh_fast_vectors());
	return 1;
}

static int f_builder_mode(lua_State *L) {
	if (OW.gettop(L) >= 1 && OW.type(L, 1) == GTAV_LUA_TNUMBER) {
		gtav_mesh_allow_builder((int)OW.tonumber(L, 1) != 0);
	}
	OW.pushboolean(L, gtav_mesh_builder());
	return 1;
}

static int f_mesh_info(lua_State *L) {
	double pushMs = 0, buildMs = 0;
	gtav_mesh_stats(&pushMs, &buildMs);
	OW.pushstring(L, gtav_mesh_why());
	OW.pushstring(L, gtav_mesh_last_path());
	OW.pushnumber(L, pushMs);
	OW.pushnumber(L, buildMs);
	OW.pushnumber(L, (double)gtav_mesh_fast_fails());
	return 5;
}

static int f_build_stats(lua_State *L) {
	OW.createtable(L, 0, 20);
	OW.pushnumber(L, BG.totalMs);  OW.setfield(L, -2, "totalMs");
	OW.pushnumber(L, BG.readMs);   OW.setfield(L, -2, "readMs");
	OW.pushnumber(L, BG.decodeMs); OW.setfield(L, -2, "decodeMs");
	OW.pushnumber(L, BG.accMs);    OW.setfield(L, -2, "accMs");
	OW.pushnumber(L, BG.meshMs);   OW.setfield(L, -2, "meshMs");
	OW.pushinteger(L, BG.tiles);   OW.setfield(L, -2, "tiles");
	OW.pushinteger(L, BG.draws);   OW.setfield(L, -2, "draws");
	OW.pushinteger(L, BG.tris);    OW.setfield(L, -2, "tris");
	OW.pushinteger(L, BG.meshRetry);    OW.setfield(L, -2, "meshRetry");
	OW.pushinteger(L, BG.meshSplit);    OW.setfield(L, -2, "meshSplit");
	OW.pushinteger(L, BG.meshFail);     OW.setfield(L, -2, "meshFail");
	OW.pushinteger(L, BG.meshFailTris); OW.setfield(L, -2, "meshFailTris");
	OW.pushinteger(L, (lua_Integer)gtav_mesh_bad_indices());
	   OW.setfield(L, -2, "meshBadIdx");
	OW.pushinteger(L, BG.flagNames);  OW.setfield(L, -2, "flagNames");
	OW.pushnumber(L, BG.flagMs);      OW.setfield(L, -2, "flagMs");
	OW.pushinteger(L, BG.handshakes); OW.setfield(L, -2, "handshakes");
	OW.pushinteger(L, BG.flagCacheN); OW.setfield(L, -2, "flagCacheN");
	OW.pushinteger(L, J.flagNamesTot); OW.setfield(L, -2, "flagNamesTot");
	OW.pushnumber(L, J.flagMsTot);     OW.setfield(L, -2, "flagMsTot");
	OW.pushnumber(L, J.prepTot);       OW.setfield(L, -2, "prepTot");
	return 1;
}

static int f_version(lua_State *L) {
	OW.pushstring(L, GTAV_VERSION);
	return 1;
}


static int f_prefetch_init(lua_State *L) {
	char err[512];
	const char *addon = NULL;
	err[0] = '\0';
	if (OW.type(L, 1) == GTAV_LUA_TSTRING) addon = OW.tolstring(L, 1, NULL);
	if (gtav_prefetch_init(addon, err, sizeof(err))) {
		OW.pushboolean(L, 1);
		OW.pushstring(L, gtav_prefetch_root());
	} else {
		OW.pushboolean(L, 0);
		OW.pushstring(L, err);
	}
	return 2;
}

static int f_prefetch_add(lua_State *L) {
	lua_Integer n = 0;
	int t = OW.type(L, 1);
	if (t == GTAV_LUA_TSTRING) {
		n = gtav_prefetch_add(OW.tolstring(L, 1, NULL));
	} else if (t == GTAV_LUA_TTABLE) {
		size_t len = OW.objlen(L, 1), i;
		for (i = 1; i <= len; ++i) {
			OW.rawgeti(L, 1, (int)i);
			if (OW.type(L, -1) == GTAV_LUA_TSTRING)
				n += gtav_prefetch_add(OW.tolstring(L, -1, NULL));
			OW.settop(L, -2);
		}
	}
	OW.pushinteger(L, n);
	return 1;
}

static int f_prefetch_stats(lua_State *L) {
	gtav_prefetch_stats s;
	gtav_prefetch_get_stats(&s);
	OW.createtable(L, 0, 7);
	OW.pushinteger(L, (lua_Integer)s.queued);  OW.setfield(L, -2, "queued");
	OW.pushinteger(L, (lua_Integer)s.done);    OW.setfield(L, -2, "done");
	OW.pushinteger(L, (lua_Integer)s.files);   OW.setfield(L, -2, "files");
	OW.pushinteger(L, (lua_Integer)s.missing); OW.setfield(L, -2, "missing");
	OW.pushinteger(L, (lua_Integer)s.dropped); OW.setfield(L, -2, "dropped");
	OW.pushinteger(L, (lua_Integer)s.pending); OW.setfield(L, -2, "pending");
	OW.pushnumber(L, s.ms);                    OW.setfield(L, -2, "ms");
	return 1;
}

static const luaL_Reg GTAV_FUNCS[] = {
	{ "version",      f_version      },
	{ "fast_vectors", f_fast_vectors },
	{ "builder_mode", f_builder_mode },
	{ "mesh_info",    f_mesh_info    },
	{ "build_stats",  f_build_stats  },
	{ "pages_init",   f_pages_init   },
	{ "pages_mapped", f_pages_mapped },
	{ "pages_budget", f_pages_budget },
	{ "shadow_init",   f_shadow_init   },
	{ "shadow_info",   f_shadow_info   },
	{ "baked_lighting", f_baked_lighting },
	{ "light_config",  f_light_config  },
	{ "sun_info",      f_sun_info      },
	{ "tile_info",    f_tile_info    },
	{ "build_tile",   f_build_tile   },
	{ "build_group",  f_build_group  },
	{ "build_async_start",  f_build_async_start  },
	{ "build_async_poll",   f_build_async_poll   },
	{ "build_async_commit", f_build_async_commit },
	{ "build_async_cancel", f_build_async_cancel },
	{ "prefetch_init",  f_prefetch_init  },
	{ "prefetch_add",   f_prefetch_add   },
	{ "prefetch_stats", f_prefetch_stats },
	{ NULL, NULL }
};


// gmod looks these up by name
__declspec(dllexport) int gmod13_open(lua_State *L);
__declspec(dllexport) int gmod13_close(lua_State *L);
__declspec(dllexport) int gmod_open(lua_State *L);
__declspec(dllexport) int gmod_close(lua_State *L);

__declspec(dllexport) int gmod13_open(lua_State *L) {
	char err[256];
	err[0] = '\0';

	if (!gtav_lua_bind(err, sizeof(err))) {
		HMODULE h = GetModuleHandleA("lua_shared.dll");
		void (*pushnil_)(lua_State *) = NULL;
		const char *(*pushstr_)(lua_State *, const char *) = NULL;
		if (h) {
			*(void **)&pushnil_ = (void *)GetProcAddress(h, "lua_pushnil");
			*(void **)&pushstr_ = (void *)GetProcAddress(h, "lua_pushstring");
		}
		if (pushnil_ && pushstr_) {
			pushnil_(L);
			pushstr_(L, err);
			return 2;
		}
		return 0;
	}

	OW.register_(L, GTAV_MODULE_NAME, GTAV_FUNCS);

	err[0] = '\0';
	gtav_mesh_init(L, err, sizeof(err));
	if (err[0]) {
		OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "print");
		if (OW.type(L, -1) == GTAV_LUA_TFUNCTION) {
			OW.pushfstring(L, "[GTAV] native: %s", err);
			OW.call(L, 1, 0);
		} else {
			OW.remove(L, -1);
		}
	}

	OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "print");
	if (OW.type(L, -1) == GTAV_LUA_TFUNCTION) {
		OW.pushfstring(L, "[GTAV] native module %s (%s vectors)",
					   GTAV_VERSION,
					   gtav_mesh_fast_vectors() ? "fast" : "slow");
		OW.call(L, 1, 0);
	} else {
		OW.remove(L, -1);
	}

	return 0;
}

__declspec(dllexport) int gmod13_close(lua_State *L) {
	(void)L;
	job_shutdown();
	gtav_prefetch_shutdown();
	gtav_mesh_shutdown();
	gtav_pages_shutdown();
	group_free(&GTILE);
	group_free(&GBUILD);
	return 0;
}

__declspec(dllexport) int gmod_open(lua_State *L)  { return gmod13_open(L); }
__declspec(dllexport) int gmod_close(lua_State *L) { return gmod13_close(L); }
