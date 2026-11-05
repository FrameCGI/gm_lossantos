#include "gtav_mesh.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

// vector calibration, all exact as float32
static const float PROBE_X =  1.25f;
static const float PROBE_Y = -3.50f;
static const float PROBE_Z =  7.75f;

static const float CHECK_X =  11.50f;
static const float CHECK_Y = -22.25f;
static const float CHECK_Z =  33.75f;

#define PROBE_WINDOW 128

static struct {
	int      ready;
	int      fast;          /* in-place Vector writes verified */
	int      indirect;      /* floats live at *(void **)userdata */
	int      builder;       /* mesh.Begin path is available */
	int      allowFast;     /* Lua permits using it */
	size_t   vecSize;
	int      offX, offY, offZ;
	int      ctorRef;       /* registry ref: Vector constructor */
	char     why[192];

	double   pushMs;        /* last build: geometry preparation */
	double   buildMs;       /* last build: total */
	const char *lastPath;

	unsigned badIndex;

	unsigned fastFails;
} M;

static void seterr(char *err, size_t errlen, const char *fmt, ...) {
	va_list ap;
	if (!err || errlen == 0) return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static double now_ms(void) {
	static LARGE_INTEGER freq = { 0 };
	LARGE_INTEGER t;
	if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

int         gtav_mesh_fast_vectors(void)     { return M.fast; }
int         gtav_mesh_builder(void)          { return M.builder && M.allowFast; }
const char *gtav_mesh_last_path(void)        { return M.lastPath ? M.lastPath : "-"; }
const char *gtav_mesh_why(void)              { return M.why; }
unsigned    gtav_mesh_fast_fails(void)       { return M.fastFails; }

void gtav_mesh_allow_builder(int on) { M.allowFast = on ? 1 : 0; }

void gtav_mesh_stats(double *pushMs, double *buildMs) {
	if (pushMs)  *pushMs  = M.pushMs;
	if (buildMs) *buildMs = M.buildMs;
}

unsigned gtav_mesh_bad_indices(void) { return M.badIndex; }


static unsigned char *vec_bytes(lua_State *L, int idx) {
	unsigned char *p = (unsigned char *)OW.touserdata(L, idx);
	if (!p) return NULL;
	if (M.indirect) {
		void *q = NULL;
		memcpy(&q, p, sizeof q);
		p = (unsigned char *)q;
	}
	return p;
}

void gtav_mesh_set_vector(lua_State *L, int idx, float x, float y, float z) {
	unsigned char *p = M.fast ? vec_bytes(L, idx) : NULL;
	if (!p) return;
	memcpy(p + M.offX, &x, 4);
	memcpy(p + M.offY, &y, 4);
	memcpy(p + M.offZ, &z, 4);
}

void gtav_mesh_push_vector(lua_State *L, float x, float y, float z) {
	OW.rawgeti(L, GTAV_LUA_REGISTRYINDEX, M.ctorRef);
	OW.pushnumber(L, x);
	OW.pushnumber(L, y);
	OW.pushnumber(L, z);
	OW.call(L, 3, 1);
}


static int find_offsets(const unsigned char *p, size_t n,
						int *ox, int *oy, int *oz) {
	int found[3] = { -1, -1, -1 };
	const float probes[3] = { PROBE_X, PROBE_Y, PROBE_Z };

	if (n < 12) return 0;

	for (int k = 0; k < 3; k++) {
		unsigned char raw[4];
		memcpy(raw, &probes[k], 4);
		for (size_t i = 0; i + 4 <= n; i++) {
			if (memcmp(p + i, raw, 4) == 0) {
				int dup = 0;
				for (size_t j = i + 4; j + 4 <= n; j++) {
					if (memcmp(p + j, raw, 4) == 0) { dup = 1; break; }
				}
				if (!dup) { found[k] = (int)i; }
				break;
			}
		}
		if (found[k] < 0) return 0;
	}

	if (found[0] == found[1] || found[1] == found[2] || found[0] == found[2]) {
		return 0;
	}
	*ox = found[0]; *oy = found[1]; *oz = found[2];
	return 1;
}


static int read_field_at(lua_State *L, int idx, const char *field, double *out) {
	OW.getfield(L, idx, field);
	if (OW.type(L, -1) != GTAV_LUA_TNUMBER) {
		OW.remove(L, -1);
		return 0;
	}
	*out = OW.tonumber(L, -1);
	OW.remove(L, -1);
	return 1;
}

static void push_ctor_vector(lua_State *L, double x, double y, double z) {
	OW.rawgeti(L, GTAV_LUA_REGISTRYINDEX, M.ctorRef);
	OW.pushnumber(L, x);
	OW.pushnumber(L, y);
	OW.pushnumber(L, z);
	OW.call(L, 3, 1);
}

// write in place, read back through __index
static int verify_fast_path(lua_State *L) {
	int top = OW.gettop(L);
	double vx = 0, vy = 0, vz = 0;
	int ok = 0;

	push_ctor_vector(L, PROBE_X, PROBE_Y, PROBE_Z);
	if (OW.type(L, -1) != GTAV_LUA_TUSERDATA) {
		OW.settop(L, top);
		return 0;
	}

	gtav_mesh_set_vector(L, -1, CHECK_X, CHECK_Y, CHECK_Z);
	if (read_field_at(L, -1, "x", &vx) &&
		read_field_at(L, -1, "y", &vy) &&
		read_field_at(L, -1, "z", &vz)) {
		ok = (vx == (double)CHECK_X) &&
			 (vy == (double)CHECK_Y) &&
			 (vz == (double)CHECK_Z);
	}

	OW.settop(L, top);
	return ok;
}


int gtav_mesh_init(lua_State *L, char *err, size_t errlen) {
	if (M.ready) return M.fast;

	int top = OW.gettop(L);
	M.why[0] = '\0';
	M.ctorRef = -1;

	OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "Vector");
	if (OW.type(L, -1) != GTAV_LUA_TFUNCTION) {
		snprintf(M.why, sizeof M.why, "global Vector() is missing");
		seterr(err, errlen, "%s", M.why);
		OW.settop(L, top);
		M.ready = 1;
		return 0;
	}
	M.ctorRef = OW.ref(L, GTAV_LUA_REGISTRYINDEX);   /* pops it */

	push_ctor_vector(L, PROBE_X, PROBE_Y, PROBE_Z);

	if (OW.type(L, -1) != GTAV_LUA_TUSERDATA) {
		snprintf(M.why, sizeof M.why, "Vector() did not return userdata");
		seterr(err, errlen, "%s", M.why);
		OW.settop(L, top);
		M.ready = 1;
		return 0;
	}

	M.vecSize = OW.objlen(L, -1);
	const unsigned char *p = (const unsigned char *)OW.touserdata(L, -1);

	if (p && find_offsets(p, M.vecSize, &M.offX, &M.offY, &M.offZ)) {
		M.indirect = 0;
		M.fast = 1;
	} else if (p && M.vecSize >= sizeof(void *)) {
		// gmod userdata wrapper, floats are one hop away
		void *q = NULL;
		memcpy(&q, p, sizeof q);
		if (q && find_offsets((const unsigned char *)q, PROBE_WINDOW,
							  &M.offX, &M.offY, &M.offZ)) {
			M.indirect = 1;
			M.fast = 1;
		}
	}

	if (!M.fast) {
		snprintf(M.why, sizeof M.why,
				 "Vector userdata (%u bytes) does not expose x/y/z; "
				 "Mesh:BuildFromTriangles with Vector() per corner",
				 (unsigned)M.vecSize);
		seterr(err, errlen, "%s", M.why);
		OW.settop(L, top);
		M.ready = 1;
		return 0;
	}

	if (!verify_fast_path(L)) {
		M.fast = 0;
		snprintf(M.why, sizeof M.why,
				 "Vector x/y/z offsets found but the write round trip was not "
				 "exact; Mesh:BuildFromTriangles with Vector() per corner");
		seterr(err, errlen, "%s", M.why);
		OW.settop(L, top);
		M.ready = 1;
		return 0;
	}

	OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "mesh");
	if (OW.type(L, -1) == GTAV_LUA_TTABLE) {
		static const char *need[] = {
			"Begin", "Position", "Normal", "TexCoord", "AdvanceVertex", "End",
			NULL
		};
		int all = 1;
		for (int i = 0; need[i]; i++) {
			OW.getfield(L, -1, need[i]);
			int isfn = (OW.type(L, -1) == GTAV_LUA_TFUNCTION);
			OW.remove(L, -1);
			if (!isfn) { all = 0; break; }
		}
		M.builder = all;
	}
	OW.settop(L, top);

	snprintf(M.why, sizeof M.why,
			 "Vector x/y/z at %d/%d/%d (%s), write round trip exact; builder %s",
			 M.offX, M.offY, M.offZ, M.indirect ? "indirect" : "inline",
			 M.builder ? "mesh.Begin" : "Mesh:BuildFromTriangles");

	M.ready = 1;
	return M.fast;
}

