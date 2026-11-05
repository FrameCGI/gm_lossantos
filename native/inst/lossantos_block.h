#ifndef LOSSANTOS_BLOCK_H
#define LOSSANTOS_BLOCK_H

#include "lossantos_crypto.h"
#include "lossantos_geo.h"
#include "lossantos_rpf.h"
#include "lossantos_tex.h"
#include "lossantos_ymap.h"

typedef LOSSANTOS_VEC(uint32_t) lossantos_u32vec;
void lossantos_rpf_walk(const lossantos_rpf *a, lossantos_u32vec *out);
int lossantos_match_archetype(const char *source, const double *lo, const double *hi, const lossantos_archetype *arch,
						int narch, float tolerance);
typedef struct {
	int drawable;            /* caller's drawable index */
	double rot[3][3];
	double scale[3];
	double off[3];
	double bmin[3], bmax[3]; /* placed bounds, metres */
} lossantos_placement;
void lossantos_quat_matrix(const float q[4], double m[3][3]);
void lossantos_place_point(const lossantos_placement *p, const float *v, double o[3]);

typedef struct {
	char outer[32];         /* x64i.rpf */
	char inner[256];        /* levels/gta5/_citye/downtown_01/dt1_05.rpf */
	char name[96];          /* dt1_05.rpf */
	char stem[96];          /* dt1_05 */
	int road;
} lossantos_blockref;
typedef LOSSANTOS_VEC(lossantos_blockref) lossantos_blockref_vec;

int lossantos_discover(const wchar_t *gta_dir, const lossantos_ng *ng, lossantos_blockref_vec *out);

typedef struct {
	size_t n, cap;
	double *pos;            /* 9 per triangle */
	double *nrm;            /* 9 per triangle (per corner) */
	double *uv;             /* 6 per triangle */
	uint32_t *mat;          /* index into the block's material table */
} lossantos_soup;
int lossantos_soup_reserve(lossantos_soup *s, size_t n);
void lossantos_soup_free(lossantos_soup *s);

typedef struct {
	char key[128];
	char path[96];
	int drop;
} lossantos_texmat;
typedef LOSSANTOS_VEC(lossantos_texmat) lossantos_texmat_vec;

typedef struct {
	const wchar_t *gta_dir;
	const lossantos_ng *ng;
	lossantos_compat compat;
	lossantos_texset atlas;
	lossantos_texreg reg;         /* every installed texture (find_installed) */
	lossantos_install_vec roads;     /* models/gtav/roads */
	lossantos_hmap roads_idx;     /* hash(key) -> index into roads */
} lossantos_world;

int lossantos_world_init(lossantos_world *w, const wchar_t *gta_dir, const lossantos_ng *ng);
void lossantos_world_free(lossantos_world *w);
int lossantos_world_load_atlases(lossantos_world *w);
int lossantos_world_finish_installs(lossantos_world *w, const lossantos_texinfo *atlas_items, size_t n);

typedef struct {
	// phase 0
	lossantos_install_vec installs;
	int installs_given;
	lossantos_texinfo_vec items;               /* the block's own install items (non-atlas) */
	lossantos_texinfo_vec local;               /* every texture in the block's own dictionaries */
	lossantos_texinfo_vec unplaced_tex;        /* embedded in drawables this block does not place */
	lossantos_strvec missing;                  /* used keys with no local texture */
	LOSSANTOS_VEC(lossantos_texinfo) atlas_items;    /* to install under models/gtav/roads */
	// phase 1
	lossantos_ybn coll;                        /* deduped collision, Source units (physics) */
	lossantos_soup vis;
	lossantos_strtab mats;
	lossantos_texmat_vec texmats;
	int double_sided;                    /* collision-only fallback output */
	int empty;                           /* no collision at all */
	// stats
	uint32_t ydr_files, placed, lod_skipped, fallbacks, dropped_keys;
	uint32_t shell_dropped, shell_tested, filler_dropped, filler_tested;
	size_t coll_tris, coll_kept;
	double t_read, t_cover, t_total;
	char error[256];
} lossantos_block_out;

void lossantos_block_out_free(lossantos_block_out *o);
// phase
// 0  placements + installs
// 1  full build
// 2  collision only fallback
int lossantos_block_run(lossantos_world *w, const lossantos_blockref *b, int phase, lossantos_block_out *out);

#endif
