// yes this includes the .c, needs the statics
#include "gtav.c"

// old accumulator, kept as the reference
static int ref_push_verts(struct acc *a, const gtav_vertex *src, uint32_t n) {
	if (a->nverts + n > a->capV) {
		uint32_t ncap = a->capV ? a->capV * 2 : 256;
		while (ncap < a->nverts + n) ncap *= 2;
		gtav_vertex *nv = (gtav_vertex *)realloc(a->verts, (size_t)ncap * sizeof(gtav_vertex));
		if (!nv) return 0;
		a->verts = nv;
		a->capV = ncap;
	}
	memcpy(a->verts + a->nverts, src, (size_t)n * sizeof(gtav_vertex));
	a->nverts += n;
	return 1;
}

static int ref_push_idx(struct acc *a, const uint32_t *src, uint32_t n) {
	if (a->nidx + n > a->capI) {
		uint32_t ncap = a->capI ? a->capI * 2 : 256;
		while (ncap < a->nidx + n) ncap *= 2;
		uint32_t *ni = (uint32_t *)realloc(a->idx, (size_t)ncap * sizeof(uint32_t));
		if (!ni) return 0;
		a->idx = ni;
		a->capI = ncap;
	}
	memcpy(a->idx + a->nidx, src, (size_t)n * sizeof(uint32_t));
	a->nidx += n;
	return 1;
}

static int ref_accumulate_tile(lua_State *L, struct group *g, const gtav_tile *tile,
							   float ox, float oy, int flagsIdx,
							   char *err, size_t errlen) {
	uint32_t *remap = NULL;
	if (tile->vertexCount > 0) {
		remap = (uint32_t *)malloc((size_t)tile->vertexCount * sizeof(uint32_t));
		if (!remap) {
			seterr(err, errlen, "out of memory");
			return 0;
		}
	}

	for (uint32_t s = 0; s < tile->submeshCount; s++) {
		const gtav_submesh *sub = &tile->submeshes[s];
		const char *matName = (sub->material < tile->materialCount)
			? tile->materials[sub->material] : "error";

		struct acc *a = group_get(g, matName);
		if (!a) continue;
		if (!a->flagsDone) {
			flags_for(L, flagsIdx, matName, &a->glass, &a->nocull);
			a->flagsDone = 1;
		}

		uint32_t ioff = sub->indexOffset;
		uint32_t icnt = sub->indexCount;
		if (ioff > tile->indexCount) continue;
		if (ioff + icnt > tile->indexCount) icnt = tile->indexCount - ioff;

		if (a->nocull) {
			icnt = drop_mirror(tile->vertices, tile->vertexCount,
							   tile->indices + ioff, icnt, sub->vertexOffset);
		}

		memset(remap, 0xFF, (size_t)tile->vertexCount * sizeof(uint32_t));
		for (uint32_t k = 0; k < icnt; k++) {
			uint32_t src = (uint32_t)tile->indices[ioff + k] + sub->vertexOffset;
			if (src >= tile->vertexCount) continue;

			uint32_t slot = remap[src];
			if (slot == 0xFFFFFFFFu) {
				gtav_vertex v = tile->vertices[src];
				v.x -= ox;
				v.y -= oy;
				slot = a->nverts;
				if (!ref_push_verts(a, &v, 1)) break;
				remap[src] = slot;
			}
			if (!ref_push_idx(a, &slot, 1)) break;
		}
	}
	free(remap);
	return 1;
}

// tableIdx is the mode
// 0  nothing
// 1  nocull + glass
// 2  nocull
// 3  glass
static void test_flags(lua_State *L, int tableIdx, const char *name,
					   int *glass, int *nocull) {
	(void)L;
	(void)name;
	*glass = (tableIdx == 1 || tableIdx == 3);
	*nocull = (tableIdx == 1 || tableIdx == 2);
}


