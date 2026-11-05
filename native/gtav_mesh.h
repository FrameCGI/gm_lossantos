#ifndef GTAV_MESH_H
#define GTAV_MESH_H

#include <stddef.h>
#include <stdint.h>

#include "gtav_decode.h"
#include "gtav_luaapi.h"

// not 65535, mesh.Begin arg 3 is ambiguous
#define GTAV_MESH_MAX_CORNERS 21843u
#define GTAV_MESH_MAX_TRIS    (GTAV_MESH_MAX_CORNERS / 3u)

int  gtav_mesh_init(lua_State *L, char *err, size_t errlen);
void gtav_mesh_shutdown(void);

int  gtav_mesh_fast_vectors(void);

int  gtav_mesh_builder(void);

unsigned gtav_mesh_fast_fails(void);

void gtav_mesh_allow_builder(int on);

const char *gtav_mesh_last_path(void);

const char *gtav_mesh_why(void);

void gtav_mesh_stats(double *pushMs, double *buildMs);

unsigned gtav_mesh_bad_indices(void);

void gtav_mesh_push_vector(lua_State *L, float x, float y, float z);

void gtav_mesh_set_vector(lua_State *L, int idx, float x, float y, float z);

int gtav_mesh_build(lua_State *L,
				  const gtav_vertex *verts, uint32_t nverts,
				  const uint32_t *indices, uint32_t nindices,
				  const float *shade,
				  char *err, size_t errlen);

#endif /* GTAV_MESH_H */
