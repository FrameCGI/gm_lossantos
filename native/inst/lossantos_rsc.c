#include "lossantos_rsc.h"
#include "lossantos_common.h"

int lossantos_rsc_init(lossantos_rsc *r, const uint8_t *data, size_t n, size_t system_size) {
	memset(r, 0, sizeof *r);
	if (system_size > n) {
		lossantos_set_error("system size %zu outside a %zu-byte payload", system_size, n);
		return 0;
	}
	r->sys = data;
	r->sys_n = system_size;
	r->gfx = data + system_size;
	r->gfx_n = n - system_size;
	return 1;
}

const char *lossantos_rsc_str(lossantos_rsc *r, uint64_t addr) {
	if (!addr) return "";
	const uint8_t *p = lossantos_rsc_ptr(r, addr, 1);
	if (!p) return "";
	const uint8_t *base = ((uint32_t)addr & LOSSANTOS_SYS_BASE) == LOSSANTOS_SYS_BASE ? r->sys : r->gfx;
	size_t n = base == r->sys ? r->sys_n : r->gfx_n;
	size_t off = (size_t)(p - base);
	// must end inside its window
	if (!memchr(p, 0, n - off)) return "";
	return (const char *)p;
}
