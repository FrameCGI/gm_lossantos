#ifndef LOSSANTOS_SLOD_H
#define LOSSANTOS_SLOD_H

#include "lossantos_block.h"

#define LOSSANTOS_SLOD_FALLBACK "models/gtav/dt1_02/im_dt1_02_wall_01"

typedef struct {
	lossantos_soup vis;               /* Source units */
	lossantos_strtab mats;
	lossantos_install_vec installs;   /* models/gtav/slod/... */
	uint32_t packs, placed, fallback_geoms;
	char error[256];
} lossantos_slod_out;

// every *_lod*.rpf of the city districts
int lossantos_slod_build(lossantos_world *w, lossantos_slod_out *out);
void lossantos_slod_free(lossantos_slod_out *o);

#endif
