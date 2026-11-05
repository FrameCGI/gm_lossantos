#ifndef LOSSANTOS_WATER_H
#define LOSSANTOS_WATER_H

#include "lossantos_crypto.h"
#include "lossantos_tile.h"

#define LOSSANTOS_WATER_MATERIAL "models/gtav/water/ocean"

typedef struct {
	double x0, x1, y0, y1, z;
} lossantos_rect;

typedef struct {
	int32_t gx, gy;
	uint32_t first, count;   /* into rects */
} lossantos_water_cell;

typedef struct {
	LOSSANTOS_VEC(lossantos_rect) quads;          /* Source units */
	LOSSANTOS_VEC(lossantos_rect) rects;          /* per cell, clipped + resolved */
	LOSSANTOS_VEC(lossantos_water_cell) cells;    /* sorted by (gx, gy) */
} lossantos_water;

// common.rpf water.xml
int lossantos_water_load(lossantos_water *w, const wchar_t *gta_dir, const lossantos_ng *ng);
int lossantos_water_cells(lossantos_water *w, int32_t gx0, int32_t gx1, int32_t gy0, int32_t gy1);
const lossantos_water_cell *lossantos_water_find(const lossantos_water *w, int32_t gx, int32_t gy);
int lossantos_water_append(const lossantos_water *w, const lossantos_water_cell *c, uint32_t mat, lossantos_tilemem *t);
void lossantos_water_free(lossantos_water *w);

#endif
