#include "lossantos_rpf.h"
#include "lossantos_inflate.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RPF_MAGIC 0x52504637u  // "RPF7"
#define BLOCK 512u
#define RSC_HEADER 16u

int lossantos_rpf_read_at(const lossantos_rpf *a, uint64_t off, void *dst, size_t len) {
	uint8_t *d = (uint8_t *)dst;
	uint64_t pos = a->start + off;
	while (len) {
		DWORD want = len > 0x40000000u ? 0x40000000u : (DWORD)len;
		OVERLAPPED ov;
		memset(&ov, 0, sizeof ov);
		ov.Offset = (DWORD)pos;
		ov.OffsetHigh = (DWORD)(pos >> 32);
		DWORD got = 0;
		if (!ReadFile((HANDLE)a->h, d, want, &got, &ov) || got != want) {
			lossantos_set_error("%s: short read at 0x%llx", a->name, (unsigned long long)pos);
			return 0;
		}
		d += got;
		pos += got;
		len -= got;
	}
	return 1;
}

static int printable_fraction_ok(const uint8_t *p, size_t n) {
	size_t good = 0;
	for (size_t i = 0; i < n; i++)
		if ((p[i] >= 0x20 && p[i] <= 0x7E) || p[i] == 0) good++;
	return n == 0 || good * 5 >= n * 4;          /* >= 0.8 */
}

static int read_toc(lossantos_rpf *a) {
	// 0   magic
	// 4   entry count
	// 8   names length
	// 12  table encryption
	uint32_t head[4];
	if (!lossantos_rpf_read_at(a, 0, head, 16)) return 0;
	if (head[0] != RPF_MAGIC) {
		lossantos_set_error("%s: not an RPF7 archive", a->name);
		return 0;
	}
	uint32_t count = head[1], names_len = head[2], enc = head[3];
	if (count == 0 || names_len > 0x1000000u) {
		lossantos_set_error("%s: damaged header", a->name);
		return 0;
	}
	uint8_t *ent = (uint8_t *)malloc((size_t)count * 16);
	char *names = (char *)malloc((size_t)names_len + 1);
	if (!ent || !names) {
		free(ent);
		free(names);
		lossantos_set_error("out of memory");
		return 0;
	}
	if (!lossantos_rpf_read_at(a, 16, ent, (size_t)count * 16) ||
		!lossantos_rpf_read_at(a, 16 + (uint64_t)count * 16, names, names_len)) {
		free(ent);
		free(names);
		return 0;
	}
	names[names_len] = 0;
	if (enc == LOSSANTOS_ENC_NG) {
		const uint8_t *key = lossantos_key_for(a->ng, a->name, (uint32_t)a->size);
		uint8_t probe[64], nprobe[64];
		size_t pl = count * 16 < 64 ? count * 16 : 64;
		memcpy(probe, ent, pl);
		lossantos_ng_decrypt(a->ng, probe, pl, key);
		// name hash picked the wrong key, try all 101
		if (!lossantos_toc_shaped(probe, count)) {
			key = NULL;
			size_t nl = names_len < 64 ? names_len : 64;
			for (int k = 0; k < LOSSANTOS_NG_KEYS && !key; k++) {
				memcpy(probe, ent, pl);
				lossantos_ng_decrypt(a->ng, probe, pl, a->ng->keys[k]);
				if (!lossantos_toc_shaped(probe, count)) continue;
				memcpy(nprobe, names, nl);
				lossantos_ng_decrypt(a->ng, nprobe, nl, a->ng->keys[k]);
				if (printable_fraction_ok(nprobe, nl)) key = a->ng->keys[k];
			}
			if (!key) {
				free(ent);
				free(names);
				lossantos_set_error("%s: no NG key decrypts the table of contents", a->name);
				return 0;
			}
		}
		lossantos_ng_decrypt(a->ng, ent, (size_t)count * 16, key);
		lossantos_ng_decrypt(a->ng, (uint8_t *)names, names_len, key);
	} else if (enc == LOSSANTOS_ENC_AES && a->ng->has_aes) {
		if (!lossantos_aes_decrypt(a->ng->aes, ent, (size_t)count * 16) ||
			!lossantos_aes_decrypt(a->ng->aes, (uint8_t *)names, names_len)) {
			free(ent);
			free(names);
			lossantos_set_error("%s: AES table decrypt failed", a->name);
			return 0;
		}
	} else if (enc != LOSSANTOS_ENC_NONE && enc != LOSSANTOS_ENC_OPEN) {
		free(ent);
		free(names);
		lossantos_set_error("%s: unsupported table encryption 0x%08x", a->name, enc);
		return 0;
	}

	a->e = (lossantos_entry *)calloc(count, sizeof(lossantos_entry));
	if (!a->e) {
		free(ent);
		free(names);
		return 0;
	}
	a->count = count;
	a->names = names;
	a->enc = enc;
	for (uint32_t i = 0; i < count; i++) {
		// 0   u16 name offset
		// 2   u24 size
		// 5   u24 offset in blocks, 0x7FFFFF = dir, top bit = resource
		// 8   dir first, binary unc size, resource sys flags
		// 12  dir count, binary encrypted, resource gfx flags
		uint32_t w[4];
		memcpy(w, ent + i * 16, 16);
		lossantos_entry *e = &a->e[i];
		uint32_t noff = w[0] & 0xFFFF;
		e->name = noff < names_len ? names + noff : names + names_len;
		e->file_size = (w[0] >> 16) | ((w[1] & 0xFF) << 16);
		e->file_offset = (w[1] >> 8) & 0xFFFFFF;
		e->parent = -1;
		if (w[1] == 0x7FFFFF00u) {
			e->kind = LOSSANTOS_DIR;
			e->first = w[2];
			e->count = w[3];
		} else if ((w[1] & 0x80000000u) == 0) {
			e->kind = LOSSANTOS_BINARY;
			e->unc_size = w[2];
			e->encrypted = w[3] == 1;
		} else {
			e->kind = LOSSANTOS_RESOURCE;
			e->file_offset &= 0x7FFFFF;
			e->sys_flags = w[2];
			e->gfx_flags = w[3];
			size_t nl = strlen(e->name);
			if (nl >= 4 && _stricmp(e->name + nl - 4, ".ysc") == 0) e->encrypted = 1;
		}
	}
	free(ent);
	if (a->e[0].kind != LOSSANTOS_DIR) {
		lossantos_set_error("%s: table of contents has no root directory", a->name);
		return 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		const lossantos_entry *d = &a->e[i];
		if (d->kind != LOSSANTOS_DIR) continue;
		if ((uint64_t)d->first + d->count > count) {
			lossantos_set_error("%s: directory points past the table of contents", a->name);
			return 0;
		}
		for (uint32_t c = d->first; c < d->first + d->count; c++)
			if (c != i) a->e[c].parent = (int32_t)i;
	}
	return 1;
}

