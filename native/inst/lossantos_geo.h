#ifndef LOSSANTOS_GEO_H
#define LOSSANTOS_GEO_H

#include "lossantos_ybn.h"

#define LOSSANTOS_SCALE 39.3700787  // source units per metre

// 1 = same as the python exporter, 0 = fixed
typedef struct {
	int dedupe_x_only;     /* dedupe_triangles sorts corners on x only */
	int median_mixed_units; /* median_strips cell from Source-unit area */
} lossantos_compat;

int lossantos_coll_push(lossantos_ybn *y, const double *a, const double *b, const double *c, uint8_t mat);
int lossantos_coll_append(lossantos_ybn *dst, const lossantos_ybn *src, const uint8_t *keep);
void lossantos_coll_init(lossantos_ybn *y);
int lossantos_merge_hi(const lossantos_ybn *base, const lossantos_ybn *hi, lossantos_ybn *out);

double lossantos_tri_normal(const double *t, double n[3]);  /* unit normal, returns area */
size_t lossantos_dedupe_tris(const double *tri, size_t n, const lossantos_compat *cx, uint8_t *keep);

enum { LOSSANTOS_CAT_ROAD, LOSSANTOS_CAT_SIDEWALK, LOSSANTOS_CAT_WINDOWS, LOSSANTOS_CAT_GRASS, LOSSANTOS_CAT_METAL,
	   LOSSANTOS_CAT_WALL, LOSSANTOS_CAT_GROUND, LOSSANTOS_CAT_CEILING, LOSSANTOS_CAT_MEDIAN, LOSSANTOS_CAT_COUNT };
extern const char *const LOSSANTOS_PALETTE_NAMES[LOSSANTOS_CAT_COUNT];   /* 'losantos' */
extern const double LOSSANTOS_PALETTE_METRES[LOSSANTOS_CAT_COUNT];

int lossantos_categories(const double *tri, const double *nrm, const uint8_t *mat, size_t n,
				   const lossantos_compat *cx, uint8_t *cat);
void lossantos_planar_uv(const double *tri, const double *nrm, double repeat, double *uv);

// covered[i] per coll tri
int lossantos_shell_covered(const double *coll, size_t ncoll, const double *art, size_t nart, uint8_t *covered);
int lossantos_filler_covered(const double *coll, size_t ncoll, const double *art, size_t nart, uint8_t *covered);

#endif
