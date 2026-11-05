#include "lossantos_radio.h"
#include "lossantos_gma.h"
#include "lossantos_inflate.h"
#include "lossantos_rpf.h"
#include "lossantos_rsc.h"
#include "lossantos_util.h"
#include "lossantos_ytd.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lossantos_run_pool(const lossantos_map_opts *o, const char *stage, size_t n, void (*fn)(void *, size_t), void *ctx);

#define RS_XXTEA_XOR 0x7B3A207Fu  // .awckey word xor
#define MATCH_MIN 0.75  // title match cutoff


static void mkdirs(const wchar_t *path) {
	wchar_t p[1024];
	wcsncpy(p, path, 1023);
	p[1023] = 0;
	for (wchar_t *s = p + 3; *s; s++)
		if (*s == L'\\' || *s == L'/') {
			wchar_t c = *s;
			*s = 0;
			CreateDirectoryW(p, NULL);
			*s = c;
		}
	CreateDirectoryW(p, NULL);
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

static int write_all(const wchar_t *path, const void *p, size_t n) {
	FILE *f = _wfopen(path, L"wb");
	if (!f) return 0;
	int ok = fwrite(p, 1, n, f) == n;
	return (fclose(f) == 0) && ok;
}

static int read_all(const wchar_t *path, lossantos_buf *b) {
	FILE *f = _wfopen(path, L"rb");
	if (!f) return 0;
	_fseeki64(f, 0, SEEK_END);
	long long n = _ftelli64(f);
	_fseeki64(f, 0, SEEK_SET);
	int ok = n >= 0 && lossantos_buf_reserve(b, (size_t)n + 1) && fread(b->p, 1, (size_t)n, f) == (size_t)n;
	if (ok) b->n = (size_t)n;
	fclose(f);
	return ok;
}

int lossantos_find_tool(const wchar_t *given, const wchar_t *sub, const wchar_t *exe, wchar_t *out, size_t cap) {
	if (given && given[0]) {
		if (GetFileAttributesW(given) == INVALID_FILE_ATTRIBUTES) return 0;
		wcsncpy(out, given, cap - 1);
		out[cap - 1] = 0;
		return 1;
	}
	wchar_t dir[1024];
	HMODULE m = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					   (LPCWSTR)(void *)lossantos_find_tool, &m);
	GetModuleFileNameW(m, dir, 1024);
	wchar_t *s = wcsrchr(dir, L'\\');
	if (s) *s = 0;
	if (sub && sub[0]) {
		swprintf(out, cap, L"%ls\\%ls\\%ls", dir, sub, exe);
		if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
	}
	swprintf(out, cap, L"%ls\\%ls", dir, exe);
	if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
	DWORD n = SearchPathW(NULL, exe, NULL, (DWORD)cap, out, NULL);
	return n > 0 && n < cap;
}


typedef struct {
	HANDLE job;                /* KILL_ON_JOB_CLOSE */
	int background;
	volatile long *cancel;
} procenv;

// only these handles get inherited
static int spawn(const procenv *pe, wchar_t *cmd, HANDLE in, HANDLE out, HANDLE err, PROCESS_INFORMATION *pi) {
	STARTUPINFOEXW si;
	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	HANDLE list[3];
	int nl = 0;
	HANDLE hs[3] = {in, out, err};
	for (int i = 0; i < 3; i++) {
		if (!hs[i]) continue;
		int dup = 0;
		for (int k = 0; k < nl; k++) dup |= list[k] == hs[i];
		if (!dup) list[nl++] = hs[i];
	}
	LPPROC_THREAD_ATTRIBUTE_LIST al = NULL;
	if (nl) {
		SIZE_T sz = 0;
		InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
		al = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(sz);
		if (!al || !InitializeProcThreadAttributeList(al, 1, 0, &sz) ||
			!UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, nl * sizeof(HANDLE), NULL, NULL)) {
			free(al);
			return 0;
		}
		si.lpAttributeList = al;
		si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput = in;
		si.StartupInfo.hStdOutput = out;
		si.StartupInfo.hStdError = err;
	}
	DWORD fl = CREATE_NO_WINDOW | CREATE_SUSPENDED | (al ? EXTENDED_STARTUPINFO_PRESENT : 0) |
			   (pe->background ? BELOW_NORMAL_PRIORITY_CLASS : 0);
	BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, nl ? TRUE : FALSE, fl, NULL, NULL, &si.StartupInfo, pi);
	if (al) {
		DeleteProcThreadAttributeList(al);
		free(al);
	}
	if (!ok) return 0;
	if (pe->job) AssignProcessToJobObject(pe->job, pi->hProcess);
	ResumeThread(pi->hThread);
	CloseHandle(pi->hThread);
	pi->hThread = NULL;
	return 1;
}

static DWORD finish_proc(const procenv *pe, HANDLE h) {
	for (;;) {
		if (WaitForSingleObject(h, 200) == WAIT_OBJECT_0) break;
		if (pe->cancel && *pe->cancel) {
			TerminateProcess(h, 3);
			WaitForSingleObject(h, INFINITE);
			break;
		}
	}
	DWORD code = 1;
	GetExitCodeProcess(h, &code);
	CloseHandle(h);
	return code;
}

static int make_pipe(HANDLE *rd, HANDLE *wr, int child_reads) {
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	if (!CreatePipe(rd, wr, &sa, 1u << 20)) return 0;
	// our end must not be inheritable
	SetHandleInformation(child_reads ? *wr : *rd, HANDLE_FLAG_INHERIT, 0);
	return 1;
}

// stdout + stderr, short tools only
static int run_capture(const procenv *pe, wchar_t *cmd, lossantos_buf *out, DWORD *code) {
	HANDLE rd, wr;
	if (!make_pipe(&rd, &wr, 0)) return 0;
	PROCESS_INFORMATION pi;
	int ok = spawn(pe, cmd, NULL, wr, wr, &pi);
	CloseHandle(wr);
	if (!ok) {
		CloseHandle(rd);
		return 0;
	}
	out->n = 0;
	for (;;) {
		if (!lossantos_buf_reserve(out, out->n + 4097)) break;
		DWORD got = 0;
		if (!ReadFile(rd, out->p + out->n, 4096, &got, NULL) || !got) break;
		out->n += got;
	}
	if (out->p) out->p[out->n] = 0;
	CloseHandle(rd);
	*code = finish_proc(pe, pi.hProcess);
	return 1;
}


static uint32_t joaat_lower(const char *s) {
	uint32_t h = 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c >= 0x80) continue;           /* encode('ascii', 'ignore') */
		if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
		h += c;
		h += h << 10;
		h ^= h >> 6;
	}
	h += h << 3;
	h ^= h >> 11;
	h += h << 15;
	return h;
}

typedef struct {
	lossantos_buf data;
	lossantos_hmap map;              /* hash -> string offset */
} gxt;

static const char *gxt_get(const gxt *g, uint32_t h) {
	const uint32_t *v = lossantos_hmap_get(&g->map, (uint64_t)h + 1);
	return v ? (const char *)g->data.p + *v : NULL;
}

