#ifndef LOSSANTOS_COMMON_H
#define LOSSANTOS_COMMON_H

#include <stddef.h>
#include <stdint.h>

// growable byte buffer
typedef struct {
	uint8_t *p;
	size_t n;
	size_t cap;
} lossantos_buf;

int lossantos_buf_reserve(lossantos_buf *b, size_t cap);   /* 0 on failure */
void lossantos_buf_free(lossantos_buf *b);

const char *lossantos_error(void);
void lossantos_set_error(const char *fmt, ...);

#endif
