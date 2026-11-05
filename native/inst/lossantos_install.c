#define LOSSANTOS_BUILD_DLL
#include "lossantos_install.h"
#include "lossantos_block.h"
#include "lossantos_common.h"
#include "lossantos_keys.h"
#include "lossantos_map.h"
#include "lossantos_radio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOSSANTOS_VERSION "lossantos_install 2026-11-05.4"

static HANDLE g_thread;
static volatile long g_cancel;
static volatile long g_result;
static char g_error[512];
static lossantos_progress_cb g_cb;
static void *g_user;
static lossantos_options g_opt;
static int g_work_given;      /* caller's dir, never delete it whole */
static wchar_t g_gta[1024], g_gmod[1024], g_work[1024], g_keys[1024], g_magic[1024], g_vgm[1024], g_ffm[1024];

#define LOSSANTOS_WIDEN2(s) L##s
#define LOSSANTOS_WIDEN(s) LOSSANTOS_WIDEN2(s)
#define LOSSANTOS_MAP_DIR LOSSANTOS_WIDEN(LOSSANTOS_MAP_NAME)

static void report(const char *stage, int done, int total, const char *msg) {
	if (g_cb) g_cb(g_user, stage, done, total, msg ? msg : "");
}

static void reportf(const char *stage, const char *fmt, ...) {
	char m[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m, sizeof m, fmt, ap);
	va_end(ap);
	report(stage, 0, 0, m);
}

static void map_progress(void *ctx, const char *stage, size_t done, size_t total) {
	(void)ctx;
	// throttle: every 1% and the end
	static volatile LONG last = -1;
	LONG pct = (LONG)(done * 100 / (total ? total : 1));
	if (done == total || InterlockedExchange(&last, pct) != pct) report(stage, (int)done, (int)total, NULL);
}

static void mkdirs(const wchar_t *path) {
	wchar_t p[1024];
	wcsncpy(p, path, 1023);
	p[1023] = 0;
	for (wchar_t *s = p + 3; *s; s++)
		if (*s == L'\\') {
			*s = 0;
			CreateDirectoryW(p, NULL);
			*s = L'\\';
		}
	CreateDirectoryW(p, NULL);
}

static int gmod_running(void) {
	HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (s == INVALID_HANDLE_VALUE) return 0;
	PROCESSENTRY32W e;
	e.dwSize = sizeof e;
	int found = 0;
	for (BOOL ok = Process32FirstW(s, &e); ok && !found; ok = Process32NextW(s, &e))
		if (_wcsicmp(e.szExeFile, L"gmod.exe") == 0 || _wcsicmp(e.szExeFile, L"hl2.exe") == 0) found = 1;
	CloseHandle(s);
	return found;
}

static void own_dir(wchar_t *out, size_t cap) {
	HMODULE m = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					   (LPCWSTR)(void *)own_dir, &m);
	GetModuleFileNameW(m, out, (DWORD)cap);
	wchar_t *s = wcsrchr(out, L'\\');
	if (s) *s = 0;
}

static void rmtree(const wchar_t *dir) {
	wchar_t pat[1024];
	swprintf(pat, 1024, L"%ls\\*", dir);
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW(pat, &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
			wchar_t p[1024];
			swprintf(p, 1024, L"%ls\\%ls", dir, d.cFileName);
			if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(p);
			else DeleteFileW(p);
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

static int run_shadow(const wchar_t *map_dir) {
	wchar_t exe[1024], cmd[4096];
	own_dir(exe, 1024);
	wcscat(exe, L"\\gtav_bake.exe");
	if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
		lossantos_set_error("gtav_bake.exe missing next to lossantos_install.dll");
		return 0;
	}
	// the tool takes narrow paths
	char mapA[2048];
	WideCharToMultiByte(CP_ACP, 0, map_dir, -1, mapA, sizeof mapA, NULL, NULL);
	swprintf(cmd, 4096, L"\"%ls\" \"%hs\" \"%hs\\sun_shadow.gvsd\" 16 256 225 52", exe, mapA, mapA);
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	DWORD pri = g_opt.background ? (BELOW_NORMAL_PRIORITY_CLASS | PROCESS_MODE_BACKGROUND_BEGIN) : 0;
	if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | (g_opt.background ? BELOW_NORMAL_PRIORITY_CLASS : 0),
						NULL, NULL, &si, &pi)) {
		lossantos_set_error("cannot start gtav_bake.exe (%lu)", GetLastError());
		return 0;
	}
	(void)pri;  // pri does nothing
	for (;;) {
		if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) break;
		if (g_cancel) {
			TerminateProcess(pi.hProcess, 3);
			WaitForSingleObject(pi.hProcess, INFINITE);
			break;
		}
	}
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (code != 0 && !g_cancel) {
		lossantos_set_error("gtav_bake.exe failed (exit %lu)", code);
		return 0;
	}
	return 1;
}