int lossantos_rpf_open(lossantos_rpf *a, const lossantos_ng *ng, const wchar_t *path) {
	memset(a, 0, sizeof *a);
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
						   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		lossantos_set_error("cannot open %ls", path);
		return 0;
	}
	LARGE_INTEGER sz;
	GetFileSizeEx(h, &sz);
	a->h = h;
	a->own_handle = 1;
	a->size = (uint64_t)sz.QuadPart;
	a->ng = ng;
	const wchar_t *base = wcsrchr(path, L'\\');
	base = base ? base + 1 : path;
	WideCharToMultiByte(CP_UTF8, 0, base, -1, a->name, sizeof a->name, NULL, NULL);
	if (!read_toc(a)) {
		lossantos_rpf_close(a);
		return 0;
	}
	return 1;
}

int lossantos_rpf_open_child(lossantos_rpf *c, const lossantos_rpf *p, uint32_t idx) {
	memset(c, 0, sizeof *c);
	if (idx >= p->count || p->e[idx].kind == LOSSANTOS_DIR) {
		lossantos_set_error("not an archive entry");
		return 0;
	}
	c->h = p->h;
	c->own_handle = 0;
	c->ng = p->ng;
	c->start = p->start + (uint64_t)p->e[idx].file_offset * BLOCK;
	c->size = lossantos_rpf_file_size((lossantos_rpf *)p, idx);
	snprintf(c->name, sizeof c->name, "%s", p->e[idx].name);
	if (!read_toc(c)) {
		lossantos_rpf_close(c);
		return 0;
	}
	return 1;
}

void lossantos_rpf_close(lossantos_rpf *a) {
	if (a->own_handle && a->h) CloseHandle((HANDLE)a->h);
	free(a->e);
	free(a->names);
	memset(a, 0, sizeof *a);
}

void lossantos_rpf_path(const lossantos_rpf *a, uint32_t idx, char *dst, size_t cap) {
	const char *parts[64];
	int n = 0;
	for (int32_t i = (int32_t)idx; i > 0 && n < 64; i = a->e[i].parent) parts[n++] = a->e[i].name;
	size_t len = (size_t)snprintf(dst, cap, "%s", a->name);
	while (n-- > 0 && len < cap) len += (size_t)snprintf(dst + len, cap - len, "/%s", parts[n]);
}

