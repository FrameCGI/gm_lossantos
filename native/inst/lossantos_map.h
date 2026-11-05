#ifndef LOSSANTOS_MAP_H
#define LOSSANTOS_MAP_H

#include <stdio.h>

#include "lossantos_block.h"

// bsp name, also the data folder
#define LOSSANTOS_MAP_NAME "gm_lossantos"

typedef void (*lossantos_progress_fn)(void *ctx, const char *stage, size_t done, size_t total);

typedef struct {
	const wchar_t *out_dir;
	const wchar_t *work_dir;  /* scratch (spill files), deleted afterwards */
	int jobs;                 /* worker threads (0 = logical CPUs) */
	int background;           /* THREAD_MODE_BACKGROUND_BEGIN on every worker */
	int loose;                /* also loose tiles/ + phys/, debug */
	const wchar_t *content_dir; /* NULL = no textures */
	uint64_t content_limit;   /* bytes per chunk (0 = 1500 MB) */
	const char *only;         /* inner path substring, testing */
	lossantos_progress_fn progress;
	void *progress_ctx;
	volatile long *cancel;
	FILE *log;                /* per-block lines (may be NULL) */
} lossantos_map_opts;

typedef struct {
	size_t blocks, detail, fallback, empty, failed, borrowed;
	size_t tiles, files, sanded, chunks, tile_tris, tile_verts, chunk_tris, chunk_dropped, materials;
	uint64_t spill_bytes;
	size_t textures, tex_skipped, tex_rebuilt, content_chunks, canon_aliases;
	uint64_t content_bytes;
	double t_content;
	double t_phase0, t_phase1, t_tiles, t_phys, t_index, t_total;
} lossantos_map_stats;

int lossantos_build_map(lossantos_world *w, const lossantos_blockref_vec *refs, const lossantos_map_opts *o, lossantos_map_stats *st);

#endif
