#include "lossantos_tex.h"
#include "lossantos_common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int lossantos_texset_init(lossantos_texset *s) {
	memset(s, 0, sizeof *s);
	return lossantos_hmap_init(&s->idx, 1024);
}

void lossantos_texset_free(lossantos_texset *s) {
	lossantos_vec_free(s->v);
	lossantos_hmap_free(&s->idx);
}

static uint64_t key_hash(const char *k) { return lossantos_hash_bytes(k, strlen(k)); }

int64_t lossantos_texset_find(const lossantos_texset *s, const char *key) {
	uint64_t h = key_hash(key);
	for (;;) {
		const uint32_t *v = lossantos_hmap_get(&s->idx, h);
		if (!v) return -1;
		if (strcmp(s->v.p[*v].key, key) == 0) return *v;
		h = lossantos_mix64(h + 1);
	}
}

int lossantos_texset_offer_info(lossantos_texset *s, const lossantos_texinfo *t) {
	uint64_t h = key_hash(t->key);
	for (;;) {
		int found;
		uint32_t *v = lossantos_hmap_put(&s->idx, h, &found);
		if (!v) return 0;
		if (!found) {
			*v = (uint32_t)s->v.n;
			return lossantos_vec_push(s->v, *t);
		}
		lossantos_texinfo *old = &s->v.p[*v];
		if (strcmp(old->key, t->key) != 0) {
			h = lossantos_mix64(h + 1);
			continue;
		}
		if ((uint64_t)t->w * t->h > (uint64_t)old->w * old->h) *old = *t;  // biggest copy wins
		return 1;
	}
}

int lossantos_texset_offer(lossantos_texset *s, const lossantos_tex *t, const lossantos_texloc *loc) {
	lossantos_texinfo i;
	memset(&i, 0, sizeof i);
	snprintf(i.name, sizeof i.name, "%s", t->name);
	lossantos_lower(i.key, t->name, sizeof i.key);
	i.w = t->width;
	i.h = t->height;
	i.fmt = t->format;
	i.levels = t->levels;
	if (loc) i.loc = *loc;
	return lossantos_texset_offer_info(s, &i);
}


void lossantos_clean_name(const char *name, int max_len, char *out, size_t cap) {
	char buf[256];
	size_t n = 0;
	int in_run = 0;
	for (const unsigned char *p = (const unsigned char *)name; *p && n + 1 < sizeof buf; p++) {
		int c = tolower(*p);
		int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (*p > 0x7F) ok = 0;
		if (ok) {
			buf[n++] = (char)c;
			in_run = 0;
		} else if (!in_run) {
			buf[n++] = '_';
			in_run = 1;
		}
	}
	buf[n] = 0;
	// strip('_')
	char *s = buf;
	while (*s == '_') s++;
	size_t l = strlen(s);
	while (l && s[l - 1] == '_') s[--l] = 0;
	if (!l) {
		snprintf(out, cap, "texture");
		return;
	}
	if (max_len > 0 && (int)l > max_len) {
		char hex[41];
		lossantos_sha1_hex(s, l, hex);
		int keep = max_len - 9;  // room for _ + 8 hex
		char head[256];
		snprintf(head, sizeof head, "%.*s", keep, s);
		size_t hl = strlen(head);
		while (hl && head[hl - 1] == '_') head[--hl] = 0;
		snprintf(out, cap, "%s_%.8s", head, hex);
		return;
	}
	snprintf(out, cap, "%s", s);
}

int lossantos_tex_supported(const lossantos_texinfo *t) {
	switch (t->fmt) {
	case LOSSANTOS_FMT_DXT1: case LOSSANTOS_FMT_DXT3: case LOSSANTOS_FMT_DXT5:
	case LOSSANTOS_FMT_A8R8G8B8: case LOSSANTOS_FMT_X8R8G8B8: case LOSSANTOS_FMT_A8B8G8R8:
	case LOSSANTOS_FMT_L8: case LOSSANTOS_FMT_A8:
		break;
	default:
		return 0;
	}
	return t->levels >= 1 && t->w <= 4096 && t->h <= 4096;
}

static int has_any(const char *k, const char *const *words) {
	for (; *words; words++)
		if (strstr(k, *words)) return 1;
	return 0;
}

