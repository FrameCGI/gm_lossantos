#ifndef LOSSANTOS_OUT_H
#define LOSSANTOS_OUT_H

#include "lossantos_common.h"
#include "lossantos_util.h"

typedef struct {
	uint32_t mat, ioff, icount, voff;
} lossantos_submesh;

typedef struct {
	lossantos_strtab names;
	LOSSANTOS_VEC(lossantos_submesh) subs;
	lossantos_buf verts;               /* 28-byte vertices */
	LOSSANTOS_VEC(uint16_t) idx;
	// current block
	int open;
	uint32_t cur_mat;
	uint32_t *wgen, *wvert, gen;
	uint64_t *whash;
	LOSSANTOS_VEC(int64_t) wkeys;      /* 8 per local vertex */
	uint32_t block_verts;
	int single_sided;           /* 0 = [fronts][mirrors] per run */
	LOSSANTOS_VEC(double) pending;     /* double sided fronts, 24 doubles each */
} lossantos_tilebuild;

void lossantos_tb_sided(lossantos_tilebuild *t, int single_sided);

int lossantos_tb_init(lossantos_tilebuild *t);
void lossantos_tb_free(lossantos_tilebuild *t);
void lossantos_tb_reset(lossantos_tilebuild *t);
int lossantos_tb_material(lossantos_tilebuild *t, const char *name);
int lossantos_tb_tri(lossantos_tilebuild *t, const double *pos, const double *nrm, const double *uv);
int lossantos_tb_close(lossantos_tilebuild *t);
uint32_t lossantos_tb_vertices(const lossantos_tilebuild *t);
int lossantos_tb_write(lossantos_tilebuild *t, const wchar_t *path);

// cap_tris 1 = biggest 21845 tris only
int lossantos_write_owc1(const wchar_t *path, const double *tri, size_t n, int cap_tris, size_t *dropped);
int lossantos_encode_owc1(const double *tri, size_t n, int cap_tris, size_t *dropped, lossantos_buf *out);

#endif
