// Small bzip2 and inflate decoders for JS5 containers.
#include "js5.h"

#include <stdlib.h>
#include <string.h>

// ---- bzip2 (headerless, as stored by JS5: the stream starts at the first block magic) ----

#define BZ_MAX_ALPHA 258
#define BZ_MAX_GROUPS 6
#define BZ_GROUP_SIZE 50
#define BZ_MAX_LEN 20
#define BZ_MAX_SELECTORS 18002
#define BZ_BLOCK 900000

typedef struct {
	const uint8_t* in;
	size_t size, pos;
	uint32_t bits;
	int count, bad;
} BitReader;

static uint32_t bz_bits(BitReader* b, int n) {
	uint32_t v = 0;
	while (n > 0) {
		if (b->count == 0) {
			if (b->pos >= b->size) {
				b->bad = 1;
				return 0;
			}
			b->bits = b->in[b->pos++];
			b->count = 8;
		}
		int take = n < b->count ? n : b->count;
		v = v << take | (b->bits >> (b->count - take) & ((1u << take) - 1));
		b->count -= take;
		n -= take;
	}
	return v;
}

typedef struct {
	int32_t limit[BZ_MAX_LEN + 2], base[BZ_MAX_LEN + 2];
	uint16_t perm[BZ_MAX_ALPHA];
	int minLen, maxLen;
} BzTable;

static void bz_table(BzTable* t, const uint8_t* lengths, int alpha) {
	int minLen = 32, maxLen = 0;
	for (int i = 0; i < alpha; i++) {
		if (lengths[i] > maxLen) maxLen = lengths[i];
		if (lengths[i] < minLen) minLen = lengths[i];
	}
	t->minLen = minLen;
	t->maxLen = maxLen;
	int pp = 0, count[BZ_MAX_LEN + 2] = {0};
	for (int len = minLen; len <= maxLen; len++)
		for (int i = 0; i < alpha; i++)
			if (lengths[i] == len) t->perm[pp++] = (uint16_t)i;
	for (int i = 0; i < alpha; i++) count[lengths[i]]++;
	int code = 0, index = 0;
	for (int len = minLen; len <= maxLen; len++) {
		t->base[len] = index - code;
		code += count[len];
		index += count[len];
		t->limit[len] = code - 1;
		code <<= 1;
	}
}

