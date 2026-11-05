#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gtav_prefetch.h"

#define PF_QCAP     32768u          /* ring slots (power of two) */
#define PF_NAME     256
#define PF_SEEN_CAP (1u << 18)      /* hash set slots (power of two) */
#define PF_READ_BUF (256 * 1024)    /* worker read scratch */

static struct {
	CRITICAL_SECTION lock;
	HANDLE           ev;
	HANDLE           thread;
	volatile LONG    stop;
	int              running;
	char             root[MAX_PATH];  /* ...\materials\ (trailing slash) */
	char           (*q)[PF_NAME];
	unsigned         head, tail;      /* ring: head = next to take */
	unsigned long long *seen;         /* 0 = empty slot */
	unsigned         seenN;
	// stats, written under the lock
	long long        queued, done, files, missing, dropped;
	double           ms;
} P;

// fnv style, lowercase and forward slashes
static unsigned long long pf_hash(const char *s) {
	unsigned long long h = 1469598103934665603ull;
	for (; *s; ++s) {
		unsigned char c = (unsigned char)*s;
		if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
		if (c == '\\') c = '/';
		h ^= c;
		h *= 1099511628211ull;
	}
	return h ? h : 1;
}

static int pf_seen_insert(unsigned long long h) {
	unsigned mask = PF_SEEN_CAP - 1u;
	unsigned i;
	// 3/4 full, stop deduping
	if (P.seenN >= (PF_SEEN_CAP / 4u) * 3u) return 1;
	i = (unsigned)h & mask;
	for (;;) {
		if (P.seen[i] == 0) {
			P.seen[i] = h;
			P.seenN++;
			return 1;
		}
		if (P.seen[i] == h) return 0;
		i = (i + 1u) & mask;
	}
}

static long long pf_read(const char *path, char *keep, size_t keepCap, size_t *keepLen) {
	static char buf[PF_READ_BUF];
	HANDLE f;
	long long total = 0;
	if (keepLen) *keepLen = 0;
	f = CreateFileA(path, GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (f == INVALID_HANDLE_VALUE) return -1;
	for (;;) {
		DWORD got = 0;
		if (!ReadFile(f, buf, (DWORD)sizeof(buf), &got, NULL) || got == 0) break;
		if (keep && keepLen && *keepLen < keepCap) {
			size_t n = keepCap - *keepLen;
			if (n > got) n = got;
			memcpy(keep + *keepLen, buf, n);
			*keepLen += n;
		}
		total += got;
	}
	CloseHandle(f);
	return total;
}

static int pf_path(char *out, size_t cap, const char *rel, const char *ext) {
	size_t r = strlen(P.root), n = strlen(rel), e = strlen(ext), i;
	if (r + n + e + 1 > cap) return 0;
	memcpy(out, P.root, r);
	for (i = 0; i < n; ++i) out[r + i] = (rel[i] == '/') ? '\\' : rel[i];
	memcpy(out + r + n, ext, e + 1);
	return 1;
}

static int pf_basetexture(const char *vmt, size_t len, char *out, size_t cap) {
	static const char key[] = "$basetexture";
	size_t klen = sizeof(key) - 1, i;
	for (i = 0; i + klen < len; ++i) {
		size_t j, o = 0;
		if (_strnicmp(vmt + i, key, klen) != 0) continue;
		j = i + klen;
		// not $basetexture2 or $basetexturetransform
		if (j < len && vmt[j] != '"' && vmt[j] != ' ' && vmt[j] != '\t') continue;
		if (j < len && vmt[j] == '"') ++j;               /* closing quote of key */
		while (j < len && (vmt[j] == ' ' || vmt[j] == '\t')) ++j;
		if (j < len && vmt[j] == '"') ++j;
		while (j < len && vmt[j] != '"' && vmt[j] != '\r' && vmt[j] != '\n'
			   && vmt[j] != ' ' && vmt[j] != '\t' && o + 1 < cap) {
			out[o++] = vmt[j++];
		}
		out[o] = '\0';
		return o > 0;
	}
	return 0;
}

static void pf_one(const char *name) {
	static char path[MAX_PATH * 2];
	static char vmt[4096];
	char base[PF_NAME];
	size_t vlen = 0;
	long long got = -1, files = 0;
	LARGE_INTEGER f, t0, t1;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);

	if (pf_path(path, sizeof(path), name, ".vmt"))
		got = pf_read(path, vmt, sizeof(vmt), &vlen);
	if (got < 0 && _strnicmp(name, "models/", 7) != 0) {
		char alt[PF_NAME + 16];
		if (strlen(name) + 13 < sizeof(alt)) {
			memcpy(alt, "models/gtav/", 12);
			strcpy(alt + 12, name);
			if (pf_path(path, sizeof(path), alt, ".vmt"))
				got = pf_read(path, vmt, sizeof(vmt), &vlen);
		}
	}
	if (got >= 0) {
		files++;
		if (pf_basetexture(vmt, vlen, base, sizeof(base))
			&& pf_path(path, sizeof(path), base, ".vtf")
			&& pf_read(path, NULL, 0, NULL) >= 0) {
			files++;
		}
	}

	QueryPerformanceCounter(&t1);
	EnterCriticalSection(&P.lock);
	P.done++;
	P.files += files;
	if (got < 0) P.missing++;
	P.ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
	LeaveCriticalSection(&P.lock);
}

