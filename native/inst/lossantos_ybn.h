#ifndef LOSSANTOS_YBN_H
#define LOSSANTOS_YBN_H

#include "lossantos_rsc.h"

typedef struct {
	size_t ntris, cap;
	double *tri;            /* ntris * 9 (a, b, c), GTA metres */
	uint8_t *mat;           /* ntris */
	double bmin[3], bmax[3];
	uint32_t blocks, composites, geometries, bvhs, boxes, skipped_polys, skipped_blocks;
} lossantos_ybn;

int lossantos_ybn_parse(const uint8_t *data, size_t n, size_t system_size, lossantos_ybn *out);
void lossantos_ybn_free(lossantos_ybn *y);

#endif
