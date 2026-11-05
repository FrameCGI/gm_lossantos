#include "lossantos_ytd.h"

#include <stdio.h>

static uint32_t block_bytes(uint32_t fmt) {
	switch (fmt) {
	case LOSSANTOS_FMT_DXT1:
	case LOSSANTOS_FMT_ATI1:
		return 8;
	case LOSSANTOS_FMT_DXT3:
	case LOSSANTOS_FMT_DXT5:
	case LOSSANTOS_FMT_ATI2:
	case LOSSANTOS_FMT_BC7:
		return 16;
	default:
		return 0;
	}
}

size_t lossantos_tex_level_size(uint32_t fmt, uint32_t width, uint32_t height, uint32_t stride, uint32_t level) {
	uint32_t w = width ? width : 1, h = height ? height : 1;
	for (uint32_t i = 0; i < level; i++) {
		w = w / 2 ? w / 2 : 1;
		h = h / 2 ? h / 2 : 1;
	}
	uint32_t bb = block_bytes(fmt);
	if (bb) {
		size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
		return (bw ? bw : 1) * (bh ? bh : 1) * bb;
	}
	size_t row;
	if (width) {
		row = ((size_t)stride * w) / width;
		if (!row) row = 1;
	} else {
		row = stride ? stride : 1;
	}
	return row * h;
}

// no bad flag, short tail levels just get dropped
static const uint8_t *probe(const lossantos_rsc *r, uint64_t addr, size_t len) {
	lossantos_rsc tmp = *r;
	return lossantos_rsc_ptr(&tmp, addr, len);
}

static int read_texture(lossantos_rsc *r, uint64_t addr, lossantos_tex *t) {
	// 0x28  name ptr
	// 0x50  width
	// 0x52  height
	// 0x54  depth
	// 0x56  stride
	// 0x58  format
	// 0x5D  mip levels
	// 0x70  data ptr
	uint64_t name_ptr = lossantos_rsc_u64(r, addr + 0x28);
	t->width = lossantos_rsc_u16(r, addr + 0x50);
	t->height = lossantos_rsc_u16(r, addr + 0x52);
	t->depth = lossantos_rsc_u16(r, addr + 0x54);
	if (!t->depth) t->depth = 1;
	t->stride = lossantos_rsc_u16(r, addr + 0x56);
	t->format = lossantos_rsc_u32(r, addr + 0x58);
	uint32_t levels = lossantos_rsc_u8(r, addr + 0x5D);
	uint64_t data_ptr = lossantos_rsc_u64(r, addr + 0x70);
	if (!t->width || !t->height || !data_ptr || !t->stride) return 0;
	if (!levels) levels = 1;
	size_t total = 0;
	uint32_t used = 0;
	for (uint32_t l = 0; l < levels; l++) {
		size_t len = lossantos_tex_level_size(t->format, t->width, t->height, t->stride, l);
		if (!probe(r, data_ptr + total, len)) break;
		total += len;
		used++;
	}
	if (!used) return 0;
	t->levels = used;
	t->data = probe(r, data_ptr, total);
	t->data_n = total;
	const char *nm = lossantos_rsc_str(r, name_ptr);
	if (*nm) snprintf(t->name, sizeof t->name, "%s", nm);
	else snprintf(t->name, sizeof t->name, "texture_%08llx", (unsigned long long)addr);
	return 1;
}

int lossantos_ytd_parse(lossantos_rsc *r, uint64_t addr, lossantos_tex *out, int cap) {
	uint64_t tex_ptr = lossantos_rsc_u64(r, addr + 0x30);
	uint32_t count = lossantos_rsc_u16(r, addr + 0x38);
	if (!tex_ptr || !count) return 0;
	int n = 0;
	for (uint32_t i = 0; i < count && n < cap; i++) {
		uint64_t p = lossantos_rsc_u64(r, tex_ptr + (uint64_t)i * 8);
		if (!p) continue;
		if (read_texture(r, p, &out[n])) n++;
	}
	return n;
}


static void rgb565(uint32_t v, int c[3]) {
	int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
	c[0] = (r << 3) | (r >> 2);
	c[1] = (g << 2) | (g >> 4);
	c[2] = (b << 3) | (b >> 2);
}

static void color_block(const uint8_t *b, int four_color, uint8_t out[16][4]) {
	uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
	uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
	int p[4][3];
	rgb565(c0, p[0]);
	rgb565(c1, p[1]);
	// c0 <= c1 is dxt1 punch through
	int opaque4 = four_color || c0 > c1;
	for (int k = 0; k < 3; k++) {
		if (opaque4) {
			p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
			p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
		} else {
			p[2][k] = (p[0][k] + p[1][k]) / 2;
			p[3][k] = 0;
		}
	}
	for (int i = 0; i < 16; i++) {
		int s = (idx >> (2 * i)) & 3;
		out[i][0] = (uint8_t)p[s][0];
		out[i][1] = (uint8_t)p[s][1];
		out[i][2] = (uint8_t)p[s][2];
		out[i][3] = (!four_color && !(c0 > c1) && s == 3) ? 0 : 255;
	}
}