// what the workshop addon cant carry
static const struct {
	const wchar_t *file, *dest;
} PAYLOAD[] = {
	{L"gmcl_gtav_win64.dll", L"lua\\bin"},
	{LOSSANTOS_MAP_DIR L".bsp", L"maps"},
	{LOSSANTOS_MAP_DIR L".png", L"maps\\thumb"},
	{L"gtav_aa_ps20b.vcs", L"shaders\\fxc"},
	{L"gtav_water_ps30.vcs", L"shaders\\fxc"},
	{L"gtav_water_vs30.vcs", L"shaders\\fxc"},
};

static void payload_src(wchar_t *out, const wchar_t *file) {
	wchar_t dir[1024];
	own_dir(dir, 1024);
	swprintf(out, 1024, L"%ls\\payload\\%ls", dir, file);
}

static int payload_ready(void) {
	for (size_t i = 0; i < sizeof PAYLOAD / sizeof PAYLOAD[0]; i++) {
		wchar_t src[1024];
		payload_src(src, PAYLOAD[i].file);
		if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
			lossantos_set_error("%ls is missing: keep the installer's files together", src);
			return 0;
		}
	}
	return 1;
}

static int place_payload(const wchar_t *data) {
	for (size_t i = 0; i < sizeof PAYLOAD / sizeof PAYLOAD[0]; i++) {
		wchar_t src[1024], dir[1024], dst[1024];
		payload_src(src, PAYLOAD[i].file);
		swprintf(dir, 1024, L"%ls\\%ls", g_gmod, PAYLOAD[i].dest);
		mkdirs(dir);
		swprintf(dst, 1024, L"%ls\\%ls", dir, PAYLOAD[i].file);
		if (!CopyFileW(src, dst, FALSE)) {
			lossantos_set_error("cannot write %ls (%lu) - is GMod running?", dst, GetLastError());
			return 0;
		}
	}
	// marker goes last, the base addon checks it
	wchar_t mark[1024];
	swprintf(mark, 1024, L"%ls\\installed.txt", data);
	FILE *f = _wfopen(mark, L"wb");
	if (!f) {
		lossantos_set_error("cannot write %ls", mark);
		return 0;
	}
	fprintf(f, "%s\n", LOSSANTOS_VERSION);
	return fclose(f) == 0;
}