int js5_bunzip2(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize) {
	static uint8_t selectors[BZ_MAX_SELECTORS];
	static BzTable tables[BZ_MAX_GROUPS];
	uint32_t* tt = malloc(sizeof(uint32_t) * BZ_BLOCK);
	if (!tt) return -1;
	BitReader b = {in, inSize, 0, 0, 0, 0};
	size_t written = 0;
	int rc = -2;
	for (;;) {
		uint32_t m1 = bz_bits(&b, 24), m2 = bz_bits(&b, 24);
		if (b.bad) break;
		if (m1 == 0x177245 && m2 == 0x385090) {
			rc = written == outSize ? 0 : -3;
			break;
		}
		if (m1 != 0x314159 || m2 != 0x265359) {
			rc = -4;
			break;
		}
		bz_bits(&b, 32); // block CRC
		if (bz_bits(&b, 1)) {
			rc = -5; // randomised blocks are obsolete
			break;
		}
		uint32_t origPtr = bz_bits(&b, 24);

		uint8_t seqToUnseq[256];
		int inUse = 0;
		uint32_t used16 = bz_bits(&b, 16);
		for (int i = 0; i < 16; i++)
			if (used16 & (0x8000u >> i)) {
				uint32_t used = bz_bits(&b, 16);
				for (int j = 0; j < 16; j++)
					if (used & (0x8000u >> j)) seqToUnseq[inUse++] = (uint8_t)(i * 16 + j);
			}
		if (inUse == 0) {
			rc = -6;
			break;
		}
		int alpha = inUse + 2;
		int groups = (int)bz_bits(&b, 3);
		int nSelectors = (int)bz_bits(&b, 15);
		if (groups < 2 || groups > BZ_MAX_GROUPS || nSelectors < 1 || nSelectors > BZ_MAX_SELECTORS) {
			rc = -7;
			break;
		}
		uint8_t mtf[BZ_MAX_GROUPS];
		for (int i = 0; i < groups; i++) mtf[i] = (uint8_t)i;
		for (int i = 0; i < nSelectors; i++) {
			int j = 0;
			while (bz_bits(&b, 1)) {
				if (++j >= groups) {
					b.bad = 1;
					break;
				}
			}
			if (b.bad) break;
			uint8_t v = mtf[j];
			memmove(mtf + 1, mtf, (size_t)j);
			mtf[0] = v;
			selectors[i] = v;
		}
		if (b.bad) break;
		for (int t = 0; t < groups; t++) {
			uint8_t lengths[BZ_MAX_ALPHA];
			int cur = (int)bz_bits(&b, 5);
			for (int i = 0; i < alpha; i++) {
				for (;;) {
					if (cur < 1 || cur > BZ_MAX_LEN) b.bad = 1;
					if (b.bad || !bz_bits(&b, 1)) break;
					cur += bz_bits(&b, 1) ? -1 : 1;
				}
				if (b.bad) break;
				lengths[i] = (uint8_t)cur;
			}
			if (b.bad) break;
			bz_table(&tables[t], lengths, alpha);
		}
		if (b.bad) break;

		uint8_t symbols[256];
		for (int i = 0; i < 256; i++) symbols[i] = (uint8_t)i;
		uint32_t unzftab[256] = {0};
		int eob = inUse + 1, selectorIndex = 0, groupLeft = 0;
		const BzTable* table = NULL;
		uint32_t blockLen = 0, runLength = 0, runBit = 0;
		for (;;) {
			if (groupLeft == 0) {
				if (selectorIndex >= nSelectors) {
					b.bad = 1;
					break;
				}
				table = &tables[selectors[selectorIndex++]];
				groupLeft = BZ_GROUP_SIZE;
			}
			groupLeft--;
			int len = table->minLen;
			int32_t code = (int32_t)bz_bits(&b, len);
			while (len <= table->maxLen && code > table->limit[len]) {
				len++;
				code = code << 1 | (int32_t)bz_bits(&b, 1);
			}
			if (b.bad || len > table->maxLen) {
				b.bad = 1;
				break;
			}
			int index = code + table->base[len];
			if (index < 0 || index >= BZ_MAX_ALPHA) {
				b.bad = 1;
				break;
			}
			int sym = table->perm[index];
			if (sym == 0 || sym == 1) { // RUNA, RUNB
				runLength += (uint32_t)(sym + 1) << runBit;
				runBit++;
				if (runBit > 24) {
					b.bad = 1;
					break;
				}
				continue;
			}
			if (runLength) {
				uint8_t value = seqToUnseq[symbols[0]];
				if (blockLen + runLength > BZ_BLOCK) {
					b.bad = 1;
					break;
				}
				unzftab[value] += runLength;
				while (runLength--) tt[blockLen++] = value;
				runLength = 0;
				runBit = 0;
			}
			if (sym == eob) break;
			if (blockLen >= BZ_BLOCK) {
				b.bad = 1;
				break;
			}
			int pos = sym - 1;
			uint8_t v = symbols[pos];
			memmove(symbols + 1, symbols, (size_t)pos);
			symbols[0] = v;
			uint8_t value = seqToUnseq[v];
			unzftab[value]++;
			tt[blockLen++] = value;
		}
		if (b.bad || origPtr >= blockLen) {
			b.bad = 1;
			break;
		}
		uint32_t cftab[256], sum = 0;
		for (int i = 0; i < 256; i++) {
			cftab[i] = sum;
			sum += unzftab[i];
		}
		for (uint32_t i = 0; i < blockLen; i++) {
			uint8_t value = (uint8_t)tt[i];
			tt[cftab[value]++] |= i << 8;
		}
		uint32_t pos = tt[origPtr] >> 8;
		int run = 0, previous = -1;
		for (uint32_t i = 0; i < blockLen; i++) {
			uint32_t entry = tt[pos];
			uint8_t value = (uint8_t)entry;
			pos = entry >> 8;
			if (run == 4) { // the byte after four repeats is a repeat count
				if (written + value > outSize) {
					b.bad = 1;
					break;
				}
				memset(out + written, previous, value);
				written += value;
				run = 0;
				previous = -1;
				continue;
			}
			if (value == previous) run++;
			else {
				run = 1;
				previous = value;
			}
			if (written >= outSize) {
				b.bad = 1;
				break;
			}
			out[written++] = value;
		}
		if (b.bad) break;
	}
	free(tt);
	// Some JS5 streams end without a footer once all the output is produced.
	if (rc != 0 && written == outSize && outSize > 0 && !(rc == -4 || rc == -5)) rc = 0;
	return rc;
}

