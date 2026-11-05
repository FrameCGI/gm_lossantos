#ifndef GTAV_SHADOW_H
#define GTAV_SHADOW_H

#include <stddef.h>
#include <stdint.h>

// GVSD, written by gtav_bake.c
// 0   magic "GVSD"
// 4   version u16
// 6   flags u16, always 0
// 8   sun azim rad
// 12  sun elev rad
// 16  bu[3]
// 28  bv[3]
// 40  bd[3]
// 52  uLo
// 56  vLo
// 60  texel size
// 64  tile texels
// 68  tiles x
// 72  tiles y
// 76  dLo
// 80  dInvScale
// 84  reserved
// 88  tile offsets u32, tilesX * tilesY, 0 = empty
// ..  tiles, tileTexels^2 u16 max d codes, row major
#define GVSD_MAGIC   0x44535647u   /* "GVSD" little endian */
#define GVSD_VERSION 1
#define GVSD_HEADER  88

int gtav_shadow_open(const char *mapRoot, char *err, size_t errlen);

int gtav_shadow_open_file(const char *path, char *err, size_t errlen);

// release the mapping
void gtav_shadow_close(void);

int gtav_shadow_active(void);

void gtav_shadow_set_baked(int on);
int  gtav_shadow_baked(void);

void  gtav_shadow_set_light(float ambient, float sun, float exposure);
void  gtav_shadow_get_light(float *ambient, float *sun, float *exposure);

void gtav_shadow_sun(float *azimRad, float *elevRad);

float gtav_shadow_visibility(float x, float y, float z,
						   float nx, float ny, float nz);

void  gtav_shadow_shade_rgb(float x, float y, float z,
						  float nx, float ny, float nz, float out[3]);

const char *gtav_shadow_info(void);

#endif /* GTAV_SHADOW_H */