int32_t lossantos_rpf_find(const lossantos_rpf *a, const char *path) {
	char buf[1024];
	snprintf(buf, sizeof buf, "%s", path);
	for (char *q = buf; *q; q++)
		if (*q == '\\') *q = '/';
	char *ctx = NULL;
	char *part = strtok_s(buf, "/", &ctx);
	if (part && _stricmp(part, a->name) == 0) part = strtok_s(NULL, "/", &ctx);
	uint32_t cur = 0;
	while (part) {
		const lossantos_entry *d = &a->e[cur];
		if (d->kind != LOSSANTOS_DIR) return -1;
		int32_t hit = -1;
		for (uint32_t c = d->first; c < d->first + d->count; c++)
			if (_stricmp(a->e[c].name, part) == 0) {
				hit = (int32_t)c;
				break;
			}
		if (hit < 0) return -1;
		cur = (uint32_t)hit;
		part = strtok_s(NULL, "/", &ctx);
	}
	return (int32_t)cur;
}

uint32_t lossantos_rpf_size_from_flags(uint32_t f) {
	// weighted page counts, page = 0x200 << low nibble
	uint32_t blocks = ((f >> 27) & 1) * 1 + ((f >> 26) & 1) * 2 + ((f >> 25) & 1) * 4 +
					  ((f >> 24) & 1) * 8 + ((f >> 17) & 0x7F) * 16 + ((f >> 11) & 0x3F) * 32 +
					  ((f >> 7) & 0xF) * 64 + ((f >> 5) & 3) * 128 + ((f >> 4) & 1) * 256;
	return (0x200u << (f & 0xF)) * blocks;
}

uint32_t lossantos_rpf_file_size(lossantos_rpf *a, uint32_t idx) {
	lossantos_entry *e = &a->e[idx];
	if (e->kind == LOSSANTOS_DIR) return 0;
	if (e->kind == LOSSANTOS_BINARY) return e->file_size ? e->file_size : e->unc_size;
	if (e->file_size == 0xFFFFFFu) {
		// real size is scattered through the rsc header, dont ask
		uint8_t h[16];
		if (lossantos_rpf_read_at(a, (uint64_t)e->file_offset * BLOCK, h, 16))
			e->file_size = (uint32_t)h[7] | ((uint32_t)h[14] << 8) | ((uint32_t)h[5] << 16) |
						   ((uint32_t)h[2] << 24);
	}
	if (e->file_size == 0)
		return lossantos_rpf_size_from_flags(e->sys_flags) + lossantos_rpf_size_from_flags(e->gfx_flags);
	return e->file_size;
}

int lossantos_rpf_read(lossantos_rpf *a, uint32_t idx, lossantos_buf *out) {
	lossantos_entry *e = &a->e[idx];
	out->n = 0;
	if (e->kind == LOSSANTOS_DIR) {
		lossantos_set_error("%s: is a directory", e->name);
		return 0;
	}
	uint32_t size = lossantos_rpf_file_size(a, idx);
	if (size == 0) return 1;
	uint8_t *raw = (uint8_t *)malloc(size);
	if (!raw) {
		lossantos_set_error("out of memory");
		return 0;
	}
	if (!lossantos_rpf_read_at(a, (uint64_t)e->file_offset * BLOCK, raw, size)) {
		free(raw);
		return 0;
	}
	uint8_t *data = raw;
	size_t len = size;
	if (e->kind == LOSSANTOS_RESOURCE) {
		if (len < RSC_HEADER) {
			free(raw);
			return 1;
		}
		data += RSC_HEADER;
		len -= RSC_HEADER;
	}
	if (e->encrypted) {
		if (a->enc == LOSSANTOS_ENC_AES && a->ng->has_aes) {
			lossantos_aes_decrypt(a->ng->aes, data, len);
		} else {
			uint32_t keylen = e->kind == LOSSANTOS_RESOURCE ? e->file_size : e->unc_size;
			lossantos_ng_decrypt(a->ng, data, len, lossantos_key_for(a->ng, e->name, keylen));
		}
	}
	int ok = 1;
	if (e->kind == LOSSANTOS_BINARY && e->file_size == 0) {
		if ((ok = lossantos_buf_reserve(out, len)) != 0) {
			memcpy(out->p, data, len);
			out->n = len;
		}
	} else {
		size_t hint = e->kind == LOSSANTOS_RESOURCE
						  ? (size_t)lossantos_rpf_size_from_flags(e->sys_flags) + lossantos_rpf_size_from_flags(e->gfx_flags)
						  : e->unc_size;
		if (!lossantos_inflate(data, len, out, hint)) {
			// inflate failed, keep the stored bytes
			ok = lossantos_buf_reserve(out, len);
			if (ok) {
				memcpy(out->p, data, len);
				out->n = len;
			}
		}
	}
	free(raw);
	return ok;
}
