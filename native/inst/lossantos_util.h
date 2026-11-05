#ifndef LOSSANTOS_UTIL_H
#define LOSSANTOS_UTIL_H

#include <stddef.h>
#include <stdint.h>

void lossantos_sha1(const void *data, size_t len, uint8_t out[20]);
void lossantos_sha1_hex(const void *data, size_t len, char out[41]);

// tmp = scratch, same size as a
size_t lossantos_sort_unique_u64(uint64_t *a, size_t n, uint64_t *tmp);
void lossantos_sort_u64(uint64_t *a, size_t n, uint64_t *tmp);
// first index i with a[i] >= v
size_t lossantos_lower_bound_u64(const uint64_t *a, size_t n, uint64_t v);

typedef struct {
	uint64_t *k;
	size_t cap, n;
	int has_zero;  // key 0 lives here, 0 = empty slot
} lossantos_hset;
int lossantos_hset_init(lossantos_hset *s, size_t expect);
void lossantos_hset_free(lossantos_hset *s);
int lossantos_hset_add(lossantos_hset *s, uint64_t key);       /* 1 if newly added, 0 if present, -1 oom */
int lossantos_hset_has(const lossantos_hset *s, uint64_t key);

typedef struct {
	uint64_t *k;
	uint32_t *v;
	uint8_t *used;
	size_t cap, n;
} lossantos_hmap;
int lossantos_hmap_init(lossantos_hmap *m, size_t expect);
void lossantos_hmap_free(lossantos_hmap *m);
// value slot, *found says if it existed
uint32_t *lossantos_hmap_put(lossantos_hmap *m, uint64_t key, int *found);
const uint32_t *lossantos_hmap_get(const lossantos_hmap *m, uint64_t key);

// murmur3 finalizer
static inline uint64_t lossantos_mix64(uint64_t x) {
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ull;
	x ^= x >> 33;
	return x;
}
uint64_t lossantos_hash_bytes(const void *p, size_t n);

typedef struct {
	char **s;
	uint32_t n, cap;
	lossantos_hmap map;          /* hash -> id (collisions chained through probe) */
} lossantos_strtab;
int lossantos_strtab_init(lossantos_strtab *t);
void lossantos_strtab_free(lossantos_strtab *t);
uint32_t lossantos_strtab_id(lossantos_strtab *t, const char *s);       /* adds if absent, UINT32_MAX oom */
int64_t lossantos_strtab_find(const lossantos_strtab *t, const char *s); /* -1 if absent */

#define LOSSANTOS_VEC(T) struct { T *p; size_t n, cap; }
int lossantos_vec_grow_(void **p, size_t *cap, size_t need, size_t elem);
#define lossantos_vec_push(v, x) \
	(((v).n < (v).cap || lossantos_vec_grow_((void **)&(v).p, &(v).cap, (v).n + 1, sizeof *(v).p)) \
		 ? ((v).p[(v).n++] = (x), 1) : 0)
#define lossantos_vec_reserve(v, k) lossantos_vec_grow_((void **)&(v).p, &(v).cap, (k), sizeof *(v).p)
#define lossantos_vec_free(v) (free((v).p), (v).p = NULL, (v).n = (v).cap = 0)

int lossantos_ends_with_ci(const char *s, const char *suffix);
int lossantos_starts_with(const char *s, const char *prefix);
void lossantos_lower(char *dst, const char *src, size_t cap);
double lossantos_now(void);   /* seconds, monotonic */

#endif
