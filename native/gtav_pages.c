#include "gtav_pages.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GTAV_PAGE_MAGIC   0x47505647u  /* "GVPG" little-endian */
#define GTAV_PAGE_SHIFT   3            /* 8x8 tiles / chunks per page */
#define GTAV_MAX_ENTRIES  65536u

struct gtav_entry {
	uint32_t nameOff;
	uint32_t blobOff;
	uint32_t blobLen;
};

struct gtav_page {
	char     name[32];
	HANDLE   file;
	HANDLE   mapping;
	uint8_t *view;
	size_t   size;
	uint32_t count;
	struct gtav_entry *entries;

	struct gtav_page *hash_next;
	struct gtav_page *lru_prev, *lru_next;
	uint32_t bucket;                /* hash bucket, so removal is direct */
};

#define GTAV_HASH_SIZE 1024

static struct {
	int          ready;
	char         root[MAX_PATH];        /* <garrysmod>/data/lossantos/<map> */
	struct gtav_page *hash[GTAV_HASH_SIZE];
	struct gtav_page *lru_head, *lru_tail;
	size_t       mapped;
	size_t       max_mapped;            /* LRU budget, in pages */
} G = { 0, "", { 0 }, 0, 0, 0, 24 };


static void strip_last_component(char *path) {
	char *slash = strrchr(path, '\\');
	if (!slash) slash = strrchr(path, '/');
	if (slash) *slash = '\0';
}

static int build_default_root(char *out, size_t outlen, const char *mapName) {
	char mod[MAX_PATH];
	HMODULE h = NULL;

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							(LPCSTR)(void *)&build_default_root, &h)) {
		return 0;
	}
	if (!GetModuleFileNameA(h, mod, (DWORD)sizeof(mod))) return 0;

	strip_last_component(mod);   /* drop the DLL name       -> .../lua/bin */
	strip_last_component(mod);   /* drop bin                -> .../lua */
	strip_last_component(mod);   /* drop lua                -> .../garrysmod */

	snprintf(out, outlen, "%s\\data\\lossantos\\%s", mod, mapName);
	return 1;
}

int gtav_pages_init(const char *mapName) {
	char root[MAX_PATH];
	if (!mapName || !mapName[0]) return 0;
	if (!build_default_root(root, sizeof(root), mapName)) return 0;
	return gtav_pages_set_root(root);
}

int gtav_pages_set_root(const char *root) {
	if (!root || !root[0]) return 0;
	gtav_pages_shutdown();
	snprintf(G.root, sizeof(G.root), "%s", root);
	G.ready = 1;
	return 1;
}

const char *gtav_pages_root(void) { return G.root; }

uint8_t *gtav_pages_read_loose(const char *subdir, const char *fileName,
							 size_t *len) {
	char path[MAX_PATH];
	if (len) *len = 0;
	if (!G.ready || !subdir || !fileName || !fileName[0]) return NULL;
	// name comes from an index, dont let it steer the path
	if (strchr(fileName, '/') || strchr(fileName, '\\') ||
		strstr(fileName, "..")) {
		return NULL;
	}
	if (strchr(subdir, '/') || strchr(subdir, '\\') || strstr(subdir, "..")) {
		return NULL;
	}

	snprintf(path, sizeof path, "%s\\%s\\%s", G.root, subdir, fileName);

	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
						   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) return NULL;

	LARGE_INTEGER sz;
	if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 ||
		sz.QuadPart > (LONGLONG)(256u * 1024u * 1024u)) {
		CloseHandle(f);
		return NULL;
	}

	uint8_t *buf = (uint8_t *)malloc((size_t)sz.QuadPart);
	if (!buf) { CloseHandle(f); return NULL; }

	DWORD got = 0;
	BOOL ok = ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL);
	CloseHandle(f);
	if (!ok || got != (DWORD)sz.QuadPart) { free(buf); return NULL; }

	if (len) *len = (size_t)got;
	return buf;
}


