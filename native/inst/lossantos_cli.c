#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lossantos_install.h"

typedef int (*start_fn)(const lossantos_options *, lossantos_progress_cb, void *);
typedef int (*wait_fn)(uint32_t);
typedef int (*result_fn)(void);
typedef const char *(*str_fn)(void);

static LARGE_INTEGER g_f, g_t0;

static void on_progress(void *user, const char *stage, int done, int total, const char *msg) {
	(void)user;
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	double s = (double)(t.QuadPart - g_t0.QuadPart) / (double)g_f.QuadPart;
	if (msg && msg[0]) printf("[%7.1f s] %-9s %s\n", s, stage, msg);
	else if (total) printf("[%7.1f s] %-9s %d/%d\r", s, stage, done, total);
	if (total && done == total && !(msg && msg[0])) printf("\n");
}

int wmain(int argc, wchar_t **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 3) {
		fprintf(stderr, "usage: lossantos_cli <gta dir> <gmod dir> [flags] [threads] [bg] [keys dir|-] [magic.dat|-]\n");
		return 2;
	}
	wchar_t dll[1024];
	GetModuleFileNameW(NULL, dll, 1024);
	wcscpy(wcsrchr(dll, L'\\') + 1, L"lossantos_install.dll");
	HMODULE m = LoadLibraryW(dll);
	if (!m) {
		fprintf(stderr, "cannot load %ls\n", dll);
		return 1;
	}
	start_fn start = (start_fn)(void *)GetProcAddress(m, "lossantos_install_start");
	wait_fn wait = (wait_fn)(void *)GetProcAddress(m, "lossantos_install_wait");
	result_fn result = (result_fn)(void *)GetProcAddress(m, "lossantos_install_result");
	str_fn err = (str_fn)(void *)GetProcAddress(m, "lossantos_install_error");
	str_fn ver = (str_fn)(void *)GetProcAddress(m, "lossantos_version");
	if (!start || !wait || !result || !err || !ver) {
		fprintf(stderr, "lossantos_install.dll is missing exports\n");
		return 1;
	}
	printf("%s\n", ver());
	lossantos_options o;
	memset(&o, 0, sizeof o);
	o.gta_dir = argv[1];
	o.gmod_dir = argv[2];
	o.flags = argc > 3 ? _wtoi(argv[3]) : 0;  // LOSSANTOS_DO_* sum, 0 = all
	o.threads = argc > 4 ? _wtoi(argv[4]) : 0;
	o.background = argc > 5 ? _wtoi(argv[5]) : 0;
	o.keys_dir = argc > 6 && wcscmp(argv[6], L"-") ? argv[6] : NULL;
	o.magic_path = argc > 7 && wcscmp(argv[7], L"-") ? argv[7] : NULL;
	o.vgmstream_path = argc > 8 && wcscmp(argv[8], L"-") ? argv[8] : NULL;
	o.ffmpeg_path = argc > 9 && wcscmp(argv[9], L"-") ? argv[9] : NULL;
	QueryPerformanceFrequency(&g_f);
	QueryPerformanceCounter(&g_t0);
	if (!start(&o, on_progress, NULL)) {
		fprintf(stderr, "refused: %s\n", err());
		return 1;
	}
	wait(0xFFFFFFFFu);
	int r = result();
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	printf("\nresult %d (%s) after %.1f min\n", r, r == 1 ? "ok" : err(),
		   (double)(t.QuadPart - g_t0.QuadPart) / (double)g_f.QuadPart / 60.0);
	return r == 1 ? 0 : 1;
}
