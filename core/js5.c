#include "js5.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 520
#define SECTOR_DATA 512
#define SECTOR_HEADER 8

static uint32_t crcTable[256];
static int crcReady = 0;

uint32_t js5_crc32(const uint8_t* data, size_t size) {
	if (!crcReady) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crcTable[i] = c;
		}
		crcReady = 1;
	}
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; i++) crc = crc >> 8 ^ crcTable[(crc ^ data[i]) & 0xFF];
	return ~crc;
}

int js5_open(Js5Cache* cache, const char* dir) {
	char path[512];
	memset(cache, 0, sizeof(*cache));
	snprintf(cache->dir, sizeof(cache->dir), "%s", dir);
	snprintf(path, sizeof(path), "%s/main_file_cache.dat2", dir);
	cache->data = fopen(path, "rb");
	if (!cache->data) {
		snprintf(cache->error, sizeof(cache->error), "cannot open main_file_cache.dat2 (errno %d)", errno);
		return -1;
	}
	setvbuf(cache->data, NULL, _IOFBF, 4096);
	fseek(cache->data, 0, SEEK_END);
	cache->sectors = ftell(cache->data) / SECTOR_SIZE;
	cache->dataPosition = -1;
	snprintf(path, sizeof(path), "%s/main_file_cache.idx255", dir);
	cache->master = fopen(path, "rb");
	if (!cache->master) {
		snprintf(cache->error, sizeof(cache->error), "cannot open main_file_cache.idx255 (errno %d)", errno);
		return -2;
	}
	return 0;
}

static FILE* index_file(Js5Cache* cache, int archive) {
	if (archive < 0 || archive >= JS5_MAX_ARCHIVES) return NULL;
	if (cache->idx[archive]) return cache->idx[archive];
	if (cache->openCount == JS5_MAX_OPEN_INDEXES) {
		int oldest = cache->openList[cache->openNext];
		fclose(cache->idx[oldest]);
		cache->idx[oldest] = NULL;
		cache->openCount--;
	}
	char path[300];
	snprintf(path, sizeof(path), "%s/main_file_cache.idx%d", cache->dir, archive);
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(cache->error, sizeof(cache->error), "cannot open main_file_cache.idx%d (errno %d)", archive, errno);
		return NULL;
	}
	cache->idx[archive] = f;
	cache->openList[cache->openNext] = archive;
	cache->openNext = (cache->openNext + 1) % JS5_MAX_OPEN_INDEXES;
	cache->openCount++;
	if (cache->openCount > cache->peakOpen) cache->peakOpen = cache->openCount;
	return f;
}

int js5_has_archive(Js5Cache* cache, int archive) {
	return index_file(cache, archive) != NULL;
}

void js5_free_index(Js5Index* index) {
	if (!index->groups) return;
	for (uint32_t i = 0; i < index->capacity; i++) {
		free(index->groups[i].fileIds);
		free(index->groups[i].fileHashes);
	}
	free(index->groups);
	memset(index, 0, sizeof(*index));
}

void js5_close(Js5Cache* cache) {
	for (int i = 0; i < JS5_MAX_ARCHIVES; i++) {
		js5_free_index(&cache->index[i]);
		if (cache->idx[i]) fclose(cache->idx[i]);
	}
	if (cache->master) fclose(cache->master);
	if (cache->data) fclose(cache->data);
	memset(cache, 0, sizeof(*cache));
}

static int read_from(Js5Cache* cache, FILE* idx, int archive, uint32_t group, Js5Buffer* out) {
	uint8_t entry[6];
	if (!idx || fseek(idx, (long)group * 6, SEEK_SET) != 0 || fread(entry, 1, 6, idx) != 6) return -1;
	uint32_t size = entry[0] << 16 | entry[1] << 8 | entry[2];
	uint32_t sector = entry[3] << 16 | entry[4] << 8 | entry[5];
	if (size == 0 || sector == 0 || (long)sector > cache->sectors) return -2;
	uint8_t* data = malloc(size);
	if (!data) return -3;
	uint8_t block[SECTOR_SIZE];
	uint32_t done = 0, chunk = 0;
	while (done < size) {
		uint32_t want = size - done > SECTOR_DATA ? SECTOR_DATA : size - done;
		long position = (long)sector * SECTOR_SIZE;
		// Sectors are usually consecutive, and skipping the seek keeps the read buffer useful.
		if (sector == 0 || (position != cache->dataPosition && fseek(cache->data, position, SEEK_SET) != 0) ||
			fread(block, 1, want + SECTOR_HEADER, cache->data) != want + SECTOR_HEADER) {
			cache->dataPosition = -1;
			free(data);
			return -4;
		}
		cache->dataPosition = position + want + SECTOR_HEADER;
		uint32_t headerGroup = block[0] << 8 | block[1], headerChunk = block[2] << 8 | block[3];
		uint32_t next = block[4] << 16 | block[5] << 8 | block[6];
		if (headerGroup != (group & 0xFFFF) || headerChunk != (chunk & 0xFFFF) || block[7] != archive || (long)next > cache->sectors) {
			free(data);
			return -5;
		}
		memcpy(data + done, block + SECTOR_HEADER, want);
		done += want;
		chunk++;
		sector = next;
	}
	out->data = data;
	out->size = size;
	return 0;
}

