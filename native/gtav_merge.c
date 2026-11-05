#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// GVT1  32 hdr, 16 subs, 64 names, 28 verts, u16 idx
// GVC1  20 hdr, 12 verts, u16 idx
#define TILE_MAGIC   0x31545647u
#define CHUNK_MAGIC  0x31435647u
#define TILE_HDR     32
#define CHUNK_HDR    20
#define SUB_STRIDE   16
#define NAME_LEN     64
#define VERT_STRIDE  28
#define CHUNK_VERT   12
#define IDX_STRIDE   2
#define MAX_VERTS    65535


static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }


typedef struct { uint8_t *p; size_t n; } buf;

static void buf_free(buf *b) { free(b->p); b->p = NULL; b->n = 0; }

static buf read_file(const char *path) {
	buf b = { NULL, 0 };
	FILE *f = fopen(path, "rb");
	if (!f) return b;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return b; }
	long sz = ftell(f);
	if (sz <= 0) { fclose(f); return b; }
	rewind(f);
	b.p = (uint8_t *)malloc((size_t)sz);
	if (!b.p) { fclose(f); return b; }
	if (fread(b.p, 1, (size_t)sz, f) != (size_t)sz) {
		free(b.p);
		fclose(f);
		b.p = NULL;
		return b;
	}
	fclose(f);
	b.n = (size_t)sz;
	return b;
}

static int write_file(const char *path, const void *data, size_t len) {
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	size_t put = fwrite(data, 1, len, f);
	return fclose(f) == 0 && put == len;
}


typedef struct {
	uint32_t vc, ic, sc, mc;
	const uint8_t *subs;    /* sc * 16 */
	const uint8_t *names;   /* mc * 64 */
	const uint8_t *verts;   /* vc * 28 */
	const uint8_t *idx;     /* ic * 2 */
} tile_view;

static int tile_parse(const buf *b, tile_view *t) {
	if (b->n < TILE_HDR) return 0;
	if (rd32(b->p) != TILE_MAGIC) return 0;

	t->vc = rd32(b->p + 8);
	t->ic = rd32(b->p + 12);
	t->sc = rd32(b->p + 16);
	t->mc = rd32(b->p + 20);

	size_t head = TILE_HDR + (size_t)t->sc * SUB_STRIDE + (size_t)t->mc * NAME_LEN;
	size_t need = head + (size_t)t->vc * VERT_STRIDE + (size_t)t->ic * IDX_STRIDE;
	if (b->n < need) return 0;

	t->subs = b->p + TILE_HDR;
	t->names = b->p + TILE_HDR + (size_t)t->sc * SUB_STRIDE;
	t->verts = b->p + head;
	t->idx = b->p + head + (size_t)t->vc * VERT_STRIDE;
	return 1;
}


typedef struct {
	uint8_t *p;
	uint32_t count, cap;
} nametab;

static void names_free(nametab *t) { free(t->p); t->p = NULL; t->count = t->cap = 0; }

