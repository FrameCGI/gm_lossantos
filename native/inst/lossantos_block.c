#include "lossantos_block.h"
#include "lossantos_common.h"
#include "lossantos_rpf.h"
#include "lossantos_ydr.h"
#include "lossantos_ymap.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCALE LOSSANTOS_SCALE

static const char *const CITY_ARCHIVES[] = {"x64i.rpf", "x64j.rpf", "x64k.rpf", "x64l.rpf",
											"x64m.rpf", "x64n.rpf", "x64o.rpf", "x64p.rpf"};
// shared road atlases, x64g gtxd.rpf
static const char *const ROAD_ATLASES[] = {
	"globalroads", "cityeastrd", "citynorthrd", "citywestrd", "beverlyhillsrd", "bhmedianshare",
	"downtownrd", "freewayrd", "hollywoodrd", "koreatownrd", "portrd", "sanpedrord", "santamonicard",
	"southcentralrd", "sunsetrd", "venicebeachrd", "fwyroads", "chrddetail", "ch1", "ch2",
	"ch3roadnorth", "ch3roadsouth"};

typedef LOSSANTOS_VEC(uint32_t) u32vec;

static void walk_all_(const lossantos_rpf *a, u32vec *out);
static void walk_dir(const lossantos_rpf *a, uint32_t dir, u32vec *out) {
	const lossantos_entry *d = &a->e[dir];
	for (uint32_t c = d->first; c < d->first + d->count; c++) {
		if (c == dir) continue;
		lossantos_vec_push(*out, c);
		if (a->e[c].kind == LOSSANTOS_DIR) walk_dir(a, c, out);
	}
}
void lossantos_rpf_walk(const lossantos_rpf *a, lossantos_u32vec *out) {
	walk_all_(a, (u32vec *)out);
}
static void walk_all(const lossantos_rpf *a, u32vec *out) { walk_all_(a, out); }
static void walk_all_(const lossantos_rpf *a, u32vec *out) {
	out->n = 0;
	if (a->count) walk_dir(a, 0, out);
}

static void open_path(wchar_t *dst, size_t cap, const wchar_t *dir, const char *name) {
	swprintf(dst, cap, L"%ls\\%hs", dir, name);
}


// everything with a ybn, road packs last
int lossantos_discover(const wchar_t *gta, const lossantos_ng *ng, lossantos_blockref_vec *out) {
	static const char *const SKIP[] = {"interior", "_lod", "metadata", "prologue", "destruction", "props"};
	static const char *const DIST[] = {"_citye", "_cityw", "_hills"};
	lossantos_blockref_vec roads = {0};
	u32vec order = {0};
	char path[1024];
	for (int ai = 0; ai < 8; ai++) {
		wchar_t p[1024];
		open_path(p, 1024, gta, CITY_ARCHIVES[ai]);
		lossantos_rpf outer;
		if (!lossantos_rpf_open(&outer, ng, p)) continue;
		walk_all(&outer, &order);
		for (size_t k = 0; k < order.n; k++) {
			uint32_t i = order.p[k];
			const lossantos_entry *e = &outer.e[i];
			if (e->kind != LOSSANTOS_BINARY || !lossantos_ends_with_ci(e->name, ".rpf")) continue;
			lossantos_rpf_path(&outer, i, path, sizeof path);
			const char *lv = strstr(path, "levels/gta5/");
			if (!lv) continue;
			const char *rel = lv + 12;
			const char *slash = strchr(rel, '/');
			if (!slash) continue;
			int dist = 0;
			for (int d = 0; d < 3; d++)
				if (strncmp(rel, DIST[d], strlen(DIST[d])) == 0) dist = 1;
			if (!dist) continue;
			char low[128];
			lossantos_lower(low, e->name, sizeof low);
			int skip = 0;
			for (int s = 0; s < 6; s++)
				if (strstr(low, SKIP[s])) skip = 1;
			if (skip) continue;
			lossantos_rpf child;
			if (!lossantos_rpf_open_child(&child, &outer, i)) continue;
			int has_ybn = 0;
			for (uint32_t c = 1; c < child.count && !has_ybn; c++)
				if (child.e[c].kind != LOSSANTOS_DIR && lossantos_ends_with_ci(child.e[c].name, ".ybn")) has_ybn = 1;
			lossantos_rpf_close(&child);
			if (!has_ybn) continue;
			lossantos_blockref b;
			memset(&b, 0, sizeof b);
			snprintf(b.outer, sizeof b.outer, "%s", CITY_ARCHIVES[ai]);
			snprintf(b.inner, sizeof b.inner, "levels/gta5/%s", rel);
			snprintf(b.name, sizeof b.name, "%s", e->name);
			snprintf(b.stem, sizeof b.stem, "%.*s", (int)(strlen(e->name) - 4), e->name);
			b.road = strstr(low, "_rd") || strstr(low, "roads");
			if (b.road) lossantos_vec_push(roads, b);
			else lossantos_vec_push(*out, b);
		}
		lossantos_rpf_close(&outer);
	}
	for (size_t i = 0; i < roads.n; i++) lossantos_vec_push(*out, roads.p[i]);
	lossantos_vec_free(roads);
	lossantos_vec_free(order);
	return 1;
}


int lossantos_soup_reserve(lossantos_soup *s, size_t n) {
	if (n <= s->cap) return 1;
	size_t nc = s->cap ? s->cap : 4096;
	while (nc < n) nc *= 2;
	double *p = (double *)realloc(s->pos, nc * 9 * sizeof(double));
	if (!p) return 0;
	s->pos = p;
	double *q = (double *)realloc(s->nrm, nc * 9 * sizeof(double));
	if (!q) return 0;
	s->nrm = q;
	double *u = (double *)realloc(s->uv, nc * 6 * sizeof(double));
	if (!u) return 0;
	s->uv = u;
	uint32_t *m = (uint32_t *)realloc(s->mat, nc * sizeof(uint32_t));
	if (!m) return 0;
	s->mat = m;
	s->cap = nc;
	return 1;
}

void lossantos_soup_free(lossantos_soup *s) {
	free(s->pos);
	free(s->nrm);
	free(s->uv);
	free(s->mat);
	memset(s, 0, sizeof *s);
}


int lossantos_world_init(lossantos_world *w, const wchar_t *gta, const lossantos_ng *ng) {
	memset(w, 0, sizeof *w);
	w->gta_dir = gta;
	w->ng = ng;
	return lossantos_texset_init(&w->atlas) && lossantos_texreg_init(&w->reg) && lossantos_hmap_init(&w->roads_idx, 4096);
}

