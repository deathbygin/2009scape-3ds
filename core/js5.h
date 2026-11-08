// Portable reader for the revision 530 JS5 disk cache. Plain C, no dependencies, runs on PC and 3DS.
#ifndef JS5_H
#define JS5_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define JS5_MAX_ARCHIVES 32

typedef struct {
	uint32_t* fileIds;    // NULL when ids are simply 0..size-1
	uint32_t* fileHashes; // indexed like fileIds, NULL if the archive has no names
	uint32_t size;        // number of files
	uint32_t crc;
	uint32_t nameHash;
	uint32_t version;
	uint8_t present;
} Js5Group;

typedef struct {
	Js5Group* groups; // indexed by group id
	uint32_t capacity;
	uint32_t count;
	uint8_t named;
} Js5Index;

// The 3DS allows few open files, so index files open on demand and only this many stay open.
#define JS5_MAX_OPEN_INDEXES 6

typedef struct {
	FILE* data;
	FILE* idx[JS5_MAX_ARCHIVES];
	FILE* master;
	long sectors, dataPosition;
	Js5Index index[JS5_MAX_ARCHIVES];
	char dir[256];
	char error[96];
	int openList[JS5_MAX_OPEN_INDEXES];
	int openCount, openNext, peakOpen;
} Js5Cache;

typedef struct {
	uint8_t* data;
	size_t size;
} Js5Buffer;

int js5_open(Js5Cache* cache, const char* dir);
void js5_close(Js5Cache* cache);

// Returns 1 if the cache has this archive's index file.
int js5_has_archive(Js5Cache* cache, int archive);

// Reads one archive's reference table. Returns 0 on success.
int js5_load_index(Js5Cache* cache, int archive);
void js5_free_index(Js5Index* index);

// Raw container bytes with the 2-byte version trailer removed. Caller frees .data. Returns 0 on success.
int js5_read_raw(Js5Cache* cache, int archive, uint32_t group, Js5Buffer* out, uint32_t* versionOut);

// Decrypts (key may be NULL) and decompresses a container in place of out. Returns 0 on success.
int js5_decompress(const Js5Buffer* raw, const int32_t* key, Js5Buffer* out);

// Reads and decompresses a group. Returns 0 on success.
int js5_read_group(Js5Cache* cache, int archive, uint32_t group, const int32_t* key, Js5Buffer* out);

// Extracts one file from a decompressed group. Returns 0 on success.
int js5_group_file(const Js5Index* index, uint32_t group, const Js5Buffer* unpacked, uint32_t fileId, Js5Buffer* out);

// Finds a group by lower-case name, or returns -1.
int js5_find_group(const Js5Index* index, const char* name);

uint32_t js5_crc32(const uint8_t* data, size_t size);

// Decoders. Each returns 0 only if exactly outSize bytes were produced.
int js5_bunzip2(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize);
int js5_inflate(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize);

#endif
