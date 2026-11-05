// keys <gta> <out> [magic.dat]
// <keys> walk <rpf> [dump.txt]
// <keys> ytdall <rpf> [filter] [dump.txt] [nodecode]
// <keys> geom <rpf> <filter> <dump.txt>
// <keys> ybn <rpf> <filter> <out dir>
// <keys> radio <gta> <out> <work> [jobs] [only|-] [vgmstream|-] [ffmpeg|-] [bg]
// <keys> map <gta> <out> <work> [jobs] [only|-] [compat] [bg,loose] [content dir]
// <keys> discover <gta>
// <keys> block <gta> <stem> <out> [materials root|-] [compat] [coll]
// <keys> cat <rpf> <inner/path> <out file>
#include "lossantos_rpf.h"
#include "lossantos_ytd.h"
#include "lossantos_ydr.h"
#include "lossantos_ymap.h"
#include "lossantos_ybn.h"
#include "lossantos_stage.h"
#include "lossantos_map.h"
#include "lossantos_keys.h"
#include "lossantos_radio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// position weighted sum, easy in numpy
static uint64_t wsum64(const uint8_t *p, size_t n) {
	uint64_t h = 0;
	for (size_t i = 0; i < n; i++) h += (uint64_t)p[i] * ((uint64_t)i * 2654435761ull + 1ull);
	return h;
}

