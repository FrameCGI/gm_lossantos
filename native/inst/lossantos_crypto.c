#include "lossantos_crypto.h"
#include "lossantos_common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "bcrypt.lib")

int lossantos_aes_decrypt(const uint8_t key[32], uint8_t *data, size_t len) {
	size_t full = len - len % 16;
	if (!full) return 1;
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_KEY_HANDLE k = NULL;
	ULONG got = 0;
	int ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) == 0 &&
			 BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == 0 &&
			 BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key, 32, 0) == 0 &&
			 BCryptDecrypt(k, data, (ULONG)full, NULL, NULL, 0, data, (ULONG)full, &got, 0) == 0;
	if (k) BCryptDestroyKey(k);
	if (alg) BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

static int read_exact(const wchar_t *dir, const wchar_t *name, void *dst, size_t len) {
	wchar_t path[1024];
	swprintf(path, 1024, L"%ls\\%ls", dir, name);
	FILE *f = _wfopen(path, L"rb");
	if (!f) {
		lossantos_set_error("missing key file %ls", path);
		return 0;
	}
	size_t got = fread(dst, 1, len, f);
	int extra = fgetc(f) != EOF;
	fclose(f);
	if (got != len || extra) {
		lossantos_set_error("key file %ls: expected %zu bytes", path, len);
		return 0;
	}
	return 1;
}

int lossantos_ng_load(lossantos_ng *ng, const wchar_t *dir) {
	int ok = read_exact(dir, L"gtav_ng_key.dat", ng->keys, sizeof ng->keys) &&
			 read_exact(dir, L"gtav_ng_decrypt_tables.dat", ng->tables, sizeof ng->tables) &&
			 read_exact(dir, L"gtav_hash_lut.dat", ng->lut, sizeof ng->lut);
	// aes key is optional, dont let it clobber the error
	char keep[512];
	snprintf(keep, sizeof keep, "%s", lossantos_error());
	ng->has_aes = ok && read_exact(dir, L"gtav_aes_key.dat", ng->aes, sizeof ng->aes);
	if (ok) lossantos_set_error("%s", keep);
	return ok;
}

uint32_t lossantos_name_hash(const lossantos_ng *ng, const char *name) {
	uint32_t result = 0;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		uint32_t temp = 1025u * (ng->lut[*p] + result);
		result = (temp >> 6) ^ temp;
	}
	return 32769u * (((9u * result) >> 11) ^ (9u * result));
}

const uint8_t *lossantos_key_for(const lossantos_ng *ng, const char *name, uint32_t size) {
	uint32_t h = lossantos_name_hash(ng, name);
	return ng->keys[(uint32_t)((uint64_t)h + size + 61u) % LOSSANTOS_NG_KEYS];
}

// rounds 0, 1 and 16 use A, rest use B
static const uint8_t PERM_A[4][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}, {12, 13, 14, 15}};
static const uint8_t PERM_B[4][4] = {{0, 7, 10, 13}, {1, 4, 11, 14}, {2, 5, 8, 15}, {3, 6, 9, 12}};

void lossantos_ng_decrypt(const lossantos_ng *ng, uint8_t *data, size_t len, const uint8_t *key) {
	uint32_t sk[LOSSANTOS_NG_ROUNDS][4];
	memcpy(sk, key, sizeof sk);
	size_t blocks = len / 16;
	for (size_t bi = 0; bi < blocks; bi++) {
		uint8_t b[16];
		memcpy(b, data + bi * 16, 16);
		for (int r = 0; r < LOSSANTOS_NG_ROUNDS; r++) {
			const uint8_t (*perm)[4] = (r == 0 || r == 1 || r == LOSSANTOS_NG_ROUNDS - 1) ? PERM_A : PERM_B;
			const uint32_t (*t)[256] = ng->tables[r];
			uint32_t w[4];
			for (int k = 0; k < 4; k++) {
				const uint8_t *pp = perm[k];
				w[k] = t[pp[0]][b[pp[0]]] ^ t[pp[1]][b[pp[1]]] ^
					   t[pp[2]][b[pp[2]]] ^ t[pp[3]][b[pp[3]]] ^ sk[r][k];
			}
			memcpy(b, w, 16);
		}
		memcpy(data + bi * 16, b, 16);
	}
}

int lossantos_toc_shaped(const uint8_t *head, uint32_t entry_count) {
	// looks like a root dir entry
	uint32_t w[4];
	memcpy(w, head, 16);
	return w[1] == 0x7FFFFF00u && w[0] == 0 && w[2] >= 1 && w[2] <= entry_count &&
		   w[3] > 0 && w[3] <= entry_count;
}
