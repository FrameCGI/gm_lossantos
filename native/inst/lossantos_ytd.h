#ifndef LOSSANTOS_YTD_H
#define LOSSANTOS_YTD_H

#include "lossantos_rsc.h"

// d3d9 format ids
#define LOSSANTOS_FMT_A8R8G8B8 21u
#define LOSSANTOS_FMT_X8R8G8B8 22u
#define LOSSANTOS_FMT_A1R5G5B5 25u
#define LOSSANTOS_FMT_A8 28u
#define LOSSANTOS_FMT_A8B8G8R8 32u
#define LOSSANTOS_FMT_L8 50u
// fourcc
#define LOSSANTOS_FMT_DXT1 0x31545844u
#define LOSSANTOS_FMT_DXT3 0x33545844u
#define LOSSANTOS_FMT_DXT5 0x35545844u
#define LOSSANTOS_FMT_ATI1 0x31495441u
#define LOSSANTOS_FMT_ATI2 0x32495441u
#define LOSSANTOS_FMT_BC7 0x20374342u

typedef struct {
	char name[128];
	uint32_t width, height, depth, stride, format;
	uint32_t levels;          /* complete levels present */
	const uint8_t *data;      /* level 0 first, levels contiguous */
	size_t data_n;
} lossantos_tex;

size_t lossantos_tex_level_size(uint32_t fmt, uint32_t width, uint32_t height, uint32_t stride, uint32_t level);

int lossantos_ytd_parse(lossantos_rsc *r, uint64_t addr, lossantos_tex *out, int cap);

int lossantos_tex_decode(const lossantos_tex *t, uint32_t level, uint8_t *dst, size_t dst_cap, uint32_t *w, uint32_t *h);

#endif
