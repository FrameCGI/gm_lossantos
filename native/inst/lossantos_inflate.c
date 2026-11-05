#include "lossantos_inflate.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#define FASTB 10
#define MAXBITS 15

typedef struct {
	uint16_t fast[1 << FASTB];   /* sym | len << 12, 0 = not in fast table */
	uint16_t count[MAXBITS + 1];
	uint16_t sym[320];
} huff;

typedef struct {
	const uint8_t *src;
	size_t len, pos;
	uint64_t buf;
	int nbits;
	int overrun;               /* bytes of zero padding consumed past the end */
} bits;

static inline void refill(bits *s) {
	while (s->nbits <= 56) {
		uint64_t byte = 0;
		if (s->pos < s->len) byte = s->src[s->pos++];
		else s->overrun++;
		s->buf |= byte << s->nbits;
		s->nbits += 8;
	}
}

static inline uint32_t getbits(bits *s, int n) {
	if (n == 0) return 0;
	if (s->nbits < n) refill(s);
	uint32_t v = (uint32_t)(s->buf & ((1ull << n) - 1));
	s->buf >>= n;
	s->nbits -= n;
	return v;
}

static int huff_build(huff *h, const uint8_t *lens, int n) {
	memset(h->count, 0, sizeof h->count);
	memset(h->fast, 0, sizeof h->fast);
	for (int i = 0; i < n; i++) h->count[lens[i]]++;
	h->count[0] = 0;
	int left = 1;
	for (int l = 1; l <= MAXBITS; l++) {
		left <<= 1;
		left -= h->count[l];
		if (left < 0) return 0;                 /* over-subscribed */
	}
	uint16_t offs[MAXBITS + 2];
	offs[1] = 0;
	for (int l = 1; l <= MAXBITS; l++) offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
	for (int i = 0; i < n; i++)
		if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
	// fast table, bit reversed codes
	uint32_t code = 0;
	int idx = 0;
	for (int l = 1; l <= MAXBITS; l++) {
		for (int j = 0; j < h->count[l]; j++, idx++, code++) {
			if (l > FASTB) continue;
			uint32_t rev = 0;
			for (int b = 0; b < l; b++) rev |= ((code >> b) & 1u) << (l - 1 - b);
			for (uint32_t f = rev; f < (1u << FASTB); f += 1u << l)
				h->fast[f] = (uint16_t)(h->sym[idx] | (l << 12));
		}
		code <<= 1;
	}
	return 1;
}