void gtav_mesh_shutdown(void) {
	M.ready = 0;
	M.fast = 0;
	M.builder = 0;
	M.fastFails = 0;
	M.ctorRef = -1;
	M.lastPath = "-";
}


// world is UnlitGeneric, nothing reads normals
#ifndef GTAV_EMIT_NORMALS
#define GTAV_EMIT_NORMALS 0
#endif

static int emit_corner(lua_State *L,
					   int fPos, int vPos, int fNrm, int vNrm,
					   int fTex, int fCol, int fAdv,
					   const gtav_vertex *v, const float *rgb) {
	gtav_mesh_set_vector(L, vPos, v->x, v->y, v->z);
	OW.pushvalue(L, fPos);
	OW.pushvalue(L, vPos);
	if (OW.pcall(L, 1, 0, 0) != 0) return 0;

#if GTAV_EMIT_NORMALS
	gtav_mesh_set_vector(L, vNrm, v->nx, v->ny, v->nz);
	OW.pushvalue(L, fNrm);
	OW.pushvalue(L, vNrm);
	if (OW.pcall(L, 1, 0, 0) != 0) return 0;
#else
	(void)fNrm;
	(void)vNrm;
#endif

	OW.pushvalue(L, fTex);
	OW.pushnumber(L, 0);
	OW.pushnumber(L, v->u);
	OW.pushnumber(L, v->v);
	if (OW.pcall(L, 3, 0, 0) != 0) return 0;

	if (fCol >= 0) {
		int cr = (int)(rgb[0] * 255.0f + 0.5f);
		int cg = (int)(rgb[1] * 255.0f + 0.5f);
		int cb = (int)(rgb[2] * 255.0f + 0.5f);
		if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
		if (cg < 0) cg = 0; else if (cg > 255) cg = 255;
		if (cb < 0) cb = 0; else if (cb > 255) cb = 255;
		OW.pushvalue(L, fCol);
		OW.pushnumber(L, cr);
		OW.pushnumber(L, cg);
		OW.pushnumber(L, cb);
		OW.pushnumber(L, 255);
		if (OW.pcall(L, 4, 0, 0) != 0) return 0;
	}

	OW.pushvalue(L, fAdv);
	return OW.pcall(L, 0, 0, 0) == 0;
}

