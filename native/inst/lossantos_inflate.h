#ifndef LOSSANTOS_INFLATE_H
#define LOSSANTOS_INFLATE_H

#include "lossantos_common.h"

// raw deflate, hint = expected size or 0
int lossantos_inflate(const uint8_t *src, size_t srclen, lossantos_buf *out, size_t hint);

#endif