static uint64_t fnv1a64(const uint8_t *p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

typedef struct {
	FILE *dump;
	uint64_t files, bytes_out, bytes_in, archives, errors;
	lossantos_buf buf;
} walk_ctx;

static int ends_with_rpf(const char *n) {
	size_t l = strlen(n);
	return l >= 4 && _stricmp(n + l - 4, ".rpf") == 0;
}

static void walk(lossantos_rpf *a, walk_ctx *w) {
	w->archives++;
	char path[2048];
	for (uint32_t i = 1; i < a->count; i++) {
		const lossantos_entry *e = &a->e[i];
		if (e->kind == LOSSANTOS_DIR) continue;
		if (ends_with_rpf(e->name)) {
			lossantos_rpf child;
			if (lossantos_rpf_open_child(&child, a, i)) {
				walk(&child, w);
				lossantos_rpf_close(&child);
			} else {
				w->errors++;
				fprintf(stderr, "child %s: %s\n", e->name, lossantos_error());
			}
			continue;
		}
		w->bytes_in += lossantos_rpf_file_size(a, i);
		if (!lossantos_rpf_read(a, i, &w->buf)) {
			w->errors++;
			lossantos_rpf_path(a, i, path, sizeof path);
			fprintf(stderr, "read %s: %s\n", path, lossantos_error());
			continue;
		}
		w->files++;
		w->bytes_out += w->buf.n;
		if (w->dump) {
			lossantos_rpf_path(a, i, path, sizeof path);
			char magic[9] = "";
			for (size_t m = 0; m < 4 && m < w->buf.n; m++)
				snprintf(magic + m * 2, 3, "%02x", w->buf.p[m]);
			fprintf(w->dump, "%s\t%zu\t%016llx\t%d\t%s\n", path, w->buf.n,
					(unsigned long long)fnv1a64(w->buf.p, w->buf.n),
					e->kind == LOSSANTOS_BINARY && e->encrypted, magic);
		}
	}
}

typedef struct {
	FILE *dump;
	const char *filter;
	uint64_t ytds, textures, decoded, bytes;
	lossantos_buf buf;
	uint8_t *rgba;
	size_t rgba_cap;
	int nodecode;
} ytd_ctx;

static void ytd_walk(lossantos_rpf *a, ytd_ctx *y, int inside) {
	int mine = inside || !y->filter[0] || strstr(a->name, y->filter) != NULL;
	char path[2048];
	static lossantos_tex tex[1024];
	for (uint32_t i = 1; i < a->count; i++) {
		const lossantos_entry *e = &a->e[i];
		if (e->kind == LOSSANTOS_DIR) continue;
		size_t nl = strlen(e->name);
		if (ends_with_rpf(e->name)) {
			lossantos_rpf child;
			if (lossantos_rpf_open_child(&child, a, i)) {
				ytd_walk(&child, y, mine);
				lossantos_rpf_close(&child);
			}
			continue;
		}
		if (!mine || e->kind != LOSSANTOS_RESOURCE || nl < 4 || _stricmp(e->name + nl - 4, ".ytd") != 0) continue;
		if (!lossantos_rpf_read(a, i, &y->buf)) continue;
		lossantos_rsc r;
		if (!lossantos_rsc_init(&r, y->buf.p, y->buf.n, lossantos_rpf_size_from_flags(e->sys_flags))) continue;
		int n = lossantos_ytd_parse(&r, LOSSANTOS_SYS_BASE, tex, 1024);
		y->ytds++;
		lossantos_rpf_path(a, i, path, sizeof path);
		for (int t = 0; t < n; t++) {
			y->textures++;
			size_t need = (size_t)tex[t].width * tex[t].height * 4;
			if (need > y->rgba_cap) {
				free(y->rgba);
				y->rgba = (uint8_t *)malloc(need);
				y->rgba_cap = need;
			}
			uint32_t w = 0, h = 0;
			int ok = y->nodecode ? 0 : lossantos_tex_decode(&tex[t], 0, y->rgba, y->rgba_cap, &w, &h);
			if (ok) {
				y->decoded++;
				y->bytes += (size_t)w * h * 4;
			}
			if (y->dump) {
				char hash[24] = "-";
				if (ok) snprintf(hash, sizeof hash, "%016llx",
								 (unsigned long long)wsum64(y->rgba, (size_t)w * h * 4));
				fprintf(y->dump, "%s:%s\t%u\t%u\t%u\t%u\t%s\n", path, tex[t].name, tex[t].width,
						tex[t].height, tex[t].format, tex[t].levels, hash);
			}
		}
	}
}

static void fhex(FILE *f, const float *v, int n) {
	for (int i = 0; i < n; i++) {
		uint32_t b;
		memcpy(&b, &v[i], 4);
		fprintf(f, "%s%08x", i ? "," : "", b);
	}
}

static void geom_walk(lossantos_rpf *a, FILE *out, const char *filter, int inside, lossantos_buf *buf, uint64_t *counts) {
	int mine = inside || !filter[0] || strstr(a->name, filter) != NULL;
	char path[2048];
	static lossantos_entity ents[16384];
	static lossantos_archetype arch[16384];
	for (uint32_t i = 1; i < a->count; i++) {
		const lossantos_entry *e = &a->e[i];
		if (e->kind == LOSSANTOS_DIR) continue;
		if (ends_with_rpf(e->name)) {
			lossantos_rpf child;
			if (lossantos_rpf_open_child(&child, a, i)) {
				geom_walk(&child, out, filter, mine, buf, counts);
				lossantos_rpf_close(&child);
			}
			continue;
		}
		size_t nl = strlen(e->name);
		if (!mine || e->kind != LOSSANTOS_RESOURCE || nl < 5) continue;
		const char *ext = e->name + nl - 5;
		int isydr = _stricmp(ext + 1, ".ydr") == 0, isymap = _stricmp(ext, ".ymap") == 0,
			isytyp = _stricmp(ext, ".ytyp") == 0;
		if (!isydr && !isymap && !isytyp) continue;
		if (!lossantos_rpf_read(a, i, buf)) continue;
		size_t sys = lossantos_rpf_size_from_flags(e->sys_flags);
		lossantos_rpf_path(a, i, path, sizeof path);
		if (isydr) {
			lossantos_drawable d;
			if (!lossantos_ydr_parse(buf->p, buf->n, sys, &d)) continue;
			counts[0]++;
			for (int g = 0; g < d.ngeoms; g++) {
				const lossantos_geom *G = &d.geoms[g];
				int64_t *ix = (int64_t *)malloc((size_t)G->ntris * 3 * 8);
				for (size_t k = 0; k < (size_t)G->ntris * 3; k++) ix[k] = G->idx[k];
				fprintf(out, "%s#%d\t%u\t%u\t%u\t%s\t%016llx\t%016llx\t", path, g, G->nverts, G->ntris,
						G->shader, G->texture[0] ? G->texture : "-",
						(unsigned long long)wsum64((const uint8_t *)G->pos, (size_t)G->nverts * 12),
						(unsigned long long)wsum64((const uint8_t *)G->uv, (size_t)G->nverts * 8));
				if (G->nrm) fprintf(out, "%016llx", (unsigned long long)wsum64((const uint8_t *)G->nrm, (size_t)G->nverts * 12));
				else fprintf(out, "-");
				fprintf(out, "\t%016llx\n", (unsigned long long)wsum64((const uint8_t *)ix, (size_t)G->ntris * 24));
				free(ix);
				counts[1]++;
			}
			fprintf(out, "%s#tex\t%d\n", path, d.ntex);
			lossantos_ydr_free(&d);
		} else if (isymap) {
			int n = lossantos_ymap_entities(buf->p, buf->n, sys, ents, 16384);
			for (int k = 0; k < n; k++) {
				fprintf(out, "%s#%d\t%08x\t%u\t%u\t", path, k, ents[k].archetype, ents[k].flags, ents[k].lod_level);
				fhex(out, ents[k].pos, 3);
				fputc('\t', out);
				fhex(out, ents[k].rot, 4);
				fputc('\t', out);
				fhex(out, &ents[k].scale_xy, 1);
				fputc(',', out);
				fhex(out, &ents[k].scale_z, 1);
				fputc('\n', out);
			}
			counts[2] += n > 0 ? n : 0;
		} else {
			int n = lossantos_ytyp_archetypes(buf->p, buf->n, sys, arch, 16384);
			for (int k = 0; k < n; k++) {
				fprintf(out, "%s#%d\t%08x\t%08x\t", path, k, arch[k].name, arch[k].asset_name);
				fhex(out, arch[k].bb_min, 3);
				fputc('\t', out);
				fhex(out, arch[k].bb_max, 3);
				fputc('\t', out);
				fhex(out, &arch[k].lod_dist, 1);
				fputc('\n', out);
			}
			counts[3] += n > 0 ? n : 0;
		}
	}
}

// .tri file
// 0  ntris u64
// 8  ntris * 9 doubles
//    ntris material bytes
static void ybn_walk(lossantos_rpf *a, const wchar_t *outdir, const char *filter, int inside, lossantos_buf *buf,
					 uint64_t *counts) {
	int mine = inside || !filter[0] || strstr(a->name, filter) != NULL;
	for (uint32_t i = 1; i < a->count; i++) {
		const lossantos_entry *e = &a->e[i];
		if (e->kind == LOSSANTOS_DIR) continue;
		if (ends_with_rpf(e->name)) {
			lossantos_rpf child;
			if (lossantos_rpf_open_child(&child, a, i)) {
				ybn_walk(&child, outdir, filter, mine, buf, counts);
				lossantos_rpf_close(&child);
			}
			continue;
		}
		size_t nl = strlen(e->name);
		if (!mine || e->kind != LOSSANTOS_RESOURCE || nl < 4 || _stricmp(e->name + nl - 4, ".ybn") != 0) continue;
		if (!lossantos_rpf_read(a, i, buf)) continue;
		lossantos_ybn y;
		if (!lossantos_ybn_parse(buf->p, buf->n, lossantos_rpf_size_from_flags(e->sys_flags), &y)) continue;
		wchar_t path[1024];
		swprintf(path, 1024, L"%ls\\%hs.tri", outdir, e->name);
		FILE *f = _wfopen(path, L"wb");
		uint64_t nt = y.ntris;
		fwrite(&nt, 8, 1, f);
		if (nt) {
			fwrite(y.tri, sizeof(double), nt * 9, f);
			fwrite(y.mat, 1, nt, f);
		}
		fclose(f);
		counts[0]++;
		counts[1] += nt;
		lossantos_ybn_free(&y);
	}
}

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static void map_progress(void *ctx, const char *stage, size_t done, size_t total) {
	(void)ctx;
	static volatile LONG last = -1;
	LONG pct = (LONG)(done * 100 / (total ? total : 1));
	if (done == total || InterlockedExchange(&last, pct) != pct) printf("\r  %-9s %zu/%zu   ", stage, done, total);
	if (done == total) printf("\n");
}

// faulting function + short stack
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	SymInitialize(proc, NULL, TRUE);
	fprintf(stderr, "CRASH code 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode,
			ep->ExceptionRecord->ExceptionAddress);
	CONTEXT ctx = *ep->ContextRecord;
	STACKFRAME64 sf;
	memset(&sf, 0, sizeof sf);
	sf.AddrPC.Offset = ctx.Rip;
	sf.AddrPC.Mode = AddrModeFlat;
	sf.AddrFrame.Offset = ctx.Rbp;
	sf.AddrFrame.Mode = AddrModeFlat;
	sf.AddrStack.Offset = ctx.Rsp;
	sf.AddrStack.Mode = AddrModeFlat;
	for (int i = 0; i < 12; i++) {
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, NULL,
						 SymFunctionTableAccess64, SymGetModuleBase64, NULL))
			break;
		char sbuf[sizeof(SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = (SYMBOL_INFO *)sbuf;
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		IMAGEHLP_LINE64 line;
		line.SizeOfStruct = sizeof line;
		DWORD ld = 0;
		const char *fn = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
		if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ld, &line))
			fprintf(stderr, "  %s  %s:%lu\n", fn, line.FileName, line.LineNumber);
		else
			fprintf(stderr, "  %s\n", fn);
	}
	fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}