static int build_via_mesh_begin(lua_State *L, int meshIdx,
								const gtav_vertex *verts, uint32_t nverts,
								const uint32_t *indices, uint32_t nindices,
								const float *shade) {
	int top = OW.gettop(L);

	OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "mesh");
	if (OW.type(L, -1) != GTAV_LUA_TTABLE) { OW.settop(L, top); return 0; }
	int mtab = OW.gettop(L);

	OW.getfield(L, mtab, "Begin");         int fBegin = OW.gettop(L);
	OW.getfield(L, mtab, "Position");      int fPos   = OW.gettop(L);
	OW.getfield(L, mtab, "Normal");        int fNrm   = OW.gettop(L);
	OW.getfield(L, mtab, "TexCoord");      int fTex   = OW.gettop(L);
	OW.getfield(L, mtab, "Color");         int fCol   = OW.gettop(L);
	if (shade == NULL || OW.type(L, -1) != GTAV_LUA_TFUNCTION) fCol = -1;
	OW.getfield(L, mtab, "AdvanceVertex"); int fAdv   = OW.gettop(L);
	OW.getfield(L, mtab, "End");           int fEnd   = OW.gettop(L);

	OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "MATERIAL_TRIANGLES");
	int primType = (int)OW.tonumber(L, -1);
	OW.remove(L, -1);

	push_ctor_vector(L, 0, 0, 0);
	if (OW.type(L, -1) != GTAV_LUA_TUSERDATA) { OW.settop(L, top); return 0; }
	int vPos = OW.gettop(L);
	push_ctor_vector(L, 0, 0, 0);
	if (OW.type(L, -1) != GTAV_LUA_TUSERDATA) { OW.settop(L, top); return 0; }
	int vNrm = OW.gettop(L);

	OW.pushvalue(L, fBegin);
	OW.pushvalue(L, meshIdx);
	OW.pushnumber(L, primType);
	// corners not tris, dont touch this
	OW.pushnumber(L, (lua_Number)nindices);
	if (OW.pcall(L, 3, 0, 0) != 0) {
		OW.settop(L, top);
		return 0;
	}

	int ok = 1;
	const gtav_vertex *prev = &verts[0];   /* caller guarantees nverts >= 1 */
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *prevCol = white;
	for (uint32_t k = 0; k < nindices; k++) {
		uint32_t vi = indices[k];
		const gtav_vertex *v;
		const float *s;
		if (vi < nverts) {
			v = &verts[vi];
			s = shade ? &shade[(size_t)vi * 3] : white;
			prev = v;
			prevCol = s;
		} else {
			// repeat last corner, keeps the triples in phase
			v = prev;
			s = prevCol;
			M.badIndex++;
		}
		if (!emit_corner(L, fPos, vPos, fNrm, vNrm, fTex, fCol, fAdv, v, s)) {
			ok = 0;
			break;
		}
	}

	OW.settop(L, vNrm);          /* drop any error message the loop left */
	OW.pushvalue(L, fEnd);
	if (OW.pcall(L, 0, 0, 0) != 0) ok = 0;

	OW.settop(L, top);
	return ok;
}

