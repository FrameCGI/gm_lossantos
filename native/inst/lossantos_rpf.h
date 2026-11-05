#ifndef LOSSANTOS_RPF_H
#define LOSSANTOS_RPF_H

#include "lossantos_common.h"
#include "lossantos_crypto.h"

enum { LOSSANTOS_DIR = 0, LOSSANTOS_BINARY = 1, LOSSANTOS_RESOURCE = 2 };

typedef struct {
	const char *name;          /* points into the archive's names block */
	uint32_t kind;
	uint32_t file_size;        /* stored size (u24, resolved for 0xFFFFFF) */
	uint32_t file_offset;      /* 512-byte blocks */
	uint32_t unc_size;         /* binary: uncompressed size */
	uint32_t encrypted;
	uint32_t first, count;     /* directory children */
	uint32_t sys_flags, gfx_flags;
	int32_t parent;            /* entry index, -1 for the root */
} lossantos_entry;

typedef struct lossantos_rpf {
	void *h;                   /* HANDLE */
	int own_handle;
	uint64_t start, size;
	char name[256];
	uint32_t count;
	lossantos_entry *e;
	char *names;
	const lossantos_ng *ng;
	uint32_t enc;              /* table cipher, entries use it too */
} lossantos_rpf;

int lossantos_rpf_open(lossantos_rpf *a, const lossantos_ng *ng, const wchar_t *path);
// shares the parent handle, close it first
int lossantos_rpf_open_child(lossantos_rpf *child, const lossantos_rpf *parent, uint32_t idx);
void lossantos_rpf_close(lossantos_rpf *a);

void lossantos_rpf_path(const lossantos_rpf *a, uint32_t idx, char *dst, size_t cap);
int32_t lossantos_rpf_find(const lossantos_rpf *a, const char *path);

uint32_t lossantos_rpf_size_from_flags(uint32_t flags);
uint32_t lossantos_rpf_file_size(lossantos_rpf *a, uint32_t idx);
int lossantos_rpf_read(lossantos_rpf *a, uint32_t idx, lossantos_buf *out);
int lossantos_rpf_read_at(const lossantos_rpf *a, uint64_t off, void *dst, size_t len);

#endif
