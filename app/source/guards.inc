// What the block check in integrity.inc cannot see. A pool that overran into its neighbour shows up in the guard after it. The draw list is checked because the world
// could be drawn wrongly with every vertex intact, and a full pass is made on exit because the block check above
// takes about half a minute to get round, so it can miss damage from just before the app was closed.

#define GUARD_WORD 0x5A17C0DEu
#define VRAM_WATCH_SIZE 128 // a small render target nothing ever draws to, so any change in it is not ours

static u32 meshListHash;
static C3D_RenderTarget* vramWatch = NULL;
static u32 vramWatchValue;
static bool vramWatchReady = false, mcuReady = false;
static long vramDamage = 0;

// Fills a patch of VRAM once, so that later checks can see whether anything but the GPU's own work changes it.
static void vramWatchStart(void) {
	vramWatch = C3D_RenderTargetCreate(VRAM_WATCH_SIZE, VRAM_WATCH_SIZE, GPU_RB_RGBA8, -1);
	if (!vramWatch) return;
	C3D_FrameBegin(0);
	C3D_RenderTargetClear(vramWatch, C3D_CLEAR_COLOR, GUARD_WORD, 0);
	C3D_FrameEnd(0);
	C3D_FrameBegin(0);
	C3D_FrameEnd(0);
	vramWatchValue = *(volatile const u32*)vramWatch->frameBuf.colorBuf;
	vramWatchReady = true;
	mcuReady = R_SUCCEEDED(mcuHwcInit());
}

static int vramWatchBroken(void) {
	if (!vramWatchReady) return 0;
	const volatile u32* p = (const volatile u32*)vramWatch->frameBuf.colorBuf;
	int broken = 0;
	for (int i = 0; i < VRAM_WATCH_SIZE * VRAM_WATCH_SIZE; i++) if (p[i] != vramWatchValue) broken++;
	return broken;
}

// The battery's temperature in degrees, or -1 if the system would not say. It follows the console's warmth.
static int consoleTemperature(void) {
	u8 value = 0;
	if (!mcuReady || R_FAILED(MCUHWC_ReadRegister(0x0A, &value, 1))) return -1;
	return value;
}

static int minutesSinceBoot(void) {
	return (int)(svcGetSystemTick() / SYSCLOCK_ARM11 / 60);
}
static long guardDamage = 0, listDamage = 0;

static void guardFill(u8* pool, size_t size) {
	if (!pool) return;
	u32* guard = (u32*)(pool + size);
	for (u32 i = 0; i < GUARD_SIZE / 4; i++) guard[i] = GUARD_WORD;
}

static void guardsSet(void) {
	guardFill(meshPool, MESH_POOL_SIZE);
	guardFill(dynamicPool, DYNAMIC_POOL_SIZE);
	guardFill(npcPool, NPC_POOL_SIZE);
}

static int guardBroken(const u8* pool, size_t size) {
	if (!pool) return 0;
	const u32* guard = (const u32*)(pool + size);
	int broken = 0;
	for (u32 i = 0; i < GUARD_SIZE / 4; i++) if (guard[i] != GUARD_WORD) broken++;
	return broken;
}

static u32 meshListFingerprint(void) {
	const u32* p = (const u32*)mesh.chunks;
	size_t words = sizeof(mesh.chunks[0]) * mesh.chunkCount / 4;
	u32 h = 2166136261u;
	for (size_t i = 0; i < words; i++) h = (h ^ p[i]) * 16777619u;
	return h;
}

static void meshListRemember(void) {
	meshListHash = meshListFingerprint();
}

// Cheap enough to run with the health line.
static void integrityQuickCheck(void) {
	int mesh1 = guardBroken(meshPool, MESH_POOL_SIZE), dynamic = guardBroken(dynamicPool, DYNAMIC_POOL_SIZE), npc = guardBroken(npcPool, NPC_POOL_SIZE);
	bool list = meshBlockCount > 0 && meshListFingerprint() != meshListHash;
	int vram = vramWatchBroken();
	if (vram) {
		if (vramDamage == 0) {
			char first[96];
			snprintf(first, sizeof(first), "DAMAGE vram words=%d up_min=%d temp=%d", vram, minutesSinceBoot(), consoleTemperature());
			logLine(first);
		}
		vramDamage += vram;
	}
	if (!mesh1 && !dynamic && !npc && !list) return;
	guardDamage += mesh1 + dynamic + npc;
	if (list) listDamage++;
	if (guardDamage + listDamage > 200) return;
	char line[96];
	snprintf(line, sizeof(line), "DAMAGE guards mesh=%d dynamic=%d npc=%d draw_list=%d", mesh1, dynamic, npc, list);
	logLine(line);
	if (list) meshListRemember();
}

// Goes over everything at once, for the end of a session.
static void integrityFullCheck(const char* when) {
	integrityQuickCheck();
	long bad = 0;
	for (u32 i = 0; i < meshBlockCount; i++) {
		if (!meshBlockDirty[i] && meshBlockFingerprint(i) != meshBlockHash[i]) bad++;
	}
	char line[160];
	snprintf(line, sizeof(line), "integrity %s blocks=%lu bad=%ld guards=%ld draw_list=%ld corrupt_seen=%ld vram=%ld vram_watch=%d up_min=%d temp=%d", when, (unsigned long)meshBlockCount, bad, guardDamage, listDamage, meshCorrupt, vramDamage, vramWatchReady, minutesSinceBoot(), consoleTemperature());
	logLine(line);
}

static void meshListRememberHook(void) {
	meshListRemember();
}