void lossantos_world_free(lossantos_world *w) {
	lossantos_texset_free(&w->atlas);
	lossantos_texreg_free(&w->reg);
	lossantos_vec_free(w->roads);
	lossantos_hmap_free(&w->roads_idx);
}

static int parse_ytd_entry(lossantos_rpf *a, uint32_t idx, lossantos_buf *buf, lossantos_tex *tex, int cap) {
	if (!lossantos_rpf_read(a, idx, buf)) return 0;
	lossantos_rsc r;
	if (!lossantos_rsc_init(&r, buf->p, buf->n, lossantos_rpf_size_from_flags(a->e[idx].sys_flags))) return 0;
	return lossantos_ytd_parse(&r, LOSSANTOS_SYS_BASE, tex, cap);
}

int lossantos_world_load_atlases(lossantos_world *w) {
	wchar_t p[1024];
	open_path(p, 1024, w->gta_dir, "x64g.rpf");
	lossantos_rpf g, gtxd;
	if (!lossantos_rpf_open(&g, w->ng, p)) return 0;
	int32_t gi = lossantos_rpf_find(&g, "levels/gta5/generic/gtxd.rpf");
	if (gi < 0 || !lossantos_rpf_open_child(&gtxd, &g, (uint32_t)gi)) {
		lossantos_rpf_close(&g);
		lossantos_set_error("x64g.rpf: no levels/gta5/generic/gtxd.rpf");
		return 0;
	}
	lossantos_buf buf = {0};
	lossantos_tex *tex = (lossantos_tex *)malloc(4096 * sizeof *tex);
	int ok = tex != NULL;
	for (int k = 0; k < 22 && ok; k++) {
		char nm[64];
		snprintf(nm, sizeof nm, "%s.ytd", ROAD_ATLASES[k]);
		int32_t ti = lossantos_rpf_find(&gtxd, nm);
		if (ti < 0) {
			lossantos_set_error("--tex-ytd: no such .ytd: %s", nm);
			ok = 0;
			break;
		}
		int n = parse_ytd_entry(&gtxd, (uint32_t)ti, &buf, tex, 4096);
		for (int t = 0; t < n; t++) {
			lossantos_texloc loc = {0};
			loc.archive = 0xFFFF;            /* x64g */
			loc.nested = (uint32_t)gi;
			loc.nested2 = UINT32_MAX;
			loc.entry = (uint32_t)ti;
			loc.tex_index = (uint16_t)t;
			ok = ok && lossantos_texset_offer(&w->atlas, &tex[t], &loc);
		}
	}
	free(tex);
	lossantos_buf_free(&buf);
	lossantos_rpf_close(&gtxd);
	lossantos_rpf_close(&g);
	return ok;
}

int lossantos_world_finish_installs(lossantos_world *w, const lossantos_texinfo *items, size_t n) {
	lossantos_texset u;
	if (!lossantos_texset_init(&u)) return 0;
	for (size_t i = 0; i < n; i++) lossantos_texset_offer_info(&u, &items[i]);
	int *ix = (int *)malloc((u.v.n ? u.v.n : 1) * sizeof *ix);
	for (size_t i = 0; i < u.v.n; i++) ix[i] = (int)i;
	w->roads.n = 0;
	int ok = lossantos_install_names(&u, ix, (int)u.v.n, "models/gtav/roads", &w->roads);
	for (size_t i = 0; ok && i < w->roads.n; i++) {
		int found;
		uint64_t h = lossantos_hash_bytes(w->roads.p[i].key, strlen(w->roads.p[i].key));
		uint32_t *v = lossantos_hmap_put(&w->roads_idx, h, &found);
		if (v) *v = (uint32_t)i;
		lossantos_texreg_add(&w->reg, w->roads.p[i].path);
	}
	free(ix);
	lossantos_texset_free(&u);
	return ok;
}

static const char *roads_path(const lossantos_world *w, const char *key) {
	const uint32_t *v = lossantos_hmap_get(&w->roads_idx, lossantos_hash_bytes(key, strlen(key)));
	if (v && strcmp(w->roads.p[*v].key, key) == 0) return w->roads.p[*v].path;
	return NULL;
}


void lossantos_block_out_free(lossantos_block_out *o) {
	lossantos_vec_free(o->installs);
	lossantos_vec_free(o->items);
	lossantos_vec_free(o->local);
	lossantos_vec_free(o->unplaced_tex);
	for (size_t i = 0; i < o->missing.n; i++) free(o->missing.p[i]);
	lossantos_vec_free(o->missing);
	lossantos_vec_free(o->atlas_items);
	lossantos_ybn_free(&o->coll);
	lossantos_soup_free(&o->vis);
	lossantos_strtab_free(&o->mats);
	lossantos_vec_free(o->texmats);
}

// _s?lod\d*(_|.|$), emissive, _emi_, _proxy.ydr
static int lod_name(const char *low) {
	for (const char *p = strchr(low, '_'); p; p = strchr(p + 1, '_')) {
		const char *q = p + 1;
		if (*q == 's') q++;
		if (strncmp(q, "lod", 3) != 0) continue;
		q += 3;
		while (*q >= '0' && *q <= '9') q++;
		if (*q == '_' || *q == '.' || *q == 0) return 1;
	}
	return strstr(low, "emissive") || strstr(low, "_emi_") || lossantos_ends_with_ci(low, "_proxy.ydr");
}

void lossantos_quat_matrix(const float q[4], double m[3][3]) {
	double x = q[0], y = q[1], z = q[2], w = q[3];
	m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w); m[0][2] = 2 * (x * z + y * w);
	m[1][0] = 2 * (x * y + z * w); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
	m[2][0] = 2 * (x * z - y * w); m[2][1] = 2 * (y * z + x * w); m[2][2] = 1 - 2 * (x * x + y * y);
}

typedef lossantos_placement placement;

typedef struct {
	lossantos_drawable d;
	char source[128];        /* entry name */
	char low[128];
	uint32_t entry;
} parsed_ydr;

void lossantos_place_point(const lossantos_placement *p, const float *v, double o[3]) {
	double s[3] = {v[0] * p->scale[0], v[1] * p->scale[1], v[2] * p->scale[2]};
	for (int j = 0; j < 3; j++) o[j] = s[0] * p->rot[j][0] + s[1] * p->rot[j][1] + s[2] * p->rot[j][2] + p->off[j];
}