// 0  magic "2TXG" or "GXT2"
// 4  count
// 8  hash, offset pairs
static int gxt_parse(gxt *g) {
	const uint8_t *d = g->data.p;
	size_t n = g->data.n;
	if (n < 8 || (memcmp(d, "2TXG", 4) && memcmp(d, "GXT2", 4))) return 0;
	uint32_t count;
	memcpy(&count, d + 4, 4);
	if (8 + (size_t)count * 8 > n) return 0;
	lossantos_buf_reserve(&g->data, n + 1);
	g->data.p[n] = 0;
	lossantos_hmap_init(&g->map, count * 2 + 16);
	for (uint32_t i = 0; i < count; i++) {
		uint32_t h, off;
		memcpy(&h, g->data.p + 8 + i * 8, 4);
		memcpy(&off, g->data.p + 12 + i * 8, 4);
		if (off >= n) continue;
		int found;
		uint32_t *slot = lossantos_hmap_put(&g->map, (uint64_t)h + 1, &found);
		if (slot) *slot = off;               /* later duplicates win, like a dict */
	}
	return 1;
}

static int find_entry_ci(const lossantos_rpf *a, const char *name) {
	for (uint32_t i = 0; i < a->count; i++)
		if (_stricmp(a->e[i].name, name) == 0) return (int)i;
	return -1;
}

static int gxt_load(lossantos_rpf *lang, const char *name, gxt *g) {
	memset(g, 0, sizeof *g);
	int idx = find_entry_ci(lang, name);
	if (idx < 0) {
		lossantos_set_error("%s not found", name);
		return 0;
	}
	// name key first, then every other ng key
	if (lossantos_rpf_read(lang, (uint32_t)idx, &g->data) && gxt_parse(g)) return 1;
	const lossantos_entry *e = &lang->e[idx];
	if (!e->encrypted) return 0;
	uint32_t stored = e->file_size ? e->file_size : e->unc_size;
	uint8_t *raw = (uint8_t *)malloc(stored + 16);
	if (!raw || !lossantos_rpf_read_at(lang, (uint64_t)e->file_offset * 512, raw, stored)) {
		free(raw);
		return 0;
	}
	uint8_t *tmp = (uint8_t *)malloc(stored + 16);
	int ok = 0;
	for (int k = 0; k < LOSSANTOS_NG_KEYS && !ok; k++) {
		memcpy(tmp, raw, stored);
		lossantos_ng_decrypt(lang->ng, tmp, stored, lang->ng->keys[k]);
		g->data.n = 0;
		if (e->file_size) {
			if (!lossantos_inflate(tmp, stored, &g->data, e->unc_size)) continue;
		} else {
			lossantos_buf_reserve(&g->data, stored + 1);
			memcpy(g->data.p, tmp, stored);
			g->data.n = stored;
		}
		ok = gxt_parse(g);
	}
	free(raw);
	free(tmp);
	return ok;
}

static void gxt_free(gxt *g) {
	lossantos_buf_free(&g->data);
	lossantos_hmap_free(&g->map);
}