static int compare_groups(struct group *ref, struct group *neu, const char *tag) {
	if (ref->n != neu->n) {
		printf("FAIL %s: material count ref=%u new=%u\n", tag, ref->n, neu->n);
		return 0;
	}
	for (uint32_t i = 0; i < ref->n; i++) {
		const struct acc *r = &ref->items[i];
		const struct acc *n = &neu->items[i];

		if (strncmp(r->name, n->name, sizeof(r->name)) != 0) {
			printf("FAIL %s: material[%u] name ref='%s' new='%s'\n",
				   tag, i, r->name, n->name);
			return 0;
		}
		if (r->glass != n->glass || r->nocull != n->nocull) {
			printf("FAIL %s: %s flags ref=%d/%d new=%d/%d\n",
				   tag, r->name, r->glass, r->nocull, n->glass, n->nocull);
			return 0;
		}
		if (r->nverts != n->nverts) {
			printf("FAIL %s: %s nverts ref=%u new=%u\n",
				   tag, r->name, r->nverts, n->nverts);
			return 0;
		}
		if (r->nidx != n->nidx) {
			printf("FAIL %s: %s nidx ref=%u new=%u\n",
				   tag, r->name, r->nidx, n->nidx);
			return 0;
		}
		if (r->nverts &&
			memcmp(r->verts, n->verts, (size_t)r->nverts * sizeof(gtav_vertex)) != 0) {
			uint32_t v = 0;
			while (v < r->nverts &&
				   memcmp(&r->verts[v], &n->verts[v], sizeof(gtav_vertex)) == 0) v++;
			printf("FAIL %s: %s vertex[%u] differs ref=%.3f %.3f %.3f new=%.3f %.3f %.3f\n",
				   tag, r->name, v,
				   (double)r->verts[v].x, (double)r->verts[v].y, (double)r->verts[v].z,
				   (double)n->verts[v].x, (double)n->verts[v].y, (double)n->verts[v].z);
			return 0;
		}
		if (r->nidx &&
			memcmp(r->idx, n->idx, (size_t)r->nidx * sizeof(uint32_t)) != 0) {
			uint32_t v = 0;
			while (v < r->nidx && r->idx[v] == n->idx[v]) v++;
			printf("FAIL %s: %s index[%u] differs ref=%u new=%u\n",
				   tag, r->name, v, r->idx[v], n->idx[v]);
			return 0;
		}
	}
	return 1;
}


#define MAX_BATCH 64

struct entry {
	char     file[64];
	int      tx, ty;
};

struct batch {
	int          originTx, originTy;
	struct entry tiles[MAX_BATCH];
	int          n;
};

static void batch_origin(struct batch *b, float *ox, float *oy) {
	*ox = (float)(b->originTx * 1024.0);
	*oy = (float)(b->originTy * 1024.0);
}

static int load_batch(struct batch *b, gtav_tile *tiles, const char *why) {
	int got = 0;
	for (int i = 0; i < b->n; i++) {
		size_t len = 0;
		uint8_t *owned = NULL;
		const uint8_t *blob = read_payload(b->tiles[i].file, b->tiles[i].tx,
										   b->tiles[i].ty, &len, &owned);
		if (!blob) {
			printf("FAIL %s: no payload for %s (%d %d)\n",
				   why, b->tiles[i].file, b->tiles[i].tx, b->tiles[i].ty);
			continue;
		}
		char err[256];
		err[0] = '\0';
		if (!gtav_tile_decode(blob, len, &tiles[i], err, sizeof(err))) {
			printf("FAIL %s: decode %s: %s\n", why, b->tiles[i].file, err);
			free(owned);
			tiles[i].submeshCount = 0;   /* skip, but keep slots aligned */
			continue;
		}
		free(owned);
		got++;
	}
	return got;
}

static void free_batch(gtav_tile *tiles, int n) {
	for (int i = 0; i < n; i++) gtav_tile_free(&tiles[i]);
}

