#ifndef LOSSANTOS_VTF_H
#define LOSSANTOS_VTF_H

#include "lossantos_common.h"
#include "lossantos_ytd.h"

typedef struct {
	lossantos_buf vtf;
	char vmt[1024];
	uint32_t flags;          /* bit0 glass, bit1 nocull, bit2 additive */
	uint8_t sha1[20];        /* of the VTF bytes (mat_canon) */
	char canon_key[256];     /* VMT parameters that must match for mat_canon */
	int rebuilt;             /* pixels were re-encoded */
} lossantos_texout;

// path = models/gtav/<region>/<stem>
int lossantos_texture_install(const lossantos_tex *tex, const char *path, lossantos_texout *out);
void lossantos_texout_free(lossantos_texout *o);
// pixels untouched, no cap
int lossantos_texture_raw(const lossantos_tex *tex, lossantos_buf *vtf);

int lossantos_water_material(lossantos_texout *out);

uint32_t lossantos_name_flags(const char *name);

#endif