static void dxt5_alpha(const uint8_t *b, uint8_t out[16][4]) {
	int a0 = b[0], a1 = b[1];
	uint64_t bits = 0;
	for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
	int lut[8];
	lut[0] = a0;
	lut[1] = a1;
	if (a0 > a1) {
		for (int k = 1; k <= 6; k++) lut[1 + k] = ((7 - k) * a0 + k * a1) / 7;
	} else {
		for (int k = 1; k <= 4; k++) lut[1 + k] = ((5 - k) * a0 + k * a1) / 5;
		lut[6] = 0;
		lut[7] = 255;
	}
	for (int p = 0; p < 16; p++) out[p][3] = (uint8_t)lut[(bits >> (3 * p)) & 7];
}

int lossantos_tex_decode(const lossantos_tex *t, uint32_t level, uint8_t *dst, size_t dst_cap, uint32_t *ow, uint32_t *oh) {
	if (level >= t->levels) level = t->levels - 1;
	size_t off = 0;
	for (uint32_t l = 0; l < level; l++) off += lossantos_tex_level_size(t->format, t->width, t->height, t->stride, l);
	size_t len = lossantos_tex_level_size(t->format, t->width, t->height, t->stride, level);
	if (off + len > t->data_n) return 0;
	const uint8_t *chunk = t->data + off;
	uint32_t w = t->width >> level, h = t->height >> level;
	if (!w) w = 1;
	if (!h) h = 1;
	*ow = w;
	*oh = h;
	if ((size_t)w * h * 4 > dst_cap) return 0;
	uint32_t fmt = t->format;
	if (fmt == LOSSANTOS_FMT_DXT1 || fmt == LOSSANTOS_FMT_DXT3 || fmt == LOSSANTOS_FMT_DXT5) {
		uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
		if (!bw) bw = 1;
		if (!bh) bh = 1;
		size_t bsz = fmt == LOSSANTOS_FMT_DXT1 ? 8 : 16;
		if ((size_t)bw * bh * bsz > len) return 0;
		for (uint32_t by = 0; by < bh; by++)
			for (uint32_t bx = 0; bx < bw; bx++) {
				const uint8_t *b = chunk + ((size_t)by * bw + bx) * bsz;
				uint8_t px[16][4];
				if (fmt == LOSSANTOS_FMT_DXT1) {
					color_block(b, 0, px);
				} else {
					color_block(b + 8, 1, px);
					if (fmt == LOSSANTOS_FMT_DXT5) {
						dxt5_alpha(b, px);
					} else {
						// dxt3 4 bit alpha
						for (int i = 0; i < 8; i++) {
							px[i * 2][3] = (uint8_t)((b[i] & 0xF) * 17);
							px[i * 2 + 1][3] = (uint8_t)((b[i] >> 4) * 17);
						}
					}
				}
				for (int i = 0; i < 16; i++) {
					uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
					if (x < w && y < h) memcpy(dst + ((size_t)y * w + x) * 4, px[i], 4);
				}
			}
		return 1;
	}
	size_t px = (size_t)w * h;
	if (fmt == LOSSANTOS_FMT_A8R8G8B8 || fmt == LOSSANTOS_FMT_X8R8G8B8) {
		if (len < px * 4) return 0;
		// bgra to rgba
		for (size_t i = 0; i < px; i++) {
			dst[i * 4 + 0] = chunk[i * 4 + 2];
			dst[i * 4 + 1] = chunk[i * 4 + 1];
			dst[i * 4 + 2] = chunk[i * 4 + 0];
			dst[i * 4 + 3] = chunk[i * 4 + 3];
		}
		return 1;
	}
	if (fmt == LOSSANTOS_FMT_A8B8G8R8) {
		if (len < px * 4) return 0;
		memcpy(dst, chunk, px * 4);
		return 1;
	}
	if (fmt == LOSSANTOS_FMT_L8 || fmt == LOSSANTOS_FMT_A8) {
		if (len < px) return 0;
		for (size_t i = 0; i < px; i++) {
			uint8_t v = chunk[i];
			dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = v;
			dst[i * 4 + 3] = fmt == LOSSANTOS_FMT_L8 ? 255 : v;
		}
		return 1;
	}
	return 0;
}
