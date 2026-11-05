#ifndef LOSSANTOS_RADIO_H
#define LOSSANTOS_RADIO_H

#include "lossantos_map.h"

// x64/audio/sfx/RADIO_*.rpf                 awc -> wav -> ogg q5
// update.rpf/scaleform_generic.rpf/hud.ytd  logos
// x64b.rpf/.../american_rel.rpf             names + titles
typedef struct {
	const wchar_t *gta_dir;
	const lossantos_ng *ng;
	uint8_t awc_key[16];       /* gtav_awc_key.dat (CodeWalker format) */
	const wchar_t *vgmstream;  /* full path of vgmstream-cli.exe */
	const wchar_t *ffmpeg;     /* full path of ffmpeg.exe */
	const wchar_t *work_dir;   /* scratch (awc, wav, ogg), removed afterwards */
	const wchar_t *out_dir;    /* chunks + manifests */
	uint64_t limit;            /* bytes per chunk (0 = 1500 MB) */
	int jobs, background;
	const char *only;          /* station substring filter (testing) */
	lossantos_progress_fn progress;
	void *progress_ctx;
	volatile long *cancel;
	FILE *log;
} lossantos_radio_opts;

typedef struct {
	size_t stations, awc, clips, failed, logos, chunks, titled;
	uint64_t bytes;
	double audio_seconds;
	double t_extract, t_scan, t_encode, t_ui, t_pack, t_total;
} lossantos_radio_stats;

int lossantos_build_radio(const lossantos_radio_opts *o, lossantos_radio_stats *st);

// given, dll dir/sub/exe, dll dir/exe, PATH
int lossantos_find_tool(const wchar_t *given, const wchar_t *sub, const wchar_t *exe, wchar_t *out, size_t cap);

#endif