int lossantos_is_marking_name(const char *k) {
	static const char *const W[] = {"roadmark", "road_mark", "marking", "genallmark", "linemark", NULL};
	return has_any(k, W);
}

int lossantos_is_overlay_name(const char *k) {
	static const char *const W[] = {"overlay", "ovly", "splat", "blend", "glue", "road_damage",
									"roadblend", "ground_dirt", "dirt_overlay", "stain", "crack",
									"skid", "tyremark", "tyreskid", "litter", NULL};
	static const char *const X[] = {"stainless", NULL};
	return has_any(k, W) && !has_any(k, X);
}

int lossantos_is_stipple_name(const char *k) {
	static const char *const W[] = {"railing", "fence", "mesh", "net", "grill", "grid", "trellis",
									"chainlink", "wicker", "shutter", "gate", "doorgate", "canopy", NULL};
	return has_any(k, W) || (strstr(k, "canopy") && lossantos_ends_with_ci(k, "_a"));
}

int lossantos_is_aux_map(const char *k) {
	static const char *const S[] = {"_n", "_s", "_nm", "_spec", "_normal", "normal_only", NULL};
	char low[256];
	lossantos_lower(low, k, sizeof low);
	if (strstr(low, "flatnormal")) return 1;
	for (int i = 0; S[i]; i++) {
		size_t a = strlen(low), b = strlen(S[i]);
		if (a >= b && strcmp(low + a - b, S[i]) == 0) return 1;
	}
	return 0;
}

int lossantos_is_seethrough_name(const char *k) {
	static const char *const W[] = {"glass", "window", "shop", "decal", "puddle", "ovly", "overlay", NULL};
	return has_any(k, W) || lossantos_is_overlay_name(k) || lossantos_is_marking_name(k);
}


static int cmp_by_name(void *ctx, const void *a, const void *b) {
	const lossantos_texset *s = (const lossantos_texset *)ctx;
	return strcmp(s->v.p[*(const int *)a].name, s->v.p[*(const int *)b].name);
}

int lossantos_install_names(const lossantos_texset *set, const int *items, int nitems, const char *prefix,
					  lossantos_install_vec *out) {
	int budget = 63 - (int)strlen(prefix) - 1;  // gvt1 path cap
	if (budget > 40) budget = 40;
	if (budget < 12) {
		lossantos_set_error("material prefix is too long for GVT1: %s", prefix);
		return 0;
	}
	int *ix = (int *)malloc((nitems ? nitems : 1) * sizeof *ix);
	if (!ix) return 0;
	memcpy(ix, items, nitems * sizeof *ix);
	qsort_s(ix, nitems, sizeof *ix, cmp_by_name, (void *)set);
	int n = 0;
	// one copy per name
	for (int i = 0; i < nitems; i++)
		if (!n || strcmp(set->v.p[ix[n - 1]].name, set->v.p[ix[i]].name) != 0) ix[n++] = ix[i];
	lossantos_strtab used;
	if (!lossantos_strtab_init(&used)) {
		free(ix);
		return 0;
	}
	int ok = 1;
	for (int i = 0; i < n && ok; i++) {
		const lossantos_texinfo *t = &set->v.p[ix[i]];
		if (!lossantos_tex_supported(t)) continue;
		char stem[128], base[128];
		lossantos_clean_name(t->name, budget, stem, sizeof stem);
		snprintf(base, sizeof base, "%s", stem);
		// name taken, add _2, _3 ...
		for (int k = 2; lossantos_strtab_find(&used, stem) >= 0; k++) {
			char sfx[16];
			snprintf(sfx, sizeof sfx, "_%d", k);
			int keep = budget - (int)strlen(sfx);
			snprintf(stem, sizeof stem, "%.*s%s", keep, base, sfx);
		}
		lossantos_strtab_id(&used, stem);
		lossantos_install in;
		memset(&in, 0, sizeof in);
		snprintf(in.key, sizeof in.key, "%s", t->key);
		snprintf(in.path, sizeof in.path, "%s/%s", prefix, stem);
		in.texture = ix[i];
		in.info = *t;
		ok = lossantos_vec_push(*out, in);
	}
	lossantos_strtab_free(&used);
	free(ix);
	return ok;
}