// name hash first, bbox as fallback
int lossantos_match_archetype(const char *source, const double *lo, const double *hi, const lossantos_archetype *arch,
						int narch, float tolerance) {
	char stem[128];
	snprintf(stem, sizeof stem, "%s", source);
	if (lossantos_ends_with_ci(stem, ".ydr")) stem[strlen(stem) - 4] = 0;
	float cz = (float)(((float)lo[2] + (float)hi[2]) * 0.5f);
	int best = -1;
	float bxy = 0, bz = 0;
	if (stem[0]) {
		uint32_t h = lossantos_joaat(stem);
		for (int i = 0; i < narch; i++) {
			if (arch[i].name != h && arch[i].asset_name != h) continue;
			float xy = fabsf(arch[i].bb_min[0] - (float)lo[0]) + fabsf(arch[i].bb_min[1] - (float)lo[1]) +
					   fabsf(arch[i].bb_max[0] - (float)hi[0]) + fabsf(arch[i].bb_max[1] - (float)hi[1]);
			float z = fabsf((arch[i].bb_min[2] + arch[i].bb_max[2]) * 0.5f - cz);
			if (best < 0 || xy < bxy || (xy == bxy && z < bz)) {
				best = i;
				bxy = xy;
				bz = z;
			}
		}
		if (best >= 0) return best;
	}
	for (int i = 0; i < narch; i++) {
		float xy = fabsf(arch[i].bb_min[0] - (float)lo[0]) + fabsf(arch[i].bb_min[1] - (float)lo[1]) +
				   fabsf(arch[i].bb_max[0] - (float)hi[0]) + fabsf(arch[i].bb_max[1] - (float)hi[1]);
		if (xy > tolerance) continue;
		float z = fabsf((arch[i].bb_min[2] + arch[i].bb_max[2]) * 0.5f - cz);
		if (best < 0 || xy < bxy || (xy == bxy && z < bz)) {
			best = i;
			bxy = xy;
			bz = z;
		}
	}
	return best;
}

static const lossantos_texmat *texmat_get(const lossantos_block_out *o, const lossantos_hmap *idx, const char *key) {
	const uint32_t *v = lossantos_hmap_get(idx, lossantos_hash_bytes(key, strlen(key)));
	if (v && strcmp(o->texmats.p[*v].key, key) == 0) return &o->texmats.p[*v];
	return NULL;
}

static int texmat_set(lossantos_block_out *o, lossantos_hmap *idx, const char *key, const char *path, int drop) {
	int found;
	uint32_t *v = lossantos_hmap_put(idx, lossantos_hash_bytes(key, strlen(key)), &found);
	if (!v) return 0;
	if (found && strcmp(o->texmats.p[*v].key, key) == 0) {
		snprintf(o->texmats.p[*v].path, sizeof o->texmats.p[*v].path, "%s", path ? path : "");
		o->texmats.p[*v].drop = drop;
		return 1;
	}
	lossantos_texmat t;
	memset(&t, 0, sizeof t);
	snprintf(t.key, sizeof t.key, "%s", key);
	snprintf(t.path, sizeof t.path, "%s", path ? path : "");
	t.drop = drop;
	*v = (uint32_t)o->texmats.n;
	return lossantos_vec_push(o->texmats, t);
}

static int cover_key(const lossantos_block_out *o, const lossantos_hmap *idx, const char *key) {
	const lossantos_texmat *t = texmat_get(o, idx, key);
	if (t && t->drop) return 0;
	return !lossantos_is_seethrough_name(key);
}


