#ifndef LOSSANTOS_RSC_H
#define LOSSANTOS_RSC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LOSSANTOS_SYS_BASE 0x50000000u  // system[n]
#define LOSSANTOS_GFX_BASE 0x60000000u  // graphics[n]

typedef struct {
	const uint8_t *sys;
	size_t sys_n;
	const uint8_t *gfx;
	size_t gfx_n;
	int bad;  // set on any bad pointer
} lossantos_rsc;

int lossantos_rsc_init(lossantos_rsc *r, const uint8_t *data, size_t n, size_t system_size);

static inline const uint8_t *lossantos_rsc_ptr(lossantos_rsc *r, uint64_t addr, size_t len) {
	uint32_t a = (uint32_t)addr;
	const uint8_t *base;
	size_t n, off;
	if ((a & LOSSANTOS_SYS_BASE) == LOSSANTOS_SYS_BASE) {
		base = r->sys; n = r->sys_n; off = a - LOSSANTOS_SYS_BASE;
	} else if ((a & LOSSANTOS_GFX_BASE) == LOSSANTOS_GFX_BASE) {
		base = r->gfx; n = r->gfx_n; off = a - LOSSANTOS_GFX_BASE;
	} else {
		r->bad = 1;
		return NULL;
	}
	if (off > n || len > n - off) {
		r->bad = 1;
		return NULL;
	}
	return base + off;
}

static inline uint8_t lossantos_rsc_u8(lossantos_rsc *r, uint64_t a) { const uint8_t *p = lossantos_rsc_ptr(r, a, 1); return p ? *p : 0; }
static inline uint16_t lossantos_rsc_u16(lossantos_rsc *r, uint64_t a) { uint16_t v = 0; const uint8_t *p = lossantos_rsc_ptr(r, a, 2); if (p) memcpy(&v, p, 2); return v; }
static inline uint32_t lossantos_rsc_u32(lossantos_rsc *r, uint64_t a) { uint32_t v = 0; const uint8_t *p = lossantos_rsc_ptr(r, a, 4); if (p) memcpy(&v, p, 4); return v; }
static inline uint64_t lossantos_rsc_u64(lossantos_rsc *r, uint64_t a) { uint64_t v = 0; const uint8_t *p = lossantos_rsc_ptr(r, a, 8); if (p) memcpy(&v, p, 8); return v; }
static inline float lossantos_rsc_f32(lossantos_rsc *r, uint64_t a) { float v = 0; const uint8_t *p = lossantos_rsc_ptr(r, a, 4); if (p) memcpy(&v, p, 4); return v; }

const char *lossantos_rsc_str(lossantos_rsc *r, uint64_t addr);

#endif
