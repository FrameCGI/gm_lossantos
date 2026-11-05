#ifndef LOSSANTOS_STAGE_H
#define LOSSANTOS_STAGE_H

#include "lossantos_block.h"

// python detail stage layout, parity checks only
int lossantos_stage_write(const wchar_t *dir, const char *map_name, const lossantos_block_out *o);

#endif
