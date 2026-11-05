#ifndef LOSSANTOS_YDR_H
#define LOSSANTOS_YDR_H

#include "lossantos_ytd.h"

typedef struct {
	uint32_t nverts, ntris;
	float *pos;        /* nverts * 3, local */
	float *uv;         /* nverts * 2, zeros if no uv0 */
	float *nrm;        /* nverts * 3, or NULL */
	uint32_t *idx;     /* ntris * 3, as stored */
	uint32_t shader;
	char texture[128]; /* "" = none */
} lossantos_geom;

typedef struct {
	double bmin[3], bmax[3];
	int ngeoms;
	lossantos_geom *geoms;
	int ntex;
	lossantos_tex *tex;      /* points into the payload, keep it alive */
} lossantos_drawable;

int lossantos_ydr_parse(const uint8_t *data, size_t n, size_t system_size, lossantos_drawable *d);
void lossantos_ydr_free(lossantos_drawable *d);

#endif
