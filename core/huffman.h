// The Huffman code the game packs player messages with. The table comes from the cache.
#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>

// bits gives the code length of each byte value.
void huffman_init(const uint8_t* bits, int count);

// Packs size bytes and returns how many came out. Values with no code are left out.
int huffman_encode(const uint8_t* src, int size, uint8_t* dest, int destSize);

// Unpacks count bytes and returns how many source bytes that took, or -1 if the data ran out or there is no table.
int huffman_decode(const uint8_t* src, int srcSize, uint8_t* dest, int count);

#endif