static int mesh_bounds_ok(lua_State *L, int meshIdx,
						  const gtav_vertex *verts, uint32_t nverts,
						  const uint32_t *indices, uint32_t nindices) {
	int top = OW.gettop(L);
	double lo[3] = {  1e30,  1e30,  1e30 };
	double hi[3] = { -1e30, -1e30, -1e30 };
	int any = 0;

	for (uint32_t k = 0; k < nindices; k++) {
		uint32_t vi = indices[k];
		if (vi >= nverts) continue;
		const gtav_vertex *v = &verts[vi];
		float c[3] = { v->x, v->y, v->z };
		for (int i = 0; i < 3; i++) {
			if (c[i] < lo[i]) lo[i] = c[i];
			if (c[i] > hi[i]) hi[i] = c[i];
		}
		any = 1;
	}
	if (!any) return 1;

	OW.getfield(L, meshIdx, "GetBounds");
	if (OW.type(L, -1) != GTAV_LUA_TFUNCTION) { OW.settop(L, top); return 1; }
	OW.pushvalue(L, meshIdx);
	if (OW.pcall(L, 1, 2, 0) != 0) { OW.settop(L, top); return 1; }

	int vlo = OW.gettop(L) - 1;
	int vhi = OW.gettop(L);
	double blo[3], bhi[3];
	int readable = 1;
	for (int i = 0; i < 3; i++) {
		const char *f = (i == 0) ? "x" : (i == 1) ? "y" : "z";
		if (!read_field_at(L, vlo, f, &blo[i]) ||
			!read_field_at(L, vhi, f, &bhi[i])) {
			readable = 0;
			break;
		}
	}
	OW.settop(L, top);
	if (!readable) return 1;

	for (int i = 0; i < 3; i++) {
		double tol = 0.05 + 0.001 * (hi[i] - lo[i]);
		if (blo[i] > lo[i] + tol || blo[i] < lo[i] - tol) return 0;
		if (bhi[i] > hi[i] + tol || bhi[i] < hi[i] - tol) return 0;
	}
	return 1;
}


