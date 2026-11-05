#include "lossantos_ymap.h"

#include <ctype.h>

#define META_ROOT 0x50000000ull
#define CMAPDATA_SIZE 512u
#define CMAPTYPES_SIZE 80u

typedef struct {
	uint32_t name, size;
	uint64_t data;
} block;

static int blocks(lossantos_rsc *r, block *out, int cap) {
	uint64_t p = lossantos_rsc_u64(r, META_ROOT + 0x30);  // block info array
	int16_t count = (int16_t)lossantos_rsc_u16(r, META_ROOT + 0x4C);  // block count
	int n = 0;
	for (int i = 0; i < count && n < cap; i++, n++) {
		out[n].name = lossantos_rsc_u32(r, p + (uint64_t)i * 16);
		out[n].size = lossantos_rsc_u32(r, p + (uint64_t)i * 16 + 4);
		out[n].data = lossantos_rsc_u64(r, p + (uint64_t)i * 16 + 8);
	}
	return n;
}

static int resolve(const block *b, int nb, uint64_t ptr, uint64_t *addr) {
	// meta ptr, low 12 bits block + 1, next 20 offset
	int index = (int)(ptr & 0xFFF) - 1;
	uint64_t off = (ptr >> 12) & 0xFFFFF;
	if (index < 0 || index >= nb) return 0;
	*addr = b[index].data + off;
	return 1;
}

static void vec(lossantos_rsc *r, uint64_t a, float *v, int n) {
	for (int i = 0; i < n; i++) v[i] = lossantos_rsc_f32(r, a + (uint64_t)i * 4);
}

int lossantos_ymap_entities(const uint8_t *data, size_t n, size_t system_size, lossantos_entity *out, int cap) {
	lossantos_rsc r;
	if (!lossantos_rsc_init(&r, data, n, system_size)) return -1;
	block b[256];
	int nb = blocks(&r, b, 256);
	uint64_t cmap = 0;
	// picks by struct size, name hash ignored, works so whatever
	for (int i = 0; i < nb; i++)
		if (b[i].size == CMAPDATA_SIZE) {
			cmap = b[i].data;
			break;
		}
	if (!cmap) return 0;
	uint64_t arr;
	uint32_t count = lossantos_rsc_u16(&r, cmap + 104);  // entity count
	if (!resolve(b, nb, lossantos_rsc_u64(&r, cmap + 96), &arr) || !count) return 0;  // entity ptr array
	int k = 0;
	for (uint32_t i = 0; i < count && k < cap; i++) {
		uint64_t a;
		if (!resolve(b, nb, lossantos_rsc_u64(&r, arr + (uint64_t)i * 8), &a)) continue;
		lossantos_entity *e = &out[k++];
		// CEntityDef
		// 8   archetype hash
		// 12  flags
		// 32  position
		// 48  rotation xyzw
		// 64  scale xy
		// 68  scale z
		// 84  lod level
		e->archetype = lossantos_rsc_u32(&r, a + 8);
		e->flags = lossantos_rsc_u32(&r, a + 12);
		vec(&r, a + 32, e->pos, 3);
		vec(&r, a + 48, e->rot, 4);
		e->scale_xy = lossantos_rsc_f32(&r, a + 64);
		e->scale_z = lossantos_rsc_f32(&r, a + 68);
		e->lod_level = lossantos_rsc_u32(&r, a + 84);
	}
	return r.bad ? -1 : k;
}

int lossantos_ytyp_archetypes(const uint8_t *data, size_t n, size_t system_size, lossantos_archetype *out, int cap) {
	lossantos_rsc r;
	if (!lossantos_rsc_init(&r, data, n, system_size)) return -1;
	block b[256];
	int nb = blocks(&r, b, 256);
	uint64_t types = 0;
	for (int i = 0; i < nb; i++)
		if (b[i].size == CMAPTYPES_SIZE) {
			types = b[i].data;
			break;
		}
	if (!types) return 0;
	uint64_t arr;
	uint32_t count = lossantos_rsc_u16(&r, types + 32);  // archetype count
	if (!resolve(b, nb, lossantos_rsc_u64(&r, types + 24), &arr) || !count) return 0;  // archetype ptr array
	int k = 0;
	for (uint32_t i = 0; i < count && k < cap; i++) {
		uint64_t a;
		if (!resolve(b, nb, lossantos_rsc_u64(&r, arr + (uint64_t)i * 8), &a)) continue;
		lossantos_archetype *t = &out[k++];
		// CBaseArchetypeDef
		// 8    lod dist
		// 32   bb min
		// 48   bb max
		// 88   name hash
		// 112  asset name hash
		t->name = lossantos_rsc_u32(&r, a + 88);
		t->asset_name = lossantos_rsc_u32(&r, a + 112);
		vec(&r, a + 32, t->bb_min, 3);
		vec(&r, a + 48, t->bb_max, 3);
		t->lod_dist = lossantos_rsc_f32(&r, a + 8);
	}
	return r.bad ? -1 : k;
}

uint32_t lossantos_joaat(const char *name) {
	uint32_t h = 0;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		if (*p > 0x7F) continue;            /* ymap.py: encode('ascii', 'ignore') */
		h += (uint32_t)tolower(*p);
		h += h << 10;
		h ^= h >> 6;
	}
	h += h << 3;
	h ^= h >> 11;
	h += h << 15;
	return h;
}