static uint32_t hash_key(char kind, int px, int py) {
	uint32_t k = (uint32_t)((uint8_t)kind) * 0x9E3779B9u;
	k ^= (uint32_t)(px + 1024) * 0x85EBCA6Bu;
	k ^= (uint32_t)(py + 1024) * 0xC2B2AE35u;
	return k % GTAV_HASH_SIZE;
}

static void lru_unlink(struct gtav_page *p) {
	if (p->lru_prev) p->lru_prev->lru_next = p->lru_next;
	else G.lru_head = p->lru_next;
	if (p->lru_next) p->lru_next->lru_prev = p->lru_prev;
	else G.lru_tail = p->lru_prev;
	p->lru_prev = p->lru_next = NULL;
}

static void lru_touch(struct gtav_page *p) {
	if (G.lru_head == p) return;
	lru_unlink(p);
	p->lru_next = G.lru_head;
	if (G.lru_head) G.lru_head->lru_prev = p;
	G.lru_head = p;
	if (!G.lru_tail) G.lru_tail = p;
}

static void page_destroy(struct gtav_page *p) {
	if (p->view) UnmapViewOfFile(p->view);
	if (p->mapping) CloseHandle(p->mapping);
	if (p->file != INVALID_HANDLE_VALUE) CloseHandle(p->file);
	free(p->entries);
	free(p);
	if (G.mapped > 0) G.mapped--;
}

static void hash_remove(struct gtav_page *p) {
	struct gtav_page **slot = &G.hash[p->bucket];
	while (*slot) {
		if (*slot == p) { *slot = p->hash_next; p->hash_next = NULL; return; }
		slot = &(*slot)->hash_next;
	}
}

static struct gtav_page *page_find(const char *name, uint32_t h) {
	struct gtav_page *p = G.hash[h];
	while (p) {
		if (strcmp(p->name, name) == 0) return p;
		p = p->hash_next;
	}
	return NULL;
}

static struct gtav_page *page_map(const char *name) {
	char path[MAX_PATH];
	struct gtav_page *p;
	LARGE_INTEGER sz;

	snprintf(path, sizeof(path), "%s\\pages\\%s", G.root, name);

	p = (struct gtav_page *)calloc(1, sizeof(*p));
	if (!p) return NULL;
	p->file = INVALID_HANDLE_VALUE;
	snprintf(p->name, sizeof(p->name), "%s", name);

	p->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
						  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (p->file == INVALID_HANDLE_VALUE) { free(p); return NULL; }

	if (!GetFileSizeEx(p->file, &sz) || sz.QuadPart < 16 ||
		sz.QuadPart > (LONGLONG)1 << 34) {
		CloseHandle(p->file); free(p); return NULL;
	}
	p->size = (size_t)sz.QuadPart;

	p->mapping = CreateFileMappingA(p->file, NULL, PAGE_READONLY, 0, 0, NULL);
	if (!p->mapping) { CloseHandle(p->file); free(p); return NULL; }

	p->view = (uint8_t *)MapViewOfFile(p->mapping, FILE_MAP_READ, 0, 0, 0);
	if (!p->view) {
		CloseHandle(p->mapping); CloseHandle(p->file); free(p); return NULL;
	}

	if (memcmp(p->view, "GVPG", 4) != 0) {
		page_destroy(p);
		return NULL;
	}
	uint32_t count;
	memcpy(&count, p->view + 8, 4);
	if (count == 0 || count > GTAV_MAX_ENTRIES ||
		16 + (size_t)count * 16 > p->size) {
		page_destroy(p);
		return NULL;
	}
	p->count = count;
	p->entries = (struct gtav_entry *)malloc((size_t)count * sizeof(struct gtav_entry));
	if (!p->entries) { page_destroy(p); return NULL; }
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = p->view + 16 + (size_t)i * 16;
		memcpy(&p->entries[i].nameOff, e, 4);
		memcpy(&p->entries[i].blobOff, e + 4, 4);
		memcpy(&p->entries[i].blobLen, e + 8, 4);
	}

	G.mapped++;
	return p;
}