int lossantos_texreg_init(lossantos_texreg *r) {
	memset(r, 0, sizeof *r);
	return lossantos_strtab_init(&r->regions) && lossantos_hset_init(&r->have, 65536);
}

void lossantos_texreg_free(lossantos_texreg *r) {
	lossantos_strtab_free(&r->regions);
	lossantos_hset_free(&r->have);
	free(r->order);
	memset(r, 0, sizeof *r);
}

static uint64_t path_hash(const char *region, const char *stem) {
	char buf[256];
	int n = snprintf(buf, sizeof buf, "%s/%s", region, stem);
	return lossantos_hash_bytes(buf, (size_t)n);
}

int lossantos_texreg_add(lossantos_texreg *r, const char *path) {
	const char *p = path;
	if (strncmp(p, "models/gtav/", 12) != 0) return 1;
	p += 12;
	const char *slash = strchr(p, '/');
	if (!slash) return 1;
	char region[96];
	snprintf(region, sizeof region, "%.*s", (int)(slash - p), p);
	if (lossantos_strtab_find(&r->regions, region) < 0) {
		lossantos_strtab_id(&r->regions, region);
		r->sorted_valid = 0;
	}
	return lossantos_hset_add(&r->have, path_hash(region, slash + 1)) >= 0;
}

int lossantos_texreg_scan(lossantos_texreg *r, const wchar_t *root) {
	wchar_t pat[1024];
	swprintf(pat, 1024, L"%ls\\models\\gtav\\*", root);
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW(pat, &d);
	if (h == INVALID_HANDLE_VALUE) return 0;
	int regions = 0;
	do {
		if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == L'.') continue;
		char region[128];
		WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, region, sizeof region, NULL, NULL);
		wchar_t sub[1024];
		swprintf(sub, 1024, L"%ls\\models\\gtav\\%ls\\*.vtf", root, d.cFileName);
		WIN32_FIND_DATAW f;
		HANDLE hf = FindFirstFileW(sub, &f);
		if (hf == INVALID_HANDLE_VALUE) continue;
		regions++;
		do {
			char stem[256];
			WideCharToMultiByte(CP_UTF8, 0, f.cFileName, -1, stem, sizeof stem, NULL, NULL);
			size_t l = strlen(stem);
			if (l < 4 || _stricmp(stem + l - 4, ".vtf") != 0) continue;
			stem[l - 4] = 0;
			char path[400];
			snprintf(path, sizeof path, "models/gtav/%s/%s", region, stem);
			lossantos_texreg_add(r, path);
		} while (FindNextFileW(hf, &f));
		FindClose(hf);
	} while (FindNextFileW(h, &d));
	FindClose(h);
	return regions;
}

static int cmp_region(void *ctx, const void *a, const void *b) {
	const lossantos_texreg *r = (const lossantos_texreg *)ctx;
	return strcmp(r->regions.s[*(const uint32_t *)a], r->regions.s[*(const uint32_t *)b]);
}

void lossantos_texreg_sort(lossantos_texreg *r) {
	free(r->order);
	r->order = (uint32_t *)malloc((r->regions.n ? r->regions.n : 1) * sizeof *r->order);
	if (!r->order) return;
	for (uint32_t i = 0; i < r->regions.n; i++) r->order[i] = i;
	qsort_s(r->order, r->regions.n, sizeof *r->order, cmp_region, r);
	r->sorted_valid = 1;
}

int lossantos_find_installed(lossantos_texreg *r, const char *name, char *out, size_t cap) {
	char low[256], stem[256];
	lossantos_lower(low, name ? name : "", sizeof low);
	lossantos_clean_name(low, 0, stem, sizeof stem);
	int empty = 1;
	for (const char *p = low; *p; p++)
		if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) { empty = 0; break; }
	if (empty) return 0;
	if (!r->sorted_valid) lossantos_texreg_sort(r);
	if (!r->order) return 0;
	char cands[12][128];
	int nc = 0;
	// budget depended on the prefix, try them all
	for (int b = 30; b <= 40; b++) lossantos_clean_name(name, b, cands[nc++], 128);
	int seen = 0;
	for (int i = 0; i < nc; i++)
		if (strcmp(cands[i], stem) == 0) seen = 1;
	if (!seen) snprintf(cands[nc++], 128, "%s", stem);
	for (uint32_t ri = 0; ri < r->regions.n; ri++) {
		const char *region = r->regions.s[r->order[ri]];
		for (int i = 0; i < nc; i++)
			if (lossantos_hset_has(&r->have, path_hash(region, cands[i]))) {
				snprintf(out, cap, "models/gtav/%s/%s", region, cands[i]);
				return 1;
			}
	}
	return 0;
}