static int isw(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80; }
static int isal(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
static int issp(unsigned char c) { return c == ' ' || (c >= 9 && c <= 13); }

// radio_ui_export._norm
static void norm(const char *text, char *out, size_t cap) {
	size_t tn = strlen(text);
	char *t = (char *)malloc(tn * 5 + 16);
	size_t n = 0;
	for (size_t i = 0; i < tn; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c == '&') {
			memcpy(t + n, " and ", 5);
			n += 5;
		} else if (c == '_') {
			t[n++] = ' ';
		} else {
			t[n++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
		}
	}
	t[n] = 0;
	// \bwanna\b -> "want to"
	char *u = (char *)malloc(n * 2 + 16);
	size_t m = 0;
	for (size_t i = 0; i < n;) {
		if (strncmp(t + i, "wanna", 5) == 0 && (i == 0 || !isw((unsigned char)t[i - 1])) && !isw((unsigned char)t[i + 5])) {
			memcpy(u + m, "want to", 7);
			m += 7;
			i += 5;
		} else {
			u[m++] = t[i++];
		}
	}
	u[m] = 0;
	static const char *const NW[10][2] = {{"one", "1"}, {"two", "2"},   {"three", "3"}, {"four", "4"}, {"five", "5"},
										  {"six", "6"}, {"seven", "7"}, {"eight", "8"}, {"nine", "9"}, {"ten", "10"}};
	size_t o = 0;
	for (size_t i = 0; i < m;) {
		if (!isal((unsigned char)u[i])) {
			i++;
			continue;
		}
		size_t j = i;
		while (j < m && isal((unsigned char)u[j])) j++;
		char w[256];
		size_t wl = j - i < 255 ? j - i : 255;
		memcpy(w, u + i, wl);
		w[wl] = 0;
		i = j;
		const char *x = w;
		for (int k = 0; k < 10; k++)
			if (!strcmp(w, NW[k][0])) x = NW[k][1];
		if (!strcmp(x, "and") || !strcmp(x, "n")) continue;
		if (!strcmp(x, "u")) x = "you";
		size_t xl = strlen(x);
		if (o + xl + 1 < cap) {
			memcpy(out + o, x, xl);
			o += xl;
		}
	}
	out[o] = 0;
	free(t);
	free(u);
}

// strips (..), [..] and feat/ft/featuring tails
static void credit_sub(const char *s, char *out, size_t cap) {
	size_t n = strlen(s), o = 0, p = 0;
	static const char *const W[3] = {"feat", "ft", "featuring"};
	while (p < n) {
		size_t q = p;
		while (q < n && issp((unsigned char)s[q])) q++;
		size_t end = 0;
		int m = 0;
		if (q < n && (s[q] == '(' || s[q] == '[')) {
			const char *c = strchr(s + q + 1, s[q] == '(' ? ')' : ']');
			if (c) {
				end = (size_t)(c - s) + 1;
				m = 1;
			}
		}
		if (!m && q < n && (q == 0 || !isw((unsigned char)s[q - 1]))) {
			for (int k = 0; k < 3 && !m; k++) {
				size_t wl = strlen(W[k]);
				if (q + wl <= n && _strnicmp(s + q, W[k], wl) == 0 && (q + wl == n || !isw((unsigned char)s[q + wl]))) {
					end = n;
					m = 1;
				}
			}
		}
		if (m) {
			p = end;
			continue;
		}
		if (o + 1 < cap) out[o++] = s[p];
		p++;
	}
	out[o] = 0;
}

static void strip(char *s) {
	size_t n = strlen(s), a = 0;
	while (a < n && issp((unsigned char)s[a])) a++;
	while (n > a && issp((unsigned char)s[n - 1])) n--;
	memmove(s, s + a, n - a);
	s[n - a] = 0;
}

// difflib SequenceMatcher, no junk
static int seq_matches(const char *a, int la, const char *b, int lb, int *prev, int *cur) {
	int stack[256][4], sp = 0, total = 0;
	stack[sp][0] = 0, stack[sp][1] = la, stack[sp][2] = 0, stack[sp][3] = lb, sp++;
	while (sp) {
		sp--;
		int alo = stack[sp][0], ahi = stack[sp][1], blo = stack[sp][2], bhi = stack[sp][3];
		int besti = alo, bestj = blo, best = 0;
		for (int j = blo; j < bhi; j++) prev[j] = 0;
		for (int i = alo; i < ahi; i++) {
			for (int j = blo; j < bhi; j++) {
				int k = 0;
				if (a[i] == b[j]) {
					k = (j > blo ? prev[j - 1] : 0) + 1;
					if (k > best) {
						besti = i - k + 1;
						bestj = j - k + 1;
						best = k;
					}
				}
				cur[j] = k;
			}
			int *t = prev;
			prev = cur;
			cur = t;
		}
		if (!best) continue;
		total += best;
		if (sp + 2 > 256) continue;
		if (alo < besti && blo < bestj) stack[sp][0] = alo, stack[sp][1] = besti, stack[sp][2] = blo, stack[sp][3] = bestj, sp++;
		if (besti + best < ahi && bestj + best < bhi)
			stack[sp][0] = besti + best, stack[sp][1] = ahi, stack[sp][2] = bestj + best, stack[sp][3] = bhi, sp++;
	}
	return total;
}

static double ratio(const char *a, const char *b) {
	int la = (int)strlen(a), lb = (int)strlen(b);
	if (!la && !lb) return 1.0;
	int *w = (int *)malloc(sizeof(int) * 2 * (size_t)(lb + 1));
	int m = seq_matches(a, la, b, lb, w, w + lb + 1);
	free(w);
	return 2.0 * m / (double)(la + lb);
}

static double quick_ratio(const char *a, const char *b) {
	int ca[256] = {0}, la = 0, lb = 0, inter = 0;
	for (const unsigned char *p = (const unsigned char *)a; *p; p++) ca[*p]++, la++;
	for (const unsigned char *p = (const unsigned char *)b; *p; p++) {
		lb++;
		if (ca[*p] > 0) ca[*p]--, inter++;
	}
	if (!la && !lb) return 1.0;
	return 2.0 * inter / (double)(la + lb);
}

// -1 = certainly below MATCH_MIN
static double score(const char *k, const char *v) {
	size_t kl = strlen(k);
	int prefix = kl >= 8 && strncmp(v, k, kl) == 0;
	double q = quick_ratio(k, v);
	if (prefix) return q > 0.9 ? fmax(ratio(k, v), 0.9) : 0.9;
	if (q < MATCH_MIN) return -1.0;
	return ratio(k, v);
}

typedef struct {
	int n;
	const char *s, *a;
	char **var;
	int nvar;
} track;

static void add_var(track *t, const char *v) {
	for (int i = 0; i < t->nvar; i++)
		if (!strcmp(t->var[i], v)) return;
	t->var = (char **)realloc(t->var, (t->nvar + 1) * sizeof *t->var);
	t->var[t->nvar++] = _strdup(v);
}

static void variants(track *t) {
	size_t ls = strlen(t->s), la = strlen(t->a);
	size_t cap = (ls + la) * 6 + 64;
	char *bare = (char *)malloc(cap), *art = (char *)malloc(cap), *tmp = (char *)malloc(cap), *nb = (char *)malloc(cap);
	credit_sub(t->s, bare, cap);
	strip(bare);
	if (!bare[0]) strcpy(bare, t->s);
	credit_sub(t->a, art, cap);
	strip(art);
	if (!art[0]) strcpy(art, t->a);
	norm(t->s, nb, cap);
	add_var(t, nb);
	norm(bare, nb, cap);
	add_var(t, nb);
	snprintf(tmp, cap, "%s %s", bare, art);
	norm(tmp, nb, cap);
	add_var(t, nb);
	snprintf(tmp, cap, "%s %s", t->s, art);
	norm(tmp, nb, cap);
	add_var(t, nb);
	for (const char *p = bare;;) {
		const char *e = p;
		while (*e && *e != '/' && *e != ',') e++;
		snprintf(tmp, cap, "%.*s", (int)(e - p), p);
		norm(tmp, nb, cap);
		if (strlen(nb) >= 4) add_var(t, nb);
		if (!*e) break;
		p = e + 1;
	}
	free(bare);
	free(art);
	free(tmp);
	free(nb);
}

typedef struct {
	double r;
	int stem;                   /* index into the song list */
	int track;
} cand;

static const char *const *g_songs;   /* for the candidate sort (single-threaded) */
static const track *g_tracks;
static int cmp_cand(const void *x, const void *y) {
	const cand *a = (const cand *)x, *b = (const cand *)y;
	if (a->r != b->r) return a->r > b->r ? -1 : 1;
	int c = strcmp(g_songs[a->stem], g_songs[b->stem]);
	if (c) return -c;
	if (g_tracks[a->track].n != g_tracks[b->track].n) return g_tracks[a->track].n > g_tracks[b->track].n ? -1 : 1;
	return 0;
}


static void jstr(FILE *f, const char *s) {
	fputc('"', f);
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		uint32_t cp;
		int len = 1;
		if (*p < 0x80) cp = *p;
		else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu), len = 2;
		else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
			cp = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu), len = 3;
		else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
			cp = ((*p & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu), len = 4;
		else cp = 0xFFFD;
		p += len;
		switch (cp) {
		case '"': fputs("\\\"", f); continue;
		case '\\': fputs("\\\\", f); continue;
		case '\n': fputs("\\n", f); continue;
		case '\r': fputs("\\r", f); continue;
		case '\t': fputs("\\t", f); continue;
		case '\b': fputs("\\b", f); continue;
		case '\f': fputs("\\f", f); continue;
		default: break;
		}
		if (cp >= 0x20 && cp < 0x7F) fputc((int)cp, f);
		else if (cp < 0x10000) fprintf(f, "\\u%04x", cp);
		else {
			cp -= 0x10000;
			fprintf(f, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
		}
	}
	fputc('"', f);
}

// repr(round(x, nd))
static void jnum(FILE *f, double x, int nd) {
	char b[64];
	snprintf(b, sizeof b, "%.*f", nd, x);
	double r = strtod(b, NULL);
	for (int d = 0; d <= nd; d++) {
		snprintf(b, sizeof b, "%.*f", d, r);
		if (strtod(b, NULL) == r) break;
	}
	if (!strchr(b, '.')) strcat(b, ".0");
	fputs(b, f);
}

static double round_nd(double x, int nd) {
	char b[64];
	snprintf(b, sizeof b, "%.*f", nd, x);
	return strtod(b, NULL);
}


static void be32(lossantos_buf *b, uint32_t v) {
	lossantos_buf_reserve(b, b->n + 4);
	b->p[b->n++] = (uint8_t)(v >> 24);
	b->p[b->n++] = (uint8_t)(v >> 16);
	b->p[b->n++] = (uint8_t)(v >> 8);
	b->p[b->n++] = (uint8_t)v;
}

static void png_chunk(lossantos_buf *b, const char *type, const uint8_t *d, size_t n) {
	be32(b, (uint32_t)n);
	lossantos_buf_reserve(b, b->n + 4 + n);
	memcpy(b->p + b->n, type, 4);
	if (n) memcpy(b->p + b->n + 4, d, n);
	uint32_t crc = lossantos_crc32(0, b->p + b->n, 4 + n);
	b->n += 4 + n;
	be32(b, crc);
}