int js5_read_raw(Js5Cache* cache, int archive, uint32_t group, Js5Buffer* out, uint32_t* versionOut) {
	if (archive < 0 || archive >= JS5_MAX_ARCHIVES) return -1;
	int rc = read_from(cache, index_file(cache, archive), archive, group, out);
	if (rc != 0) return rc;
	if (out->size < 2) {
		free(out->data);
		return -6;
	}
	if (versionOut) *versionOut = out->data[out->size - 2] << 8 | out->data[out->size - 1];
	out->size -= 2;
	return 0;
}

static uint32_t be32(const uint8_t* p) {
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void tea_decrypt(uint8_t* data, size_t size, const int32_t* key) {
	size_t blocks = (size - 5) / 8;
	uint8_t* p = data + 5;
	for (size_t i = 0; i < blocks; i++, p += 8) {
		uint32_t sum = 0xC6EF3720u, v0 = be32(p), v1 = be32(p + 4);
		for (int r = 0; r < 32; r++) {
			v1 -= ((uint32_t)key[sum >> 11 & 3] + sum) ^ (v0 + (v0 >> 5 ^ v0 << 4));
			sum -= 0x9E3779B9u;
			v0 -= ((v1 >> 5 ^ v1 << 4) + v1) ^ ((uint32_t)key[sum & 3] + sum);
		}
		p[0] = v0 >> 24; p[1] = v0 >> 16; p[2] = v0 >> 8; p[3] = v0;
		p[4] = v1 >> 24; p[5] = v1 >> 16; p[6] = v1 >> 8; p[7] = v1;
	}
}

int js5_decompress(const Js5Buffer* raw, const int32_t* key, Js5Buffer* out) {
	if (raw->size < 5) return -1;
	uint8_t* in = raw->data;
	uint8_t* copy = NULL;
	if (key && (key[0] || key[1] || key[2] || key[3])) {
		copy = malloc(raw->size);
		if (!copy) return -2;
		memcpy(copy, raw->data, raw->size);
		tea_decrypt(copy, raw->size, key);
		in = copy;
	}
	int type = in[0], rc = 0;
	uint32_t len = be32(in + 1);
	out->data = NULL;
	out->size = 0;
	if (type == 0) {
		if ((size_t)len + 5 > raw->size) rc = -3;
		else if (!(out->data = malloc(len ? len : 1))) rc = -2;
		else {
			memcpy(out->data, in + 5, len);
			out->size = len;
		}
	} else {
		if (raw->size < 9 || (size_t)len + 9 > raw->size) rc = -3;
		else {
			uint32_t outLen = be32(in + 5);
			if (!(out->data = malloc(outLen ? outLen : 1))) rc = -2;
			else {
				if (type == 1) rc = js5_bunzip2(in + 9, len, out->data, outLen);
				else if (type == 2) {
					if (len < 18 || in[9] != 0x1F || in[10] != 0x8B) rc = -4;
					else rc = js5_inflate(in + 19, len - 18, out->data, outLen);
				} else rc = -5;
				if (rc == 0) out->size = outLen;
				else {
					free(out->data);
					out->data = NULL;
				}
			}
		}
	}
	free(copy);
	return rc;
}

int js5_read_group(Js5Cache* cache, int archive, uint32_t group, const int32_t* key, Js5Buffer* out) {
	Js5Buffer raw;
	int rc = js5_read_raw(cache, archive, group, &raw, NULL);
	if (rc != 0) return rc;
	rc = js5_decompress(&raw, key, out);
	free(raw.data);
	return rc;
}

typedef struct {
	const uint8_t* p;
	const uint8_t* end;
	int bad;
} Reader;

static uint32_t g1(Reader* r) {
	if (r->p + 1 > r->end) { r->bad = 1; return 0; }
	return *r->p++;
}
static uint32_t g2(Reader* r) {
	if (r->p + 2 > r->end) { r->bad = 1; return 0; }
	uint32_t v = r->p[0] << 8 | r->p[1];
	r->p += 2;
	return v;
}
static uint32_t g4(Reader* r) {
	if (r->p + 4 > r->end) { r->bad = 1; return 0; }
	uint32_t v = be32(r->p);
	r->p += 4;
	return v;
}

int js5_load_index(Js5Cache* cache, int archive) {
	Js5Index* index = &cache->index[archive];
	if (index->groups) return 0;
	Js5Buffer raw, data;
	int rc = read_from(cache, cache->master, 255, (uint32_t)archive, &raw);
	if (rc != 0) return rc;
	rc = js5_decompress(&raw, NULL, &data);
	free(raw.data);
	if (rc != 0) return rc;

	Reader r = {data.data, data.data + data.size, 0};
	uint32_t format = g1(&r);
	if (format != 5 && format != 6) {
		free(data.data);
		return -10;
	}
	if (format >= 6) g4(&r);
	uint32_t flags = g1(&r);
	uint32_t count = g2(&r);
	uint32_t* ids = malloc(sizeof(uint32_t) * (count ? count : 1));
	if (!ids) {
		free(data.data);
		return -11;
	}
	uint32_t delta = 0, maxId = 0;
	for (uint32_t i = 0; i < count; i++) {
		delta += g2(&r);
		ids[i] = delta;
		if (delta > maxId) maxId = delta;
	}
	index->capacity = count ? maxId + 1 : 0;
	index->count = count;
	index->named = flags != 0;
	index->groups = calloc(index->capacity ? index->capacity : 1, sizeof(Js5Group));
	if (!index->groups) {
		free(ids);
		free(data.data);
		return -11;
	}
	for (uint32_t i = 0; i < count; i++) index->groups[ids[i]].present = 1;
	if (flags) for (uint32_t i = 0; i < count; i++) index->groups[ids[i]].nameHash = g4(&r);
	for (uint32_t i = 0; i < count; i++) index->groups[ids[i]].crc = g4(&r);
	for (uint32_t i = 0; i < count; i++) index->groups[ids[i]].version = g4(&r);
	for (uint32_t i = 0; i < count; i++) index->groups[ids[i]].size = g2(&r);
	for (uint32_t i = 0; i < count; i++) {
		Js5Group* g = &index->groups[ids[i]];
		uint32_t fileDelta = 0, maxFile = 0;
		g->fileIds = malloc(sizeof(uint32_t) * (g->size ? g->size : 1));
		for (uint32_t f = 0; f < g->size && g->fileIds; f++) {
			fileDelta += g2(&r);
			g->fileIds[f] = fileDelta;
			if (fileDelta > maxFile) maxFile = fileDelta;
		}
		if (g->size && maxFile + 1 == g->size) {
			free(g->fileIds);
			g->fileIds = NULL;
		}
	}
	if (flags) {
		for (uint32_t i = 0; i < count; i++) {
			Js5Group* g = &index->groups[ids[i]];
			g->fileHashes = malloc(sizeof(uint32_t) * (g->size ? g->size : 1));
			for (uint32_t f = 0; f < g->size; f++) {
				uint32_t h = g4(&r);
				if (g->fileHashes) g->fileHashes[f] = h;
			}
		}
	}
	free(ids);
	free(data.data);
	return r.bad ? -12 : 0;
}

int js5_group_file(const Js5Index* index, uint32_t group, const Js5Buffer* unpacked, uint32_t fileId, Js5Buffer* out) {
	if (group >= index->capacity || !index->groups[group].present) return -1;
	const Js5Group* g = &index->groups[group];
	uint32_t slot = fileId;
	if (g->fileIds) {
		for (slot = 0; slot < g->size && g->fileIds[slot] != fileId; slot++) {}
	}
	if (slot >= g->size) return -2;
	if (g->size == 1) {
		out->data = malloc(unpacked->size ? unpacked->size : 1);
		if (!out->data) return -3;
		memcpy(out->data, unpacked->data, unpacked->size);
		out->size = unpacked->size;
		return 0;
	}
	if (unpacked->size < 1) return -4;
	size_t pos = unpacked->size - 1;
	uint32_t stripes = unpacked->data[pos];
	size_t tableSize = (size_t)g->size * stripes * 4;
	if (tableSize > pos) return -5;
	const uint8_t* table = unpacked->data + pos - tableSize;

	size_t total = 0, source = 0, sourceStart = 0;
	for (uint32_t s = 0; s < stripes; s++) {
		int32_t len = 0;
		for (uint32_t f = 0; f < g->size; f++) {
			len += (int32_t)be32(table + ((size_t)s * g->size + f) * 4);
			if (f == slot) total += (size_t)len;
		}
	}
	out->data = malloc(total ? total : 1);
	if (!out->data) return -3;
	out->size = total;
	size_t written = 0;
	for (uint32_t s = 0; s < stripes; s++) {
		int32_t len = 0;
		for (uint32_t f = 0; f < g->size; f++) {
			len += (int32_t)be32(table + ((size_t)s * g->size + f) * 4);
			if (f == slot) {
				memcpy(out->data + written, unpacked->data + source, (size_t)len);
				written += (size_t)len;
			}
			source += (size_t)len;
		}
	}
	(void)sourceStart;
	return 0;
}

int js5_find_group(const Js5Index* index, const char* name) {
	uint32_t hash = 0;
	for (const char* p = name; *p; p++) hash = (uint32_t)(uint8_t)tolower((unsigned char)*p) + (hash << 5) - hash;
	for (uint32_t i = 0; i < index->capacity; i++) {
		if (index->groups[i].present && index->groups[i].nameHash == hash) return (int)i;
	}
	return -1;
}
