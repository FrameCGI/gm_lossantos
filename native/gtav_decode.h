#ifndef GTAV_DECODE_H
#define GTAV_DECODE_H

#include <stddef.h>
#include <stdint.h>

// GVT1 tile, exporter/gtav/world.py
// 0   magic "GVT1"
// 4   version u16
// 6   flags u16, bit0 lightmap uv, bit1 normals
// 8   vertex count
// 12  index count
// 16  submesh count
// 20  material count
// 24  vertex stride
// 28  reserved
// 32  submeshes, 16 bytes each (gtav_submesh order)
// ..  material names, 64 bytes each
// ..  vertices, stride bytes each
// ..  indices u16

// vertex, stride 28
// 0   x y z f32
// 12  u v f32
// 20  lightmap u v u16
// 24  nx ny nz i8 (20 without lightmap uv), pad

typedef struct {
	float x, y, z;
	float u, v;
	float nx, ny, nz;
} gtav_vertex;

typedef struct {
	uint32_t material;      /* index into gtav_tile::materials */
	uint32_t indexOffset;
	uint32_t indexCount;
	uint32_t vertexOffset;
} gtav_submesh;

typedef struct {
	uint16_t version;
	uint16_t flags;
	uint32_t vertexCount;
	uint32_t indexCount;
	uint32_t submeshCount;
	uint32_t materialCount;
	uint32_t stride;

	uint32_t clamped;

	char      (*materials)[64];
	gtav_vertex *vertices;
	uint16_t  *indices;
	gtav_submesh *submeshes;
} gtav_tile;

int  gtav_tile_decode(const uint8_t *blob, size_t len, gtav_tile *out,
					char *err, size_t errlen);
void gtav_tile_free(gtav_tile *t);

#endif /* GTAV_DECODE_H */
