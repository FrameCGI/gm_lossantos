#ifndef LOSSANTOS_CONTENT_H
#define LOSSANTOS_CONTENT_H

#include "lossantos_map.h"
#include "lossantos_vtf.h"

typedef struct {
	char path[96];             /* models/gtav/<region>/<stem> */
	const lossantos_texinfo *info;   /* NULL = the generated water material */
} lossantos_content_job;

typedef struct {
	int ok;
	uint32_t flags;
	uint8_t sha1[20];
	char canon_key[256];
} lossantos_content_result;

typedef struct {
	size_t jobs, written, skipped, rebuilt, files, chunks;
	uint64_t bytes;
	double t_convert, t_pack;
} lossantos_content_stats;

// lossantos_content_mat_NN.gma, res[i] is job i
int lossantos_build_content(const wchar_t *gta_dir, const lossantos_ng *ng, const lossantos_map_opts *o, const wchar_t *content_dir,
					  uint64_t limit, const lossantos_content_job *jobs, size_t n, lossantos_content_result *res,
					  lossantos_content_stats *st);

// update.rpf water textures, never on the workshop
int lossantos_build_water_chunk(const wchar_t *gta_dir, const lossantos_ng *ng, const wchar_t *work_dir, const wchar_t *content_dir);

#endif