// this thing is huge but it works
int lossantos_block_run(lossantos_world *w, const lossantos_blockref *b, int phase, lossantos_block_out *out) {
	double t0 = lossantos_now();
	int ok = 0;
	lossantos_rpf outer = {0}, blk = {0}, meta = {0};
	int have_outer = 0, have_blk = 0, have_meta = 0;
	lossantos_buf buf = {0};
	u32vec order = {0};
	LOSSANTOS_VEC(lossantos_ybn) ybns = {0};
	LOSSANTOS_VEC(parsed_ydr) ydrs = {0};
	LOSSANTOS_VEC(placement) places = {0};
	lossantos_entity *ents = NULL;
	lossantos_archetype *arch = NULL;
	lossantos_tex *tex = NULL;
	lossantos_texset avail;
	int have_avail = 0;
	lossantos_ybn combined;
	lossantos_coll_init(&combined);
	lossantos_strtab used;
	int have_used = 0;
	LOSSANTOS_VEC(int) items = {0};
	lossantos_hmap tmidx = {0};
	lossantos_soup dsoup = {0};          /* placed drawable triangles (scaled) */
	LOSSANTOS_VEC(uint32_t) dkey = {0};  /* per dsoup triangle: index into `used` keys */
	char path[1024];

	if (!lossantos_strtab_init(&out->mats) || !lossantos_hmap_init(&tmidx, 512)) goto done;

	wchar_t p[1024];
	open_path(p, 1024, w->gta_dir, b->outer);
	if (!lossantos_rpf_open(&outer, w->ng, p)) goto done;
	have_outer = 1;
	uint16_t arch_id = 0;
	for (uint16_t k = 0; k < 8; k++)
		if (_stricmp(CITY_ARCHIVES[k], b->outer) == 0) arch_id = k;
	int32_t bi = lossantos_rpf_find(&outer, b->inner);
	if (bi < 0 || !lossantos_rpf_open_child(&blk, &outer, (uint32_t)bi)) {
		snprintf(out->error, sizeof out->error, "no block archive %s", b->inner);
		goto done;
	}
	have_blk = 1;
	walk_all(&blk, &order);

	// collision, every ybn merged with its hi@
	double bmin[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, bmax[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
	{
		LOSSANTOS_VEC(uint32_t) sel = {0};
		for (size_t k = 0; k < order.n; k++) {
			const lossantos_entry *e = &blk.e[order.p[k]];
			if (e->kind != LOSSANTOS_RESOURCE || !lossantos_ends_with_ci(e->name, ".ybn")) continue;
			if (_strnicmp(e->name, "hi@", 3) == 0) continue;
			if (phase == 2 && _strnicmp(e->name, "ma@", 3) == 0) continue;
			lossantos_vec_push(sel, order.p[k]);
		}
		if (phase == 2 && !sel.n)
			for (size_t k = 0; k < order.n; k++) {
				const lossantos_entry *e = &blk.e[order.p[k]];
				if (e->kind != LOSSANTOS_DIR && lossantos_ends_with_ci(e->name, ".ybn")) lossantos_vec_push(sel, order.p[k]);
			}
		for (size_t s = 0; s < sel.n; s++) {
			uint32_t i = sel.p[s];
			lossantos_ybn base, hi;
			memset(&base, 0, sizeof base);
			memset(&hi, 0, sizeof hi);
			int has_base = 0, has_hi = 0;
			if (lossantos_rpf_read(&blk, i, &buf) &&
				lossantos_ybn_parse(buf.p, buf.n, lossantos_rpf_size_from_flags(blk.e[i].sys_flags), &base)) {
				if (base.ntris) {
					has_base = 1;
					for (int k = 0; k < 3; k++) {
						if (base.bmin[k] < bmin[k]) bmin[k] = base.bmin[k];
						if (base.bmax[k] > bmax[k]) bmax[k] = base.bmax[k];
					}
				} else lossantos_ybn_free(&base);
			}
			char hn[160];
			snprintf(hn, sizeof hn, "hi@%s", blk.e[i].name);
			int32_t hidx = -1;
			for (size_t k = 0; k < order.n; k++) {
				const lossantos_entry *e = &blk.e[order.p[k]];
				if (e->kind == LOSSANTOS_RESOURCE && _stricmp(e->name, hn) == 0) {
					hidx = (int32_t)order.p[k];
					break;
				}
			}
			if (hidx >= 0 && lossantos_rpf_read(&blk, (uint32_t)hidx, &buf) &&
				lossantos_ybn_parse(buf.p, buf.n, lossantos_rpf_size_from_flags(blk.e[hidx].sys_flags), &hi)) {
				if (hi.ntris) has_hi = 1;
				else lossantos_ybn_free(&hi);
			}
			if (!has_base && has_hi) {
				base = hi;
				has_base = 1;
				has_hi = 0;
			}
			if (has_base && has_hi) {
				lossantos_ybn merged;
				int mok = lossantos_merge_hi(&base, &hi, &merged);
				lossantos_ybn_free(&base);
				lossantos_ybn_free(&hi);
				if (!mok) { lossantos_vec_free(sel); goto done; }
				base = merged;
			}
			if (has_base) lossantos_vec_push(ybns, base);
		}
		lossantos_vec_free(sel);
		for (size_t k = 0; k < ybns.n; k++) lossantos_coll_append(&combined, &ybns.p[k], NULL);
		if (!combined.ntris) {
			snprintf(out->error, sizeof out->error, "no collision triangles in any .ybn of this archive");
			out->empty = 1;
			goto done;
		}
	}

	// collision only fallback, double sided palette
	if (phase == 2) {
		size_t n = combined.ntris;
		double *sc = (double *)malloc((n ? n : 1) * 72);
		uint8_t *keep = (uint8_t *)malloc(n ? n : 1);
		if (!sc || !keep) { free(sc); free(keep); goto done; }
		for (size_t i = 0; i < n * 9; i++) sc[i] = combined.tri[i] * SCALE;
		lossantos_dedupe_tris(sc, n, &w->compat, keep);
		lossantos_coll_init(&out->coll);
		for (size_t i = 0; i < n; i++)
			if (keep[i]) lossantos_coll_push(&out->coll, sc + i * 9, sc + i * 9 + 3, sc + i * 9 + 6, combined.mat[i]);
		free(sc);
		free(keep);
		size_t m = out->coll.ntris;
		double *nrm = (double *)malloc((m ? m : 1) * 24);
		uint8_t *cat = (uint8_t *)malloc(m ? m : 1);
		if (!nrm || !cat || !lossantos_soup_reserve(&out->vis, m)) { free(nrm); free(cat); goto done; }
		for (size_t i = 0; i < m; i++) lossantos_tri_normal(out->coll.tri + i * 9, nrm + i * 3);
		lossantos_categories(out->coll.tri, nrm, out->coll.mat, m, &w->compat, cat);
		uint32_t palette_id[LOSSANTOS_CAT_COUNT];
		for (int c = 0; c < LOSSANTOS_CAT_COUNT; c++) palette_id[c] = lossantos_strtab_id(&out->mats, LOSSANTOS_PALETTE_NAMES[c]);
		for (size_t i = 0; i < m; i++) {
			size_t o = out->vis.n++;
			memcpy(out->vis.pos + o * 9, out->coll.tri + i * 9, 72);
			for (int c = 0; c < 3; c++) memcpy(out->vis.nrm + o * 9 + c * 3, nrm + i * 3, 24);
			lossantos_planar_uv(out->coll.tri + i * 9, nrm + i * 3, LOSSANTOS_PALETTE_METRES[cat[i]] * SCALE, out->vis.uv + o * 6);
			out->vis.mat[o] = palette_id[cat[i]];
		}
		free(nrm);
		free(cat);
		out->coll_tris = out->coll_kept = m;
		out->double_sided = 1;
		ok = 1;
		goto done;
	}

	// archetypes + entities of the district
	int narch = 0, nents = 0;
	{
		char block_dir[256];
		snprintf(block_dir, sizeof block_dir, "%s", b->inner);
		char *cut = strrchr(block_dir, '/');
		if (cut) *cut = 0;
		const char *dbase = strrchr(block_dir, '/');
		dbase = dbase ? dbase + 1 : block_dir;
		char want[160], needle[300];
		snprintf(want, sizeof want, "%s_metadata.rpf", dbase);
		snprintf(needle, sizeof needle, "%s/", block_dir);
		u32vec oo = {0};
		walk_all(&outer, &oo);
		int32_t first = -1, exact = -1;
		for (size_t k = 0; k < oo.n; k++) {
			const lossantos_entry *e = &outer.e[oo.p[k]];
			if (e->kind != LOSSANTOS_BINARY || !lossantos_ends_with_ci(e->name, ".rpf")) continue;
			char low[128];
			lossantos_lower(low, e->name, sizeof low);
			if (!strstr(low, "metadata")) continue;
			lossantos_rpf_path(&outer, oo.p[k], path, sizeof path);
			if (!strstr(path, needle)) continue;
			if (first < 0) first = (int32_t)oo.p[k];
			if (exact < 0 && strcmp(low, want) == 0) exact = (int32_t)oo.p[k];
		}
		lossantos_vec_free(oo);
		int32_t mi = exact >= 0 ? exact : first;
		if (mi < 0) {
			snprintf(out->error, sizeof out->error, "no metadata .rpf next to %s", b->inner);
			goto done;
		}
		if (!lossantos_rpf_open_child(&meta, &outer, (uint32_t)mi)) goto done;
		have_meta = 1;
		arch = (lossantos_archetype *)malloc(65536 * sizeof *arch);
		ents = (lossantos_entity *)malloc(262144 * sizeof *ents);
		if (!arch || !ents) goto done;
		u32vec mo = {0};
		walk_all(&meta, &mo);
		for (size_t k = 0; k < mo.n; k++) {
			const lossantos_entry *e = &meta.e[mo.p[k]];
			if (e->kind == LOSSANTOS_DIR || !lossantos_ends_with_ci(e->name, ".ytyp")) continue;
			if (!lossantos_rpf_read(&meta, mo.p[k], &buf)) continue;
			int n = lossantos_ytyp_archetypes(buf.p, buf.n, lossantos_rpf_size_from_flags(e->sys_flags), arch + narch, 65536 - narch);
			if (n > 0) narch += n;
		}
		for (size_t k = 0; k < mo.n; k++) {
			const lossantos_entry *e = &meta.e[mo.p[k]];
			if (e->kind == LOSSANTOS_DIR || !lossantos_ends_with_ci(e->name, ".ymap")) continue;
			if (!lossantos_rpf_read(&meta, mo.p[k], &buf)) continue;
			int n = lossantos_ymap_entities(buf.p, buf.n, lossantos_rpf_size_from_flags(e->sys_flags), ents + nents, 262144 - nents);
			if (n > 0) nents += n;
		}
		lossantos_vec_free(mo);
	}

	// drawables placed by the ymap entities
	if (!lossantos_texset_init(&avail)) goto done;
	have_avail = 1;
	tex = (lossantos_tex *)malloc(4096 * sizeof *tex);
	if (!tex) goto done;
	{
		const double margin = 50.0;  /* max(clip_margin 30, 30) + 20 */
		double lo[3], hi[3];
		for (int k = 0; k < 3; k++) {
			lo[k] = bmin[k] - margin;
			hi[k] = bmax[k] + margin;
		}
		lossantos_hset seen;
		lossantos_hset_init(&seen, 4096);
		for (size_t k = 0; k < order.n; k++) {
			uint32_t i = order.p[k];
			const lossantos_entry *e = &blk.e[i];
			if (e->kind == LOSSANTOS_DIR || !lossantos_ends_with_ci(e->name, ".ydr")) continue;
			if (!lossantos_rpf_read(&blk, i, &buf)) continue;
			parsed_ydr y;
			memset(&y, 0, sizeof y);
			if (!lossantos_ydr_parse(buf.p, buf.n, lossantos_rpf_size_from_flags(e->sys_flags), &y.d)) {
				lossantos_ydr_free(&y.d);
				continue;
			}
			out->ydr_files++;
			snprintf(y.source, sizeof y.source, "%s", e->name);
			lossantos_lower(y.low, e->name, sizeof y.low);
			y.entry = i;
			int ai = lossantos_match_archetype(y.source, y.d.bmin, y.d.bmax, arch, narch, 8.0f);
			int nplaced = 0;
			if (ai >= 0) {
				int lodname = lod_name(y.low);
				for (int en = 0; en < nents; en++) {
					const lossantos_entity *ent = &ents[en];
					if (ent->archetype != arch[ai].name) continue;
					int in = 1;
					for (int c = 0; c < 3; c++)
						if (!(ent->pos[c] >= lo[c] && ent->pos[c] <= hi[c])) in = 0;
					if (!in) continue;
					// hd + orphan hd only
					if (ent->lod_level != 0 && ent->lod_level != 5) {
						out->lod_skipped++;
						continue;
					}
					if (lodname) {
						out->lod_skipped++;
						continue;
					}
					// dedupe on (name, round(pos, 2))
					int32_t q[3];
					for (int c = 0; c < 3; c++) q[c] = (int32_t)nearbyintf(ent->pos[c] * 100.0f);
					uint64_t sk = lossantos_hash_bytes(y.low, strlen(y.low)) ^ lossantos_hash_bytes(q, sizeof q);
					if (lossantos_hset_add(&seen, sk) != 1) continue;
					placement pl;
					memset(&pl, 0, sizeof pl);
					pl.drawable = (int)ydrs.n;
					lossantos_quat_matrix(ent->rot, pl.rot);
					pl.scale[0] = pl.scale[1] = ent->scale_xy;
					pl.scale[2] = ent->scale_z;
					for (int c = 0; c < 3; c++) pl.off[c] = ent->pos[c];
					for (int c = 0; c < 3; c++) {
						pl.bmin[c] = DBL_MAX;
						pl.bmax[c] = -DBL_MAX;
					}
					for (int cx = 0; cx < 2; cx++)
						for (int cy = 0; cy < 2; cy++)
							for (int cz = 0; cz < 2; cz++) {
								float v[3] = {(float)(cx ? y.d.bmax[0] : y.d.bmin[0]),
											  (float)(cy ? y.d.bmax[1] : y.d.bmin[1]),
											  (float)(cz ? y.d.bmax[2] : y.d.bmin[2])};
								double o[3];
								lossantos_place_point(&pl, v, o);
								for (int c = 0; c < 3; c++) {
									if (o[c] < pl.bmin[c]) pl.bmin[c] = o[c];
									if (o[c] > pl.bmax[c]) pl.bmax[c] = o[c];
								}
							}
					lossantos_vec_push(places, pl);
					nplaced++;
					for (int t = 0; t < y.d.ntex; t++) {
						lossantos_texloc loc = {0};
						loc.embedded = 1;
						loc.entry = i;
						loc.nested = (uint32_t)bi;
						loc.archive = arch_id;
						loc.nested2 = UINT32_MAX;
						loc.tex_index = (uint16_t)t;
						lossantos_texset_offer(&avail, &y.d.tex[t], &loc);
					}
				}
			}
			// not placed, other blocks can still borrow these
			if (!nplaced && phase == 0) {
				for (int t = 0; t < y.d.ntex; t++) {
					lossantos_texinfo ti;
					memset(&ti, 0, sizeof ti);
					snprintf(ti.name, sizeof ti.name, "%s", y.d.tex[t].name);
					lossantos_lower(ti.key, y.d.tex[t].name, sizeof ti.key);
					ti.w = y.d.tex[t].width;
					ti.h = y.d.tex[t].height;
					ti.fmt = y.d.tex[t].format;
					ti.levels = y.d.tex[t].levels;
					ti.loc.embedded = 1;
					ti.loc.entry = i;
					ti.loc.nested = (uint32_t)bi;
					ti.loc.nested2 = UINT32_MAX;
					ti.loc.archive = arch_id;
					ti.loc.tex_index = (uint16_t)t;
					lossantos_vec_push(out->unplaced_tex, ti);
				}
			}
			if (nplaced) {
				free(y.d.tex);
				y.d.tex = NULL;
				y.d.ntex = 0;
				lossantos_vec_push(ydrs, y);
				out->placed += nplaced;
			} else {
				lossantos_ydr_free(&y.d);
			}
		}
		lossantos_hset_free(&seen);
	}

	for (size_t k = 0; k < order.n; k++) {
		uint32_t i = order.p[k];
		const lossantos_entry *e = &blk.e[i];
		if (e->kind != LOSSANTOS_RESOURCE || !lossantos_ends_with_ci(e->name, ".ytd")) continue;
		int n = parse_ytd_entry(&blk, i, &buf, tex, 4096);
		for (int t = 0; t < n; t++) {
			lossantos_texloc loc = {0};
			loc.entry = i;
			loc.nested = (uint32_t)bi;
						loc.archive = arch_id;
			loc.nested2 = UINT32_MAX;
			loc.tex_index = (uint16_t)t;
			lossantos_texset_offer(&avail, &tex[t], &loc);
		}
	}
	for (size_t t = 0; t < w->atlas.v.n; t++) lossantos_texset_offer_info(&avail, &w->atlas.v.p[t]);
	out->t_read = lossantos_now() - t0;

	if (!lossantos_strtab_init(&used)) goto done;
	have_used = 1;
	for (size_t d = 0; d < ydrs.n; d++)
		for (int g = 0; g < ydrs.p[d].d.ngeoms; g++) {
			char k[128];
			lossantos_lower(k, ydrs.p[d].d.geoms[g].texture, sizeof k);
			lossantos_strtab_id(&used, k);
		}
	// -3 available, >=0 near match, else nothing
	int64_t *nearv = (int64_t *)malloc((used.n ? used.n : 1) * sizeof *nearv);
	if (!nearv) goto done;
	for (uint32_t u = 0; u < used.n; u++) {
		nearv[u] = -2;
		int64_t a = lossantos_texset_find(&avail, used.s[u]);
		if (a >= 0) {
			lossantos_vec_push(items, (int)a);
			nearv[u] = -3;   /* available */
		} else {
			nearv[u] = lossantos_near_texture(&avail, used.s[u]);
			if (nearv[u] >= 0) lossantos_vec_push(items, (int)nearv[u]);   /* alias target (superset) */
		}
	}
	{
		char pref[160];
		snprintf(pref, sizeof pref, "models/gtav/%s/", b->stem);
		for (int c = 0; c < LOSSANTOS_CAT_COUNT; c++) {
			const char *pp = LOSSANTOS_PALETTE_NAMES[c];
			if (strncmp(pp, pref, strlen(pref)) != 0) continue;
			char k[128];
			lossantos_lower(k, strrchr(pp, '/') + 1, sizeof k);
			int64_t a = lossantos_texset_find(&avail, k);
			if (a >= 0) lossantos_vec_push(items, (int)a);
		}
	}
	{
		LOSSANTOS_VEC(int) own = {0};
		for (size_t i = 0; i < items.n; i++) {
			const lossantos_texinfo *t = &avail.v.p[items.p[i]];
			if (lossantos_texset_find(&w->atlas, t->key) >= 0) {
				if (phase == 0) lossantos_vec_push(out->atlas_items, *t);
			} else {
				lossantos_vec_push(own, items.p[i]);
				if (phase == 0) lossantos_vec_push(out->items, *t);
			}
		}
		if (!out->installs_given) {
			char prefix[160];
			snprintf(prefix, sizeof prefix, "models/gtav/%s", b->stem);
			out->installs.n = 0;
			int iok = lossantos_install_names(&avail, own.p, (int)own.n, prefix, &out->installs);
			if (!iok) {
				snprintf(out->error, sizeof out->error, "%s", lossantos_error());
				lossantos_vec_free(own);
				free(nearv);
				goto done;
			}
		}
		lossantos_vec_free(own);
	}
	if (phase == 0) {
		for (size_t i = 0; i < avail.v.n; i++)
			if (avail.v.p[i].loc.archive != 0xFFFF) lossantos_vec_push(out->local, avail.v.p[i]);
		for (size_t i = 0; i < out->unplaced_tex.n; i++) lossantos_vec_push(out->local, out->unplaced_tex.p[i]);
		for (uint32_t u = 0; u < used.n; u++) {
			const char *key = used.s[u];
			if (!key[0] || nearv[u] == -3) continue;
			int want = nearv[u] < 0;
			if (!want) {
				const char *nk = avail.v.p[nearv[u]].key;
				size_t nl = strlen(nk);
				want = nl >= 4 && strcmp(nk + nl - 4, "_lod") == 0 && !lossantos_ends_with_ci(key, "_lod");
			}
			if (want) lossantos_vec_push(out->missing, _strdup(key));
		}
		free(nearv);
		ok = 1;
		goto done;
	}

	for (size_t i = 0; i < out->installs.n; i++)
		texmat_set(out, &tmidx, out->installs.p[i].key, out->installs.p[i].path, 0);
	for (size_t i = 0; i < items.n; i++) {
		const lossantos_texinfo *t = &avail.v.p[items.p[i]];
		const char *rp = roads_path(w, t->key);
		if (rp && lossantos_texset_find(&w->atlas, t->key) >= 0) texmat_set(out, &tmidx, t->key, rp, 0);
	}
	for (uint32_t u = 0; u < used.n; u++) {
		if (nearv[u] == -3) continue;
		const char *key = used.s[u];
		char found[200];
		int64_t near = nearv[u];
		if (near >= 0) {
			const char *nk = avail.v.p[near].key;
			size_t nl = strlen(nk);
			int near_lod = nl >= 4 && strcmp(nk + nl - 4, "_lod") == 0;
			if (near_lod && !lossantos_ends_with_ci(key, "_lod") && lossantos_find_installed(&w->reg, key, found, sizeof found)) {
				texmat_set(out, &tmidx, key, found, 0);
				continue;
			}
			const lossantos_texmat *tm = texmat_get(out, &tmidx, nk);
			if (tm && !tm->drop && tm->path[0]) texmat_set(out, &tmidx, key, tm->path, 0);
			continue;
		}
		if (lossantos_find_installed(&w->reg, key, found, sizeof found)) {
			texmat_set(out, &tmidx, key, found, 0);
			continue;
		}
		const char *fb = lossantos_word_fallback(key);
		out->fallbacks++;
		if (fb) texmat_set(out, &tmidx, key, fb, 0);
		else {
			texmat_set(out, &tmidx, key, NULL, 1);
			out->dropped_keys++;
		}
	}
	free(nearv);

	{
		size_t total = 0;
		for (size_t pi = 0; pi < places.n; pi++) {
			const lossantos_drawable *d = &ydrs.p[places.p[pi].drawable].d;
			for (int g = 0; g < d->ngeoms; g++) total += d->geoms[g].ntris;
		}
		if (!lossantos_soup_reserve(&dsoup, total) || !lossantos_vec_reserve(dkey, total)) goto done;
		for (size_t pi = 0; pi < places.n; pi++) {
			const placement *pl = &places.p[pi];
			const lossantos_drawable *d = &ydrs.p[pl->drawable].d;
			for (int g = 0; g < d->ngeoms; g++) {
				const lossantos_geom *G = &d->geoms[g];
				char k[128];
				lossantos_lower(k, G->texture, sizeof k);
				uint32_t kid = (uint32_t)lossantos_strtab_find(&used, k);
				for (uint32_t t = 0; t < G->ntris; t++) {
					size_t o = dsoup.n++;
					dkey.p[dkey.n++] = kid;
					double fn[3] = {0, 0, 1};
					for (int c = 0; c < 3; c++) {
						uint32_t vi = G->idx[t * 3 + c];
						double wp[3];
						lossantos_place_point(pl, G->pos + (size_t)vi * 3, wp);
						for (int a = 0; a < 3; a++) dsoup.pos[o * 9 + c * 3 + a] = wp[a] * SCALE;
						dsoup.uv[o * 6 + c * 2] = G->uv[(size_t)vi * 2];
						dsoup.uv[o * 6 + c * 2 + 1] = G->uv[(size_t)vi * 2 + 1];
						if (G->nrm) {
							const float *n = G->nrm + (size_t)vi * 3;
							for (int a = 0; a < 3; a++)
								dsoup.nrm[o * 9 + c * 3 + a] =
									(double)n[0] * pl->rot[a][0] + (double)n[1] * pl->rot[a][1] + (double)n[2] * pl->rot[a][2];
						}
					}
					if (!G->nrm) {
						lossantos_tri_normal(dsoup.pos + o * 9, fn);
						for (int c = 0; c < 3; c++)
							for (int a = 0; a < 3; a++) dsoup.nrm[o * 9 + c * 3 + a] = fn[a];
					}
				}
			}
		}
	}

	{
		size_t n = combined.ntris;
		double *sc = (double *)malloc(n * 9 * sizeof(double));
		uint8_t *keep = (uint8_t *)malloc(n);
		if (!sc || !keep) { free(sc); free(keep); goto done; }
		for (size_t i = 0; i < n * 9; i++) sc[i] = combined.tri[i] * SCALE;
		lossantos_dedupe_tris(sc, n, &w->compat, keep);
		lossantos_coll_init(&out->coll);
		for (size_t i = 0; i < n; i++)
			if (keep[i]) lossantos_coll_push(&out->coll, sc + i * 9, sc + i * 9 + 3, sc + i * 9 + 6, combined.mat[i]);
		for (int k = 0; k < 3; k++) {
			out->coll.bmin[k] = combined.bmin[k];   /* GTA metres, informational */
			out->coll.bmax[k] = combined.bmax[k];
		}
		free(sc);
		free(keep);
	}

	{
		double tc = lossantos_now();
		size_t n = out->coll.ntris;
		out->coll_tris = n;
		double *col = (double *)malloc((n ? n : 1) * 9 * sizeof(double));
		double *cen = (double *)malloc((n ? n : 1) * 3 * sizeof(double));
		uint8_t *keep = (uint8_t *)malloc(n ? n : 1);
		// lower_b
		// 1  low part of a building box, 5 cm down
		// 2  road surface, 3 cm down
		uint8_t *lower_b = (uint8_t *)calloc(n ? n : 1, 1);
		uint8_t *high_any = (uint8_t *)calloc(n ? n : 1, 1);
		if (!col || !cen || !keep || !lower_b || !high_any) {
			free(col); free(cen); free(keep); free(lower_b); free(high_any);
			goto done;
		}
		memcpy(col, out->coll.tri, n * 9 * sizeof(double));
		memset(keep, 1, n);
		for (size_t i = 0; i < n; i++)
			for (int a = 0; a < 3; a++) cen[i * 3 + a] = (col[i * 9 + a] + col[i * 9 + 3 + a] + col[i * 9 + 6 + a]) / 3.0;
		static const char *const ROADW[] = {"sect", "ovly", "road", "_rd", "fwy", "tunnel", "tnl",
											"kerb", "pave", "walk", "median", "crossing", NULL};
		for (size_t pi = 0; pi < places.n; pi++) {
			const placement *pl = &places.p[pi];
			const char *src = ydrs.p[pl->drawable].low;
			double lo[3], hi[3];
			for (int c = 0; c < 3; c++) {
				lo[c] = pl->bmin[c] * SCALE;
				hi[c] = pl->bmax[c] * SCALE;
			}
			double height_m = (hi[2] - lo[2]) / SCALE;
			int surface = b->road || height_m <= 3.0;
			for (int k = 0; !surface && ROADW[k]; k++)
				if (strstr(src, ROADW[k])) surface = 1;
			for (size_t i = 0; i < n; i++) {
				const double *c = cen + i * 3;
				int inxy = c[0] >= lo[0] && c[0] <= hi[0] && c[1] >= lo[1] && c[1] <= hi[1];
				if (!inxy) continue;
				if (surface) {
					lower_b[i] |= 2;            /* road lowering (applied once) */
				} else {
					if (c[2] > lo[2] + 2.0 * SCALE) high_any[i] = 1;
					else lower_b[i] |= 1;
				}
			}
		}
		for (size_t i = 0; i < n; i++)
			if (lower_b[i] & 2)
				for (int c = 0; c < 3; c++) col[i * 9 + c * 3 + 2] -= 0.03 * SCALE;
		// cover art, stuff you cant see through
		size_t nart = 0;
		double *art = (double *)malloc((dsoup.n ? dsoup.n : 1) * 9 * sizeof(double));
		if (!art) { free(col); free(cen); free(keep); free(lower_b); free(high_any); goto done; }
		for (size_t t = 0; t < dsoup.n; t++)
			if (cover_key(out, &tmidx, used.s[dkey.p[t]])) memcpy(art + 9 * nart++, dsoup.pos + t * 9, 72);
		// shell (G3)
		{
			size_t nh = 0;
			for (size_t i = 0; i < n; i++) nh += high_any[i];
			if (nh) {
				double *ht = (double *)malloc(nh * 72);
				uint8_t *cv = (uint8_t *)malloc(nh);
				size_t k = 0;
				for (size_t i = 0; i < n; i++)
					if (high_any[i]) memcpy(ht + 9 * k++, col + i * 9, 72);
				lossantos_shell_covered(ht, nh, art, nart, cv);
				k = 0;
				for (size_t i = 0; i < n; i++)
					if (high_any[i]) {
						if (cv[k]) { keep[i] = 0; out->shell_dropped++; }
						k++;
					}
				out->shell_tested = (uint32_t)nh;
				free(ht);
				free(cv);
			}
		}
		// filler (G4b)
		if (places.n) {
			size_t nc = 0;
			for (size_t i = 0; i < n; i++) nc += keep[i];
			double *ct = (double *)malloc((nc ? nc : 1) * 72);
			uint8_t *cv = (uint8_t *)malloc(nc ? nc : 1);
			size_t k = 0;
			for (size_t i = 0; i < n; i++)
				if (keep[i]) memcpy(ct + 9 * k++, col + i * 9, 72);
			lossantos_filler_covered(ct, nc, art, nart, cv);
			k = 0;
			for (size_t i = 0; i < n; i++)
				if (keep[i]) {
					if (cv[k]) { keep[i] = 0; out->filler_dropped++; }
					k++;
				}
			out->filler_tested = (uint32_t)nc;
			free(ct);
			free(cv);
		}
		free(art);
		for (size_t i = 0; i < n; i++)
			if ((lower_b[i] & 1) && !high_any[i])
				for (int c = 0; c < 3; c++) col[i * 9 + c * 3 + 2] -= 0.05 * SCALE;
		out->t_cover = lossantos_now() - tc;

		double *nrm = (double *)malloc((n ? n : 1) * 3 * sizeof(double));
		uint8_t *cat = (uint8_t *)malloc(n ? n : 1);
		if (!nrm || !cat) { free(nrm); free(cat); free(col); free(cen); free(keep); free(lower_b); free(high_any); goto done; }
		for (size_t i = 0; i < n; i++) lossantos_tri_normal(col + i * 9, nrm + i * 3);
		lossantos_categories(col, nrm, out->coll.mat, n, &w->compat, cat);
		uint32_t palette_id[LOSSANTOS_CAT_COUNT];
		for (int c = 0; c < LOSSANTOS_CAT_COUNT; c++) palette_id[c] = lossantos_strtab_id(&out->mats, LOSSANTOS_PALETTE_NAMES[c]);

		size_t nk = 0;
		for (size_t i = 0; i < n; i++) nk += keep[i];
		if (!lossantos_soup_reserve(&out->vis, nk + dsoup.n)) { free(nrm); free(cat); free(col); free(cen); free(keep); free(lower_b); free(high_any); goto done; }
		for (size_t i = 0; i < n; i++) {
			if (!keep[i]) continue;
			size_t o = out->vis.n++;
			memcpy(out->vis.pos + o * 9, col + i * 9, 72);
			for (int c = 0; c < 3; c++) memcpy(out->vis.nrm + o * 9 + c * 3, nrm + i * 3, 24);
			lossantos_planar_uv(col + i * 9, nrm + i * 3, LOSSANTOS_PALETTE_METRES[cat[i]] * SCALE, out->vis.uv + o * 6);
			out->vis.mat[o] = palette_id[cat[i]];
		}
		out->coll_kept = nk;
		uint32_t *kmat = (uint32_t *)malloc((used.n ? used.n : 1) * sizeof *kmat);
		for (uint32_t u = 0; u < used.n; u++) {
			const lossantos_texmat *tm = texmat_get(out, &tmidx, used.s[u]);
			if (tm && tm->drop) kmat[u] = UINT32_MAX;
			else if (!tm || !tm->path[0]) kmat[u] = palette_id[LOSSANTOS_CAT_WALL];
			else kmat[u] = lossantos_strtab_id(&out->mats, tm->path);
		}
		for (size_t t = 0; t < dsoup.n; t++) {
			uint32_t m = kmat[dkey.p[t]];
			if (m == UINT32_MAX) continue;
			size_t o = out->vis.n++;
			memcpy(out->vis.pos + o * 9, dsoup.pos + t * 9, 72);
			memcpy(out->vis.nrm + o * 9, dsoup.nrm + t * 9, 72);
			memcpy(out->vis.uv + o * 6, dsoup.uv + t * 6, 48);
			out->vis.mat[o] = m;
		}
		free(kmat);
		free(nrm); free(cat); free(col); free(cen); free(keep); free(lower_b); free(high_any);
	}
	ok = 1;

done:
	if (!ok && !out->error[0]) snprintf(out->error, sizeof out->error, "%s", lossantos_error());
	out->t_total = lossantos_now() - t0;
	lossantos_soup_free(&dsoup);
	lossantos_vec_free(dkey);
	lossantos_hmap_free(&tmidx);
	if (have_used) lossantos_strtab_free(&used);
	lossantos_vec_free(items);
	if (have_avail) lossantos_texset_free(&avail);
	lossantos_ybn_free(&combined);
	for (size_t k = 0; k < ybns.n; k++) lossantos_ybn_free(&ybns.p[k]);
	lossantos_vec_free(ybns);
	for (size_t k = 0; k < ydrs.n; k++) lossantos_ydr_free(&ydrs.p[k].d);
	lossantos_vec_free(ydrs);
	lossantos_vec_free(places);
	lossantos_vec_free(order);
	free(ents);
	free(arch);
	free(tex);
	lossantos_buf_free(&buf);
	if (have_meta) lossantos_rpf_close(&meta);
	if (have_blk) lossantos_rpf_close(&blk);
	if (have_outer) lossantos_rpf_close(&outer);
	return ok;
}