// ---- inflate ----

#define FAST_BITS 9

typedef struct {
	const uint8_t* in;
	size_t size, pos;
	uint32_t bits;
	int count, bad;
} InflateState;

typedef struct {
	uint16_t count[16];
	uint16_t symbol[288];
	uint16_t fast[1 << FAST_BITS]; // symbol << 4 | length, 0 when the code is longer than FAST_BITS
} Huffman;

static void in_refill(InflateState* s) {
	while (s->count <= 24 && s->pos < s->size) {
		s->bits |= (uint32_t)s->in[s->pos++] << s->count;
		s->count += 8;
	}
}

static uint32_t in_bits(InflateState* s, int need) {
	if (need == 0) return 0;
	if (s->count < need) in_refill(s);
	if (s->count < need) {
		s->bad = 1;
		return 0;
	}
	uint32_t v = s->bits & ((1u << need) - 1);
	s->bits >>= need;
	s->count -= need;
	return v;
}

static int in_decode(InflateState* s, const Huffman* h) {
	if (s->count < FAST_BITS) in_refill(s);
	uint16_t entry = h->fast[s->bits & ((1u << FAST_BITS) - 1)];
	if (entry && (entry & 15) <= s->count) {
		s->bits >>= entry & 15;
		s->count -= entry & 15;
		return entry >> 4;
	}
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= 15; len++) {
		code |= (int)in_bits(s, 1);
		if (s->bad) return -1;
		int count = h->count[len];
		if (code - count < first) return h->symbol[index + (code - first)];
		index += count;
		first = (first + count) << 1;
		code <<= 1;
	}
	return -1;
}

static void in_build(Huffman* h, const uint8_t* lengths, int n) {
	uint16_t offs[16], next[16];
	memset(h->count, 0, sizeof(h->count));
	memset(h->fast, 0, sizeof(h->fast));
	for (int i = 0; i < n; i++) h->count[lengths[i]]++;
	h->count[0] = 0;
	offs[1] = 0;
	next[1] = 0;
	for (int i = 1; i < 15; i++) {
		offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
		next[i + 1] = (uint16_t)((next[i] + h->count[i]) << 1);
	}
	for (int i = 0; i < n; i++) {
		int len = lengths[i];
		if (!len) continue;
		h->symbol[offs[len]++] = (uint16_t)i;
		uint32_t code = next[len]++;
		if (len > FAST_BITS) continue;
		uint32_t reversed = 0;
		for (int b = 0; b < len; b++) reversed |= (code >> b & 1) << (len - 1 - b);
		for (uint32_t j = reversed; j < (1u << FAST_BITS); j += 1u << len) h->fast[j] = (uint16_t)(i << 4 | len);
	}
}

