#include "lossantos_stage.h"
#include "lossantos_out.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int64_t gx, gy;
	uint32_t i;
} binned;

static int cmp_binned(const void *a, const void *b) {
	const binned *x = (const binned *)a, *y = (const binned *)b;
	if (x->gx != y->gx) return x->gx < y->gx ? -1 : 1;
	if (x->gy != y->gy) return x->gy < y->gy ? -1 : 1;
	return x->i < y->i ? -1 : x->i > y->i;
}

static binned *bin_tris(const double *pos, size_t n, double cell) {
	binned *b = (binned *)malloc((n ? n : 1) * sizeof *b);
	if (!b) return NULL;
	for (size_t i = 0; i < n; i++) {
		const double *t = pos + i * 9;
		b[i].gx = (int64_t)floor((t[0] + t[3] + t[6]) / 3.0 / cell);
		b[i].gy = (int64_t)floor((t[1] + t[4] + t[7]) / 3.0 / cell);
		b[i].i = (uint32_t)i;
	}
	qsort(b, n, sizeof *b, cmp_binned);
	return b;
}

static void json_str(FILE *f, const char *s) {
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') fputc('\\', f);
		fputc(*s, f);
	}
	fputc('"', f);
}

int lossantos_stage_write(const wchar_t *dir, const char *map_name, const lossantos_block_out *o) {
	wchar_t p[1024];
	CreateDirectoryW(dir, NULL);
	swprintf(p, 1024, L"%ls\\tiles", dir);
	CreateDirectoryW(p, NULL);
	swprintf(p, 1024, L"%ls\\phys", dir);
	CreateDirectoryW(p, NULL);
	swprintf(p, 1024, L"%ls\\index.json", dir);
	FILE *idx = _wfopen(p, L"w");
	if (!idx) return 0;
	fprintf(idx, "{\"map\":");
	json_str(idx, map_name);
	fprintf(idx, ",\"tile_size\":1024,\"cell_size\":8192,\"phys_chunk_size\":1024,\"materials\":{");
	for (uint32_t m = 0; m < o->mats.n; m++) {
		fprintf(idx, "%s\"%u\":", m ? "," : "", m);
		json_str(idx, o->mats.s[m]);
	}
	fprintf(idx, "},\"tiles\":{");

	lossantos_tilebuild tb;
	if (!lossantos_tb_init(&tb)) {
		fclose(idx);
		return 0;
	}
	binned *b = bin_tris(o->vis.pos, o->vis.n, 1024.0);
	int first = 1, ok = b != NULL;
	uint32_t *bym = (uint32_t *)malloc((o->vis.n ? o->vis.n : 1) * sizeof *bym);
	for (size_t s = 0; ok && s < o->vis.n;) {
		size_t e = s;
		while (e < o->vis.n && b[e].gx == b[s].gx && b[e].gy == b[s].gy) e++;
		lossantos_tb_reset(&tb);
		lossantos_tb_sided(&tb, !o->double_sided);
		size_t k = 0;
		for (size_t i = s; i < e; i++) bym[k++] = b[i].i;
		for (uint32_t m = 0; m < o->mats.n; m++) {
			int started = 0;
			for (size_t i = 0; i < k; i++) {
				uint32_t t = bym[i];
				if (o->vis.mat[t] != m) continue;
				if (!started) {
					lossantos_tb_material(&tb, o->mats.s[m]);
					started = 1;
				}
				lossantos_tb_tri(&tb, o->vis.pos + (size_t)t * 9, o->vis.nrm + (size_t)t * 9, o->vis.uv + (size_t)t * 6);
			}
		}
		swprintf(p, 1024, L"%ls\\tiles\\t_%lld_%lld_0.bin", dir, b[s].gx, b[s].gy);
		if (lossantos_tb_write(&tb, p) == 1) {
			char fn[96];
			snprintf(fn, sizeof fn, "t_%lld_%lld_0.bin", b[s].gx, b[s].gy);
			fprintf(idx, "%s\"%lld %lld\":{", first ? "" : ",", b[s].gx, b[s].gy);
			// every lod points at the same file
			for (int l = 0; l < 5; l++) fprintf(idx, "%s\"lod%d\":{\"file\":\"%s\"}", l ? "," : "", l, fn);
			fputc('}', idx);
			first = 0;
		}
		s = e;
	}
	free(bym);
	free(b);
	lossantos_tb_free(&tb);

	fprintf(idx, "},\"phys_chunks\":{");
	b = bin_tris(o->coll.tri, o->coll.ntris, 1024.0);
	double *tri = (double *)malloc((o->coll.ntris ? o->coll.ntris : 1) * 72);
	first = 1;
	for (size_t s = 0; b && tri && s < o->coll.ntris;) {
		size_t e = s;
		while (e < o->coll.ntris && b[e].gx == b[s].gx && b[e].gy == b[s].gy) e++;
		for (size_t i = s; i < e; i++) memcpy(tri + (i - s) * 9, o->coll.tri + (size_t)b[i].i * 9, 72);
		swprintf(p, 1024, L"%ls\\phys\\g_%lld_%lld.bin", dir, b[s].gx, b[s].gy);
		size_t dropped;
		if (lossantos_write_owc1(p, tri, e - s, 1, &dropped) > 0) {
			fprintf(idx, "%s\"%lld %lld\":{\"file\":\"g_%lld_%lld.bin\"}", first ? "" : ",", b[s].gx, b[s].gy,
					b[s].gx, b[s].gy);
			first = 0;
		}
		s = e;
	}
	free(tri);
	free(b);
	fprintf(idx, "}}\n");
	fclose(idx);
	return ok;
}