int wmain(int argc, wchar_t **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	SetUnhandledExceptionFilter(crash_filter);
	if (argc >= 4 && wcscmp(argv[1], L"keys") == 0) {
		wchar_t exe[1024];
		swprintf(exe, 1024, L"%ls\\GTA5.exe", argv[2]);
		uint8_t aes[32], awc[16];
		double t0 = lossantos_now();
		if (!lossantos_keys_find_aes(exe, aes)) {
			fprintf(stderr, "%s\n", lossantos_error());
			return 1;
		}
		printf("AES key found in GTA5.exe (%.2f s)\n", lossantos_now() - t0);
		uint8_t *magic = NULL;
		size_t mn = 0;
		if (argc > 4) {
			FILE *f = _wfopen(argv[4], L"rb");
			if (f) {
				fseek(f, 0, SEEK_END);
				mn = (size_t)ftell(f);
				fseek(f, 0, SEEK_SET);
				magic = (uint8_t *)malloc(mn);
				if (fread(magic, 1, mn, f) != mn) mn = 0;
				fclose(f);
			}
		} else {
			mn = lossantos_keys_fetch_magic(&magic);
		}
		if (!mn) {
			fprintf(stderr, "no magic.dat: %s\n", lossantos_error());
			return 1;
		}
		static lossantos_ng kng;
		if (!lossantos_keys_unlock(magic, mn, aes, &kng, awc) || !lossantos_keys_write(argv[3], &kng, aes, awc)) {
			fprintf(stderr, "%s\n", lossantos_error());
			return 1;
		}
		printf("keys written to %ls (magic.dat %zu bytes, %.2f s total)\n", argv[3], mn, lossantos_now() - t0);
		return 0;
	}
	if (argc < 4) {
		fprintf(stderr, "usage: lossantos_test <keys dir> walk <archive.rpf> [dump.txt]\n");
		return 2;
	}
	static lossantos_ng ng;
	if (!lossantos_ng_load(&ng, argv[1])) {
		fprintf(stderr, "%s\n", lossantos_error());
		return 1;
	}
	if (wcscmp(argv[2], L"radio") == 0 && argc >= 6) {
		lossantos_radio_opts ro;
		memset(&ro, 0, sizeof ro);
		wchar_t kp[1024], vgm[1024], ff[1024];
		swprintf(kp, 1024, L"%ls\\gtav_awc_key.dat", argv[1]);
		FILE *kf = _wfopen(kp, L"rb");
		if (!kf || fread(ro.awc_key, 1, 16, kf) != 16) {
			fprintf(stderr, "cannot read %ls\n", kp);
			return 1;
		}
		fclose(kf);
		char only[64] = "";
		if (argc > 7 && wcscmp(argv[7], L"-")) WideCharToMultiByte(CP_UTF8, 0, argv[7], -1, only, sizeof only, NULL, NULL);
		if (!lossantos_find_tool(argc > 8 && wcscmp(argv[8], L"-") ? argv[8] : NULL, L"vgmstream", L"vgmstream-cli.exe", vgm, 1024) ||
			!lossantos_find_tool(argc > 9 && wcscmp(argv[9], L"-") ? argv[9] : NULL, L"ffmpeg", L"ffmpeg.exe", ff, 1024)) {
			fprintf(stderr, "vgmstream-cli.exe / ffmpeg.exe not found\n");
			return 1;
		}
		printf("vgmstream %ls\nffmpeg    %ls\n", vgm, ff);
		ro.gta_dir = argv[3];
		ro.ng = &ng;
		ro.out_dir = argv[4];
		ro.work_dir = argv[5];
		ro.jobs = argc > 6 ? _wtoi(argv[6]) : 0;
		ro.only = only[0] ? only : NULL;
		ro.vgmstream = vgm;
		ro.ffmpeg = ff;
		ro.background = argc > 10 && _wtoi(argv[10]) != 0;
		ro.progress = map_progress;
		ro.log = stdout;
		lossantos_radio_stats rs;
		int rok = lossantos_build_radio(&ro, &rs);
		printf("\nradio %s: %zu stations, %zu awc, %zu clips (%zu failed), %.1f h audio, %zu logos, %zu titles, "
			   "%zu chunks %.2f GB\n",
			   rok ? "OK" : "FAILED", rs.stations, rs.awc, rs.clips, rs.failed, rs.audio_seconds / 3600.0, rs.logos,
			   rs.titled, rs.chunks, rs.bytes / 1073741824.0);
		printf("time: scan %.1f s, encode %.1f s, ui %.1f s, pack %.1f s, total %.1f s\n", rs.t_extract, rs.t_encode,
			   rs.t_ui, rs.t_pack, rs.t_total);
		if (!rok) fprintf(stderr, "%s\n", lossantos_error());
		return rok ? 0 : 1;
	}
	if (wcscmp(argv[2], L"walk") == 0) {
		lossantos_rpf a;
		if (!lossantos_rpf_open(&a, &ng, argv[3])) {
			fprintf(stderr, "%s\n", lossantos_error());
			return 1;
		}
		walk_ctx w;
		memset(&w, 0, sizeof w);
		if (argc > 4) w.dump = _wfopen(argv[4], L"w");
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t0);
		walk(&a, &w);
		QueryPerformanceCounter(&t1);
		double s = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
		printf("archives %llu files %llu errors %llu | in %.1f MB out %.1f MB | %.2f s = %.0f MB/s out\n",
			   (unsigned long long)w.archives, (unsigned long long)w.files,
			   (unsigned long long)w.errors, w.bytes_in / 1048576.0, w.bytes_out / 1048576.0, s,
			   w.bytes_out / 1048576.0 / (s > 0 ? s : 1));
		if (w.dump) fclose(w.dump);
		lossantos_rpf_close(&a);
		lossantos_buf_free(&w.buf);
		return w.errors ? 1 : 0;
	}
	if (wcscmp(argv[2], L"ytdall") == 0 && argc >= 4) {
		lossantos_rpf a;
		if (!lossantos_rpf_open(&a, &ng, argv[3])) {
			fprintf(stderr, "%s\n", lossantos_error());
			return 1;
		}
		ytd_ctx y;
		memset(&y, 0, sizeof y);
		char filter[256] = "";
		if (argc > 4) WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, filter, sizeof filter, NULL, NULL);
		y.filter = filter;
		if (argc > 5) y.dump = _wfopen(argv[5], L"w");
		y.nodecode = argc > 6 && wcscmp(argv[6], L"nodecode") == 0;
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t0);
		ytd_walk(&a, &y, 0);
		QueryPerformanceCounter(&t1);
		double s = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
		printf("ytds %llu textures %llu decoded %llu (%.1f MB RGBA) in %.2f s\n",
			   (unsigned long long)y.ytds, (unsigned long long)y.textures,
			   (unsigned long long)y.decoded, y.bytes / 1048576.0, s);
		if (y.dump) fclose(y.dump);
		lossantos_rpf_close(&a);
		return 0;
	}
	if (wcscmp(argv[2], L"geom") == 0 && argc >= 6) {
		lossantos_rpf a;
		if (!lossantos_rpf_open(&a, &ng, argv[3])) {
			fprintf(stderr, "%s\n", lossantos_error());
			return 1;
		}
		char filter[256];
		WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, filter, sizeof filter, NULL, NULL);
		FILE *out = _wfopen(argv[5], L"w");
		lossantos_buf b = {0};
		uint64_t counts[4] = {0};
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t0);
		geom_walk(&a, out, filter, 0, &b, counts);
		QueryPerformanceCounter(&t1);
		fclose(out);
		printf("ydr %llu geoms %llu entities %llu archetypes %llu in %.2f s\n",
			   (unsigned long long)counts[0], (unsigned long long)counts[1], (unsigned long long)counts[2],
			   (unsigned long long)counts[3], (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
		lossantos_rpf_close(&a);
		lossantos_buf_free(&b);
		return 0;
	}
	if (wcscmp(argv[2], L"ybn") == 0 && argc >= 6) {
		lossantos_rpf a;
		if (!lossantos_rpf_open(&a, &ng, argv[3])) {
			fprintf(stderr, "%s\n", lossantos_error());
			return 1;
		}
		char filter[256];
		WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, filter, sizeof filter, NULL, NULL);
		CreateDirectoryW(argv[5], NULL);
		lossantos_buf b = {0};
		uint64_t counts[2] = {0};
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t0);
		ybn_walk(&a, argv[5], filter, 0, &b, counts);
		QueryPerformanceCounter(&t1);
		printf("ybn %llu triangles %llu in %.2f s\n", (unsigned long long)counts[0],
			   (unsigned long long)counts[1], (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
		lossantos_rpf_close(&a);
		lossantos_buf_free(&b);
		return 0;
	}
	if (wcscmp(argv[2], L"map") == 0 && argc >= 6) {
		lossantos_blockref_vec refs = {0};
		double t0 = lossantos_now();
		lossantos_discover(argv[3], &ng, &refs);
		static lossantos_world w;
		if (!lossantos_world_init(&w, argv[3], &ng) || !lossantos_world_load_atlases(&w)) {
			fprintf(stderr, "world: %s\n", lossantos_error());
			return 1;
		}
		int compat = argc > 8 ? _wtoi(argv[8]) : 0;
		w.compat.dedupe_x_only = compat;
		w.compat.median_mixed_units = compat;
		char only[256] = "";
		if (argc > 7 && wcscmp(argv[7], L"-") != 0)
			WideCharToMultiByte(CP_UTF8, 0, argv[7], -1, only, sizeof only, NULL, NULL);
		lossantos_map_opts o;
		memset(&o, 0, sizeof o);
		o.out_dir = argv[4];
		o.work_dir = argv[5];
		o.jobs = argc > 6 ? _wtoi(argv[6]) : 0;
		o.only = only[0] ? only : NULL;
		o.background = argc > 9 && wcsstr(argv[9], L"bg") != NULL;
		o.loose = argc > 9 && wcsstr(argv[9], L"loose") != NULL;
		o.content_dir = argc > 10 ? argv[10] : NULL;
		o.progress = map_progress;
		wchar_t lp[1024];
		CreateDirectoryW(argv[4], NULL);
		swprintf(lp, 1024, L"%ls\\lossantos_build.log", argv[4]);
		o.log = _wfopen(lp, L"w");
		printf("discover + atlases: %zu blocks, %.2f s\n", refs.n, lossantos_now() - t0);
		lossantos_map_stats st;
		int ok = lossantos_build_map(&w, &refs, &o, &st);
		if (o.log) fclose(o.log);
		printf("\nmap %s: blocks %zu (detail %zu, fallback %zu, empty %zu, failed %zu), %zu textures borrowed\n",
			   ok ? "OK" : "FAILED", st.blocks, st.detail, st.fallback, st.empty, st.failed, st.borrowed);
		printf("cells %zu, %zu tile files (%zu tris, %zu verts), %zu sanded, chunks %zu (%zu tris, %zu dropped), %zu materials, spill %.2f GB\n",
			   st.tiles, st.files, st.tile_tris, st.tile_verts, st.sanded, st.chunks, st.chunk_tris, st.chunk_dropped, st.materials,
			   st.spill_bytes / 1073741824.0);
		if (o.content_dir)
			printf("content: %zu textures (%zu rebuilt, %zu skipped), %zu chunks %.2f GB, %zu canon aliases, %.1f s\n",
				   st.textures, st.tex_rebuilt, st.tex_skipped, st.content_chunks, st.content_bytes / 1073741824.0,
				   st.canon_aliases, st.t_content);
		printf("time: textures %.1f s, blocks %.1f s, tiles %.1f s, physics %.1f s, index %.1f s, total %.1f s\n",
			   st.t_phase0, st.t_phase1, st.t_tiles, st.t_phys, st.t_index, st.t_total);
		return ok ? 0 : 1;
	}
	if (wcscmp(argv[2], L"discover") == 0 && argc >= 4) {
		lossantos_blockref_vec refs = {0};
		lossantos_discover(argv[3], &ng, &refs);
		for (size_t i = 0; i < refs.n; i++)
			printf("%s\t%s\t%s\t%d\n", refs.p[i].stem, refs.p[i].outer, refs.p[i].inner, refs.p[i].road);
		return 0;
	}
	if (wcscmp(argv[2], L"block") == 0 && argc >= 6) {
		char stem[96];
		WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, stem, sizeof stem, NULL, NULL);
		double t0 = lossantos_now();
		lossantos_blockref_vec refs = {0};
		lossantos_discover(argv[3], &ng, &refs);
		double t1 = lossantos_now();
		const lossantos_blockref *ref = NULL;
		for (size_t i = 0; i < refs.n; i++)
			if (_stricmp(refs.p[i].stem, stem) == 0) ref = &refs.p[i];
		printf("discover: %zu blocks/road packs in %.2f s\n", refs.n, t1 - t0);
		if (!ref) {
			fprintf(stderr, "no block %s\n", stem);
			return 1;
		}
		printf("block %s: %s / %s%s\n", ref->stem, ref->outer, ref->inner, ref->road ? " (road pack)" : "");
		static lossantos_world w;
		if (!lossantos_world_init(&w, argv[3], &ng) || !lossantos_world_load_atlases(&w)) {
			fprintf(stderr, "world: %s\n", lossantos_error());
			return 1;
		}
		int compat = argc > 7 ? _wtoi(argv[7]) : 1;
		w.compat.dedupe_x_only = compat;
		w.compat.median_mixed_units = compat;
		if (argc > 6 && wcscmp(argv[6], L"-") != 0)
			printf("registry: %d regions scanned\n", lossantos_texreg_scan(&w.reg, argv[6]));
		printf("atlases: %zu textures (%.2f s)\n", w.atlas.v.n, lossantos_now() - t1);
		static lossantos_block_out o0, o1;
		if (!lossantos_block_run(&w, ref, 0, &o0)) {
			fprintf(stderr, "phase 0: %s\n", o0.error);
			return 1;
		}
		for (size_t i = 0; i < o0.installs.n; i++) lossantos_texreg_add(&w.reg, o0.installs.p[i].path);
		lossantos_world_finish_installs(&w, o0.atlas_items.p, o0.atlas_items.n);
		printf("phase 0: %zu own installs, %zu atlas items, %.2f s\n", o0.installs.n, o0.atlas_items.n, o0.t_total);
		int coll_only = argc > 8 && wcscmp(argv[8], L"coll") == 0;
		if (!lossantos_block_run(&w, ref, coll_only ? 2 : 1, &o1)) {
			fprintf(stderr, "phase 1: %s\n", o1.error);
			return 1;
		}
		printf("phase 1: ydr %u placed %u lodskip %u | fallbacks %u dropped keys %u | coll %zu kept %zu | "
			   "shell %u/%u filler %u/%u | visual %zu tris, %u materials | read %.2f cover %.2f total %.2f s\n",
			   o1.ydr_files, o1.placed, o1.lod_skipped, o1.fallbacks, o1.dropped_keys, o1.coll_tris,
			   o1.coll_kept, o1.shell_dropped, o1.shell_tested, o1.filler_dropped, o1.filler_tested, o1.vis.n,
			   o1.mats.n, o1.t_read, o1.t_cover, o1.t_total);
		wchar_t dir[1024];
		CreateDirectoryW(argv[5], NULL);
		char mapname[160];
		if (coll_only) {
			// city.staging_name
			const char *rel = ref->inner + 12;
			int n = snprintf(mapname, sizeof mapname, "%.*s_", (int)(strchr(ref->outer, '.') - ref->outer), ref->outer);
			for (const char *q = rel; *q && n < 150; q++) mapname[n++] = *q == '/' ? '_' : *q;
			mapname[n] = 0;
			char *dot = strstr(mapname, ".rpf");
			if (dot) *dot = 0;
		} else {
			snprintf(mapname, sizeof mapname, "%s_detail", ref->stem);
		}
		swprintf(dir, 1024, L"%ls\\%hs", argv[5], mapname);
		double t2 = lossantos_now();
		if (!lossantos_stage_write(dir, mapname, &o1)) {
			fprintf(stderr, "stage write failed: %s\n", lossantos_error());
			return 1;
		}
		printf("stage written in %.2f s\n", lossantos_now() - t2);
		return 0;
	}
	if (wcscmp(argv[2], L"cat") == 0 && argc >= 6) {
		lossantos_rpf stack[8];
		int depth = 0;
		if (!lossantos_rpf_open(&stack[0], &ng, argv[3])) {
			fprintf(stderr, "%s\n", lossantos_error());
			return 1;
		}
		char inner[1024];
		WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, inner, sizeof inner, NULL, NULL);
		char *rest = inner;
		for (;;) {
			char *cut = strstr(rest, ".rpf/");
			if (!cut) break;
			cut[4] = 0;
			int32_t ci = lossantos_rpf_find(&stack[depth], rest);
			if (ci < 0 || depth == 7 || !lossantos_rpf_open_child(&stack[depth + 1], &stack[depth], (uint32_t)ci)) {
				fprintf(stderr, "no child archive %s\n", rest);
				return 1;
			}
			depth++;
			rest = cut + 5;
		}
		int32_t idx = lossantos_rpf_find(&stack[depth], rest);
		lossantos_buf b = {0};
		if (idx < 0 || !lossantos_rpf_read(&stack[depth], (uint32_t)idx, &b)) {
			fprintf(stderr, "cannot read %s: %s\n", rest, lossantos_error());
			return 1;
		}
		FILE *f = _wfopen(argv[5], L"wb");
		fwrite(b.p, 1, b.n, f);
		fclose(f);
		printf("%zu bytes\n", b.n);
		while (depth >= 0) lossantos_rpf_close(&stack[depth--]);
		lossantos_buf_free(&b);
		return 0;
	}
	fprintf(stderr, "unknown command\n");
	return 2;
}