static int in_codes(InflateState* s, uint8_t* out, size_t outSize, size_t* written, const Huffman* lit, const Huffman* dist) {
	static const uint16_t LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
	static const uint8_t LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
	static const uint16_t DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
	static const uint8_t DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
	for (;;) {
		int sym = in_decode(s, lit);
		if (sym < 0) return -1;
		if (sym < 256) {
			if (*written >= outSize) return -2;
			out[(*written)++] = (uint8_t)sym;
		} else if (sym == 256) {
			return 0;
		} else {
			sym -= 257;
			if (sym >= 29) return -3;
			size_t len = LEN_BASE[sym] + in_bits(s, LEN_EXTRA[sym]);
			int d = in_decode(s, dist);
			if (d < 0 || d >= 30) return -4;
			size_t back = DIST_BASE[d] + in_bits(s, DIST_EXTRA[d]);
			if (s->bad || back > *written || *written + len > outSize) return -5;
			for (size_t i = 0; i < len; i++, (*written)++) out[*written] = out[*written - back];
		}
	}
}

int js5_inflate(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize) {
	static Huffman lit, dist;
	InflateState s = {in, inSize, 0, 0, 0, 0};
	size_t written = 0;
	int last;
	do {
		last = (int)in_bits(&s, 1);
		int type = (int)in_bits(&s, 2);
		if (s.bad) return -1;
		if (type == 0) {
			s.pos -= (size_t)(s.count / 8); // return whole buffered bytes before a stored block
			s.bits = 0;
			s.count = 0;
			if (s.pos + 4 > s.size) return -2;
			size_t len = s.in[s.pos] | s.in[s.pos + 1] << 8;
			s.pos += 4;
			if (s.pos + len > s.size || written + len > outSize) return -3;
			memcpy(out + written, s.in + s.pos, len);
			s.pos += len;
			written += len;
		} else if (type == 1) {
			uint8_t lengths[320];
			int i = 0;
			for (; i < 144; i++) lengths[i] = 8;
			for (; i < 256; i++) lengths[i] = 9;
			for (; i < 280; i++) lengths[i] = 7;
			for (; i < 288; i++) lengths[i] = 8;
			in_build(&lit, lengths, 288);
			for (i = 0; i < 30; i++) lengths[i] = 5;
			in_build(&dist, lengths, 30);
			if (in_codes(&s, out, outSize, &written, &lit, &dist) != 0) return -4;
		} else if (type == 2) {
			static const uint8_t ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
			uint8_t lengths[320] = {0};
			int nlen = (int)in_bits(&s, 5) + 257, ndist = (int)in_bits(&s, 5) + 1, ncode = (int)in_bits(&s, 4) + 4;
			if (nlen > 286 || ndist > 30) return -5;
			for (int i = 0; i < ncode; i++) lengths[ORDER[i]] = (uint8_t)in_bits(&s, 3);
			in_build(&lit, lengths, 19);
			int index = 0;
			while (index < nlen + ndist) {
				int sym = in_decode(&s, &lit);
				if (sym < 0) return -6;
				if (sym < 16) lengths[index++] = (uint8_t)sym;
				else {
					int prev = 0, repeat;
					if (sym == 16) {
						if (index == 0) return -7;
						prev = lengths[index - 1];
						repeat = 3 + (int)in_bits(&s, 2);
					} else if (sym == 17) repeat = 3 + (int)in_bits(&s, 3);
					else repeat = 11 + (int)in_bits(&s, 7);
					if (index + repeat > nlen + ndist) return -8;
					while (repeat--) lengths[index++] = (uint8_t)prev;
				}
			}
			static uint8_t dlen[32];
			memcpy(dlen, lengths + nlen, (size_t)ndist);
			in_build(&lit, lengths, nlen);
			in_build(&dist, dlen, ndist);
			if (in_codes(&s, out, outSize, &written, &lit, &dist) != 0) return -9;
		} else {
			return -10;
		}
		if (s.bad) return -11;
	} while (!last);
	return written == outSize ? 0 : -12;
}