static DWORD WINAPI pf_worker(LPVOID arg) {
	char name[PF_NAME];
	(void)arg;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	while (!P.stop) {
		int have = 0;
		EnterCriticalSection(&P.lock);
		if (P.head != P.tail) {
			memcpy(name, P.q[P.head & (PF_QCAP - 1u)], PF_NAME);
			P.head++;
			have = 1;
		}
		LeaveCriticalSection(&P.lock);
		if (!have) {
			WaitForSingleObject(P.ev, 250);
			continue;
		}
		pf_one(name);
	}
	return 0;
}

int gtav_prefetch_init(const char *addon, char *err, size_t errlen) {
	HMODULE self = NULL;
	char dll[MAX_PATH];
	char *cut;
	int ups;
	DWORD attr;

	if (P.running) return 1;
	if (!addon || !*addon) addon = "lossantos";

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							(LPCSTR)(void *)&pf_worker, &self)
		|| !GetModuleFileNameA(self, dll, (DWORD)sizeof(dll))) {
		_snprintf(err, errlen, "prefetch: cannot locate own DLL (%lu)", GetLastError());
		return 0;
	}
	for (ups = 0; ups < 3; ++ups) {
		cut = strrchr(dll, '\\');
		if (!cut) {
			_snprintf(err, errlen, "prefetch: unexpected DLL path %s", dll);
			return 0;
		}
		*cut = '\0';
	}
	if (_snprintf(P.root, sizeof(P.root), "%s\\addons\\%s\\materials\\", dll, addon) < 0) {
		_snprintf(err, errlen, "prefetch: path too long");
		return 0;
	}
	attr = GetFileAttributesA(P.root);
	if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		_snprintf(err, errlen, "prefetch: no materials dir at %s", P.root);
		P.root[0] = '\0';
		return 0;
	}

	P.q = (char (*)[PF_NAME])calloc(PF_QCAP, PF_NAME);
	P.seen = (unsigned long long *)calloc(PF_SEEN_CAP, sizeof(unsigned long long));
	if (!P.q || !P.seen) {
		free(P.q);
		free(P.seen);
		P.q = NULL;
		P.seen = NULL;
		_snprintf(err, errlen, "prefetch: out of memory");
		return 0;
	}
	InitializeCriticalSection(&P.lock);
	P.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
	P.stop = 0;
	P.head = P.tail = 0;
	P.seenN = 0;
	P.queued = P.done = P.files = P.missing = P.dropped = 0;
	P.ms = 0.0;
	P.thread = CreateThread(NULL, 0, pf_worker, NULL, 0, NULL);
	if (!P.ev || !P.thread) {
		if (P.ev) CloseHandle(P.ev);
		DeleteCriticalSection(&P.lock);
		free(P.q);
		free(P.seen);
		P.q = NULL;
		P.seen = NULL;
		_snprintf(err, errlen, "prefetch: thread start failed (%lu)", GetLastError());
		return 0;
	}
	P.running = 1;
	return 1;
}

int gtav_prefetch_add(const char *name) {
	size_t n;
	int ok = 0;
	if (!P.running || !name) return 0;
	n = strlen(name);
	if (n == 0 || n >= PF_NAME) return 0;
	EnterCriticalSection(&P.lock);
	if (pf_seen_insert(pf_hash(name))) {
		if (P.tail - P.head < PF_QCAP) {
			memcpy(P.q[P.tail & (PF_QCAP - 1u)], name, n + 1);
			P.tail++;
			P.queued++;
			ok = 1;
		} else {
			P.dropped++;
		}
	}
	LeaveCriticalSection(&P.lock);
	if (ok) SetEvent(P.ev);
	return ok;
}

void gtav_prefetch_get_stats(gtav_prefetch_stats *out) {
	memset(out, 0, sizeof(*out));
	if (!P.running) return;
	EnterCriticalSection(&P.lock);
	out->queued = P.queued;
	out->done = P.done;
	out->files = P.files;
	out->missing = P.missing;
	out->dropped = P.dropped;
	out->pending = (long long)(P.tail - P.head);
	out->ms = P.ms;
	LeaveCriticalSection(&P.lock);
}

const char *gtav_prefetch_root(void) {
	return P.running ? P.root : "";
}

void gtav_prefetch_shutdown(void) {
	if (!P.running) return;
	InterlockedExchange(&P.stop, 1);
	SetEvent(P.ev);
	if (WaitForSingleObject(P.thread, 3000) == WAIT_OBJECT_0) {
		CloseHandle(P.thread);
		CloseHandle(P.ev);
		DeleteCriticalSection(&P.lock);
		free(P.q);
		free(P.seen);
		P.q = NULL;
		P.seen = NULL;
	}
	P.running = 0;
}