static inline int decode(bits *s, const huff *h) {
	if (s->nbits < MAXBITS) refill(s);
	uint16_t e = h->fast[s->buf & ((1u << FASTB) - 1)];
	if (e) {
		int l = e >> 12;
		s->buf >>= l;
		s->nbits -= l;
		return e & 0x1FF;
	}
	// slow path, one bit at a time
	int code = 0, first = 0, index = 0;
	for (int l = 1; l <= MAXBITS; l++) {
		code |= (int)(s->buf & 1);
		s->buf >>= 1;
		s->nbits--;
		int count = h->count[l];
		if (code - count < first) return h->sym[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

// rfc 1951 length and distance tables
static const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
								   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
								 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
								   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
								   6145, 8193, 12289, 16385, 24577};
static const uint8_t DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
								 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(bits *s, lossantos_buf *out, const huff *lh, const huff *dh) {
	for (;;) {
		int sym = decode(s, lh);
		if (sym < 0) return 0;
		if (sym < 256) {
			if (out->n + 1 > out->cap && !lossantos_buf_reserve(out, out->n + 1)) return 0;
			out->p[out->n++] = (uint8_t)sym;
		} else if (sym == 256) {
			return 1;
		} else {
			sym -= 257;
			if (sym >= 29) return 0;
			size_t len = LBASE[sym] + getbits(s, LEXT[sym]);
			int ds = decode(s, dh);
			if (ds < 0 || ds >= 30) return 0;
			size_t dist = DBASE[ds] + getbits(s, DEXT[ds]);
			if (dist > out->n) return 0;
			if (out->n + len > out->cap && !lossantos_buf_reserve(out, out->n + len)) return 0;
			uint8_t *d = out->p + out->n;
			const uint8_t *from = d - dist;
			if (dist >= len) memcpy(d, from, len);
			else for (size_t i = 0; i < len; i++) d[i] = from[i];
			out->n += len;
		}
		if (s->overrun > 8) return 0;
	}
}

static huff g_fixed_l, g_fixed_d;
static INIT_ONCE g_fixed_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK build_fixed(PINIT_ONCE once, PVOID param, PVOID *ctx) {
	(void)once; (void)param; (void)ctx;
	uint8_t l[288];
	int i = 0;
	for (; i < 144; i++) l[i] = 8;
	for (; i < 256; i++) l[i] = 9;
	for (; i < 280; i++) l[i] = 7;
	for (; i < 288; i++) l[i] = 8;
	huff tl, td;
	huff_build(&tl, l, 288);
	uint8_t d[30];
	for (i = 0; i < 30; i++) d[i] = 5;
	huff_build(&td, d, 30);
	g_fixed_l = tl;
	g_fixed_d = td;
	return TRUE;
}

static void fixed_tables(void) { InitOnceExecuteOnce(&g_fixed_once, build_fixed, NULL, NULL); }

static int dynamic(bits *s, lossantos_buf *out) {
	static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	int nlen = (int)getbits(s, 5) + 257;
	int ndist = (int)getbits(s, 5) + 1;
	int ncode = (int)getbits(s, 4) + 4;
	if (nlen > 286 || ndist > 30) return 0;
	uint8_t lens[320] = {0};
	for (int i = 0; i < ncode; i++) lens[ORDER[i]] = (uint8_t)getbits(s, 3);
	huff ch;
	if (!huff_build(&ch, lens, 19)) return 0;
	int idx = 0;
	memset(lens, 0, sizeof lens);
	while (idx < nlen + ndist) {
		int sym = decode(s, &ch);
		if (sym < 0) return 0;
		if (sym < 16) {
			lens[idx++] = (uint8_t)sym;
		} else {
			uint8_t val = 0;
			int rep;
			if (sym == 16) {
				if (idx == 0) return 0;
				val = lens[idx - 1];
				rep = 3 + (int)getbits(s, 2);
			} else if (sym == 17) {
				rep = 3 + (int)getbits(s, 3);
			} else {
				rep = 11 + (int)getbits(s, 7);
			}
			if (idx + rep > nlen + ndist) return 0;
			while (rep--) lens[idx++] = val;
		}
	}
	if (lens[256] == 0) return 0;
	huff lh, dh;
	if (!huff_build(&lh, lens, nlen)) return 0;
	if (!huff_build(&dh, lens + nlen, ndist)) return 0;
	return codes(s, out, &lh, &dh);
}

int lossantos_inflate(const uint8_t *src, size_t srclen, lossantos_buf *out, size_t hint) {
	bits s = {src, srclen, 0, 0, 0, 0};
	out->n = 0;
	if (!lossantos_buf_reserve(out, hint ? hint : srclen * 4 + 64)) return 0;
	fixed_tables();
	int last;
	do {
		last = (int)getbits(&s, 1);
		int type = (int)getbits(&s, 2);
		if (type == 0) {  // stored
			int drop = s.nbits & 7;
			getbits(&s, drop);
			uint32_t len = getbits(&s, 16);
			uint32_t nlen = getbits(&s, 16);
			if ((len ^ 0xFFFFu) != nlen) goto bad;
			if (!lossantos_buf_reserve(out, out->n + len)) return 0;
			while (len && s.nbits >= 8) {
				out->p[out->n++] = (uint8_t)getbits(&s, 8);
				len--;
			}
			if (len) {
				if (s.pos + len > s.len) goto bad;
				memcpy(out->p + out->n, s.src + s.pos, len);
				out->n += len;
				s.pos += len;
			}
		} else if (type == 1) {  // fixed
			if (!codes(&s, out, &g_fixed_l, &g_fixed_d)) goto bad;
		} else if (type == 2) {  // dynamic
			if (!dynamic(&s, out)) goto bad;
		} else {
			goto bad;
		}
	} while (!last);
	return 1;
bad:
	lossantos_set_error("inflate: corrupt deflate stream at byte %zu", s.pos);
	return 0;
}
