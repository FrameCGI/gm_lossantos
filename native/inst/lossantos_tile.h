#ifndef LOSSANTOS_TILE_H
#define LOSSANTOS_TILE_H

#include <string.h>

#include "lossantos_common.h"
#include "lossantos_util.h"

typedef struct {
	uint32_t mat;               /* caller's material id */
	uint32_t nverts, nidx;
	uint8_t *v;                 /* 28 * nverts, GVT1 vertex encoding */
	uint16_t *ix;               /* nidx local indices */
} lossantos_trun;

typedef struct {
	LOSSANTOS_VEC(lossantos_trun) runs;
} lossantos_tilemem;

void lossantos_tilemem_free(lossantos_tilemem *t);
// append a run (copies the data)
int lossantos_tilemem_add(lossantos_tilemem *t, uint32_t mat, const uint8_t *v, uint32_t nverts, const uint16_t *ix,
					uint32_t nidx);
size_t lossantos_tilemem_tris(const lossantos_tilemem *t);

// 1 written, 0 empty, -1 error
int lossantos_tilemem_encode(const lossantos_tilemem *t, const char *const *names, lossantos_buf *out, size_t *tris,
					   size_t *verts);

// GVPG sector page, v_px_py.bin / p_px_py.bin
typedef struct {
	char name[40];
	size_t off, len;
} lossantos_page_entry;
typedef struct {
	LOSSANTOS_VEC(lossantos_page_entry) entries;
	lossantos_buf data;
} lossantos_page;
int lossantos_page_add(lossantos_page *pg, const char *name, const void *blob, size_t len);
int lossantos_page_write(lossantos_page *pg, const wchar_t *path);   /* entries sorted by name */
void lossantos_page_free(lossantos_page *pg);

int lossantos_tilemem_write(const lossantos_tilemem *t, const char *const *names, const wchar_t *path, size_t *tris,
					  size_t *verts);

// coarse 0    position + normal step 4 + uv
// coarse 1    position + normal step nrm_step
// nrm_step 0  position only
// box         tile x0 y0 x1 y1, border verts stay
int lossantos_lod_cluster(const lossantos_tilemem *in, double cell, int coarse, int nrm_step, const double *box,
					lossantos_tilemem *out);

// decode helpers
static inline void lossantos_vtx_pos(const uint8_t *v, double p[3]) {
	float f[3];
	memcpy(f, v, 12);
	p[0] = f[0]; p[1] = f[1]; p[2] = f[2];
}
void lossantos_vtx_encode(uint8_t *o, const double *p, const double *n, const double *uv);

#endif
