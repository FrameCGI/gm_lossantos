#include "lossantos_keys.h"
#include "lossantos_common.h"
#include "lossantos_inflate.h"
#include "lossantos_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wininet.lib")

// sha1 of the PC aes key
static const uint8_t AES_SHA1[20] = {0xA0, 0x79, 0x61, 0x28, 0xA7, 0x75, 0x72, 0x0A, 0xC2, 0x04,
									 0xD9, 0x81, 0x9F, 0x68, 0xC1, 0x72, 0xE3, 0x95, 0x2C, 0x6D};
#define MAGIC_URL "https://raw.githubusercontent.com/dexyfex/CodeWalker/master/CodeWalker.Core/Resources/magic.dat"

int lossantos_keys_find_aes(const wchar_t *exe, uint8_t key[32]) {
	FILE *f = _wfopen(exe, L"rb");
	if (!f) {
		lossantos_set_error("cannot open %ls", exe);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = (uint8_t *)malloc((size_t)n);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		lossantos_set_error("cannot read %ls", exe);
		return 0;
	}
	fclose(f);
	int found = 0;
	// aligned windows first, then every offset
	for (int pass = 0; pass < 2 && !found; pass++) {
		long step = pass == 0 ? 8 : 1;
		for (long i = 0; i + 32 <= n && !found; i += step) {
			if (pass == 1 && (i % 8) == 0) continue;
			uint8_t h[20];
			lossantos_sha1(b + i, 32, h);
			if (memcmp(h, AES_SHA1, 20) == 0) {
				memcpy(key, b + i, 32);
				found = 1;
			}
		}
	}
	free(b);
	if (!found) lossantos_set_error("no AES key in %ls (not a PC GTA5.exe?)", exe);
	return found;
}

// .NET System.Random
typedef struct {
	int32_t sa[56];
	int inext, inextp;
} netrand;

static int32_t wrap(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }

static void nr_init(netrand *r, int32_t seed) {
	const int32_t MBIG = 2147483647, MSEED = 161803398;
	int32_t sub = seed == INT32_MIN ? MBIG : (seed < 0 ? -seed : seed);
	int32_t mj = MSEED - sub, mk = 1;
	memset(r->sa, 0, sizeof r->sa);
	r->sa[55] = mj;
	for (int i = 1; i < 55; i++) {
		int ii = (21 * i) % 55;
		r->sa[ii] = mk;
		mk = wrap((int64_t)mj - mk);
		if (mk < 0) mk += MBIG;
		mj = r->sa[ii];
	}
	for (int k = 0; k < 4; k++)
		for (int i = 1; i < 56; i++) {
			r->sa[i] = wrap((int64_t)r->sa[i] - r->sa[1 + (i + 30) % 55]);
			if (r->sa[i] < 0) r->sa[i] += MBIG;
		}
	r->inext = 0;
	r->inextp = 21;
}

static int32_t nr_sample(netrand *r) {
	const int32_t MBIG = 2147483647;
	r->inext = r->inext + 1 >= 56 ? 1 : r->inext + 1;
	r->inextp = r->inextp + 1 >= 56 ? 1 : r->inextp + 1;
	int32_t v = wrap((int64_t)r->sa[r->inext] - r->sa[r->inextp]);
	if (v == MBIG) v--;
	if (v < 0) v += MBIG;
	r->sa[r->inext] = v;
	return v;
}

static uint32_t joaat_raw(const uint8_t *p, size_t n) {
	uint32_t h = 0;
	for (size_t i = 0; i < n; i++) {
		h += p[i];
		h += h << 10;
		h ^= h >> 6;
	}
	h += h << 3;
	h ^= h >> 11;
	h += h << 15;
	return h;
}