// old chunks go to lossantos_old, one backup
static int swap_chunks(const wchar_t *cdir, const wchar_t *new_dir, const wchar_t *prefix) {
	wchar_t pat[1024], bak[1024];
	WIN32_FIND_DATAW d;
	swprintf(bak, 1024, L"%ls\\lossantos_old", cdir);
	CreateDirectoryW(bak, NULL);
	swprintf(pat, 1024, L"%ls\\%ls*.gma", bak, prefix);
	HANDLE h = FindFirstFileW(pat, &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			wchar_t p[1024];
			swprintf(p, 1024, L"%ls\\%ls", bak, d.cFileName);
			DeleteFileW(p);
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
	swprintf(pat, 1024, L"%ls\\%ls*.gma", cdir, prefix);
	h = FindFirstFileW(pat, &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			wchar_t p[1024], q[1024];
			swprintf(p, 1024, L"%ls\\%ls", cdir, d.cFileName);
			swprintf(q, 1024, L"%ls\\%ls", bak, d.cFileName);
			if (!MoveFileExW(p, q, MOVEFILE_REPLACE_EXISTING)) {
				FindClose(h);
				lossantos_set_error("cannot replace %ls (%lu) - is GMod running?", d.cFileName, GetLastError());
				return 0;
			}
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
	swprintf(pat, 1024, L"%ls\\%ls*.gma", new_dir, prefix);
	h = FindFirstFileW(pat, &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			wchar_t a[1024], b[1024];
			swprintf(a, 1024, L"%ls\\%ls", new_dir, d.cFileName);
			swprintf(b, 1024, L"%ls\\%ls", cdir, d.cFileName);
			if (!MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
				FindClose(h);
				lossantos_set_error("cannot move %ls into place (%lu)", d.cFileName, GetLastError());
				return 0;
			}
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
	return 1;
}

// loose, the base addon is a workshop gma
static int copy_manifests(const wchar_t *new_radio) {
	wchar_t pat[1024], dst_root[1024];
	swprintf(pat, 1024, L"%ls\\manifests\\*", new_radio);
	swprintf(dst_root, 1024, L"%ls\\sound\\lossantos_radio", g_gmod);
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW(pat, &d);
	if (h == INVALID_HANDLE_VALUE) return 1;
	int ok = 1;
	do {
		if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == L'.') continue;
		wchar_t src[1024], dd[1024], dst[1024];
		swprintf(src, 1024, L"%ls\\manifests\\%ls\\manifest.json", new_radio, d.cFileName);
		swprintf(dd, 1024, L"%ls\\%ls", dst_root, d.cFileName);
		mkdirs(dd);
		swprintf(dst, 1024, L"%ls\\manifest.json", dd);
		if (!CopyFileW(src, dst, FALSE)) {
			lossantos_set_error("cannot write %ls (%lu)", dst, GetLastError());
			ok = 0;
		}
	} while (ok && FindNextFileW(h, &d));
	FindClose(h);
	return ok;
}

static int swap_in(const wchar_t *data, const wchar_t *new_map, const wchar_t *new_content, const wchar_t *new_radio,
				   int do_map, int do_content, int do_radio) {
	wchar_t live[1024], old[1024];
	if (do_map) {
		swprintf(live, 1024, L"%ls\\" LOSSANTOS_MAP_DIR, data);
		swprintf(old, 1024, L"%ls\\" LOSSANTOS_MAP_DIR L".lossantos_old", data);
		if (GetFileAttributesW(live) != INVALID_FILE_ATTRIBUTES) {
			if (GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES) rmtree(old);   /* keep one backup */
			if (!MoveFileExW(live, old, 0)) {
				lossantos_set_error("cannot move the old map aside (%lu) - is GMod running?", GetLastError());
				return 0;
			}
		}
		if (!MoveFileExW(new_map, live, 0)) {
			MoveFileExW(old, live, 0);
			lossantos_set_error("cannot move the new map into place (%lu)", GetLastError());
			return 0;
		}
	}
	if (do_content || do_radio) {
		wchar_t cdir[1024], pat[1024];
		swprintf(cdir, 1024, L"%ls\\content", data);
		CreateDirectoryW(cdir, NULL);
		if (do_content && !swap_chunks(cdir, new_content, L"lossantos_content_mat_")) return 0;
		if (do_radio && (!swap_chunks(cdir, new_radio, L"lossantos_content_radio_") || !copy_manifests(new_radio))) return 0;
		char list[8192] = "";
		WIN32_FIND_DATAW d;
		HANDLE h;
		swprintf(pat, 1024, L"%ls\\lossantos_content_*.gma", cdir);
		char names[64][96];
		int nn = 0;
		h = FindFirstFileW(pat, &d);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if (nn < 64) WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, names[nn++], 96, NULL, NULL);
			} while (FindNextFileW(h, &d));
			FindClose(h);
		}
		// sort by hand lol, theres like 5 of these
		for (int i = 1; i < nn; i++)
			for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
				char t[96];
				memcpy(t, names[j], 96);
				memcpy(names[j], names[j - 1], 96);
				memcpy(names[j - 1], t, 96);
			}
		size_t at = 0;
		for (int i = 0; i < nn; i++) at += (size_t)snprintf(list + at, sizeof list - at, "%s\n", names[i]);
		wchar_t lp[1024];
		swprintf(lp, 1024, L"%ls\\chunks.txt", cdir);
		FILE *f = _wfopen(lp, L"wb");
		if (!f) {
			lossantos_set_error("cannot write chunks.txt");
			return 0;
		}
		fwrite(list, 1, at, f);
		fclose(f);
		if (do_content) rmtree(new_content);
		if (do_radio) rmtree(new_radio);
	}
	return 1;
}