static const char *const SUFFIXES[] = {"_a", "_d", "_n", "_s", "_b"};

static size_t strip_suffix(const char *n, char *out, size_t cap) {
	snprintf(out, cap, "%s", n);
	size_t l = strlen(out);
	for (int i = 0; i < 5; i++)
		if (l >= 2 && strcmp(out + l - 2, SUFFIXES[i]) == 0) {
			out[l - 2] = 0;
			return l - 2;
		}
	return l;
}

static int folded(const char *a, size_t la, const char *b, size_t lb) {
	if (lb > la) return 0;
	return strncmp(a, b, lb) == 0 || strcmp(a + la - lb, b) == 0;
}

int64_t lossantos_near_texture(const lossantos_texset *s, const char *key) {
	size_t kl = strlen(key);
	// exact base name first
	for (int i = 0; i < 5; i++)
		if (kl >= 2 && strcmp(key + kl - 2, SUFFIXES[i]) == 0) {
			char base[128];
			snprintf(base, sizeof base, "%.*s", (int)(kl - 2), key);
			int64_t hit = lossantos_texset_find(s, base);
			if (hit >= 0) return hit;
		}
	if (kl < 6) return -1;
	char k[128];
	size_t lk = strip_suffix(key, k, sizeof k);
	int key_is_map = lossantos_is_aux_map(key);
	int64_t best = -1;
	char bestd[128] = "";
	size_t bestl = 0;
	// yeah this is slow, whatever
	for (size_t i = 0; i < s->v.n; i++) {
		const char *cand = s->v.p[i].key;
		if (!key_is_map && lossantos_is_aux_map(cand)) continue;
		char d[128];
		size_t ld = strip_suffix(cand, d, sizeof d);
		if (ld < 6 || lk < 6) continue;
		if (!(strcmp(d, k) == 0 || folded(k, lk, d, ld) || folded(d, ld, k, lk))) continue;
		if (best < 0 || ld < bestl || (ld == bestl && strcmp(d, bestd) < 0)) {
			best = (int64_t)i;
			bestl = ld;
			snprintf(bestd, sizeof bestd, "%s", d);
		}
	}
	return best;
}

const char *lossantos_word_fallback(const char *key) {
	static const char *const KERB[] = {"kerb", "sidewalk", "pavement", "pave", "floortile", NULL};
	static const char *const THIN[] = {"weed", "litter", "splat", "stain", "cable", "wire", NULL};
	static const char *const GRASS[] = {"grass", "hedge", "leaflitter", NULL};
	static const char *const DIRT[] = {"dirt", "gravel", "sand", "mud", "stone", "rock", "erode", "cliff", NULL};
	static const char *const ROAD[] = {"road", "crossing", "tarmac", "blend", "overlay", "ground", NULL};
	if (strstr(key, "window")) return "models/gtav/bh1_06/bh1_06_rsn_os_window_0001";
	if (strstr(key, "shop")) return "models/gtav/ap1_01_a/im_concrete17";
	if (strstr(key, "glass")) return "models/gtav/bh1_06/bh1_06_rsn_os_window_0001";
	if (lossantos_is_marking_name(key)) return NULL;
	if (lossantos_is_overlay_name(key)) return NULL;
	if (strstr(key, "drain")) return "models/gtav/dt1_02/im_drains02";
	if (has_any(key, KERB)) return "models/gtav/dt1_02/im_sidewalk010";
	if (has_any(key, THIN)) return NULL;
	if (has_any(key, GRASS)) return "models/gtav/bh1_06/im_grass11b_fur";
	if (has_any(key, DIRT)) return "models/gtav/bh1_21/am_glue_dirtbrown";
	if (strstr(key, "metal") || strstr(key, "plate")) return "models/gtav/bh1_06/bh1_06_rsn_mc_metal_0001";
	if (has_any(key, ROAD)) return "models/gtav/dt1_02/im_road_012";
	return "models/gtav/ap1_01_a/im_concrete17";
}
