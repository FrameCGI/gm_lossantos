#include "lossantos_gma.h"
#include "lossantos_common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t g_crc_table[256];
static INIT_ONCE g_crc_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK crc_init(PINIT_ONCE o, PVOID p, PVOID *c) {
	(void)o; (void)p; (void)c;
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t v = i;
		for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
		g_crc_table[i] = v;
	}
	return TRUE;
}

uint32_t lossantos_crc32(uint32_t crc, const void *p, size_t n) {
	InitOnceExecuteOnce(&g_crc_once, crc_init, NULL, NULL);
	const uint8_t *b = (const uint8_t *)p;
	crc = ~crc;
	for (size_t i = 0; i < n; i++) crc = g_crc_table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

typedef struct {
	FILE *f;
	uint32_t crc;
	uint64_t n;
	int bad;
} wr;

static void put(wr *w, const void *p, size_t n) {
	if (fwrite(p, 1, n, w->f) != n) w->bad = 1;
	w->crc = lossantos_crc32(w->crc, p, n);
	w->n += n;
}

static int read_at(HANDLE h, uint64_t off, void *dst, size_t len) {
	uint8_t *d = (uint8_t *)dst;
	while (len) {
		DWORD want = len > (1u << 30) ? (1u << 30) : (DWORD)len, got = 0;
		OVERLAPPED ov;
		memset(&ov, 0, sizeof ov);
		ov.Offset = (DWORD)off;
		ov.OffsetHigh = (DWORD)(off >> 32);
		if (!ReadFile(h, d, want, &got, &ov) || got != want) return 0;
		d += got;
		off += got;
		len -= got;
	}
	return 1;
}

uint64_t lossantos_gma_write(const wchar_t *out_path, const char *title, const lossantos_gma_file *files, size_t n,
					   void *const *spills) {
	wchar_t tmp[1024];
	swprintf(tmp, 1024, L"%ls.tmp", out_path);
	wr w = {0};
	w.f = _wfopen(tmp, L"wb");
	if (!w.f) {
		lossantos_set_error("cannot write %ls", tmp);
		return 0;
	}
	setvbuf(w.f, NULL, _IOFBF, 1 << 20);
	uint8_t v = 3;
	uint64_t z = 0, t = (uint64_t)time(NULL);
	put(&w, "GMAD", 4);
	put(&w, &v, 1);  // version
	put(&w, &z, 8);  // steamid 0
	put(&w, &t, 8);  // timestamp
	put(&w, "", 1);                       /* required content: none */
	put(&w, title, strlen(title) + 1);
	static const char DESC[] = "{\n\t\"description\": \"Description\",\n\t\"type\": \"servercontent\",\n\t\"tags\": "
							   "[\n\t\t\"scenic\",\n\t\t\"realism\"\n\t]\n}";
	put(&w, DESC, sizeof DESC);
	put(&w, "Author Name", 12);
	int32_t ver = 1;
	put(&w, &ver, 4);  // addon version
	// file table, u32 0 ends it
	for (size_t i = 0; i < n; i++) {
		uint32_t idx = (uint32_t)(i + 1);
		int64_t size = files[i].size;
		put(&w, &idx, 4);
		put(&w, files[i].path, strlen(files[i].path) + 1);
		put(&w, &size, 8);
		put(&w, &files[i].crc, 4);
	}
	uint32_t end = 0;
	put(&w, &end, 4);
	uint8_t *buf = NULL;
	size_t cap = 0;
	for (size_t i = 0; i < n && !w.bad; i++) {
		if (files[i].size > cap) {
			free(buf);
			cap = files[i].size * 2;
			buf = (uint8_t *)malloc(cap);
			if (!buf) { w.bad = 1; break; }
		}
		if (!read_at((HANDLE)spills[files[i].spill], files[i].off, buf, files[i].size)) {
			w.bad = 1;
			break;
		}
		put(&w, buf, files[i].size);
	}
	free(buf);
	uint32_t crc = w.crc;
	if (fwrite(&crc, 4, 1, w.f) != 1) w.bad = 1;  // crc of everything above
	if (fclose(w.f)) w.bad = 1;
	if (w.bad || !MoveFileExW(tmp, out_path, MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(tmp);
		lossantos_set_error("writing %ls failed", out_path);
		return 0;
	}
	return w.n + 4;
}
