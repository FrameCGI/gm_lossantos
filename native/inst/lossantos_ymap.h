#ifndef LOSSANTOS_YMAP_H
#define LOSSANTOS_YMAP_H

#include "lossantos_rsc.h"

typedef struct {
	uint32_t archetype;
	float pos[3];
	float rot[4];          /* x, y, z, w */
	float scale_xy, scale_z;
	uint32_t flags, lod_level;
} lossantos_entity;

typedef struct {
	uint32_t name, asset_name;
	float bb_min[3], bb_max[3];
	float lod_dist;
} lossantos_archetype;

int lossantos_ymap_entities(const uint8_t *data, size_t n, size_t system_size, lossantos_entity *out, int cap);
int lossantos_ytyp_archetypes(const uint8_t *data, size_t n, size_t system_size, lossantos_archetype *out, int cap);

// gta name hash, lowercase ascii
uint32_t lossantos_joaat(const char *name);

#endif
