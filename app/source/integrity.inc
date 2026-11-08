// Watches the world's geometry for anything overwriting it. The GPU only reads this memory and the game only
// changes it through hideObject, so a block whose fingerprint changes means some other write landed there.
// That separates "the data was damaged" from "the data is fine but the GPU drew it wrongly".

#define MESH_BLOCK_SIZE (256u * 1024)
#define MESH_BLOCK_MAX ((MESH_POOL_SIZE / MESH_BLOCK_SIZE) + 1)
#define MESH_CHECK_FRAMES 6

static u32 meshBlockHash[MESH_BLOCK_MAX];
static u8 meshBlockDirty[MESH_BLOCK_MAX];
static u32 meshBlockCount = 0, meshBlockNext = 0;
static long meshCorrupt = 0, meshChecks = 0;
static int meshCheckWait = 0;

static u32 meshBlockFingerprint(u32 block) {
	size_t start = (size_t)block * MESH_BLOCK_SIZE, size = mesh.poolUsed - start;
	if (size > MESH_BLOCK_SIZE) size = MESH_BLOCK_SIZE;
	const u32* p = (const u32*)(meshPool + start);
	u32 h = 2166136261u;
	for (size_t i = 0; i < size / 4; i++) h = (h ^ p[i]) * 16777619u;
	return h;
}

static void meshListRememberHook(void);

static void meshHashAll(void) {
	meshBlockCount = (u32)((mesh.poolUsed + MESH_BLOCK_SIZE - 1) / MESH_BLOCK_SIZE);
	for (u32 i = 0; i < meshBlockCount; i++) {
		meshBlockHash[i] = meshBlockFingerprint(i);
		meshBlockDirty[i] = 0;
	}
	meshBlockNext = 0;
	meshListRememberHook();
	gpuCheckReset();
}

// The game changed this memory on purpose, so its fingerprint is taken again at the next check.
static void meshMarkChanged(const void* address, size_t size) {
	gpuCheckReset();
	size_t start = (size_t)((const u8*)address - meshPool);
	for (size_t b = start / MESH_BLOCK_SIZE; b <= (start + size) / MESH_BLOCK_SIZE && b < MESH_BLOCK_MAX; b++) meshBlockDirty[b] = 1;
}

static void meshCheckStep(void) {
	if (meshBlockCount == 0 || ++meshCheckWait < MESH_CHECK_FRAMES) return;
	meshCheckWait = 0;
	u32 block = meshBlockNext;
	meshBlockNext = (meshBlockNext + 1) % meshBlockCount;
	u32 now = meshBlockFingerprint(block);
	meshChecks++;
	if (!meshBlockDirty[block] && now != meshBlockHash[block]) {
		meshCorrupt++;
		if (meshCorrupt <= 20) {
			char line[96];
			snprintf(line, sizeof(line), "MESH CORRUPT block=%lu of %lu checks=%ld", (unsigned long)block, (unsigned long)meshBlockCount, meshChecks);
			logLine(line);
		}
	}
	meshBlockDirty[block] = 0;
	meshBlockHash[block] = now;
}
