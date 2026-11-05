#include "lossantos_slod.h"
#include "lossantos_common.h"
#include "lossantos_ydr.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const CITY[] = {"x64i.rpf", "x64j.rpf", "x64k.rpf", "x64l.rpf",
								   "x64m.rpf", "x64n.rpf", "x64o.rpf", "x64p.rpf"};
static const char *const DIST[] = {"_citye", "_cityw", "_hills"};

typedef struct {
	int outer;
	char lod[300];
	char meta[300];
} lodpack;

typedef LOSSANTOS_VEC(lodpack) lodpack_vec;

typedef struct {
	char base[256];
	LOSSANTOS_VEC(uint32_t) items;    /* entry indices */
} lodir;

void lossantos_slod_free(lossantos_slod_out *o) {
	lossantos_soup_free(&o->vis);
	lossantos_strtab_free(&o->mats);
	lossantos_vec_free(o->installs);
}

// slod.discover_lods
static int discover(lossantos_world *w, lodpack_vec *packs) {
	char path[1024];
	for (int ai = 0; ai < 8; ai++) {
		wchar_t p[1024];
		swprintf(p, 1024, L"%ls\\%hs", w->gta_dir, CITY[ai]);
		lossantos_rpf outer;
		if (!lossantos_rpf_open(&outer, w->ng, p)) continue;
		lossantos_u32vec order = {0};
		lossantos_rpf_walk(&outer, &order);
		LOSSANTOS_VEC(lodir) dirs = {0};
		for (size_t k = 0; k < order.n; k++) {
			const lossantos_entry *e = &outer.e[order.p[k]];
			if (!lossantos_ends_with_ci(e->name, ".rpf")) continue;
			lossantos_rpf_path(&outer, order.p[k], path, sizeof path);
			const char *lv = strstr(path, "levels/gta5/");
			if (!lv) continue;
			const char *rel = lv + 12, *slash = strrchr(rel, '/');
			if (!slash || !strchr(rel, '/')) continue;
			int dist = 0;
			for (int d = 0; d < 3; d++)
				if (strncmp(rel, DIST[d], strlen(DIST[d])) == 0) dist = 1;
			if (!dist) continue;
			char low[128];
			lossantos_lower(low, e->name, sizeof low);
			if (!strstr(low, "lod")) continue;
			char base[256];
			snprintf(base, sizeof base, "%.*s", (int)(slash - rel), rel);
			lodir *dd = NULL;
			for (size_t i = 0; i < dirs.n; i++)
				if (strcmp(dirs.p[i].base, base) == 0) dd = &dirs.p[i];
			if (!dd) {
				lodir nd;
				memset(&nd, 0, sizeof nd);
				snprintf(nd.base, sizeof nd.base, "%s", base);
				lossantos_vec_push(dirs, nd);
				dd = &dirs.p[dirs.n - 1];
			}
			lossantos_vec_push(dd->items, order.p[k]);
		}
		for (size_t di = 0; di < dirs.n; di++) {
			lodir *d = &dirs.p[di];
			// metadata rpf of this folder
			char meta[300] = "";
			for (size_t i = 0; i < d->items.n && !meta[0]; i++) {
				char low[128];
				lossantos_lower(low, outer.e[d->items.p[i]].name, sizeof low);
				if (strstr(low, "metadata")) {
					lossantos_rpf_path(&outer, d->items.p[i], path, sizeof path);
					snprintf(meta, sizeof meta, "%s", strstr(path, "levels/gta5/"));
				}
			}
			if (!meta[0]) {
				char needle[300];
				snprintf(needle, sizeof needle, "levels/gta5/%s/", d->base);
				for (size_t k = 0; k < order.n && !meta[0]; k++) {
					const lossantos_entry *e = &outer.e[order.p[k]];
					char low[128];
					lossantos_lower(low, e->name, sizeof low);
					if (!lossantos_ends_with_ci(e->name, ".rpf") || !strstr(low, "metadata")) continue;
					lossantos_rpf_path(&outer, order.p[k], path, sizeof path);
					if (strstr(path, needle)) snprintf(meta, sizeof meta, "levels/gta5/%s/%s", d->base, e->name);
				}
			}
			for (size_t i = 0; i < d->items.n; i++) {
				char low[128];
				lossantos_lower(low, outer.e[d->items.p[i]].name, sizeof low);
				if (strstr(low, "metadata") || !meta[0]) continue;
				lodpack lp;
				memset(&lp, 0, sizeof lp);
				lp.outer = ai;
				lossantos_rpf_path(&outer, d->items.p[i], path, sizeof path);
				snprintf(lp.lod, sizeof lp.lod, "%s", strstr(path, "levels/gta5/"));
				snprintf(lp.meta, sizeof lp.meta, "%s", meta);
				lossantos_vec_push(*packs, lp);
			}
			lossantos_vec_free(d->items);
		}
		lossantos_vec_free(dirs);
		lossantos_vec_free(order);
		lossantos_rpf_close(&outer);
	}
	return 1;
}