const gtav_page *gtav_pages_open(char kind, int coordX, int coordY) {
	char name[32];
	uint32_t h;

	if (!G.ready) return NULL;
	if (kind != 'v' && kind != 'p') return NULL;

	snprintf(name, sizeof(name), "%c_%d_%d.bin", kind,
			 coordX >> GTAV_PAGE_SHIFT, coordY >> GTAV_PAGE_SHIFT);
	h = hash_key(kind, coordX >> GTAV_PAGE_SHIFT, coordY >> GTAV_PAGE_SHIFT);

	struct gtav_page *p = page_find(name, h);
	if (p) { lru_touch(p); return p; }

	p = page_map(name);
	if (!p) return NULL;

	p->hash_next = G.hash[h];
	p->bucket = h;
	G.hash[h] = p;
	lru_touch(p);
	return p;
}

const uint8_t *gtav_pages_blob(const gtav_page *pg, const char *fileName,
							 size_t *outLen) {
	if (outLen) *outLen = 0;
	if (!pg || !fileName) return NULL;

	size_t want = strlen(fileName);
	for (uint32_t i = 0; i < pg->count; i++) {
		const struct gtav_entry *e = &pg->entries[i];
		if (e->nameOff >= pg->size) continue;
		const char *nm = (const char *)pg->view + e->nameOff;
		size_t room = pg->size - e->nameOff;
		size_t n = 0;
		while (n < room && nm[n] != '\0') n++;
		if (n == want && memcmp(nm, fileName, want) == 0) {
			if ((size_t)e->blobOff + e->blobLen > pg->size) return NULL;
			if (outLen) *outLen = e->blobLen;
			return pg->view + e->blobOff;
		}
	}
	return NULL;
}

uint32_t gtav_pages_count(const gtav_page *p) { return p ? p->count : 0; }

int gtav_pages_enum(const gtav_page *pg, uint32_t index, const char **name,
				  const uint8_t **blob, size_t *len) {
	const struct gtav_entry *e;
	if (!pg || index >= pg->count) return 0;
	e = &pg->entries[index];

	if (name) {
		const char *nm = (const char *)pg->view + e->nameOff;
		*name = (e->nameOff < pg->size) ? nm : "";
	}
	if (blob || len) {
		if ((size_t)e->blobOff + e->blobLen > pg->size) {
			if (blob) *blob = NULL;
			if (len) *len = 0;
			return 1;
		}
		if (blob) *blob = pg->view + e->blobOff;
		if (len) *len = e->blobLen;
	}
	return 1;
}

static SRWLOCK g_lock = SRWLOCK_INIT;

void gtav_pages_lock(void)   { AcquireSRWLockExclusive(&g_lock); }
void gtav_pages_unlock(void) { ReleaseSRWLockExclusive(&g_lock); }

size_t gtav_pages_mapped(void) { return G.mapped; }

void gtav_pages_trim(void) {
	while (G.mapped > G.max_mapped && G.lru_tail) {
		struct gtav_page *victim = G.lru_tail;
		lru_unlink(victim);
		hash_remove(victim);
		page_destroy(victim);
	}
}

void gtav_pages_shutdown(void) {
	for (int i = 0; i < GTAV_HASH_SIZE; i++) {
		struct gtav_page *p = G.hash[i];
		while (p) {
			struct gtav_page *next = p->hash_next;
			page_destroy(p);
			p = next;
		}
		G.hash[i] = NULL;
	}
	G.lru_head = G.lru_tail = NULL;
	G.mapped = 0;
	G.ready = 0;
	G.root[0] = '\0';
}

void gtav_pages_set_budget(size_t maxMapped) {
	G.max_mapped = maxMapped ? maxMapped : 1;
	gtav_pages_trim();
}