static int build_via_triangles(lua_State *L,
							   const gtav_vertex *verts, uint32_t nverts,
							   const uint32_t *indices, uint32_t nindices,
							   const float *shade) {
	int top = OW.gettop(L);
	int ok = 0;

	int *posRef = (int *)malloc(sizeof(int) * (nverts ? nverts : 1));
	int *nrmRef = (int *)malloc(sizeof(int) * (nverts ? nverts : 1));
	int *colRef = (int *)malloc(sizeof(int) * (nverts ? nverts : 1));
	if (!posRef || !nrmRef || !colRef) {
		free(posRef); free(nrmRef); free(colRef);
		return 0;
	}
	for (uint32_t i = 0; i < nverts; i++) {
		posRef[i] = -2; nrmRef[i] = -2; colRef[i] = -2;
	}

	int colorCtor = -1;
	if (shade) {
		OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "Color");
		if (OW.type(L, -1) == GTAV_LUA_TFUNCTION) {
			colorCtor = OW.ref(L, GTAV_LUA_REGISTRYINDEX);
		} else {
			OW.remove(L, -1);
		}
	}

	OW.createtable(L, (int)nindices, 0);

	int n = 0;

	for (uint32_t k = 0; k < nindices; k++) {
		uint32_t vi = indices[k];
		const gtav_vertex *v;

		if (vi >= nverts) {
			M.badIndex++;
			if (n > 0) {
				OW.rawgeti(L, -1, n);
				n = n + 1;
				OW.rawseti(L, -2, n);
				continue;
			}
			vi = 0;
		}
		v = &verts[vi];

		OW.createtable(L, 0, 4);

		if (posRef[vi] == -2) {
			push_ctor_vector(L, 0, 0, 0);
			gtav_mesh_set_vector(L, -1, v->x, v->y, v->z);
			posRef[vi] = OW.ref(L, GTAV_LUA_REGISTRYINDEX);
		}
		OW.rawgeti(L, GTAV_LUA_REGISTRYINDEX, posRef[vi]);
		OW.setfield(L, -2, "pos");

		OW.pushnumber(L, v->u);
		OW.setfield(L, -2, "u");
		OW.pushnumber(L, v->v);
		OW.setfield(L, -2, "v");

		if (nrmRef[vi] == -2) {
			push_ctor_vector(L, 0, 0, 0);
			gtav_mesh_set_vector(L, -1, v->nx, v->ny, v->nz);
			nrmRef[vi] = OW.ref(L, GTAV_LUA_REGISTRYINDEX);
		}
		OW.rawgeti(L, GTAV_LUA_REGISTRYINDEX, nrmRef[vi]);
		OW.setfield(L, -2, "normal");

		if (colorCtor >= 0) {
			if (colRef[vi] == -2) {
				const float *rgb = &shade[(size_t)vi * 3];
				int cr = (int)(rgb[0] * 255.0f + 0.5f);
				int cg = (int)(rgb[1] * 255.0f + 0.5f);
				int cb = (int)(rgb[2] * 255.0f + 0.5f);
				if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
				if (cg < 0) cg = 0; else if (cg > 255) cg = 255;
				if (cb < 0) cb = 0; else if (cb > 255) cb = 255;
				OW.rawgeti(L, GTAV_LUA_REGISTRYINDEX, colorCtor);
				OW.pushnumber(L, cr);
				OW.pushnumber(L, cg);
				OW.pushnumber(L, cb);
				OW.pushnumber(L, 255);
				OW.call(L, 4, 1);
				colRef[vi] = OW.ref(L, GTAV_LUA_REGISTRYINDEX);
			}
			OW.rawgeti(L, GTAV_LUA_REGISTRYINDEX, colRef[vi]);
			OW.setfield(L, -2, "color");
		}

		n = n + 1;
		OW.rawseti(L, -2, n);
	}

	OW.getfield(L, -2, "BuildFromTriangles");
	if (OW.type(L, -1) == GTAV_LUA_TFUNCTION) {
		OW.pushvalue(L, -3);    /* self = mesh */
		OW.pushvalue(L, -3);    /* arg  = tris */
		ok = (OW.pcall(L, 2, 0, 0) == 0);
	}

	for (uint32_t i = 0; i < nverts; i++) {
		if (posRef[i] != -2) OW.unref(L, GTAV_LUA_REGISTRYINDEX, posRef[i]);
		if (nrmRef[i] != -2) OW.unref(L, GTAV_LUA_REGISTRYINDEX, nrmRef[i]);
		if (colRef[i] != -2) OW.unref(L, GTAV_LUA_REGISTRYINDEX, colRef[i]);
	}
	if (colorCtor >= 0) OW.unref(L, GTAV_LUA_REGISTRYINDEX, colorCtor);
	free(posRef); free(nrmRef); free(colRef);

	OW.settop(L, top);
	return ok;
}