static double run_pass(struct batch *batches, uint32_t nb,
					   gtav_tile (*store)[MAX_BATCH], int useNew) {
	double t0 = now_ms();
	for (uint32_t bi = 0; bi < nb; bi++) {
		struct batch *b = &batches[bi];
		float ox, oy;
		batch_origin(b, &ox, &oy);
		struct group g;
		memset(&g, 0, sizeof(g));
		char err[256];
		for (int i = 0; i < b->n; i++) {
			if (store[bi][i].submeshCount == 0) continue;
			err[0] = '\0';
			if (useNew) {
				accumulate_tile(NULL, &g, &store[bi][i], ox, oy, 0, 0,
								err, sizeof(err));
			} else {
				ref_accumulate_tile(NULL, &g, &store[bi][i], ox, oy, 0,
									err, sizeof(err));
			}
		}
		group_free(&g);
	}
	return now_ms() - t0;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr,
				"usage: %s <mapdir> <manifest> [bench] [iters]\n", argv[0]);
		return 2;
	}
	if (!gtav_pages_set_root(argv[1])) {
		fprintf(stderr, "cannot use map dir '%s'\n", argv[1]);
		return 2;
	}

	FILE *f = fopen(argv[2], "rb");
	if (!f) {
		fprintf(stderr, "cannot open manifest '%s'\n", argv[2]);
		return 2;
	}

	int bench = (argc > 3 && strcmp(argv[3], "bench") == 0);
	int iters = bench ? ((argc > 4) ? atoi(argv[4]) : 3) : 1;
	if (iters < 1) iters = 1;

	flags_for = test_flags;

	struct batch *batches = NULL;
	uint32_t nb = 0, capB = 0;

	char line[512];
	struct batch *cur = NULL;
	// G <tx> <ty>  new group
	// F <name> <tx> <ty>  tile
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == 'G') {
			if (nb == capB) {
				uint32_t ncap = capB ? capB * 2 : 64;
				struct batch *nb2 = (struct batch *)realloc(batches,
									(size_t)ncap * sizeof(struct batch));
				if (!nb2) { fprintf(stderr, "out of memory\n"); return 2; }
				batches = nb2;
				capB = ncap;
			}
			cur = &batches[nb++];
			memset(cur, 0, sizeof(*cur));
			if (sscanf(line + 1, "%d %d", &cur->originTx, &cur->originTy) != 2) {
				fprintf(stderr, "bad G line: %s", line);
				return 2;
			}
		} else if (line[0] == 'F' && cur) {
			if (cur->n >= MAX_BATCH) {
				fprintf(stderr, "batch overflow at %s", line);
				return 2;
			}
			struct entry *e = &cur->tiles[cur->n];
			if (sscanf(line + 1, "%63s %d %d", e->file, &e->tx, &e->ty) != 3) {
				fprintf(stderr, "bad F line: %s", line);
				return 2;
			}
			cur->n++;
		}
	}
	fclose(f);

	if (nb == 0) {
		fprintf(stderr, "manifest has no groups\n");
		return 2;
	}

	uint32_t checked = 0, tilesSeen = 0, subsTotal = 0;
	uint64_t vertsTotal = 0, idxTotal = 0, memsetBytes = 0;

	for (uint32_t bi = 0; bi < nb; bi++) {
		struct batch *b = &batches[bi];
		gtav_tile tiles[MAX_BATCH];
		memset(tiles, 0, sizeof(tiles));
		int got = load_batch(b, tiles, "parity");
		if (got == 0) {
			free_batch(tiles, b->n);
			continue;
		}

		float ox, oy;
		batch_origin(b, &ox, &oy);

		char tag[128];
		snprintf(tag, sizeof(tag), "batch %u (%d %d)", bi, b->originTx, b->originTy);

		for (int mode = 0; mode <= 3; mode++) {
			struct group gr, gn;
			memset(&gr, 0, sizeof(gr));
			memset(&gn, 0, sizeof(gn));
			char err[256];

			for (int i = 0; i < b->n; i++) {
				if (tiles[i].submeshCount == 0) continue;
				if (mode == 0) {
					memsetBytes += (uint64_t)tiles[i].vertexCount *
								   (uint64_t)tiles[i].submeshCount * 4u;
					subsTotal += tiles[i].submeshCount;
				}
				err[0] = '\0';
				ref_accumulate_tile(NULL, &gr, &tiles[i], ox, oy, mode,
									err, sizeof(err));
				err[0] = '\0';
				accumulate_tile(NULL, &gn, &tiles[i], ox, oy, mode, 0,
								err, sizeof(err));
			}

			char mtag[160];
			snprintf(mtag, sizeof(mtag), "%s mode %d", tag, mode);
			int ok = compare_groups(&gr, &gn, mtag);

			if (ok) {
				group_reset(&gn);
				for (int i = 0; i < b->n; i++) {
					if (tiles[i].submeshCount == 0) continue;
					err[0] = '\0';
					accumulate_tile(NULL, &gn, &tiles[i], ox, oy, mode, 0,
									err, sizeof(err));
				}
				char rtag[192];
				snprintf(rtag, sizeof(rtag),
						 "%s (second build after group_reset)", mtag);
				ok = compare_groups(&gr, &gn, rtag);
			}

			if (mode == 0) {
				for (uint32_t i = 0; i < gn.n; i++) {
					vertsTotal += gn.items[i].nverts;
					idxTotal += gn.items[i].nidx;
				}
			}

			group_free(&gr);
			group_free(&gn);
			if (!ok) {
				free_batch(tiles, b->n);
				free(batches);
				fprintf(stderr, "RESULT PARITY=FAIL BATCH=%u MODE=%d\n", bi, mode);
				return 1;
			}
			checked++;
		}
		tilesSeen += (uint32_t)got;
		free_batch(tiles, b->n);
	}

	printf("parity OK: %u batches x 4 flag modes, %u tiles, %u submeshes, "
		   "%llu verts, %llu indices, old memset volume %.1f MB\n",
		   nb, tilesSeen, subsTotal,
		   (unsigned long long)vertsTotal, (unsigned long long)idxTotal,
		   (double)memsetBytes / (1024.0 * 1024.0));

	// same batches through the real worker
	uint32_t asyncChecks = 0;
	for (int mode = 0; mode <= 3; mode++) {
		job_cache_clear();
		for (uint32_t bi = 0; bi < nb; bi++) {
			struct batch *b = &batches[bi];
			gtav_tile tiles[MAX_BATCH];
			memset(tiles, 0, sizeof(tiles));
			int got = load_batch(b, tiles, "async");
			if (got == 0) {
				free_batch(tiles, b->n);
				continue;
			}

			float ox, oy;
			batch_origin(b, &ox, &oy);

			struct group gr;
			memset(&gr, 0, sizeof(gr));
			char err[256];
			for (int i = 0; i < b->n; i++) {
				if (tiles[i].submeshCount == 0) continue;
				err[0] = '\0';
				ref_accumulate_tile(NULL, &gr, &tiles[i], ox, oy, mode,
									err, sizeof(err));
			}

			struct job_entry ent[MAX_BATCH];
			for (int i = 0; i < b->n; i++) {
				snprintf(ent[i].file, sizeof(ent[i].file), "%s", b->tiles[i].file);
				ent[i].tx = b->tiles[i].tx;
				ent[i].ty = b->tiles[i].ty;
			}
			if (!job_begin(ent, b->n, ox, oy, NULL, 0)) {
				printf("FAIL async: job refused at batch %u\n", bi);
				return 1;
			}

			double t0 = now_ms();
			const double slice = (bi % 64 == 0) ? 0.0001 : 0.0;
			for (;;) {
				int answered = job_answer(NULL, mode, slice);
				LONG st = InterlockedCompareExchange(&J.state, JOB_IDLE, JOB_IDLE);
				if (st == JOB_DONE || st == JOB_FAILED) break;
				if (now_ms() - t0 > 120000.0) {
					printf("FAIL async: batch %u timed out in state %ld\n",
						   bi, (long)st);
					return 1;
				}
				if (answered == 0) Sleep(1);
			}

			char tag[160];
			snprintf(tag, sizeof(tag), "async batch %u (%d %d) mode %d",
					 bi, b->originTx, b->originTy, mode);

			int ok;
			if (InterlockedCompareExchange(&J.state, JOB_IDLE, JOB_IDLE) == JOB_FAILED) {
				printf("FAIL %s: job failed: %s\n", tag, J.err);
				ok = 0;
			} else {
				ok = compare_groups(&gr, &J.g, tag);
			}

			group_free(&gr);
			group_reset(&J.g);
			InterlockedExchange(&J.state, JOB_IDLE);
			free_batch(tiles, b->n);
			if (!ok) {
				free(batches);
				fprintf(stderr, "RESULT PARITY=FAIL ASYNC=1 BATCH=%u MODE=%d\n",
						bi, mode);
				job_shutdown();
				return 1;
			}
			asyncChecks++;
		}
	}

	printf("threaded prepare OK: %u groups through the worker + handshake "
		   "(byte-identical to the synchronous build)\n", asyncChecks);

	if (!bench) {
		printf("RESULT PARITY=OK ASYNC=%u BATCHES=%u TILES=%u SUBS=%u MEMSET_MB=%.1f\n",
			   asyncChecks, nb, tilesSeen, subsTotal,
			   (double)memsetBytes / (1024.0 * 1024.0));
		job_shutdown();
		free(batches);
		return 0;
	}

	gtav_tile (*store)[MAX_BATCH] =
		(gtav_tile (*)[MAX_BATCH])calloc(nb, sizeof(gtav_tile) * (size_t)MAX_BATCH);
	if (!store) {
		fprintf(stderr, "out of memory\n");
		free(batches);
		return 2;
	}
	for (uint32_t bi = 0; bi < nb; bi++) {
		load_batch(&batches[bi], store[bi], "bench");
	}

	double refMs = 0, newMs = 0;

	for (int it = 0; it < iters; it++) {
		double r, n;
		if ((it & 1) == 0) {
			r = run_pass(batches, nb, store, 0);
			n = run_pass(batches, nb, store, 1);
		} else {
			n = run_pass(batches, nb, store, 1);
			r = run_pass(batches, nb, store, 0);
		}
		refMs += r;
		newMs += n;
		printf("  iter %d: ref=%.1fms new=%.1fms\n", it, r, n);
	}

	for (uint32_t bi = 0; bi < nb; bi++) free_batch(store[bi], batches[bi].n);
	free(store);

	printf("bench iters=%d ref=%.1fms new=%.1fms speedup=%.2fx\n",
		   iters, refMs, newMs, refMs > 0 ? refMs / newMs : 0.0);
	printf("RESULT PARITY=OK BENCH=OK REF_MS=%.1f NEW_MS=%.1f\n", refMs, newMs);
	job_shutdown();
	free(batches);
	return 0;
}
