#include "lossantos_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static __declspec(thread) char g_err[512] = "";

const char *lossantos_error(void) { return g_err; }

void lossantos_set_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_err, sizeof g_err, fmt, ap);
	va_end(ap);
}

int lossantos_buf_reserve(lossantos_buf *b, size_t cap) {
	if (cap <= b->cap) return 1;
	size_t nc = b->cap ? b->cap : 4096;
	while (nc < cap) nc *= 2;
	uint8_t *p = (uint8_t *)realloc(b->p, nc);
	if (!p) {
		lossantos_set_error("out of memory (%zu bytes)", nc);
		return 0;
	}
	b->p = p;
	b->cap = nc;
	return 1;
}

void lossantos_buf_free(lossantos_buf *b) {
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}
