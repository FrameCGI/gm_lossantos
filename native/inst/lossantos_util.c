#include "lossantos_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
	uint32_t w[80];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
		else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
		else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
		uint32_t t = rol(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void lossantos_sha1(const void *data, size_t len, uint8_t out[20]) {
	uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
	const uint8_t *p = (const uint8_t *)data;
	size_t full = len / 64;
	for (size_t i = 0; i < full; i++) sha1_block(h, p + i * 64);
	uint8_t tail[128];
	size_t rem = len - full * 64;
	memcpy(tail, p + full * 64, rem);
	tail[rem] = 0x80;
	size_t tl = rem + 1 <= 56 ? 64 : 128;
	memset(tail + rem + 1, 0, tl - rem - 1);
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
	sha1_block(h, tail);
	if (tl == 128) sha1_block(h, tail + 64);
	for (int i = 0; i < 5; i++) {
		out[i * 4] = (uint8_t)(h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)h[i];
	}
}

void lossantos_sha1_hex(const void *data, size_t len, char out[41]) {
	uint8_t d[20];
	lossantos_sha1(data, len, d);
	for (int i = 0; i < 20; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}


void lossantos_sort_u64(uint64_t *a, size_t n, uint64_t *tmp) {
	if (n < 2) return;
	if (n < 64) {
		for (size_t i = 1; i < n; i++) {
			uint64_t v = a[i];
			size_t j = i;
			while (j && a[j - 1] > v) { a[j] = a[j - 1]; j--; }
			a[j] = v;
		}
		return;
	}
	size_t *cnt = (size_t *)malloc(65536 * sizeof *cnt);
	// no memory, insertion sort instead
	if (!cnt) {
		for (size_t i = 1; i < n; i++) {
			uint64_t v = a[i];
			size_t j = i;
			while (j && a[j - 1] > v) { a[j] = a[j - 1]; j--; }
			a[j] = v;
		}
		return;
	}
	uint64_t *src = a, *dst = tmp;
	uint64_t lo_all = ~0ull, hi_all = 0;
	for (size_t i = 0; i < n; i++) { lo_all &= a[i]; hi_all |= a[i]; }
	// lsd radix, 16 bit digits, constant ones skipped
	uint64_t varying = lo_all ^ hi_all;
	for (int pass = 0; pass < 4; pass++) {
		int sh = pass * 16;
		if (((varying >> sh) & 0xFFFF) == 0) continue;
		memset(cnt, 0, 65536 * sizeof *cnt);
		for (size_t i = 0; i < n; i++) cnt[(src[i] >> sh) & 0xFFFF]++;
		size_t sum = 0;
		for (size_t d = 0; d < 65536; d++) { size_t c = cnt[d]; cnt[d] = sum; sum += c; }
		for (size_t i = 0; i < n; i++) dst[cnt[(src[i] >> sh) & 0xFFFF]++] = src[i];
		uint64_t *t = src; src = dst; dst = t;
	}
	if (src != a) memcpy(a, src, n * sizeof *a);
	free(cnt);
}

size_t lossantos_sort_unique_u64(uint64_t *a, size_t n, uint64_t *tmp) {
	if (!n) return 0;
	lossantos_sort_u64(a, n, tmp);
	size_t k = 1;
	for (size_t i = 1; i < n; i++)
		if (a[i] != a[k - 1]) a[k++] = a[i];
	return k;
}

size_t lossantos_lower_bound_u64(const uint64_t *a, size_t n, uint64_t v) {
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (a[mid] < v) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}


static size_t pow2_at_least(size_t n) {
	size_t c = 16;
	while (c < n) c <<= 1;
	return c;
}

int lossantos_hset_init(lossantos_hset *s, size_t expect) {
	memset(s, 0, sizeof *s);
	s->cap = pow2_at_least(expect * 2 + 16);
	s->k = (uint64_t *)calloc(s->cap, sizeof *s->k);
	return s->k != NULL;
}

void lossantos_hset_free(lossantos_hset *s) {
	free(s->k);
	memset(s, 0, sizeof *s);
}

static int hset_grow(lossantos_hset *s) {
	size_t nc = s->cap * 2;
	uint64_t *nk = (uint64_t *)calloc(nc, sizeof *nk);
	if (!nk) return 0;
	for (size_t i = 0; i < s->cap; i++) {
		uint64_t k = s->k[i];
		if (!k) continue;
		size_t j = lossantos_mix64(k) & (nc - 1);
		while (nk[j]) j = (j + 1) & (nc - 1);
		nk[j] = k;
	}
	free(s->k);
	s->k = nk;
	s->cap = nc;
	return 1;
}

int lossantos_hset_add(lossantos_hset *s, uint64_t key) {
	if (!key) {
		if (s->has_zero) return 0;
		s->has_zero = 1;
		s->n++;
		return 1;
	}
	if ((s->n + 1) * 2 > s->cap && !hset_grow(s)) return -1;
	size_t j = lossantos_mix64(key) & (s->cap - 1);
	while (s->k[j]) {
		if (s->k[j] == key) return 0;
		j = (j + 1) & (s->cap - 1);
	}
	s->k[j] = key;
	s->n++;
	return 1;
}

int lossantos_hset_has(const lossantos_hset *s, uint64_t key) {
	if (!key) return s->has_zero;
	size_t j = lossantos_mix64(key) & (s->cap - 1);
	while (s->k[j]) {
		if (s->k[j] == key) return 1;
		j = (j + 1) & (s->cap - 1);
	}
	return 0;
}


int lossantos_hmap_init(lossantos_hmap *m, size_t expect) {
	memset(m, 0, sizeof *m);
	m->cap = pow2_at_least(expect * 2 + 16);
	m->k = (uint64_t *)malloc(m->cap * sizeof *m->k);
	m->v = (uint32_t *)malloc(m->cap * sizeof *m->v);
	m->used = (uint8_t *)calloc(m->cap, 1);
	return m->k && m->v && m->used;
}

void lossantos_hmap_free(lossantos_hmap *m) {
	free(m->k);
	free(m->v);
	free(m->used);
	memset(m, 0, sizeof *m);
}

static int hmap_grow(lossantos_hmap *m) {
	size_t nc = m->cap * 2;
	uint64_t *nk = (uint64_t *)malloc(nc * sizeof *nk);
	uint32_t *nv = (uint32_t *)malloc(nc * sizeof *nv);
	uint8_t *nu = (uint8_t *)calloc(nc, 1);
	if (!nk || !nv || !nu) {
		free(nk); free(nv); free(nu);
		return 0;
	}
	for (size_t i = 0; i < m->cap; i++) {
		if (!m->used[i]) continue;
		size_t j = lossantos_mix64(m->k[i]) & (nc - 1);
		while (nu[j]) j = (j + 1) & (nc - 1);
		nu[j] = 1;
		nk[j] = m->k[i];
		nv[j] = m->v[i];
	}
	free(m->k); free(m->v); free(m->used);
	m->k = nk; m->v = nv; m->used = nu;
	m->cap = nc;
	return 1;
}

uint32_t *lossantos_hmap_put(lossantos_hmap *m, uint64_t key, int *found) {
	if ((m->n + 1) * 2 > m->cap && !hmap_grow(m)) return NULL;
	size_t j = lossantos_mix64(key) & (m->cap - 1);
	while (m->used[j]) {
		if (m->k[j] == key) {
			*found = 1;
			return &m->v[j];
		}
		j = (j + 1) & (m->cap - 1);
	}
	m->used[j] = 1;
	m->k[j] = key;
	m->v[j] = 0;
	m->n++;
	*found = 0;
	return &m->v[j];
}

const uint32_t *lossantos_hmap_get(const lossantos_hmap *m, uint64_t key) {
	size_t j = lossantos_mix64(key) & (m->cap - 1);
	while (m->used[j]) {
		if (m->k[j] == key) return &m->v[j];
		j = (j + 1) & (m->cap - 1);
	}
	return NULL;
}

uint64_t lossantos_hash_bytes(const void *p, size_t n) {
	const uint8_t *b = (const uint8_t *)p;
	// fnv-1a, the basis is a digit short, whatever
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return lossantos_mix64(h);
}


int lossantos_strtab_init(lossantos_strtab *t) {
	memset(t, 0, sizeof *t);
	return lossantos_hmap_init(&t->map, 256);
}

void lossantos_strtab_free(lossantos_strtab *t) {
	for (uint32_t i = 0; i < t->n; i++) free(t->s[i]);
	free(t->s);
	lossantos_hmap_free(&t->map);
	memset(t, 0, sizeof *t);
}

int64_t lossantos_strtab_find(const lossantos_strtab *t, const char *s) {
	uint64_t h = lossantos_hash_bytes(s, strlen(s));
	for (;;) {
		const uint32_t *v = lossantos_hmap_get(&t->map, h);
		if (!v) return -1;
		if (strcmp(t->s[*v], s) == 0) return *v;
		// hash collision, rehash
		h = lossantos_mix64(h + 1);
	}
}

uint32_t lossantos_strtab_id(lossantos_strtab *t, const char *s) {
	uint64_t h = lossantos_hash_bytes(s, strlen(s));
	for (;;) {
		int found;
		uint32_t *v = lossantos_hmap_put(&t->map, h, &found);
		if (!v) return UINT32_MAX;
		if (found) {
			if (strcmp(t->s[*v], s) == 0) return *v;
			h = lossantos_mix64(h + 1);
			continue;
		}
		if (t->n == t->cap) {
			uint32_t nc = t->cap ? t->cap * 2 : 64;
			char **ns = (char **)realloc(t->s, nc * sizeof *ns);
			if (!ns) return UINT32_MAX;
			t->s = ns;
			t->cap = nc;
		}
		size_t l = strlen(s);
		char *c = (char *)malloc(l + 1);
		if (!c) return UINT32_MAX;
		memcpy(c, s, l + 1);
		t->s[t->n] = c;
		*v = t->n;
		return t->n++;
	}
}


int lossantos_vec_grow_(void **p, size_t *cap, size_t need, size_t elem) {
	if (need <= *cap) return 1;
	size_t nc = *cap ? *cap : 16;
	while (nc < need) nc *= 2;
	void *np = realloc(*p, nc * elem);
	if (!np) return 0;
	*p = np;
	*cap = nc;
	return 1;
}


int lossantos_ends_with_ci(const char *s, const char *suffix) {
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && _stricmp(s + a - b, suffix) == 0;
}

int lossantos_starts_with(const char *s, const char *prefix) {
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

void lossantos_lower(char *dst, const char *src, size_t cap) {
	size_t i = 0;
	for (; src[i] && i + 1 < cap; i++) dst[i] = (char)tolower((unsigned char)src[i]);
	dst[i] = 0;
}

double lossantos_now(void) {
	static LARGE_INTEGER f;
	if (!f.QuadPart) QueryPerformanceFrequency(&f);
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / (double)f.QuadPart;
}
