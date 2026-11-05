#include "gtav_decode.h"
#include "gtav_pages.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump_tile(const uint8_t *blob, size_t len, const char *name) {
	gtav_tile t;
	char err[256];
	err[0] = '\0';

	if (!gtav_tile_decode(blob, len, &t, err, sizeof(err))) {
		printf("DECODE-FAIL %s: %s\n", name, err);
		return;
	}
	if (err[0]) printf("NOTE %s: %s\n", name, err);

	printf("tile %s bytes=%u\n", name, (unsigned)len);
	printf("hdr version=%u flags=%u vc=%u ic=%u sc=%u mc=%u stride=%u clamped=%u\n",
		   t.version, t.flags, t.vertexCount, t.indexCount,
		   t.submeshCount, t.materialCount, t.stride, t.clamped);

	for (uint32_t i = 0; i < t.materialCount; i++) {
		printf("mat[%u]=%s\n", i, t.materials[i]);
	}
	for (uint32_t i = 0; i < t.submeshCount; i++) {
		printf("sub[%u] mat=%u ioff=%u icnt=%u voff=%u\n", i,
			   t.submeshes[i].material, t.submeshes[i].indexOffset,
			   t.submeshes[i].indexCount, t.submeshes[i].vertexOffset);
	}

	uint32_t show = t.vertexCount < 3 ? t.vertexCount : 3;
	for (uint32_t i = 0; i < show; i++) {
		const gtav_vertex *v = &t.vertices[i];
		printf("vert[%u] %.6f %.6f %.6f | %.6f %.6f | %.6f %.6f %.6f\n",
			   i, v->x, v->y, v->z, v->u, v->v, v->nx, v->ny, v->nz);
	}
	for (uint32_t i = 0; i < show; i++) {
		uint32_t j = t.vertexCount - 1 - i;
		const gtav_vertex *v = &t.vertices[j];
		printf("vert[%u] %.6f %.6f %.6f | %.6f %.6f | %.6f %.6f %.6f\n",
			   j, v->x, v->y, v->z, v->u, v->v, v->nx, v->ny, v->nz);
	}

	unsigned long long sumIdx = 0;
	for (uint32_t i = 0; i < t.indexCount; i++) sumIdx += t.indices[i];
	printf("sum_idx=%llu\n", sumIdx);

	unsigned long long sumBits = 0;
	float mn[3] = { 1e30f, 1e30f, 1e30f };
	float mx[3] = { -1e30f, -1e30f, -1e30f };
	for (uint32_t i = 0; i < t.vertexCount; i++) {
		const gtav_vertex *v = &t.vertices[i];
		float p[3] = { v->x, v->y, v->z };
		for (int k = 0; k < 3; k++) {
			uint32_t bits;
			memcpy(&bits, &p[k], 4);
			sumBits += bits;
			if (p[k] < mn[k]) mn[k] = p[k];
			if (p[k] > mx[k]) mx[k] = p[k];
		}
	}
	printf("sum_posbits=%llu\n", sumBits);
	if (t.vertexCount) {
		printf("bounds %.3f %.3f %.3f -> %.3f %.3f %.3f\n",
			   mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
	}

	int bad = 0;
	for (uint32_t i = 0; i < t.submeshCount; i++) {
		uint32_t end = t.submeshes[i].indexOffset + t.submeshes[i].indexCount;
		if (end > t.indexCount) bad++;
		if (t.submeshes[i].vertexOffset > t.vertexCount) bad++;
	}
	printf("range_errors=%d\n", bad);

	gtav_tile_free(&t);
}

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
		 | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <mapdir> <kind> <coordX> <coordY> [file]\n"
			"       %s <mapdir> scan <radius> <eyeX> <eyeY>\n",
			argv[0], argv[0]);
		return 2;
	}

	if (!gtav_pages_set_root(argv[1])) {
		fprintf(stderr, "cannot use map dir '%s'\n", argv[1]);
		return 1;
	}

	// bulk scan
	if (strcmp(argv[2], "scan") == 0) {
		if (argc < 6) { fprintf(stderr, "scan needs radius eyeX eyeY\n"); return 2; }
		double radius = atof(argv[3]);
		double eyeX = atof(argv[4]);
		double eyeY = atof(argv[5]);
		const double tileSize = 1024.0;
		const double pageSpan = 8.0 * tileSize;

		unsigned long long pages = 0, blobs = 0, verts = 0, idxs = 0, subs = 0;
		unsigned long long failures = 0, clamped = 0, rangeErr = 0;
		unsigned long long shortTiles = 0, lostVerts = 0, lostIdx = 0;

		int span = (int)(radius / pageSpan) + 2;
		int epx = (int)floor(eyeX / pageSpan);
		int epy = (int)floor(eyeY / pageSpan);

		for (int px = epx - span; px <= epx + span; px++) {
			for (int py = epy - span; py <= epy + span; py++) {
				double cx0 = px * pageSpan, cy0 = py * pageSpan;
				double nx = eyeX < cx0 ? cx0 : (eyeX > cx0 + pageSpan ? cx0 + pageSpan : eyeX);
				double ny = eyeY < cy0 ? cy0 : (eyeY > cy0 + pageSpan ? cy0 + pageSpan : eyeY);
				double dx = eyeX - nx, dy = eyeY - ny;
				if (dx * dx + dy * dy > radius * radius) continue;

				const gtav_page *pg = gtav_pages_open('v', px * 8, py * 8);
				if (!pg) continue;
				pages++;

				uint32_t n = gtav_pages_count(pg);
				for (uint32_t i = 0; i < n; i++) {
					const char *name = NULL;
					const uint8_t *blob = NULL;
					size_t len = 0;
					if (!gtav_pages_enum(pg, i, &name, &blob, &len)) continue;
					if (!blob) { failures++; continue; }

					gtav_tile t;
					char err[256];
					err[0] = '\0';
					if (!gtav_tile_decode(blob, len, &t, err, sizeof(err))) {
						failures++;
						if (failures <= 5) printf("FAIL %s: %s\n", name, err);
						continue;
					}
					blobs++;
					verts += t.vertexCount;
					idxs += t.indexCount;
					subs += t.submeshCount;

					// header vs what actually decoded
					uint32_t hdrVC = rd32(blob + 8), hdrIC = rd32(blob + 12);
					if (t.vertexCount < hdrVC)
						lostVerts += (unsigned long long)(hdrVC - t.vertexCount);
					if (t.indexCount < hdrIC)
						lostIdx += (unsigned long long)(hdrIC - t.indexCount);
					if (t.vertexCount < hdrVC || t.indexCount < hdrIC) {
						shortTiles++;
						if (shortTiles <= 5)
							printf("SHORT %s: header vc=%u ic=%u, decoded vc=%u ic=%u\n",
								   name, hdrVC, hdrIC,
								   (unsigned)t.vertexCount, (unsigned)t.indexCount);
					}
					if (t.clamped) {
						clamped++;
						if (clamped <= 5) printf("CLAMPED %s: %s\n", name, err);
					}
					for (uint32_t s = 0; s < t.submeshCount; s++) {
						if (t.submeshes[s].indexOffset + t.submeshes[s].indexCount
							> t.indexCount) rangeErr++;
					}
					gtav_tile_free(&t);
				}
				gtav_pages_trim();
			}
		}

		printf("pages=%llu blobs=%llu verts=%llu idx=%llu subs=%llu\n",
			   pages, blobs, verts, idxs, subs);
		printf("decode_failures=%llu clamped=%llu range_errors=%llu mapped=%u\n",
			   failures, clamped, rangeErr, (unsigned)gtav_pages_mapped());
		printf("short_tiles=%llu lost_verts=%llu lost_idx=%llu\n",
			   shortTiles, lostVerts, lostIdx);
		gtav_pages_shutdown();
		return (failures || rangeErr || clamped || shortTiles) ? 1 : 0;
	}

	int cx = atoi(argv[3]);
	int cy = atoi(argv[4]);

	const gtav_page *pg = gtav_pages_open('v', cx, cy);
	if (!pg) {
		fprintf(stderr, "no page for tile %d %d\n", cx, cy);
		return 1;
	}
	printf("page entries=%u root=%s\n", gtav_pages_count(pg), gtav_pages_root());

	if (argc >= 6) {
		size_t len = 0;
		const uint8_t *blob = gtav_pages_blob(pg, argv[5], &len);
		if (!blob) {
			fprintf(stderr, "no entry '%s' in that page\n", argv[5]);
			return 1;
		}
		dump_tile(blob, len, argv[5]);
	} else {
		printf("(pass a file name to dump one tile)\n");
	}

	gtav_pages_shutdown();
	return 0;
}