static DWORD WINAPI install_thread(LPVOID arg) {
	(void)arg;
	if (g_opt.background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
	int flags = g_opt.flags ? g_opt.flags : LOSSANTOS_DO_ALL;
	int ok = 0;
	double t0 = lossantos_now();
	static lossantos_ng ng;
	static lossantos_world w;
	lossantos_blockref_vec refs = {0};
	wchar_t data[1024], new_map[1024], new_content[1024], new_radio[1024], spill[1024] = L"", radio_work[1024];
	int swap_only = flags == LOSSANTOS_DO_SWAP;
	uint8_t awc[16];
	int have_awc = 0;
	swprintf(data, 1024, L"%ls\\data\\lossantos", g_gmod);
	mkdirs(data);
	swprintf(new_map, 1024, L"%ls\\" LOSSANTOS_MAP_DIR L".lossantos_new", data);
	swprintf(new_content, 1024, L"%ls\\content.lossantos_new", data);
	swprintf(new_radio, 1024, L"%ls\\content_radio.lossantos_new", data);
	swprintf(radio_work, 1024, L"%ls\\radio", g_work);
	wchar_t vgm[1024], ffm[1024];
	if ((flags & LOSSANTOS_DO_SWAP) && !payload_ready()) goto done;
	if (flags & LOSSANTOS_DO_RADIO) {
		if (!lossantos_find_tool(g_vgm, L"vgmstream", L"vgmstream-cli.exe", vgm, 1024)) {
			lossantos_set_error("vgmstream-cli.exe not found (next to lossantos_install.dll, in its vgmstream folder, or on the PATH)");
			goto done;
		}
		if (!lossantos_find_tool(g_ffm, L"ffmpeg", L"ffmpeg.exe", ffm, 1024)) {
			lossantos_set_error("ffmpeg.exe not found (next to lossantos_install.dll, in its ffmpeg folder, or on the PATH)");
			goto done;
		}
	}
	swprintf(spill, 1024, L"%ls\\spill", g_work);
	mkdirs(g_work);

	if (swap_only) goto keys_done;
	report("keys", 0, 1, "loading key material");
	if (g_keys[0]) {
		if (!lossantos_ng_load(&ng, g_keys)) goto done;
		wchar_t kp[1024];
		swprintf(kp, 1024, L"%ls\\gtav_awc_key.dat", g_keys);
		FILE *kf = _wfopen(kp, L"rb");
		if (kf) {
			have_awc = fread(awc, 1, 16, kf) == 16;
			fclose(kf);
		}
	} else {
		wchar_t exe[1024];
		swprintf(exe, 1024, L"%ls\\GTA5.exe", g_gta);
		uint8_t aes[32];
		if (!lossantos_keys_find_aes(exe, aes)) goto done;
		uint8_t *magic = NULL;
		size_t mn = 0;
		if (g_magic[0]) {
			FILE *f = _wfopen(g_magic, L"rb");
			if (f) {
				fseek(f, 0, SEEK_END);
				mn = (size_t)ftell(f);
				fseek(f, 0, SEEK_SET);
				magic = (uint8_t *)malloc(mn);
				if (!magic || fread(magic, 1, mn, f) != mn) mn = 0;
				fclose(f);
			}
			if (!mn) lossantos_set_error("cannot read %ls", g_magic);
		} else {
			reportf("keys", "downloading CodeWalker magic.dat");
			mn = lossantos_keys_fetch_magic(&magic);
		}
		if (!mn) goto done;
		int kok = lossantos_keys_unlock(magic, mn, aes, &ng, awc);
		free(magic);
		if (!kok) goto done;
		have_awc = 1;  // memory only, any server lua can read data/
	}
	report("keys", 1, 1, "keys ready");
keys_done:
	if (g_cancel) goto done;

	if (flags & (LOSSANTOS_DO_MAP | LOSSANTOS_DO_CONTENT)) {
		report("discover", 0, 1, "finding the world blocks");
		if (!lossantos_world_init(&w, g_gta, &ng) || !lossantos_world_load_atlases(&w)) goto done;
		w.compat.dedupe_x_only = 0;
		w.compat.median_mixed_units = 0;
		lossantos_discover(g_gta, &ng, &refs);
		lossantos_set_error("%s", "");   /* unreadable archives are fine */
		if (!refs.n) {
			lossantos_set_error("no world blocks found under %ls - is this a GTA V install?", g_gta);
			goto done;
		}
		reportf("discover", "%zu blocks", refs.n);
		if (GetFileAttributesW(new_map) != INVALID_FILE_ATTRIBUTES) rmtree(new_map);
		if (GetFileAttributesW(new_content) != INVALID_FILE_ATTRIBUTES) rmtree(new_content);
		lossantos_map_opts o;
		memset(&o, 0, sizeof o);
		o.out_dir = new_map;
		o.work_dir = spill;
		o.jobs = g_opt.threads;
		o.background = g_opt.background;
		o.progress = map_progress;
		o.cancel = &g_cancel;
		o.content_dir = (flags & LOSSANTOS_DO_CONTENT) ? new_content : NULL;
		wchar_t lp[1024];
		CreateDirectoryW(new_map, NULL);
		swprintf(lp, 1024, L"%ls\\lossantos_build.log", new_map);
		o.log = _wfopen(lp, L"w");
		lossantos_map_stats st;
		int mok = lossantos_build_map(&w, &refs, &o, &st);
		if (o.log) fclose(o.log);
		if (g_cancel) goto done;
		if (!mok) {
			if (!lossantos_error()[0]) lossantos_set_error("map build failed (see lossantos_build.log)");
			goto done;
		}
		reportf("map", "%zu blocks, %zu cells, %zu textures in %.0f s", st.blocks, st.tiles, st.textures, st.t_total);
	}

	if ((flags & LOSSANTOS_DO_SHADOW) && !g_cancel) {
		report("shadow", 0, 1, "baking the sun shadow map");
		if (!run_shadow(new_map)) goto done;
		report("shadow", 1, 1, NULL);
	}
	if (g_cancel) goto done;

	if (flags & LOSSANTOS_DO_RADIO) {
		if (!have_awc) {
			lossantos_set_error("no AWC key (gtav_awc_key.dat) - derive the keys from GTA5.exe or add it to the key folder");
			goto done;
		}
		if (GetFileAttributesW(new_radio) != INVALID_FILE_ATTRIBUTES) rmtree(new_radio);
		lossantos_radio_opts ro;
		memset(&ro, 0, sizeof ro);
		ro.gta_dir = g_gta;
		ro.ng = &ng;
		memcpy(ro.awc_key, awc, 16);
		ro.vgmstream = vgm;
		ro.ffmpeg = ffm;
		ro.work_dir = radio_work;
		ro.out_dir = new_radio;
		ro.jobs = g_opt.threads;
		ro.background = g_opt.background;
		ro.progress = map_progress;
		ro.cancel = &g_cancel;
		mkdirs(new_radio);
		wchar_t lp[1024];
		swprintf(lp, 1024, L"%ls\\lossantos_radio.log", new_radio);
		ro.log = _wfopen(lp, L"w");
		lossantos_radio_stats rs;
		int rok = lossantos_build_radio(&ro, &rs);
		if (ro.log) fclose(ro.log);
		if (g_cancel) goto done;
		if (!rok) goto done;
		reportf("radio", "%zu stations, %zu clips (%zu failed), %.1f h of audio in %.0f s", rs.stations, rs.clips,
				rs.failed, rs.audio_seconds / 3600.0, rs.t_total);
	}
	if (g_cancel) goto done;

	if (flags & LOSSANTOS_DO_SWAP) {
		report("swap", 0, 1, "installing into Garry's Mod");
		if (gmod_running()) {
			lossantos_set_error("Garry's Mod is running - close it and run the install step again "
						  "(the converted data is kept in %ls)", data);
			goto done;
		}
// swap alone moves what an earlier run built
#define LOSSANTOS_BUILT(p) (swap_only && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
		if (!swap_in(data, new_map, new_content, new_radio, (flags & LOSSANTOS_DO_MAP) != 0 || LOSSANTOS_BUILT(new_map),
					 (flags & LOSSANTOS_DO_CONTENT) != 0 || LOSSANTOS_BUILT(new_content),
					 (flags & LOSSANTOS_DO_RADIO) != 0 || LOSSANTOS_BUILT(new_radio)))
			goto done;
#undef LOSSANTOS_BUILT
		if (!place_payload(data)) goto done;
		rmtree(g_work_given ? spill : g_work);
		report("swap", 1, 1, NULL);
	}
	ok = 1;
	reportf("done", "finished in %.1f min", (lossantos_now() - t0) / 60.0);
done:
	if (!ok) snprintf(g_error, sizeof g_error, "%s", g_cancel ? "cancelled" : lossantos_error());
	// cancel leaves nothing, a refused swap keeps its build
	if (g_cancel && spill[0]) {
		report("cancel", 0, 0, "removing the partial build");
		rmtree(new_map);
		rmtree(new_content);
		rmtree(new_radio);
		rmtree(g_work_given ? spill : g_work);
	}
	lossantos_vec_free(refs);
	lossantos_world_free(&w);
	memset(&w, 0, sizeof w);
	InterlockedExchange(&g_result, ok ? 1 : (g_cancel ? -2 : -1));
	return 0;
}

static void copyw(wchar_t *dst, const wchar_t *src) {
	dst[0] = 0;
	if (src) wcsncpy(dst, src, 1023), dst[1023] = 0;
}

LOSSANTOS_API int lossantos_install_start(const lossantos_options *o, lossantos_progress_cb cb, void *user) {
	if (g_thread) {
		if (WaitForSingleObject(g_thread, 0) != WAIT_OBJECT_0) {
			snprintf(g_error, sizeof g_error, "an install is already running");
			return 0;
		}
		CloseHandle(g_thread);
		g_thread = NULL;
	}
	if (!o || !o->gta_dir || !o->gmod_dir) {
		snprintf(g_error, sizeof g_error, "gta_dir and gmod_dir are required");
		return 0;
	}
	g_opt = *o;
	copyw(g_gta, o->gta_dir);
	copyw(g_gmod, o->gmod_dir);
	copyw(g_keys, o->keys_dir);
	copyw(g_magic, o->magic_path);
	copyw(g_vgm, o->vgmstream_path);
	copyw(g_ffm, o->ffmpeg_path);
	g_work_given = o->work_dir && o->work_dir[0];
	if (g_work_given) copyw(g_work, o->work_dir);
	else swprintf(g_work, 1024, L"%ls\\data\\lossantos\\.lossantos_work", g_gmod);
	g_cb = cb;
	g_user = user;
	g_cancel = 0;
	g_result = 0;
	g_error[0] = 0;
	g_thread = CreateThread(NULL, 16u << 20, install_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
	if (!g_thread) {
		snprintf(g_error, sizeof g_error, "cannot start the install thread");
		return 0;
	}
	return 1;
}

LOSSANTOS_API void lossantos_install_cancel(void) { InterlockedExchange(&g_cancel, 1); }

LOSSANTOS_API int lossantos_install_wait(uint32_t ms) {
	if (!g_thread) return 1;
	return WaitForSingleObject(g_thread, ms) == WAIT_OBJECT_0;
}

LOSSANTOS_API int lossantos_install_result(void) { return (int)g_result; }
LOSSANTOS_API const char *lossantos_install_error(void) { return g_error; }
LOSSANTOS_API const char *lossantos_version(void) { return LOSSANTOS_VERSION; }