int gtav_mesh_build(lua_State *L,
				  const gtav_vertex *verts, uint32_t nverts,
				  const uint32_t *indices, uint32_t nindices,
				  const float *shade,
				  char *err, size_t errlen) {
	if (!verts || nverts == 0 || !indices || nindices == 0) {
		seterr(err, errlen, "empty geometry");
		return 0;
	}
	if (nindices % 3 != 0) {
		seterr(err, errlen, "index count %u is not a multiple of 3",
			   (unsigned)nindices);
		return 0;
	}
	if (nindices > GTAV_MESH_MAX_CORNERS) {
		seterr(err, errlen,
			   "%u corners exceeds the %u per-mesh limit; split this build",
			   (unsigned)nindices, (unsigned)GTAV_MESH_MAX_CORNERS);
		return 0;
	}
	if (M.ctorRef < 0) {
		seterr(err, errlen, "Vector constructor was never resolved: %s",
			   M.why[0] ? M.why : "gtav_mesh_init has not run");
		return 0;
	}
	if (!OW.checkstack(L, 64)) {
		seterr(err, errlen, "Lua stack overflow");
		return 0;
	}

	int top = OW.gettop(L);
	double t0 = now_ms();

	if (gtav_mesh_builder()) {
		// mesh = Mesh()
		OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "Mesh");
		if (OW.type(L, -1) != GTAV_LUA_TFUNCTION) {
			seterr(err, errlen, "global Mesh() is missing");
			OW.settop(L, top);
			return 0;
		}
		OW.call(L, 0, 1);
		if (OW.type(L, -1) != GTAV_LUA_TUSERDATA) {
			seterr(err, errlen, "Mesh() did not return a Mesh object");
			OW.settop(L, top);
			return 0;
		}
		int meshIdx = OW.gettop(L);

		if (build_via_mesh_begin(L, meshIdx, verts, nverts, indices, nindices,
								 shade) &&
			mesh_bounds_ok(L, meshIdx, verts, nverts, indices, nindices)) {
			M.pushMs = 0;
			M.buildMs = now_ms() - t0;
			M.lastPath = "fast";
			return 1;
		}
		M.fastFails++;
		OW.settop(L, top);
	}

	// slow path
	OW.getfield(L, GTAV_LUA_GLOBALSINDEX, "Mesh");
	if (OW.type(L, -1) != GTAV_LUA_TFUNCTION) {
		seterr(err, errlen, "global Mesh() is missing");
		OW.settop(L, top);
		return 0;
	}
	OW.call(L, 0, 1);
	if (OW.type(L, -1) != GTAV_LUA_TUSERDATA) {
		seterr(err, errlen, "Mesh() did not return a Mesh object");
		OW.settop(L, top);
		return 0;
	}

	if (!build_via_triangles(L, verts, nverts, indices, nindices, shade)) {
		seterr(err, errlen, "Mesh:BuildFromTriangles failed");
		OW.settop(L, top);
		return 0;
	}

	M.pushMs = 0;
	M.buildMs = now_ms() - t0;
	M.lastPath = "triangles";
	return 1;
}