static int names_find(const nametab *t, const uint8_t *n) {
	for (uint32_t i = 0; i < t->count; i++) {
		if (strncmp((const char *)t->p + (size_t)i * NAME_LEN,
					(const char *)n, NAME_LEN) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static int names_add(nametab *t, const uint8_t *n) {
	int found = names_find(t, n);
	if (found >= 0) return found;
	if (t->count == t->cap) {
		uint32_t cap = t->cap ? t->cap * 2 : 16;
		uint8_t *p = (uint8_t *)realloc(t->p, (size_t)cap * NAME_LEN);
		if (!p) return -1;
		t->p = p;
		t->cap = cap;
	}
	memcpy(t->p + (size_t)t->count * NAME_LEN, n, NAME_LEN);
	return (int)t->count++;
}


typedef struct {
	buf b;
	tile_view t;
} tile_in;

// one (input, material) run
typedef struct {
	uint32_t in;              /* index into the kept[] input array */
	const uint8_t *name;      /* 64-byte material name in that input's table */
	uint32_t *used;           /* sorted unique vertex ids, `nused` of them */
	uint32_t nused;
	uint32_t *local;          /* `nidx` ids into `used` */
	uint32_t nidx;
	uint32_t name_id;         /* merged name table id, filled when placed */
	uint32_t ioff;            /* output index offset of this run */
	uint32_t voff;            /* submesh vertexOffset: its material block base */
	uint32_t vadd;            /* added to `local` to index within the block */
	int kept;
} run;

typedef struct {
	const uint8_t *name;
	uint32_t *runs;
	uint32_t nruns, cap;
} matgroup;

static int cmp_u32(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static uint32_t lower_bound_u32(const uint32_t *a, uint32_t n, uint32_t v) {
	uint32_t lo = 0, hi = n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (a[mid] < v) lo = mid + 1; else hi = mid;
	}
	return lo;
}

static long merge_tile_group(const char **ins, int nin, const char *out,
							 long *dropped) {
	*dropped = 0;

	tile_in *kept = (tile_in *)calloc((size_t)(nin > 0 ? nin : 1), sizeof(tile_in));
	if (!kept) return -1;

	run *runs = NULL;
	uint32_t nruns = 0, runs_cap = 0;
	matgroup *mats = NULL;
	uint32_t nmats = 0, mats_cap = 0;
	nametab names = { NULL, 0, 0 };
	int nkept = 0, failed = 0;

	for (int i = 0; i < nin && !failed; i++) {
		buf b = read_file(ins[i]);
		tile_view t;
		if (!b.p || !tile_parse(&b, &t)) { buf_free(&b); failed++; continue; }

		int sane = 1;
		for (uint32_t s = 0; s < t.sc; s++) {
			if (rd32(t.subs + (size_t)s * SUB_STRIDE) >= t.mc) { sane = 0; break; }
		}
		if (!sane) { buf_free(&b); failed++; continue; }

		int slot = nkept;
		kept[slot].b = b;
		kept[slot].t = t;
		nkept++;

		for (uint32_t s = 0; s < t.sc; s++) {
			const uint8_t *s32 = t.subs + (size_t)s * SUB_STRIDE;
			uint32_t mat  = rd32(s32);
			uint32_t ioff = rd32(s32 + 4);
			uint32_t icnt = rd32(s32 + 8);
			int32_t  voff = (int32_t)rd32(s32 + 12);
			if (icnt == 0 || (uint64_t)ioff + icnt > t.ic) continue;

			uint32_t *ids = (uint32_t *)malloc((size_t)icnt * sizeof(uint32_t));
			uint32_t *srt = (uint32_t *)malloc((size_t)icnt * sizeof(uint32_t));
			if (!ids || !srt) { free(ids); free(srt); failed++; break; }

			int bad = 0;
			for (uint32_t k = 0; k < icnt; k++) {
				int64_t v = (int64_t)rd16(t.idx + (size_t)(ioff + k) * IDX_STRIDE)
						  + (int64_t)voff;
				if (v < 0 || (uint64_t)v >= t.vc) { bad = 1; break; }
				ids[k] = (uint32_t)v;
				srt[k] = (uint32_t)v;
			}
			if (bad) {
				free(ids); free(srt); failed++; continue;
			}

			// sorted unique, same as np.unique in city.py
			qsort(srt, icnt, sizeof(uint32_t), cmp_u32);
			uint32_t nused = 0;
			for (uint32_t k = 0; k < icnt; k++) {
				if (nused == 0 || srt[nused - 1] != srt[k]) srt[nused++] = srt[k];
			}
			uint32_t *local = (uint32_t *)malloc((size_t)icnt * sizeof(uint32_t));
			if (!local) { free(ids); free(srt); failed++; break; }
			for (uint32_t k = 0; k < icnt; k++) {
				local[k] = lower_bound_u32(srt, nused, ids[k]);
			}
			free(ids);

			if (nruns == runs_cap) {
				uint32_t cap = runs_cap ? runs_cap * 2 : 64;
				run *p = (run *)realloc(runs, (size_t)cap * sizeof(run));
				if (!p) { free(srt); free(local); failed++; break; }
				runs = p; runs_cap = cap;
			}
			run *R = &runs[nruns++];
			R->in = (uint32_t)slot;
			R->name = t.names + (size_t)mat * NAME_LEN;
			R->used = srt;
			R->nused = nused;
			R->local = local;
			R->nidx = icnt;
			R->name_id = 0; R->ioff = 0; R->voff = 0; R->vadd = 0; R->kept = 0;

			// by name, dense ids collide between packs
			matgroup *g = NULL;
			for (uint32_t m = 0; m < nmats; m++) {
				if (strncmp((const char *)mats[m].name, (const char *)R->name,
							NAME_LEN) == 0) { g = &mats[m]; break; }
			}
			if (!g) {
				if (nmats == mats_cap) {
					uint32_t cap = mats_cap ? mats_cap * 2 : 16;
					matgroup *p = (matgroup *)realloc(mats, (size_t)cap * sizeof(matgroup));
					if (!p) { failed++; break; }
					mats = p; mats_cap = cap;
				}
				g = &mats[nmats++];
				g->name = R->name;
				g->runs = NULL; g->nruns = 0; g->cap = 0;
			}
			if (g->nruns == g->cap) {
				uint32_t cap = g->cap ? g->cap * 2 : 8;
				uint32_t *p = (uint32_t *)realloc(g->runs, (size_t)cap * sizeof(uint32_t));
				if (!p) { failed++; break; }
				g->runs = p; g->cap = cap;
			}
			g->runs[g->nruns++] = nruns - 1;
		}
	}

	// place runs, full block spills into the next
	uint32_t vbase = 0, ibase = 0, sc_out = 0;
	if (!failed) {
		for (uint32_t g = 0; g < nmats && !failed; g++) {
			uint32_t block_start = vbase, block_verts = 0;
			for (uint32_t r = 0; r < mats[g].nruns; r++) {
				run *R = &runs[mats[g].runs[r]];
				if (R->nused > MAX_VERTS) {
					(*dropped)++;
					continue;
				}
				if ((uint64_t)block_verts + R->nused > MAX_VERTS) {
					vbase += block_verts;
					block_start = vbase;
					block_verts = 0;
				}
				int id = names_add(&names, mats[g].name);
				if (id < 0) { failed++; break; }
				R->kept = 1;
				R->name_id = (uint32_t)id;
				R->ioff = ibase;
				R->voff = block_start;
				R->vadd = block_verts;
				block_verts += R->nused;
				ibase += R->nidx;
				sc_out++;
			}
			vbase += block_verts;
		}
	}

	// this cleanup is pasted 4 times, whatever
	if (failed) {
		names_free(&names);
		for (int i = 0; i < nkept; i++) buf_free(&kept[i].b);
		for (uint32_t r = 0; r < nruns; r++) { free(runs[r].used); free(runs[r].local); }
		for (uint32_t g = 0; g < nmats; g++) free(mats[g].runs);
		free(runs); free(mats); free(kept);
		return -1;
	}

	if (sc_out == 0) {
		names_free(&names);
		for (int i = 0; i < nkept; i++) buf_free(&kept[i].b);
		for (uint32_t r = 0; r < nruns; r++) { free(runs[r].used); free(runs[r].local); }
		for (uint32_t g = 0; g < nmats; g++) free(mats[g].runs);
		free(runs); free(mats); free(kept);
		return 0;
	}

	uint32_t mc_out = names.count;

	size_t out_len = TILE_HDR
		+ (size_t)sc_out * SUB_STRIDE
		+ (size_t)mc_out * NAME_LEN
		+ (size_t)vbase * VERT_STRIDE
		+ (size_t)ibase * IDX_STRIDE;
	uint8_t *o = (uint8_t *)malloc(out_len);
	if (!o) {
		names_free(&names);
		for (int i = 0; i < nkept; i++) buf_free(&kept[i].b);
		for (uint32_t r = 0; r < nruns; r++) { free(runs[r].used); free(runs[r].local); }
		for (uint32_t g = 0; g < nmats; g++) free(mats[g].runs);
		free(runs); free(mats); free(kept);
		return -1;
	}

	memset(o, 0, out_len);
	wr32(o + 0, TILE_MAGIC);
	wr16(o + 4, 1);              /* version */
	wr16(o + 6, 3);              /* flags: stride-28 vertex layout */
	wr32(o + 8, vbase);
	wr32(o + 12, ibase);
	wr32(o + 16, sc_out);
	wr32(o + 20, mc_out);
	wr32(o + 24, VERT_STRIDE);
	wr32(o + 28, 0);             /* reserved */

	size_t o_subs = TILE_HDR;
	size_t o_names = o_subs + (size_t)sc_out * SUB_STRIDE;
	size_t o_verts = o_names + (size_t)mc_out * NAME_LEN;
	size_t o_idx = o_verts + (size_t)vbase * VERT_STRIDE;

	if (mc_out) memcpy(o + o_names, names.p, (size_t)mc_out * NAME_LEN);

	uint32_t vcur = 0, icur = 0, scur = 0;
	for (uint32_t g = 0; g < nmats; g++) {
		for (uint32_t r = 0; r < mats[g].nruns; r++) {
			run *R = &runs[mats[g].runs[r]];
			if (!R->kept) continue;
			const tile_view *t = &kept[R->in].t;

			for (uint32_t k = 0; k < R->nused; k++) {
				memcpy(o + o_verts + (size_t)(vcur + k) * VERT_STRIDE,
					   t->verts + (size_t)R->used[k] * VERT_STRIDE, VERT_STRIDE);
			}
			for (uint32_t k = 0; k < R->nidx; k++) {
				uint32_t v = R->local[k] + R->vadd;
				wr16(o + o_idx + (size_t)(icur + k) * IDX_STRIDE, (uint16_t)v);
			}

			uint8_t *dst = o + o_subs + (size_t)scur * SUB_STRIDE;
			wr32(dst + 0, R->name_id);
			wr32(dst + 4, R->ioff);
			wr32(dst + 8, R->nidx);
			wr32(dst + 12, R->voff);
			scur++;
			vcur += R->nused;
			icur += R->nidx;
		}
	}

	int ok = write_file(out, o, out_len);
	free(o);
	names_free(&names);
	for (int i = 0; i < nkept; i++) buf_free(&kept[i].b);
	for (uint32_t r = 0; r < nruns; r++) { free(runs[r].used); free(runs[r].local); }
	for (uint32_t g = 0; g < nmats; g++) free(mats[g].runs);
	free(runs); free(mats); free(kept);

	return ok ? (long)vbase : -1;
}


typedef struct {
	buf b;
	uint32_t vc, tris;
	const uint8_t *verts;
	const uint8_t *idx;
} chunk_in;

static int chunk_parse(const buf *b, chunk_in *c) {
	if (b->n < CHUNK_HDR) return 0;
	if (rd32(b->p) != CHUNK_MAGIC) return 0;

	c->tris = rd32(b->p + 8);
	c->vc = rd32(b->p + 12);

	size_t need = CHUNK_HDR + (size_t)c->vc * CHUNK_VERT
				+ (size_t)c->tris * 3 * IDX_STRIDE;
	if (b->n < need) return 0;

	c->verts = b->p + CHUNK_HDR;
	c->idx = b->p + CHUNK_HDR + (size_t)c->vc * CHUNK_VERT;
	return 1;
}

static long merge_chunk_group(const char **ins, int nin, const char *out,
							  long *dropped) {
	*dropped = 0;

	chunk_in *kept = (chunk_in *)calloc((size_t)(nin > 0 ? nin : 1), sizeof(chunk_in));
	if (!kept) return -1;

	uint32_t vbase = 0, ibase = 0, tris_out = 0;
	int nkept = 0, failed = 0;

	for (int i = 0; i < nin; i++) {
		buf b = read_file(ins[i]);
		chunk_in c;
		if (!b.p || !chunk_parse(&b, &c)) { buf_free(&b); failed++; continue; }

		if ((uint64_t)vbase + c.vc > MAX_VERTS) {
			buf_free(&b);
			(*dropped)++;
			continue;
		}

		vbase += c.vc;
		ibase += c.tris * 3;
		tris_out += c.tris;

		kept[nkept].b = b;
		kept[nkept].vc = c.vc;
		kept[nkept].tris = c.tris;
		kept[nkept].verts = c.verts;
		kept[nkept].idx = c.idx;
		nkept++;
	}

	if (nkept == 0 || failed) {
		for (int i = 0; i < nkept; i++) buf_free(&kept[i].b);
		free(kept);
		return -1;
	}

	size_t out_len = CHUNK_HDR + (size_t)vbase * CHUNK_VERT
				   + (size_t)ibase * IDX_STRIDE;
	uint8_t *o = (uint8_t *)malloc(out_len);
	if (!o) {
		for (int i = 0; i < nkept; i++) buf_free(&kept[i].b);
		free(kept);
		return -1;
	}

	wr32(o + 0, CHUNK_MAGIC);
	wr16(o + 4, 1);              /* version */
	wr16(o + 6, 0);              /* flags */
	wr32(o + 8, tris_out);
	wr32(o + 12, vbase);
	wr32(o + 16, 0);             /* reserved */

	size_t o_verts = CHUNK_HDR;
	size_t o_idx = o_verts + (size_t)vbase * CHUNK_VERT;

	uint32_t vcur = 0, icur = 0;
	for (int i = 0; i < nkept; i++) {
		const chunk_in *c = &kept[i];
		if (c->vc) {
			memcpy(o + o_verts + (size_t)vcur * CHUNK_VERT,
				   c->verts, (size_t)c->vc * CHUNK_VERT);
		}
		uint32_t nidx = c->tris * 3;
		for (uint32_t k = 0; k < nidx; k++) {
			uint32_t v = (uint32_t)rd16(c->idx + (size_t)k * IDX_STRIDE) + vcur;
			wr16(o + o_idx + (size_t)(icur + k) * IDX_STRIDE, (uint16_t)v);
		}
		vcur += c->vc;
		icur += nidx;
	}

	int ok = write_file(out, o, out_len);
	free(o);
	for (int i = 0; i < nkept; i++) buf_free(&kept[i].b);
	free(kept);

	return ok ? (long)tris_out : -1;
}


typedef struct {
	char **tok;
	int ntok, cap;
} line;

static void line_free(line *l) {
	for (int i = 0; i < l->ntok; i++) free(l->tok[i]);
	free(l->tok);
	l->tok = NULL;
	l->ntok = l->cap = 0;
}

static int line_split(line *l, char *s) {
	line_free(l);
	char *p = s;
	while (*p && *p != '\n' && *p != '\r') {
		char *end = strchr(p, '\t');
		size_t len = end ? (size_t)(end - p) : strcspn(p, "\r\n");
		if (l->ntok == l->cap) {
			int cap = l->cap ? l->cap * 2 : 16;
			char **t = (char **)realloc(l->tok, (size_t)cap * sizeof(char *));
			if (!t) return 0;
			l->tok = t;
			l->cap = cap;
		}
		l->tok[l->ntok] = (char *)malloc(len + 1);
		if (!l->tok[l->ntok]) return 0;
		memcpy(l->tok[l->ntok], p, len);
		l->tok[l->ntok][len] = '\0';
		l->ntok++;
		if (!end) break;
		p = end + 1;
	}
	return l->ntok > 0;
}

// manifest, tab separated
// T <out> <in1> <in2> ...  merge tiles
// C <out> <in1> <in2> ...  merge chunks
// report line  <out> <count> <dropped>
int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: gtav_mergetest.exe <manifest> <report>\n");
		return 2;
	}

	FILE *mf = fopen(argv[1], "rb");
	if (!mf) { fprintf(stderr, "gtav_mergetest: cannot open manifest %s\n", argv[1]); return 2; }
	FILE *rf = fopen(argv[2], "wb");
	if (!rf) {
		fprintf(stderr, "gtav_mergetest: cannot open report %s\n", argv[2]);
		fclose(mf);
		return 2;
	}

	line l = { NULL, 0, 0 };
	char *raw = NULL;
	size_t rawcap = 0;
	long ntile = 0, nchunk = 0, nerr = 0, ndropped = 0, ndone = 0;

	for (;;) {
		if (!raw) {
			rawcap = 1 << 16;
			raw = (char *)malloc(rawcap);
			if (!raw) break;
		}
		if (!fgets(raw, (int)rawcap, mf)) break;

		while (strchr(raw, '\n') == NULL && !feof(mf)) {
			size_t len = strlen(raw);
			if (len + 1 >= rawcap) {
				char *p = (char *)realloc(raw, rawcap * 2);
				if (!p) break;
				raw = p;
				rawcap *= 2;
			}
			if (!fgets(raw + len, (int)(rawcap - len), mf)) break;
		}

		if (!line_split(&l, raw)) continue;

		char kind = l.tok[0][0];
		if ((kind != 'T' && kind != 'C') || l.ntok < 3) continue;

		const char **ins = (const char **)(l.tok + 2);
		int nin = l.ntok - 2;

		long dropped = 0;
		long count;
		if (kind == 'T') {
			count = merge_tile_group(ins, nin, l.tok[1], &dropped);
			ntile++;
		} else {
			count = merge_chunk_group(ins, nin, l.tok[1], &dropped);
			nchunk++;
		}

		if (count < 0) { nerr++; count = 0; }
		ndropped += dropped;
		fprintf(rf, "%s\t%ld\t%ld\n", l.tok[1], count, dropped);

		ndone++;
		if ((ndone % 5000) == 0) {
			fprintf(stderr, "gtav_mergetest: %ld groups (%ld tile, %ld chunk, %ld err)\n",
					ndone, ntile, nchunk, nerr);
		}
	}

	free(raw);
	line_free(&l);
	fclose(mf);
	if (fclose(rf) != 0) nerr++;

	fprintf(stderr, "gtav_mergetest: done %ld groups (%ld tile, %ld chunk), "
			"%ld runs dropped, %ld errors\n",
			ndone, ntile, nchunk, ndropped, nerr);
	return nerr ? 1 : 0;
}