// zlib stored blocks, the logos are small
static int png_write(const uint8_t *rgba, uint32_t w, uint32_t h, lossantos_buf *out) {
	size_t row = (size_t)w * 4 + 1, raw_n = row * h;
	uint8_t *raw = (uint8_t *)malloc(raw_n + 1);
	if (!raw) return 0;
	for (uint32_t y = 0; y < h; y++) {
		raw[y * row] = 0;
		memcpy(raw + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
	}
	size_t blocks = raw_n / 65535 + 1;
	lossantos_buf z = {0};
	lossantos_buf_reserve(&z, raw_n + blocks * 5 + 6);
	z.p[z.n++] = 0x78;
	z.p[z.n++] = 0x01;
	uint32_t s1 = 1, s2 = 0;
	for (size_t i = 0; i < raw_n; i++) {
		s1 = (s1 + raw[i]) % 65521;
		s2 = (s2 + s1) % 65521;
	}
	size_t pos = 0;
	do {
		size_t len = raw_n - pos > 65535 ? 65535 : raw_n - pos;
		int final = pos + len == raw_n;
		z.p[z.n++] = (uint8_t)final;
		z.p[z.n++] = (uint8_t)len;
		z.p[z.n++] = (uint8_t)(len >> 8);
		z.p[z.n++] = (uint8_t)~len;
		z.p[z.n++] = (uint8_t)(~len >> 8);
		memcpy(z.p + z.n, raw + pos, len);
		z.n += len;
		pos += len;
	} while (pos < raw_n);
	be32(&z, (s2 << 16) | s1);
	free(raw);
	out->n = 0;
	static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	lossantos_buf_reserve(out, z.n + 128);
	memcpy(out->p, SIG, 8);
	out->n = 8;
	uint8_t ih[13];
	ih[0] = (uint8_t)(w >> 24), ih[1] = (uint8_t)(w >> 16), ih[2] = (uint8_t)(w >> 8), ih[3] = (uint8_t)w;
	ih[4] = (uint8_t)(h >> 24), ih[5] = (uint8_t)(h >> 16), ih[6] = (uint8_t)(h >> 8), ih[7] = (uint8_t)h;
	ih[8] = 8, ih[9] = 6, ih[10] = 0, ih[11] = 0, ih[12] = 0;
	png_chunk(out, "IHDR", ih, 13);
	png_chunk(out, "IDAT", z.p, z.n);
	png_chunk(out, "IEND", NULL, 0);
	lossantos_buf_free(&z);
	return 1;
}


typedef struct {
	char name[64];             /* lowercase rpf stem: radio_01_class_rock */
	lossantos_rpf rpf;
	int open;
	wchar_t awc_dir[1024];
} station;

typedef struct {
	int st;
	uint32_t entry;
	char ename[128];           /* name as stored (sort key) */
	char stem[128];            /* lowercase, no .awc */
	uint64_t size;
	int nsub;
	int ok;                    /* extracted */
	volatile LONG left;        /* subsongs not yet encoded */
} awc_job;

typedef struct {
	uint32_t awc;
	int sub;
	int ok;
	double seconds;            /* round(.., 3) */
	char rel[200];             /* stem.ogg | stem/NNN.ogg */
	uint32_t size, crc;
} clip_job;

typedef struct {
	char gpath[192];
	wchar_t disk[1024];
	uint32_t size, crc;
	int group;                 /* 0 logos, 1 + station */
} pfile;
typedef LOSSANTOS_VEC(pfile) pfile_vec;

typedef struct {
	const lossantos_radio_opts *o;
	procenv pe;
	station *st;
	size_t nst;
	awc_job *awc;
	size_t nawc;
	clip_job *clip;
	size_t nclip;
	uint32_t *order;
	wchar_t ogg_dir[1024];
	CRITICAL_SECTION lock;
	volatile LONG failed;
	volatile LONG64 kb_done;   /* progress by audio size */
	LONG64 kb_total;
} rctx;

static void rlog(rctx *c, const char *fmt, ...) {
	if (!c->o->log) return;
	va_list ap;
	va_start(ap, fmt);
	EnterCriticalSection(&c->lock);
	vfprintf(c->o->log, fmt, ap);
	fflush(c->o->log);
	LeaveCriticalSection(&c->lock);
	va_end(ap);
}

static void awc_path(rctx *c, const awc_job *a, wchar_t *out) {
	swprintf(out, 1024, L"%ls\\%hs.awc", c->st[a->st].awc_dir, a->stem);
}

// extract one awc, count its streams
static void extract_job(void *ctx, size_t i) {
	rctx *c = (rctx *)ctx;
	awc_job *a = &c->awc[i];
	lossantos_buf b = {0};
	wchar_t p[1024];
	awc_path(c, a, p);
	if (!lossantos_rpf_read(&c->st[a->st].rpf, a->entry, &b) || !write_all(p, b.p, b.n)) {
		rlog(c, "radio: %s/%s: extract failed (%s)\n", c->st[a->st].name, a->ename, lossantos_error());
		lossantos_buf_free(&b);
		InterlockedIncrement(&c->failed);
		return;
	}
	a->size = b.n;
	wchar_t cmd[4096];
	swprintf(cmd, 4096, L"\"%ls\" -m \"%ls\"", c->o->vgmstream, p);
	DWORD code = 0;
	a->nsub = 1;
	if (run_capture(&c->pe, cmd, &b, &code) && b.p) {
		for (char *ln = (char *)b.p; ln && *ln;) {
			char *nl = strchr(ln, '\n');
			if (_strnicmp(ln, "stream count:", 13) == 0) {
				int v = atoi(ln + 13);
				if (v > 0) a->nsub = v;
				break;
			}
			ln = nl ? nl + 1 : NULL;
		}
	}
	lossantos_buf_free(&b);
	a->left = a->nsub;
	a->ok = 1;
}

// riff header -> rate, frames
static int wav_info(const uint8_t *h, size_t n, uint32_t *rate, uint64_t *frames, size_t *hdr_len) {
	if (n < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
	size_t p = 12;
	uint16_t ch = 0, bits = 0;
	uint32_t r = 0;
	while (p + 8 <= n) {
		uint32_t sz;
		memcpy(&sz, h + p + 4, 4);
		if (!memcmp(h + p, "fmt ", 4) && p + 8 + 16 <= n) {
			memcpy(&ch, h + p + 10, 2);
			memcpy(&r, h + p + 12, 4);
			memcpy(&bits, h + p + 22, 2);
		} else if (!memcmp(h + p, "data", 4)) {
			uint32_t fs = (uint32_t)ch * ((bits + 7u) / 8u);
			if (!fs || !r) return 0;
			*rate = r;
			*frames = sz / fs;
			*hdr_len = p + 8;
			return 1;
		}
		p += 8 + sz + (sz & 1);
	}
	return 0;
}

static LONG64 clip_kb(const awc_job *a) { return (LONG64)(a->size / (uint64_t)a->nsub / 1024) + 1; }

// vgmstream | ffmpeg, the wav never hits the disk
static void encode_job(void *ctx, size_t oi) {
	rctx *c = (rctx *)ctx;
	clip_job *j = &c->clip[c->order[oi]];
	awc_job *a = &c->awc[j->awc];
	station *s = &c->st[a->st];
	wchar_t awc[1024], ogg[1024], cmd1[4096], cmd2[4096];
	awc_path(c, a, awc);
	swprintf(ogg, 1024, L"%ls\\%hs\\%hs", c->ogg_dir, s->name, j->rel);
	for (wchar_t *q = ogg; *q; q++)
		if (*q == L'/') *q = L'\\';
	{
		wchar_t d[1024];
		wcscpy(d, ogg);
		*wcsrchr(d, L'\\') = 0;
		mkdirs(d);
	}
	swprintf(cmd1, 4096, L"\"%ls\" -s %d -p \"%ls\"", c->o->vgmstream, j->sub, awc);
	swprintf(cmd2, 4096, L"\"%ls\" -y -loglevel error -f wav -i pipe:0 -c:a libvorbis -q:a 5 \"%ls\"", c->o->ffmpeg, ogg);
	HANDLE r1, w1, r2, w2;
	PROCESS_INFORMATION p1, p2;
	memset(&p1, 0, sizeof p1);
	memset(&p2, 0, sizeof p2);
	int ok = 0;
	uint64_t data_bytes = 0, frames = 0;
	uint32_t rate = 0;
	if (!make_pipe(&r1, &w1, 0)) goto out;
	if (!make_pipe(&r2, &w2, 1)) {
		CloseHandle(r1);
		CloseHandle(w1);
		goto out;
	}
	int s1 = spawn(&c->pe, cmd1, NULL, w1, NULL, &p1);
	CloseHandle(w1);
	int s2 = s1 && spawn(&c->pe, cmd2, r2, NULL, NULL, &p2);
	CloseHandle(r2);
	if (!s1 || !s2) {
		if (s1) TerminateProcess(p1.hProcess, 3), CloseHandle(p1.hProcess);
		CloseHandle(r1);
		CloseHandle(w2);
		rlog(c, "radio: cannot start the decoder/encoder (%lu)\n", GetLastError());
		goto out;
	}
	{
		// piping vgmstream into ffmpeg by hand. ugly as shit but no temp wavs
		uint8_t *buf = (uint8_t *)malloc(1 << 18);
		uint8_t head[4096];
		size_t hn = 0, hdr_len = 0;
		int have_hdr = 0;
		uint64_t total = 0;
		for (;;) {
			DWORD got = 0, put = 0;
			if (!ReadFile(r1, buf, 1 << 18, &got, NULL) || !got) break;
			if (!have_hdr && hn < sizeof head) {
				size_t k = got < sizeof head - hn ? got : sizeof head - hn;
				memcpy(head + hn, buf, k);
				hn += k;
				have_hdr = wav_info(head, hn, &rate, &frames, &hdr_len);
			}
			total += got;
			if (!WriteFile(w2, buf, got, &put, NULL) || put != got) break;
			if (c->o->cancel && *c->o->cancel) break;
		}
		free(buf);
		CloseHandle(r1);
		CloseHandle(w2);
		data_bytes = total > hdr_len ? total - hdr_len : 0;
		DWORD e1 = finish_proc(&c->pe, p1.hProcess), e2 = finish_proc(&c->pe, p2.hProcess);
		if (e1 != 0 || !have_hdr || !data_bytes) {
			rlog(c, "radio: %s/%s subsong %d: decode failed (exit %lu)\n", s->name, a->stem, j->sub, e1);
			DeleteFileW(ogg);
			goto out;
		}
		if (e2 != 0) {
			rlog(c, "radio: %s/%s subsong %d: encode failed (exit %lu)\n", s->name, a->stem, j->sub, e2);
			DeleteFileW(ogg);
			goto out;
		}
	}
	{
		lossantos_buf b = {0};
		if (read_all(ogg, &b) && b.n) {
			j->size = (uint32_t)b.n;
			j->crc = lossantos_crc32(0, b.p, b.n);
			j->seconds = round_nd((double)frames / (double)rate, 3);
			ok = 1;
		}
		lossantos_buf_free(&b);
	}
out:
	j->ok = ok;
	if (!ok) InterlockedIncrement(&c->failed);
	if (InterlockedDecrement(&a->left) == 0) DeleteFileW(awc);
	{
		LONG64 done = InterlockedAdd64(&c->kb_done, clip_kb(a));
		if (c->o->progress) c->o->progress(c->o->progress_ctx, "radio encode", (size_t)done, (size_t)c->kb_total);
	}
}

static int cmp_awc(const void *x, const void *y) {
	const awc_job *a = (const awc_job *)x, *b = (const awc_job *)y;
	if (a->st != b->st) return a->st - b->st;
	return strcmp(a->ename, b->ename);
}

static const clip_job *g_cl;
static const awc_job *g_aw;
static int cmp_cost(const void *x, const void *y) {
	const clip_job *a = &g_cl[*(const uint32_t *)x], *b = &g_cl[*(const uint32_t *)y];
	double ca = (double)g_aw[a->awc].size / g_aw[a->awc].nsub, cb = (double)g_aw[b->awc].size / g_aw[b->awc].nsub;
	if (ca != cb) return ca > cb ? -1 : 1;
	return *(const uint32_t *)x < *(const uint32_t *)y ? -1 : 1;
}

static int cmp_pfile(const void *x, const void *y) {
	const pfile *a = (const pfile *)x, *b = (const pfile *)y;
	if (a->group != b->group) return a->group - b->group;
	return strcmp(a->gpath, b->gpath);
}

static int match_re_id(const char *s, const char *prefix) {
	size_t pl = strlen(prefix);
	if (strncmp(s, prefix, pl)) return 0;
	s += pl;
	if (!*s) return 0;
	for (; *s; s++)
		if (*s < '0' || *s > '9') return 0;
	return 1;
}

// hud.ytd textures named *radio*
static int export_logos(rctx *c, pfile_vec *files, size_t *nlogos) {
	const lossantos_radio_opts *o = c->o;
	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\update\\update.rpf", o->gta_dir);
	lossantos_rpf up, sg;
	if (!lossantos_rpf_open(&up, o->ng, p)) return 0;
	int si = find_entry_ci(&up, "scaleform_generic.rpf");
	int ok = 0;
	if (si >= 0 && lossantos_rpf_open_child(&sg, &up, (uint32_t)si)) {
		int hi = find_entry_ci(&sg, "hud.ytd");
		lossantos_buf b = {0};
		if (hi >= 0 && lossantos_rpf_read(&sg, (uint32_t)hi, &b)) {
			lossantos_rsc r;
			lossantos_tex *tex = (lossantos_tex *)malloc(1024 * sizeof *tex);
			int n = 0;
			if (tex && lossantos_rsc_init(&r, b.p, b.n, lossantos_rpf_size_from_flags(sg.e[hi].sys_flags)))
				n = lossantos_ytd_parse(&r, LOSSANTOS_SYS_BASE, tex, 1024);
			wchar_t dir[1024];
			swprintf(dir, 1024, L"%ls\\_logos", c->ogg_dir);
			mkdirs(dir);
			lossantos_buf png = {0};
			for (int i = 0; i < n; i++) {
				char lo[128];
				lossantos_lower(lo, tex[i].name, sizeof lo);
				if (!strstr(lo, "radio")) continue;
				size_t cap = (size_t)tex[i].width * tex[i].height * 4;
				uint8_t *px = (uint8_t *)malloc(cap + 16);
				uint32_t w, h;
				if (px && lossantos_tex_decode(&tex[i], 0, px, cap, &w, &h) && png_write(px, w, h, &png)) {
					pfile f;
					memset(&f, 0, sizeof f);
					snprintf(f.gpath, sizeof f.gpath, "materials/lossantos_radio/%s.png", lo);
					swprintf(f.disk, 1024, L"%ls\\%hs.png", dir, lo);
					if (write_all(f.disk, png.p, png.n)) {
						f.size = (uint32_t)png.n;
						f.crc = lossantos_crc32(0, png.p, png.n);
						f.group = 0;
						lossantos_vec_push(*files, f);
						(*nlogos)++;
					}
				} else {
					rlog(c, "radio: logo %s: unsupported format 0x%x\n", tex[i].name, tex[i].format);
				}
				free(px);
			}
			lossantos_buf_free(&png);
			free(tex);
			ok = 1;
		}
		lossantos_buf_free(&b);
		lossantos_rpf_close(&sg);
	}
	lossantos_rpf_close(&up);
	return ok;
}

// names, kinds, titles, one manifest per station
static int write_manifests(rctx *c, lossantos_radio_stats *st) {
	const lossantos_radio_opts *o = c->o;
	gxt glob, tid;
	memset(&glob, 0, sizeof glob);
	memset(&tid, 0, sizeof tid);
	int have_names = 0;
	wchar_t p[1024];
	swprintf(p, 1024, L"%ls\\x64b.rpf", o->gta_dir);
	lossantos_rpf outer, lang;
	if (lossantos_rpf_open(&outer, o->ng, p)) {
		int li = find_entry_ci(&outer, "american_rel.rpf");
		if (li >= 0 && lossantos_rpf_open_child(&lang, &outer, (uint32_t)li)) {
			have_names = gxt_load(&lang, "global.gxt2", &glob) && gxt_load(&lang, "trackid.gxt2", &tid);
			lossantos_rpf_close(&lang);
		}
		lossantos_rpf_close(&outer);
	}
	if (!have_names) rlog(c, "radio: station names / titles unavailable (%s)\n", lossantos_error());
	LOSSANTOS_VEC(track) tracks = {0};
	if (have_names)
		for (int n = 0; n < 30000; n++) {
			// <n>S title, <n>A artist
			char ks[16], ka[16];
			snprintf(ks, sizeof ks, "%dS", n);
			snprintf(ka, sizeof ka, "%dA", n);
			const char *s = gxt_get(&tid, joaat_lower(ks)), *a = gxt_get(&tid, joaat_lower(ka));
			if (s && a && s[0] && a[0]) {
				track t;
				memset(&t, 0, sizeof t);
				t.n = n;
				t.s = s;
				t.a = a;
				variants(&t);
				lossantos_vec_push(tracks, t);
			}
		}
	rlog(c, "radio: trackid.gxt2: %zu title/artist pairs\n", tracks.n);

	size_t ai = 0;
	for (size_t si = 0; si < c->nst; si++) {
		station *s = &c->st[si];
		size_t a0 = ai;
		while (ai < c->nawc && c->awc[ai].st == (int)si) ai++;
		wchar_t md[1024];
		swprintf(md, 1024, L"%ls\\manifests\\%hs", o->out_dir, s->name);
		mkdirs(md);
		wcscat(md, L"\\manifest.json");
		FILE *f = _wfopen(md, L"wb");
		if (!f) return 0;
		fprintf(f, "{\n \"station\": ");
		jstr(f, s->name);
		fprintf(f, ",\n \"files\": {");
		LOSSANTOS_VEC(const char *) songs = {0};
		int any_id = 0, has_general = 0, any_mono = 0;
		size_t ci = 0;
		int first_file = 1;
		for (size_t k = a0; k < ai; k++) {
			const awc_job *a = &c->awc[k];
			if (!a->ok) continue;
			while (ci < c->nclip && c->clip[ci].awc < k) ci++;
			size_t c0 = ci;
			int nok = 0;
			double secs = 0;
			for (size_t q = c0; q < c->nclip && c->clip[q].awc == k; q++)
				if (c->clip[q].ok) nok++, secs = c->clip[q].seconds;
			fprintf(f, "%s\n  ", first_file ? "" : ",");
			first_file = 0;
			jstr(f, a->stem);
			if (!nok) {
				fprintf(f, ": []");
			} else {
				fprintf(f, ": [");
				int fi = 1;
				for (size_t q = c0; q < c->nclip && c->clip[q].awc == k; q++) {
					if (!c->clip[q].ok) continue;
					fprintf(f, "%s\n   {\n    \"path\": ", fi ? "" : ",");
					fi = 0;
					jstr(f, c->clip[q].rel);
					fprintf(f, ",\n    \"seconds\": ");
					jnum(f, c->clip[q].seconds, 3);
					fprintf(f, "\n   }");
				}
				fprintf(f, "\n  ]");
			}
			if (match_re_id(a->stem, "id_")) any_id = 1;
			if (!strcmp(a->stem, "general")) has_general = 1;
			if (!strncmp(a->stem, "mono_", 5) && strncmp(a->stem, "mono_solo", 9)) any_mono = 1;
			if (nok == 1 && secs > 60 && !match_re_id(a->stem, "id_") && !match_re_id(a->stem, "mono_solo_"))
				lossantos_vec_push(songs, a->stem);
		}
		fprintf(f, first_file ? "}" : "\n }");
		int shared = !strcmp(s->name, "radio_adverts") || !strcmp(s->name, "radio_news");
		if (!shared) {
			const char *name = have_names ? gxt_get(&glob, joaat_lower(s->name)) : NULL;
			if (name) {
				fprintf(f, ",\n \"name\": ");
				jstr(f, name);
			}
			// no id = mix, no general + mono = talk
			const char *kind = !any_id ? "mix" : (!has_general && any_mono) ? "talk" : "music";
			fprintf(f, ",\n \"kind\": \"%s\",\n \"titles\": {", kind);
			size_t nt = 0;
			if (!strcmp(kind, "music") && tracks.n) {
				LOSSANTOS_VEC(cand) cs = {0};
				char k[512];
				for (size_t q = 0; q < songs.n; q++) {
					// hardcoded title, this sucks but it works
					if (!strcmp(songs.p[q], "smokin_and_ridin")) {
						char want[256];
						norm("Smoke And Ride", want, sizeof want);
						for (size_t t = 0; t < tracks.n; t++)
							for (int v = 0; v < tracks.p[t].nvar; v++)
								if (!strcmp(tracks.p[t].var[v], want)) {
									cand cd = {1.0, (int)q, (int)t};
									lossantos_vec_push(cs, cd);
									break;
								}
						continue;
					}
					norm(songs.p[q], k, sizeof k);
					for (size_t t = 0; t < tracks.n; t++) {
						double best = -1;
						for (int v = 0; v < tracks.p[t].nvar; v++) {
							double r = score(k, tracks.p[t].var[v]);
							if (r > best) best = r;
						}
						if (best >= MATCH_MIN) {
							cand cd = {best, (int)q, (int)t};
							lossantos_vec_push(cs, cd);
						}
					}
				}
				g_songs = songs.p;
				g_tracks = tracks.p;
				qsort(cs.p, cs.n, sizeof *cs.p, cmp_cand);
				char *used_s = (char *)calloc(songs.n + 1, 1), *used_t = (char *)calloc(tracks.n + 1, 1);
				for (size_t q = 0; q < cs.n; q++) {
					if (used_s[cs.p[q].stem] || used_t[cs.p[q].track]) continue;
					used_s[cs.p[q].stem] = used_t[cs.p[q].track] = 1;
					const track *t = &tracks.p[cs.p[q].track];
					fprintf(f, "%s\n  ", nt ? "," : "");
					jstr(f, songs.p[cs.p[q].stem]);
					fprintf(f, ": {\n   \"id\": %d,\n   \"title\": ", t->n);
					jstr(f, t->s);
					fprintf(f, ",\n   \"artist\": ");
					jstr(f, t->a);
					fprintf(f, ",\n   \"score\": ");
					jnum(f, cs.p[q].r, 2);
					fprintf(f, "\n  }");
					nt++;
				}
				free(used_s);
				free(used_t);
				lossantos_vec_free(cs);
			} else if (!strcmp(kind, "talk")) {
				// talk shows, title comes from the stem
				static const char *const SMALL[] = {"a", "an", "of", "the", "and", "you", "to", "in", "on", "my", "her", "his", NULL};
				for (size_t q = 0; q < songs.n; q++) {
					const char *w0 = songs.p[q];
					if (!strncmp(w0, "mono_", 5)) w0 += 5;
					char words[256];
					snprintf(words, sizeof words, "%s", w0);
					char *us = strchr(words, '_');
					size_t l0 = us ? (size_t)(us - words) : strlen(words);
					int drop = 0;
					// stray leading letter on the first word
					if (l0 > 3) {
						for (size_t r2 = 0; r2 < songs.n && !drop; r2++) {
							if (r2 == q) continue;
							const char *o2 = songs.p[r2];
							if (!strncmp(o2, "mono_", 5)) o2 += 5;
							const char *e2 = strchr(o2, '_');
							size_t ol = e2 ? (size_t)(e2 - o2) : strlen(o2);
							if (ol == l0 - 1 && !strncmp(o2, words + 1, ol)) drop = 1;
						}
					}
					char title[512];
					size_t tl = 0;
					const char *wp = words + (drop ? 1 : 0);
					for (;;) {
						const char *e = strchr(wp, '_');
						size_t wl = e ? (size_t)(e - wp) : strlen(wp);
						char x[128];
						snprintf(x, sizeof x, "%.*s", (int)wl, wp);
						int alpha = x[0] != 0, small = 0;
						for (char *z = x; *z; z++)
							if (!((*z >= 'a' && *z <= 'z') || (*z >= 'A' && *z <= 'Z'))) alpha = 0;
						for (int m = 0; SMALL[m]; m++)
							if (!strcmp(x, SMALL[m])) small = 1;
						if (strlen(x) <= 3 && alpha && !small) {
							for (char *z = x; *z; z++) *z = (char)toupper((unsigned char)*z);
						} else {
							for (char *z = x; *z; z++) *z = (char)tolower((unsigned char)*z);
							x[0] = (char)toupper((unsigned char)x[0]);
						}
						tl += (size_t)snprintf(title + tl, sizeof title - tl, "%s%s", tl ? " " : "", x);
						if (!e) break;
						wp = e + 1;
					}
					fprintf(f, "%s\n  ", nt ? "," : "");
					jstr(f, songs.p[q]);
					fprintf(f, ": {\n   \"title\": ");
					jstr(f, title);
					fprintf(f, ",\n   \"show\": true\n  }");
					nt++;
				}
			}
			fprintf(f, nt ? "\n }" : "}");
			st->titled += nt;
			rlog(c, "radio: %-24s %-26s %s, titles %zu/%zu\n", s->name, name ? name : "(no label)", kind, nt, songs.n);
		}
		fprintf(f, "\n}");
		lossantos_vec_free(songs);
		if (fclose(f)) return 0;
	}
	for (size_t t = 0; t < tracks.n; t++) {
		for (int v = 0; v < tracks.p[t].nvar; v++) free(tracks.p[t].var[v]);
		free(tracks.p[t].var);
	}
	lossantos_vec_free(tracks);
	gxt_free(&glob);
	gxt_free(&tid);
	return 1;
}

static int cmp_station(const void *a, const void *b) { return strcmp(((const station *)a)->name, ((const station *)b)->name); }

int lossantos_build_radio(const lossantos_radio_opts *o, lossantos_radio_stats *st) {
	double t0 = lossantos_now();
	memset(st, 0, sizeof *st);
	rctx *c = (rctx *)calloc(1, sizeof *c);
	if (!c) return 0;
	c->o = o;
	InitializeCriticalSection(&c->lock);
	c->pe.background = o->background;
	c->pe.cancel = o->cancel;
	c->pe.job = CreateJobObjectW(NULL, NULL);
	if (c->pe.job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
		memset(&li, 0, sizeof li);
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(c->pe.job, JobObjectExtendedLimitInformation, &li, sizeof li);
	}
	int ok = 0;
	pfile_vec files = {0};
	lossantos_map_opts po;
	memset(&po, 0, sizeof po);
	po.jobs = o->jobs;
	po.background = o->background;
	po.progress = o->progress;
	po.progress_ctx = o->progress_ctx;
	po.cancel = o->cancel;

	swprintf(c->ogg_dir, 1024, L"%ls\\ogg", o->work_dir);
	mkdirs(c->ogg_dir);
	mkdirs(o->out_dir);

	// .awckey, each le word ^ xor, big endian
	uint8_t awckey[16];
	for (int i = 0; i < 4; i++) {
		uint32_t w;
		memcpy(&w, o->awc_key + 4 * i, 4);
		w ^= RS_XXTEA_XOR;
		awckey[4 * i] = (uint8_t)(w >> 24);
		awckey[4 * i + 1] = (uint8_t)(w >> 16);
		awckey[4 * i + 2] = (uint8_t)(w >> 8);
		awckey[4 * i + 3] = (uint8_t)w;
	}

	// stations
	wchar_t pat[1024];
	swprintf(pat, 1024, L"%ls\\x64\\audio\\sfx\\RADIO_*.rpf", o->gta_dir);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		lossantos_set_error("no radio archives under %ls\\x64\\audio\\sfx", o->gta_dir);
		goto done;
	}
	do {
		char nm[260];
		WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof nm, NULL, NULL);
		char *dot = strrchr(nm, '.');
		if (dot) *dot = 0;
		lossantos_lower(nm, nm, sizeof nm);
		if (o->only && !strstr(nm, o->only)) continue;
		c->st = (station *)realloc(c->st, (c->nst + 1) * sizeof *c->st);
		station *s = &c->st[c->nst++];
		memset(s, 0, sizeof *s);
		snprintf(s->name, sizeof s->name, "%s", nm);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	if (!c->nst) {
		lossantos_set_error("no radio stations matched");
		goto done;
	}
	qsort(c->st, c->nst, sizeof *c->st, cmp_station);
	for (size_t i = 0; i < c->nst; i++) {
		station *s = &c->st[i];
		wchar_t p[1024], up[64];
		int k = 0;
		for (; s->name[k] && k < 63; k++) up[k] = (wchar_t)toupper((unsigned char)s->name[k]);
		up[k] = 0;
		swprintf(p, 1024, L"%ls\\x64\\audio\\sfx\\%ls.rpf", o->gta_dir, up);
		if (!lossantos_rpf_open(&s->rpf, o->ng, p)) {
			rlog(c, "radio: %s: %s\n", s->name, lossantos_error());
			continue;
		}
		s->open = 1;
		swprintf(s->awc_dir, 1024, L"%ls\\awc\\%hs", o->work_dir, s->name);
		mkdirs(s->awc_dir);
		swprintf(p, 1024, L"%ls\\.awckey", s->awc_dir);
		write_all(p, awckey, 16);
		for (uint32_t e = 0; e < s->rpf.count; e++) {
			if (s->rpf.e[e].kind == LOSSANTOS_DIR || !lossantos_ends_with_ci(s->rpf.e[e].name, ".awc")) continue;
			c->awc = (awc_job *)realloc(c->awc, (c->nawc + 1) * sizeof *c->awc);
			awc_job *a = &c->awc[c->nawc++];
			memset(a, 0, sizeof *a);
			a->st = (int)i;
			a->entry = e;
			snprintf(a->ename, sizeof a->ename, "%s", s->rpf.e[e].name);
			lossantos_lower(a->stem, a->ename, sizeof a->stem);
			a->stem[strlen(a->stem) - 4] = 0;
			a->size = lossantos_rpf_file_size(&s->rpf, e);   /* resolved here, single-threaded */
		}
	}
	qsort(c->awc, c->nawc, sizeof *c->awc, cmp_awc);
	st->stations = c->nst;
	st->awc = c->nawc;

	// 1: extract + stream counts
	double t = lossantos_now();
	lossantos_run_pool(&po, "radio scan", c->nawc, extract_job, c);
	st->t_extract = lossantos_now() - t;
	if (o->cancel && *o->cancel) goto done;

	for (size_t i = 0; i < c->nawc; i++) {
		if (!c->awc[i].ok) continue;
		for (int s = 1; s <= c->awc[i].nsub; s++) {
			c->clip = (clip_job *)realloc(c->clip, (c->nclip + 1) * sizeof *c->clip);
			clip_job *j = &c->clip[c->nclip++];
			memset(j, 0, sizeof *j);
			j->awc = (uint32_t)i;
			j->sub = s;
			if (c->awc[i].nsub == 1) snprintf(j->rel, sizeof j->rel, "%s.ogg", c->awc[i].stem);
			else snprintf(j->rel, sizeof j->rel, "%s/%03d.ogg", c->awc[i].stem, s);
		}
	}
	// 2: encode every stream, biggest first
	c->order = (uint32_t *)malloc((c->nclip + 1) * sizeof *c->order);
	for (size_t i = 0; i < c->nclip; i++) c->order[i] = (uint32_t)i;
	g_cl = c->clip;
	g_aw = c->awc;
	qsort(c->order, c->nclip, sizeof *c->order, cmp_cost);
	for (size_t i = 0; i < c->nclip; i++) c->kb_total += clip_kb(&c->awc[c->clip[i].awc]);
	if (o->progress) o->progress(o->progress_ctx, "radio encode", 0, (size_t)c->kb_total);
	po.progress = NULL;       /* encode_job reports by audio size */
	t = lossantos_now();
	lossantos_run_pool(&po, "radio encode", c->nclip, encode_job, c);
	po.progress = o->progress;
	st->t_encode = lossantos_now() - t;
	if (o->cancel && *o->cancel) goto done;
	for (size_t i = 0; i < c->nclip; i++)
		if (c->clip[i].ok) {
			st->clips++;
			st->audio_seconds += c->clip[i].seconds;
		}
	st->failed = (size_t)c->failed;

	// 3: logos, names, titles, manifests
	t = lossantos_now();
	if (o->progress) o->progress(o->progress_ctx, "radio ui", 0, 1);
	if (!export_logos(c, &files, &st->logos)) rlog(c, "radio: logos unavailable (%s)\n", lossantos_error());
	if (!write_manifests(c, st)) {
		lossantos_set_error("cannot write the radio manifests");
		goto done;
	}
	if (o->progress) o->progress(o->progress_ctx, "radio ui", 1, 1);
	st->t_ui = lossantos_now() - t;

	// 4: pack, logos then one group per station
	t = lossantos_now();
	for (size_t i = 0; i < c->nclip; i++) {
		const clip_job *j = &c->clip[i];
		if (!j->ok) continue;
		const station *s = &c->st[c->awc[j->awc].st];
		pfile f;
		memset(&f, 0, sizeof f);
		snprintf(f.gpath, sizeof f.gpath, "sound/lossantos_radio/%s/%s", s->name, j->rel);
		swprintf(f.disk, 1024, L"%ls\\%hs\\%hs", c->ogg_dir, s->name, j->rel);
		for (wchar_t *q = f.disk; *q; q++)
			if (*q == L'/') *q = L'\\';
		f.size = j->size;
		f.crc = j->crc;
		f.group = 1 + c->awc[j->awc].st;
		lossantos_vec_push(files, f);
	}
	qsort(files.p, files.n, sizeof *files.p, cmp_pfile);
	uint64_t limit = o->limit ? o->limit : 1500ull << 20;
	wchar_t lst[1024];
	swprintf(lst, 1024, L"%ls\\chunks_radio.txt", o->out_dir);
	FILE *lf = _wfopen(lst, L"wb");
	int chunk = 1, bad = 0;
	for (size_t s0 = 0; s0 < files.n && !bad;) {
		size_t e0 = s0;
		uint64_t size = 0;
		while (e0 < files.n) {
			size_t ge = e0;
			uint64_t gs = 0;
			while (ge < files.n && files.p[ge].group == files.p[e0].group) gs += files.p[ge++].size;
			if (e0 > s0 && size + gs > limit) break;
			size += gs;
			e0 = ge;
		}
		size_t n = e0 - s0;
		lossantos_gma_file *gf = (lossantos_gma_file *)calloc(n, sizeof *gf);
		HANDLE *hs = (HANDLE *)calloc(n, sizeof *hs);
		for (size_t k = 0; k < n && !bad; k++) {
			const pfile *f = &files.p[s0 + k];
			snprintf(gf[k].path, sizeof gf[k].path, "%s", f->gpath);
			gf[k].spill = (uint32_t)k;
			gf[k].off = 0;
			gf[k].size = f->size;
			gf[k].crc = f->crc;
			hs[k] = CreateFileW(f->disk, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
			if (hs[k] == INVALID_HANDLE_VALUE) {
				hs[k] = NULL;
				lossantos_set_error("cannot reopen %ls", f->disk);
				bad = 1;
			}
		}
		if (!bad) {
			char name[64], title[128];
			snprintf(name, sizeof name, "lossantos_content_radio_%02d", chunk++);
			snprintf(title, sizeof title, "GTAV converted content (%s)", name);
			wchar_t out[1024];
			swprintf(out, 1024, L"%ls\\%hs.gma", o->out_dir, name);
			uint64_t b = lossantos_gma_write(out, title, gf, n, (void *const *)hs);
			if (!b) bad = 1;
			st->bytes += b;
			st->chunks++;
			if (lf) fprintf(lf, "%s.gma\n", name);
		}
		for (size_t k = 0; k < n; k++)
			if (hs[k]) CloseHandle(hs[k]);
		free(gf);
		free(hs);
		s0 = e0;
	}
	if (lf) fclose(lf);
	st->t_pack = lossantos_now() - t;
	ok = !bad;

done:
	for (size_t i = 0; i < c->nst; i++)
		if (c->st[i].open) lossantos_rpf_close(&c->st[i].rpf);
	if (c->pe.job) CloseHandle(c->pe.job);
	rmtree(o->work_dir);
	lossantos_vec_free(files);
	free(c->st);
	free(c->awc);
	free(c->clip);
	free(c->order);
	DeleteCriticalSection(&c->lock);
	free(c);
	st->t_total = lossantos_now() - t0;
	return ok;
}