int lossantos_slod_build(lossantos_world *w, lossantos_slod_out *out) {
	memset(out, 0, sizeof *out);
	lossantos_strtab_init(&out->mats);
	lodpack_vec packs = {0};
	discover(w, &packs);
	out->packs = (uint32_t)packs.n;
	lossantos_texset tex;
	lossantos_texset_init(&tex);
	lossantos_strtab keys;               /* geometry texture keys */
	lossantos_strtab_init(&keys);
	lossantos_soup tmp = {0};            /* triangles with key ids in .mat */
	lossantos_hset seen;
	lossantos_hset_init(&seen, 4096);
	lossantos_buf buf = {0};
	lossantos_tex *tx = (lossantos_tex *)malloc(4096 * sizeof *tx);
	lossantos_archetype *arch = (lossantos_archetype *)malloc(65536 * sizeof *arch);
	lossantos_entity *ents = (lossantos_entity *)malloc(262144 * sizeof *ents);
	int ok = tx && arch && ents;
	for (size_t pi = 0; ok && pi < packs.n; pi++) {
		const lodpack *lp = &packs.p[pi];
		wchar_t p[1024];
		swprintf(p, 1024, L"%ls\\%hs", w->gta_dir, CITY[lp->outer]);
		lossantos_rpf outer, meta, lod;
		if (!lossantos_rpf_open(&outer, w->ng, p)) continue;
		int32_t mi = lossantos_rpf_find(&outer, lp->meta), li = lossantos_rpf_find(&outer, lp->lod);
		int narch = 0, nents = 0;
		if (mi >= 0 && lossantos_rpf_open_child(&meta, &outer, (uint32_t)mi)) {
			lossantos_u32vec mo = {0};
			lossantos_rpf_walk(&meta, &mo);
			for (size_t k = 0; k < mo.n; k++) {
				const lossantos_entry *e = &meta.e[mo.p[k]];
				if (e->kind == LOSSANTOS_DIR || !lossantos_ends_with_ci(e->name, ".ytyp") || !lossantos_rpf_read(&meta, mo.p[k], &buf)) continue;
				int n = lossantos_ytyp_archetypes(buf.p, buf.n, lossantos_rpf_size_from_flags(e->sys_flags), arch + narch, 65536 - narch);
				if (n > 0) narch += n;
			}
			for (size_t k = 0; k < mo.n; k++) {
				const lossantos_entry *e = &meta.e[mo.p[k]];
				if (e->kind == LOSSANTOS_DIR || !lossantos_ends_with_ci(e->name, ".ymap") || !lossantos_rpf_read(&meta, mo.p[k], &buf)) continue;
				int n = lossantos_ymap_entities(buf.p, buf.n, lossantos_rpf_size_from_flags(e->sys_flags), ents + nents, 262144 - nents);
				if (n > 0) nents += n;
			}
			lossantos_vec_free(mo);
			lossantos_rpf_close(&meta);
		}
		if (li < 0 || !lossantos_rpf_open_child(&lod, &outer, (uint32_t)li)) {
			lossantos_rpf_close(&outer);
			continue;
		}
		lossantos_u32vec lo = {0};
		lossantos_rpf_walk(&lod, &lo);
		// the pack's dictionaries
		for (size_t k = 0; k < lo.n; k++) {
			const lossantos_entry *e = &lod.e[lo.p[k]];
			if (e->kind != LOSSANTOS_RESOURCE || !lossantos_ends_with_ci(e->name, ".ytd") || !lossantos_rpf_read(&lod, lo.p[k], &buf)) continue;
			lossantos_rsc r;
			if (!lossantos_rsc_init(&r, buf.p, buf.n, lossantos_rpf_size_from_flags(e->sys_flags))) continue;
			int n = lossantos_ytd_parse(&r, LOSSANTOS_SYS_BASE, tx, 4096);
			for (int t = 0; t < n; t++) {
				lossantos_texloc loc = {0};
				loc.archive = (uint16_t)lp->outer;
				loc.nested = (uint32_t)li;
				loc.nested2 = UINT32_MAX;
				loc.entry = lo.p[k];
				loc.tex_index = (uint16_t)t;
				lossantos_texset_offer(&tex, &tx[t], &loc);
			}
		}
		for (size_t k = 0; k < lo.n; k++) {
			const lossantos_entry *e = &lod.e[lo.p[k]];
			if (e->kind == LOSSANTOS_DIR || !lossantos_ends_with_ci(e->name, ".ydr")) continue;
			char low[128];
			lossantos_lower(low, e->name, sizeof low);
			if (strstr(low, "emissive") || strstr(low, "_emi_") || strstr(low, "proxy")) continue;
			if (!lossantos_rpf_read(&lod, lo.p[k], &buf)) continue;
			lossantos_drawable d;
			if (!lossantos_ydr_parse(buf.p, buf.n, lossantos_rpf_size_from_flags(e->sys_flags), &d)) {
				lossantos_ydr_free(&d);
				continue;
			}
			int ai = lossantos_match_archetype(e->name, d.bmin, d.bmax, arch, narch, 300.0f);  // way looser than blocks (8)
			if (ai >= 0) {
				for (int en = 0; en < nents; en++) {
					const lossantos_entity *ent = &ents[en];
					if (ent->archetype != arch[ai].name) continue;
					// dedupe on (name, round(pos, 1))
					int32_t q[3];
					for (int c = 0; c < 3; c++) q[c] = (int32_t)nearbyintf(ent->pos[c] * 10.0f);
					uint64_t sk = lossantos_hash_bytes(low, strlen(low)) ^ lossantos_hash_bytes(q, sizeof q);
					if (lossantos_hset_add(&seen, sk) != 1) continue;
					lossantos_placement pl;
					memset(&pl, 0, sizeof pl);
					lossantos_quat_matrix(ent->rot, pl.rot);
					pl.scale[0] = pl.scale[1] = ent->scale_xy;
					pl.scale[2] = ent->scale_z;
					for (int c = 0; c < 3; c++) pl.off[c] = ent->pos[c];
					out->placed++;
					for (int t = 0; t < d.ntex; t++) {
						lossantos_texloc loc = {0};
						loc.archive = (uint16_t)lp->outer;
						loc.nested = (uint32_t)li;
						loc.nested2 = UINT32_MAX;
						loc.entry = lo.p[k];
						loc.embedded = 1;
						loc.tex_index = (uint16_t)t;
						lossantos_texset_offer(&tex, &d.tex[t], &loc);
					}
					for (int g = 0; g < d.ngeoms; g++) {
						const lossantos_geom *G = &d.geoms[g];
						char kk[128];
						lossantos_lower(kk, G->texture, sizeof kk);
						uint32_t kid = lossantos_strtab_id(&keys, kk);
						if (!lossantos_soup_reserve(&tmp, tmp.n + G->ntris)) { ok = 0; break; }
						for (uint32_t t = 0; t < G->ntris; t++) {
							size_t o = tmp.n++;
							for (int c = 0; c < 3; c++) {
								uint32_t vi = G->idx[t * 3 + c];
								double wp[3];
								lossantos_place_point(&pl, G->pos + (size_t)vi * 3, wp);
								for (int a = 0; a < 3; a++) tmp.pos[o * 9 + c * 3 + a] = wp[a] * LOSSANTOS_SCALE;
								tmp.uv[o * 6 + c * 2] = G->uv[(size_t)vi * 2];
								tmp.uv[o * 6 + c * 2 + 1] = G->uv[(size_t)vi * 2 + 1];
								if (G->nrm)
									for (int a = 0; a < 3; a++) {
										const float *n = G->nrm + (size_t)vi * 3;
										tmp.nrm[o * 9 + c * 3 + a] = (double)n[0] * pl.rot[a][0] +
																	 (double)n[1] * pl.rot[a][1] +
																	 (double)n[2] * pl.rot[a][2];
									}
							}
							if (!G->nrm) {
								double fn[3];
								lossantos_tri_normal(tmp.pos + o * 9, fn);
								for (int c = 0; c < 3; c++) memcpy(tmp.nrm + o * 9 + c * 3, fn, 24);
							}
							tmp.mat[o] = kid;
						}
					}
				}
			}
			lossantos_ydr_free(&d);
		}
		lossantos_vec_free(lo);
		lossantos_rpf_close(&lod);
		lossantos_rpf_close(&outer);
	}
	if (ok) {
		// every pack texture under models/gtav/slod
		int *ix = (int *)malloc((tex.v.n ? tex.v.n : 1) * sizeof *ix);
		for (size_t i = 0; i < tex.v.n; i++) ix[i] = (int)i;
		ok = lossantos_install_names(&tex, ix, (int)tex.v.n, "models/gtav/slod", &out->installs);
		free(ix);
	}
	uint32_t *kmat = (uint32_t *)malloc((keys.n ? keys.n : 1) * sizeof *kmat);
	// installed, else borrowed, else the fallback wall
	for (uint32_t k = 0; ok && k < keys.n; k++) {
		const char *path = NULL;
		// linear scan per key, slow but whatever
		for (size_t i = 0; i < out->installs.n; i++)
			if (strcmp(out->installs.p[i].key, keys.s[k]) == 0) {
				path = out->installs.p[i].path;
				break;
			}
		char found[200];
		if (!path && lossantos_find_installed(&w->reg, keys.s[k], found, sizeof found)) path = found;
		if (!path) path = LOSSANTOS_SLOD_FALLBACK;
		kmat[k] = lossantos_strtab_id(&out->mats, path);
	}
	if (ok && lossantos_soup_reserve(&out->vis, tmp.n)) {
		for (size_t t = 0; t < tmp.n; t++) {
			size_t o = out->vis.n++;
			memcpy(out->vis.pos + o * 9, tmp.pos + t * 9, 72);
			memcpy(out->vis.nrm + o * 9, tmp.nrm + t * 9, 72);
			memcpy(out->vis.uv + o * 6, tmp.uv + t * 6, 48);
			out->vis.mat[o] = kmat[tmp.mat[t]];
			if (strcmp(out->mats.s[out->vis.mat[o]], LOSSANTOS_SLOD_FALLBACK) == 0) out->fallback_geoms++;
		}
	}
	free(kmat);
	lossantos_soup_free(&tmp);
	lossantos_strtab_free(&keys);
	lossantos_texset_free(&tex);
	lossantos_hset_free(&seen);
	lossantos_buf_free(&buf);
	free(tx);
	free(arch);
	free(ents);
	lossantos_vec_free(packs);
	if (!ok) snprintf(out->error, sizeof out->error, "%s", lossantos_error());
	return ok;
}
