#include "huffman.h"

#include <string.h>

#define MAX_SYMBOLS 256
#define MAX_NODES 4096

static uint8_t lengths[MAX_SYMBOLS];
static uint32_t codewords[MAX_SYMBOLS];
static int32_t tree[MAX_NODES];
static int ready = 0;

// Follows HuffmanCodec's constructor: codes are kept in the top bits of a word.
void huffman_init(const uint8_t* bits, int count) {
	uint32_t next[33];
	int nextNode = 0;
	memset(next, 0, sizeof(next));
	memset(tree, 0, sizeof(tree));
	memset(lengths, 0, sizeof(lengths));
	if (count > MAX_SYMBOLS) count = MAX_SYMBOLS;
	for (int symbol = 0; symbol < count; symbol++) {
		int length = bits[symbol];
		lengths[symbol] = (uint8_t)length;
		if (length == 0 || length > 32) continue;
		uint32_t bit = 1u << (32 - length), code = next[length], following;
		codewords[symbol] = code;
		if ((code & bit) == 0) {
			for (int i = length - 1; i >= 1; i--) {
				uint32_t parent = next[i], mask = 1u << (32 - i);
				if (code != parent) break;
				if (parent & mask) {
					next[i] = next[i - 1];
					break;
				}
				next[i] = mask | parent;
			}
			following = code | bit;
		} else {
			following = next[length - 1];
		}
		next[length] = following;
		for (int i = length + 1; i <= 32; i++) if (code == next[i]) next[i] = following;
		int node = 0;
		for (int i = 0; i < length; i++) {
			if ((code & (0x80000000u >> i)) == 0) {
				node++;
			} else {
				if (tree[node] == 0) tree[node] = nextNode;
				node = tree[node];
			}
			if (node >= MAX_NODES) return;
		}
		tree[node] = ~symbol;
		if (node >= nextNode) nextNode = node + 1;
	}
	ready = 1;
}

int huffman_encode(const uint8_t* src, int size, uint8_t* dest, int destSize) {
	int bitPos = 0;
	memset(dest, 0, (size_t)destSize);
	for (int i = 0; i < size; i++) {
		int length = lengths[src[i]];
		uint32_t code = codewords[src[i]];
		if (length == 0 || (bitPos + length + 7) / 8 > destSize) continue;
		for (int k = 0; k < length; k++, bitPos++) {
			if (code & (0x80000000u >> k)) dest[bitPos >> 3] |= (uint8_t)(0x80 >> (bitPos & 7));
		}
	}
	return (bitPos + 7) / 8;
}

int huffman_decode(const uint8_t* src, int srcSize, uint8_t* dest, int count) {
	if (!ready) return -1;
	if (count == 0) return 0;
	int node = 0, done = 0;
	for (int at = 0; at < srcSize; at++) {
		for (int k = 7; k >= 0; k--) {
			if (src[at] >> k & 1) node = tree[node];
			else node++;
			if (node < 0 || node >= MAX_NODES) return -1;
			int32_t symbol = tree[node];
			if (symbol < 0) {
				dest[done++] = (uint8_t)~symbol;
				if (done >= count) return at + 1;
				node = 0;
			}
		}
	}
	return -1;
}
