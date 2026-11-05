#ifndef LOSSANTOS_CLIP_H
#define LOSSANTOS_CLIP_H

#include <stdint.h>

typedef void (*lossantos_piece_fn)(void *ctx, int64_t cx, int64_t cy, const double *pos, const double *nrm,
							 const double *uv);

// returns pieces emitted
int lossantos_clip_tri(const double *pos, const double *nrm, const double *uv, double cell, lossantos_piece_fn fn,
				 void *ctx);

#endif