int lossantos_keys_unlock(const uint8_t *magic, size_t n, const uint8_t aes[32], lossantos_ng *ng, uint8_t awc[16]) {
	uint8_t *db = (uint8_t *)malloc(n ? n : 1);
	if (!db) return 0;
	memcpy(db, magic, n);
	netrand r;
	nr_init(&r, (int32_t)joaat_raw(aes, 32));
	// magic minus 4 rounds of NextBytes
	for (int k = 0; k < 4; k++)
		for (size_t i = 0; i < n; i++) db[i] = (uint8_t)(db[i] - (uint8_t)(nr_sample(&r) & 0xFF));
	// AES-256-ECB over whole blocks
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_KEY_HANDLE key = NULL;
	int ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) == 0 &&
			 BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == 0 &&
			 BCryptGenerateSymmetricKey(alg, &key, NULL, 0, (PUCHAR)aes, 32, 0) == 0;
	ULONG got = 0;
	size_t full = n - n % 16;
	if (ok && full) ok = BCryptDecrypt(key, db, (ULONG)full, NULL, NULL, 0, db, (ULONG)full, &got, 0) == 0;
	if (key) BCryptDestroyKey(key);
	if (alg) BCryptCloseAlgorithmProvider(alg, 0);
	lossantos_buf out = {0};
	if (ok) ok = lossantos_inflate(db, n, &out, 400000);
	free(db);
	const size_t SZ[4] = {27472, 278528, 256, 16};  // ng keys, ng tables, hash lut, awc key
	if (ok && out.n < SZ[0] + SZ[1] + SZ[2] + SZ[3]) {
		lossantos_set_error("unlocked key blob too short (%zu bytes): wrong AES key or magic.dat", out.n);
		ok = 0;
	}
	if (ok) {
		memcpy(ng->keys, out.p, SZ[0]);
		memcpy(ng->tables, out.p + SZ[0], SZ[1]);
		memcpy(ng->lut, out.p + SZ[0] + SZ[1], SZ[2]);
		memcpy(awc, out.p + SZ[0] + SZ[1] + SZ[2], SZ[3]);
		memcpy(ng->aes, aes, 32);
		ng->has_aes = 1;
	} else if (!lossantos_error()[0]) {
		lossantos_set_error("magic.dat could not be unlocked");
	}
	lossantos_buf_free(&out);
	return ok;
}

size_t lossantos_keys_fetch_magic(uint8_t **out) {
	*out = NULL;
	HINTERNET net = InternetOpenA("lossantos_install", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if (!net) return 0;
	HINTERNET url = InternetOpenUrlA(net, MAGIC_URL, NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
	size_t n = 0, cap = 0;
	uint8_t *b = NULL;
	if (url) {
		for (;;) {
			if (n + 65536 > cap) {
				cap = (n + 65536) * 2;
				uint8_t *nb = (uint8_t *)realloc(b, cap);
				if (!nb) break;
				b = nb;
			}
			DWORD got = 0;
			if (!InternetReadFile(url, b + n, 65536, &got) || !got) break;
			n += got;
		}
		InternetCloseHandle(url);
	}
	InternetCloseHandle(net);
	if (!n) {
		free(b);
		lossantos_set_error("could not download magic.dat from the CodeWalker repository");
		return 0;
	}
	*out = b;
	return n;
}

static int write_file(const wchar_t *dir, const wchar_t *name, const void *p, size_t n) {
	wchar_t path[1024];
	swprintf(path, 1024, L"%ls\\%ls", dir, name);
	FILE *f = _wfopen(path, L"wb");
	if (!f) return 0;
	int ok = fwrite(p, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

int lossantos_keys_write(const wchar_t *dir, const lossantos_ng *ng, const uint8_t aes[32], const uint8_t awc[16]) {
	CreateDirectoryW(dir, NULL);
	return write_file(dir, L"gtav_ng_key.dat", ng->keys, sizeof ng->keys) &&
		   write_file(dir, L"gtav_ng_decrypt_tables.dat", ng->tables, sizeof ng->tables) &&
		   write_file(dir, L"gtav_hash_lut.dat", ng->lut, sizeof ng->lut) &&
		   write_file(dir, L"gtav_aes_key.dat", aes, 32) && write_file(dir, L"gtav_awc_key.dat", awc, 16);
}
