// flags
// 1   LOSSANTOS_DO_MAP
// 2   LOSSANTOS_DO_CONTENT
// 4   LOSSANTOS_DO_SHADOW
// 8   LOSSANTOS_DO_SWAP
// 16  LOSSANTOS_DO_RADIO
// 31  LOSSANTOS_DO_ALL
#ifndef LOSSANTOS_INSTALL_H
#define LOSSANTOS_INSTALL_H

#include <stdint.h>
#include <wchar.h>

#ifdef LOSSANTOS_BUILD_DLL
#define LOSSANTOS_API __declspec(dllexport)
#else
#define LOSSANTOS_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LOSSANTOS_DO_MAP 1        /* world geometry + physics + pages + index */
#define LOSSANTOS_DO_CONTENT 2    /* textures -> GMA chunks (+ flags, canon) */
#define LOSSANTOS_DO_SHADOW 4     /* sun shadow bake (gtav_bake.exe) */
#define LOSSANTOS_DO_SWAP 8       /* into gmod, gmod has to be closed */
#define LOSSANTOS_DO_RADIO 16     /* radio stations (needs vgmstream-cli + ffmpeg) */
#define LOSSANTOS_DO_ALL 31

typedef void (*lossantos_progress_cb)(void *user, const char *stage, int done, int total, const char *message);

typedef struct {
	const wchar_t *gta_dir;    /* "D:\\Games\\Grand Theft Auto V" */
	const wchar_t *gmod_dir;   /* "...\\GarrysMod\\garrysmod" */
	const wchar_t *work_dir;   /* NULL = data/lossantos/.lossantos_work */
	const wchar_t *keys_dir;   /* NULL = derive from GTA5.exe */
	const wchar_t *magic_path; /* NULL = download magic.dat */
	int threads;               /* 0 = every logical CPU */
	int background;            /* 1 = low priority */
	int flags;                 /* LOSSANTOS_DO_* sum, 0 = LOSSANTOS_DO_ALL */
	const wchar_t *vgmstream_path;
	const wchar_t *ffmpeg_path;
} lossantos_options;

LOSSANTOS_API int lossantos_install_start(const lossantos_options *o, lossantos_progress_cb cb, void *user);
LOSSANTOS_API void lossantos_install_cancel(void);
LOSSANTOS_API int lossantos_install_wait(uint32_t ms);
// result
// 0   running or not started
// 1   ok
// -1  failed
// -2  cancelled
LOSSANTOS_API int lossantos_install_result(void);
LOSSANTOS_API const char *lossantos_install_error(void);
LOSSANTOS_API const char *lossantos_version(void);

#ifdef __cplusplus
}
#endif

#endif
