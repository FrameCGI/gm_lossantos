#ifndef LOSSANTOS_BEACH_H
#define LOSSANTOS_BEACH_H

#include "lossantos_tile.h"

#define LOSSANTOS_SAND_MATERIAL "models/gtav/vb_36/im_ground_sand_01"

typedef struct {
	float x0, y0, x1, y1, z;
	uint8_t height;      /* counts for the local z */
} lossantos_marker;
typedef LOSSANTOS_VEC(lossantos_marker) lossantos_marker_vec;

typedef struct {
	int64_t gx0, gy0, nx, ny;
	double cell;
	int rc, rcz;        /* band, z radius in cells */
	uint8_t *band;      /* marker within 75 m */
	double *zmean;      /* NaN = no reference */
} lossantos_beach;

int lossantos_is_marker_name(const char *name);
int lossantos_is_height_marker_name(const char *name);
int lossantos_is_sand_source(const char *name);
int lossantos_is_sand_ambig(const char *name);

int lossantos_beach_build(lossantos_beach *b, int32_t tx0, int32_t tx1, int32_t ty0, int32_t ty1, const lossantos_marker *m,
					size_t n);
void lossantos_beach_free(lossantos_beach *b);

// returns tris sanded
size_t lossantos_beach_apply(const lossantos_beach *b, lossantos_tilemem *t, const char *const *names, uint32_t sand_mat);

#endif
