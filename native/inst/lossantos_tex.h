#ifndef LOSSANTOS_TEX_H
#define LOSSANTOS_TEX_H

#include "lossantos_util.h"
#include "lossantos_ytd.h"

typedef struct {
	uint16_t archive;       /* index into the build's archive list */
	uint16_t tex_index;     /* index inside its dictionary */
	uint32_t entry;         /* entry index in that (possibly nested) archive */
	uint32_t nested;        /* nested .rpf entry, or UINT32_MAX */
	uint32_t nested2;       /* second nesting level, or UINT32_MAX */
	uint8_t embedded;       /* dictionary is inside a .ydr */
} lossantos_texloc;

typedef struct {
	char name[128];         /* original spelling */
	char key[128];          /* lower case */
	uint32_t w, h, fmt, levels;
	lossantos_texloc loc;
	int32_t owner;          /* block that holds it, -1 = n/a */
} lossantos_texinfo;
typedef LOSSANTOS_VEC(lossantos_texinfo) lossantos_texinfo_vec;
typedef LOSSANTOS_VEC(char *) lossantos_strvec;

typedef struct {
	LOSSANTOS_VEC(lossantos_texinfo) v; /* insertion order */
	lossantos_hmap idx;           /* hash(key) -> index */
} lossantos_texset;

int lossantos_texset_init(lossantos_texset *s);
void lossantos_texset_free(lossantos_texset *s);
int64_t lossantos_texset_find(const lossantos_texset *s, const char *key);
int lossantos_texset_offer(lossantos_texset *s, const lossantos_tex *t, const lossantos_texloc *loc);
int lossantos_texset_offer_info(lossantos_texset *s, const lossantos_texinfo *t);

void lossantos_clean_name(const char *name, int max_len, char *out, size_t cap);
int lossantos_tex_supported(const lossantos_texinfo *t);

int lossantos_is_marking_name(const char *k);
int lossantos_is_overlay_name(const char *k);
int lossantos_is_stipple_name(const char *k);
int lossantos_is_aux_map(const char *k);
int lossantos_is_seethrough_name(const char *k);

typedef struct {
	char key[128];
	char path[96];          /* prefix/stem */
	int texture;            /* index into the texset it came from */
	lossantos_texinfo info;       /* the source texture (where its pixels live) */
} lossantos_install;

typedef LOSSANTOS_VEC(lossantos_install) lossantos_install_vec;

int lossantos_install_names(const lossantos_texset *set, const int *items, int nitems, const char *prefix,
					  lossantos_install_vec *out);

typedef struct {
	lossantos_strtab regions;     /* region names */
	lossantos_hset have;          /* hash(region/stem) */
	int sorted_valid;
	uint32_t *order;        /* region ids sorted by name */
} lossantos_texreg;

int lossantos_texreg_init(lossantos_texreg *r);
void lossantos_texreg_free(lossantos_texreg *r);
int lossantos_texreg_add(lossantos_texreg *r, const char *path);      /* models/gtav/<region>/<stem> */
int lossantos_texreg_scan(lossantos_texreg *r, const wchar_t *materials_root);
void lossantos_texreg_sort(lossantos_texreg *r);
// out = models/gtav/<region>/<stem>, 0 if none
int lossantos_find_installed(lossantos_texreg *r, const char *name, char *out, size_t cap);

int64_t lossantos_near_texture(const lossantos_texset *s, const char *key);

// NULL = drop the geometry
const char *lossantos_word_fallback(const char *key);

#endif
