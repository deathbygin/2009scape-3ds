// 2009scape 3DS client: builds scenes from the game cache on the SD card and connects to a server.
// The game shows on one screen and the interface panel on the other. Select swaps them.
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <limits.h>
#include "vshader_shbin.h"
#include "world.h"
#include "netclient.h"
#include "minimap.h"
#include "pick.h"
#include "anim.h"
#include "iface.h"
#include "cs2.h"
#include "huffman.h"
#include "font.h"
#include "icon.h"
#include "sprite.h"
#include <malloc.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// The scene builder needs CPU heap. libctru gives the rest to the GPU-accessible linear heap.
u32 __ctru_heap_size = 56 * 1024 * 1024;
#define MESH_POOL_SIZE (48u * 1024 * 1024)
#define GUARD_SIZE 4096u // a fenced strip after each GPU pool, to catch anything running past the end

// Commands for one frame go in this buffer. When it fills, libctru silently drops what follows, including the
// end of the list, and the GPU runs on into whatever comes next and hangs the whole system. So drawing stops
// well before that: a few missing objects for one frame is far better than a hung console.
#define CMDBUF_SIZE 0x80000
#define CMDBUF_LIMIT 0.85f

#define MAX_CHUNKS MESH_MAX_CHUNKS
#define VERT_SIZE MESH_VERTEX_SIZE
#define CLEAR_COLOR 0x87A0C0FF
#define ALIGN16(x) (((x) + 15) & ~15u)
#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
	u32 start[2], count[2]; // index ranges: 0 = with ground decor, 1 = without
	float nomX, nomZ;       // nominal centre, used for draw distance
	float cx, cy, cz, r;    // bounding sphere, used for the view test
} Cell;

typedef struct {
	u8* verts;
	u16* indices[2]; // 0 = with ground decor, 1 = without
	Cell cells[16];  // 2x2-tile cells in Morton order
	Cell groups[4];  // 4x4-tile groups: each covers four consecutive cells
} Chunk;

static const char* const SCENE_NAMES[] = {"lumbridge", "varrock", "grand_exchange"};
#define SCENE_COUNT 3
static const int SCENE_TILES[SCENE_COUNT][2] = {{3222, 3218}, {3213, 3428}, {3165, 3487}};
static const int RADII[] = {15, 20, 25, 28};
#define RADIUS_COUNT 4
static const char* const MODE_NAMES[] = {"full", "no_decor"};
#define MODE_COUNT 2

static Chunk chunks[MAX_CHUNKS];
static u32 chunkCount = 0;
static World world;
static Scene scene;
static Mesh mesh;
static u8* meshPool = NULL;
static char loadReport[192] = "";
static int32_t sceneHeights[4][105][105];
#define MAP_TILES_PX 416
static u16 mapImage[MAP_TILES_PX * MAP_TILES_PX]; // the scene at 4 pixels per tile, north up
static float targetX, targetZ, targetH; // view space, in tiles
static float yaw = 0.0f, pitch = 0.6f, zoom = 13.0f;
static int sceneIndex = 0, radiusIndex = 1, modeIndex = 0;
static int uLoc_mvp;
static C3D_Mtx projectionTop, projectionBottom;
static C3D_Mtx modelScale;
static u32 trisSubmitted, drawCalls;
static float cmdPeak = 0, cmdPeakRecent = 0;
static long panelsShown = 0;
static double frameMsPeak = 0, gpuMsPeak = 0;
static u32 drawCallsPeak = 0;
static long cmdFramesCut = 0;
static char statusLine[96] = "";
static bool noViewTest = false;
static float viewTestScale = 1.0f; // below 1 only for the self-test negative control
static bool gameOnBottom = false, panelDirty = true, online = false, runMode = false;
static float cursorX = 200.0f, cursorY = 120.0f;
static double hudFrameMs = 0;
static long actionsSent = 0; // game actions sent to the server, one per deliberate press
static float playerViewX, playerViewZ, playerViewH;
#define MAX_PICK_OBJECTS 16384
static PickObject pickList[MAX_PICK_OBJECTS];
static int pickCount = 0;
typedef struct {
	u8* vertices;
	u32 vertexCount;
	int level, x, y, layer;
} DynamicObject;
#define DYNAMIC_MAX 256
static DynamicObject dynamicObjects[DYNAMIC_MAX];
static int dynamicCount = 0;
static long changesApplied = 0;
static int walkFlagX = -1, walkFlagY = -1; // local tile of the walk destination
static NetClient net;
static C3D_RenderTarget* targetTop;
static C3D_RenderTarget* targetBottom;

static void boundsToCell(Cell* c, int minX, int maxX, int minZ, int maxZ, int minY, int maxY) {
	c->cx = (minX + maxX) / 256.0f;
	c->cz = -(minZ + maxZ) / 256.0f;
	c->cy = -(minY + maxY) / 256.0f;
	float dx = (maxX - minX) / 256.0f, dy = (maxY - minY) / 256.0f, dz = (maxZ - minZ) / 256.0f;
	c->r = sqrtf(dx * dx + dy * dy + dz * dz);
}

static void logLine(const char* text);
static void meshHashAll(void);
static void gpuCheckReset(void);
static void meshMarkChanged(const void* address, size_t size);

static long nowMs(void) {
	return (long)osGetTime();
}

// Builds a whole scene from the cache on the SD card.
static bool loadZone(int zoneX, int zoneY, const int32_t (*keys)[4], int keyCount, int localX, int localY, int plane, const char* name) {
	long t0 = nowMs();
	chunkCount = 0;
	WorldTiming timing;
	memset(&timing, 0, sizeof(timing));
	int rc = world_build_zone(&world, &scene, zoneX, zoneY, keys, keyCount, nowMs, &timing);
	if (rc != 0) {
		snprintf(statusLine, sizeof(statusLine), "scene build failed: %d", rc);
		scene_free(&scene);
		return false;
	}
	memcpy(sceneHeights, scene.terrain.heights, sizeof(sceneHeights));
	int height = sceneHeights[plane & 3][localX][localY];
	pickCount = pick_build(&scene, pickList, MAX_PICK_OBJECTS);
	uint32_t* mapPixels = malloc(MINIMAP_SIZE * MINIMAP_SIZE * 4);
	if (mapPixels) {
		minimap_render(&scene, plane & 3, mapPixels);
		for (int y = 0; y < MAP_TILES_PX; y++)
			for (int x = 0; x < MAP_TILES_PX; x++) {
				u32 c = mapPixels[(y + 48) * MINIMAP_SIZE + x + 48];
				mapImage[y * MAP_TILES_PX + x] = (u16)((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F));
			}
		free(mapPixels);
	}
	long t1 = nowMs();
	rc = mesh_build(&mesh, &scene, world.palette, meshPool, MESH_POOL_SIZE);
	long t2 = nowMs();
	scene_free(&scene);
	long t3 = nowMs();
	if (rc != 0) {
		snprintf(statusLine, sizeof(statusLine), "mesh failed: %d, used %luMB", rc, (unsigned long)(mesh.poolUsed >> 20));
		return false;
	}
	GSPGPU_FlushDataCache(meshPool, mesh.poolUsed);
	meshHashAll();

	for (u32 i = 0; i < mesh.chunkCount; i++) {
		const MeshChunk* m = &mesh.chunks[i];
		Chunk* c = &chunks[i];
		memset(c, 0, sizeof(*c));
		c->verts = m->vertices;
		c->indices[0] = m->indices[0];
		c->indices[1] = m->indices[1];
		float baseX = (m->sector % MESH_SECTOR_GRID) * 8.0f, baseZ = (m->sector / MESH_SECTOR_GRID) * 8.0f;
		for (int n = 0; n < 16; n++) {
			const MeshCell* w = &m->cells[n];
			Cell* cell = &c->cells[n];
			int cx = (n & 1) | ((n >> 2 & 1) << 1), cz = (n >> 1 & 1) | ((n >> 3 & 1) << 1);
			cell->nomX = baseX + cx * 2 + 1.0f;
			cell->nomZ = -(baseZ + cz * 2 + 1.0f);
			if (!w->used) continue;
			cell->start[0] = w->start[0];
			cell->start[1] = w->start[1];
			cell->count[0] = w->count[0];
			cell->count[1] = w->count[1];
			boundsToCell(cell, w->minX, w->maxX, w->minZ, w->maxZ, w->minY, w->maxY);
		}
	}
	chunkCount = mesh.chunkCount;
	targetX = localX + 0.5f;
	targetZ = -(localY + 0.5f);
	targetH = -height / 128.0f;
	snprintf(loadReport, sizeof(loadReport), "%s total_ms=%ld terrain_ms=%ld locs_ms=%ld tiles_ms=%ld merge_ms=%ld mesh_ms=%ld free_ms=%ld objects=%ld triangles=%ld mesh_mb=%.1f",
		name, t3 - t0, timing.readMs, timing.locsMs, timing.tilesMs, timing.mergeMs, t2 - t1, t3 - t2, timing.objects, mesh.triangles, mesh.poolUsed / 1048576.0);
	snprintf(statusLine, sizeof(statusLine), "Loaded in %ld ms", t3 - t0);
	return true;
}

static bool loadScene(int index) {
	int x = SCENE_TILES[index][0], y = SCENE_TILES[index][1], zoneX = x >> 3, zoneY = y >> 3;
	if (!loadZone(zoneX, zoneY, NULL, 0, x - (zoneX - 6) * 8, y - (zoneY - 6) * 8, 0, SCENE_NAMES[index])) return false;
	sceneIndex = index;
	return true;
}

static void sceneInit(void) {
	DVLB_s* dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	static shaderProgram_s program;
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
	C3D_BindProgram(&program);
	uLoc_mvp = shaderInstanceGetUniformLocation(program.vertexShader, "mvp");

	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);
	AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	C3D_CullFace(GPU_CULL_BACK_CCW);
	Mtx_PerspTilt(&projectionTop, C3D_AngleFromDegrees(50.0f), C3D_AspectRatioTop, 0.5f, 90.0f, false);
	Mtx_PerspTilt(&projectionBottom, C3D_AngleFromDegrees(50.0f), C3D_AspectRatioBot, 0.5f, 90.0f, false);

	// Game space is x east, y down, z north, 128 units per tile.
	Mtx_Identity(&modelScale);
	modelScale.r[0].x = 1.0f / 128.0f;
	modelScale.r[1].y = -1.0f / 128.0f;
	modelScale.r[2].z = -1.0f / 128.0f;
}

static void drawPlayerMarker(void);
static void drawGameCursor(void);
static void drawNpcs(const C3D_Mtx* mvp);
static bool dialogActive(void);
static void drawDialog(u8* fb, int width);
static void drawBag(u8* fb, int width);
static void drawWorn(u8* fb, int width);
static void drawStats(u8* fb, int width);
static void drawQuests(u8* fb, int width);
// A spell waiting for its target, chosen on the Magic tab. -1 when there is none.
static int castComponent = -1, castInterface, castMask;
static char castName[28];
static void cancelCast(void);
static void drawSummon(u8* fb, int width);
static int summonButtonAt(int px, int py, int width);
static void pressSummon(int index);
static bool renderNpcHead(int npc, u32* pixels);
static bool runCast(const void* entry);
static void drawCombat(u8* fb, int width);
static void drawSocial(u8* fb, int width);
static void drawChat(u8* fb, int width);
static void pressSocialMessage(void);
static void pressChatSay(void);
static void pressChatPrivate(void);
static void drawTextOverlay(void);
static bool headShown(void);
static bool headOnRight(void);
static void drawHead(u8* fb, int width);
static void updateHead(void);
static void resetHead(void);
static void applyPoolResets(void);
static void waitForGpu(void);
static void guardsSet(void);
static void vramWatchStart(void);
static int consoleTemperature(void);
static int minutesSinceBoot(void);
static void integrityQuickCheck(void);
static void integrityFullCheck(const char* when);
static bool pageActive(void);
static bool pageIsConfirm(void);
static void pressChatRow(int y);
static void drawPage(u8* fb, int width);
static bool pressPage(int x, int y, int width);
static void pressPageCell(int x, int y, int width);
static bool openPageMenu(int x, int y, int width);
static bool runPage(const void* entry);
static int* pageList(int* top, int* bottom, int* total);
static void updateStore(void);
static void updatePrompt(void);
static void resetStore(void);
static int guideSkill = -1;
static bool guideActive(void);
static void drawGuide(u8* fb, int width);
static bool pressGuide(int x, int y, int width);
static int* guideList(int* top, int* bottom, int* total);
static void updateGuide(void);
static void resetGuide(void);
static const Sprite* cachedSprite(int id);
static void pressLogout(void);
static bool logoutPressed = false;
static char testFocus[16] = ""; // the automatic test runs only this part when set
static void pressSocialAdd(void);
static void pressSocialDelete(void);
static void drawGrid(u8* fb, int width);
static void drawMusic(u8* fb, int width);
static int combatButtonAt(int px, int py, int width);
static void pressCombat(int index);
static void drawJournal(u8* fb, int width);
static bool journalActive(void);
static bool pressList(int x, int y, int width);
static bool listTouchBegin(int x, int y, int width);
static bool listTouching(void);
static void listTouchCancel(void);
static void handleListTouch(u32 held, touchPosition touch);
static int statsPlaceAt(int px, int py, int width);
static void pressStats(int place);
static int wornSlotAt(int px, int py, int width);
static int wornItemAt(int slot);
static void pressWorn(int slot);
static void buildWornMenu(int slot);
#define BAG_INTERFACE (149 << 16)
static int usingSlot = -1, usingItem = -1; // the item chosen with Use, waiting for what to use it on
static int bagSlotAt(int px, int py, int width);
static int bagPressSlot = -1, bagPressItem = -1; // the slot the stylus went down on
static int bagTouchX, bagTouchY, bagStartX, bagStartY;
static bool bagDragging = false;
static int bagFarSamples = 0;
static int bagHistoryX[3], bagHistoryY[3]; // recent stylus positions, newest last
static void logLine(const char* text);
static int bagItemAt(int slot);
static void pressBag(int slot);
static void buildBagMenu(int slot);
static void cancelUse(void);
static const char* itemName(int id);
static bool ensureIcon(int id, int amount);
static void drawItemIconScaled(u8* fb, int width, int id, int amount, int x, int y, int scale);
static bool renderModelPicture(int model, int yaw, u32* pixels);
static void addGroundEntries(int sx, int sy, int width, bool examine, int limit);
static const NpcDef* npcShown(int type);
static int npcModelTop(u64 key);
static int selfCombat(void);
static bool selfHidden = false; // for comparisons in the self-test

// The current camera, for testing whether a sphere can be seen.
static struct {
	float eyeX, eyeY, eyeZ, fX, fY, fZ, rX, rZ, uX, uY, uZ, tanH, tanV, kH, kV;
} viewTest;

#include "gpucheck.inc"

static void drawScene(void) {
	float cosP = cosf(pitch), sinP = sinf(pitch);
	float eyeX = targetX + sinf(yaw) * cosP * zoom;
	float eyeZ = targetZ + cosf(yaw) * cosP * zoom;
	float eyeY = targetH + sinP * zoom;

	C3D_Mtx view, viewModel, mvp;
	Mtx_LookAt(&view, FVec3_New(eyeX, eyeY, eyeZ), FVec3_New(targetX, targetH, targetZ), FVec3_New(0, 1, 0), false);
	Mtx_Multiply(&viewModel, &view, &modelScale);
	Mtx_Multiply(&mvp, gameOnBottom ? &projectionBottom : &projectionTop, &viewModel);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_mvp, &mvp);

	// Camera basis for the view test. The camera is tilted, so this must be a true 3D test:
	// a flat test misses the wider view near the bottom corners of the screen.
	float fX = -sinf(yaw) * cosP, fY = -sinP, fZ = -cosf(yaw) * cosP;
	float rLen = sqrtf(fX * fX + fZ * fZ);
	float rX = -fZ / rLen, rZ = fX / rLen;
	float uX = -rZ * fY, uY = rZ * fX - rX * fZ, uZ = rX * fY;
	const float tanV = tanf(C3D_AngleFromDegrees(25.0f));
	const float tanH = tanV * (gameOnBottom ? C3D_AspectRatioBot : C3D_AspectRatioTop) * viewTestScale;
	const float kV = sqrtf(1.0f + tanV * tanV), kH = sqrtf(1.0f + tanH * tanH);
	viewTest.eyeX = eyeX;
	viewTest.eyeY = eyeY;
	viewTest.eyeZ = eyeZ;
	viewTest.fX = fX;
	viewTest.fY = fY;
	viewTest.fZ = fZ;
	viewTest.rX = rX;
	viewTest.rZ = rZ;
	viewTest.uX = uX;
	viewTest.uY = uY;
	viewTest.uZ = uZ;
	viewTest.tanH = tanH;
	viewTest.tanV = tanV;
	viewTest.kH = kH;
	viewTest.kV = kV;
	float radius = (float)RADII[radiusIndex];
	int stream = modeIndex == 1 ? 1 : 0;
	const bool coarse = false;
	int units = coarse ? 4 : 16;
	float nominalRadius = coarse ? 3.25f : 1.625f;

	trisSubmitted = 0;
	drawCalls = 0;
	bool cut = false;
	for (u32 i = 0; i < chunkCount; i++) {
		if (C3D_GetCmdBufUsage() > CMDBUF_LIMIT) {
			cut = true;
			break;
		}
		Chunk* c = &chunks[i];
		const Cell* list = coarse ? c->groups : c->cells;
		bool bound = false;
		u32 runStart = 0, runCount = 0;
		for (int u = 0; u <= units; u++) {
			bool visible = false;
			if (u < units && list[u].count[stream]) {
				const Cell* cell = &list[u];
				float px = cell->nomX - targetX, pz = cell->nomZ - targetZ;
				if (sqrtf(px * px + pz * pz) <= radius + nominalRadius) {
					float dx = cell->cx - eyeX, dy = cell->cy - eyeY, dz = cell->cz - eyeZ;
					float depth = dx * fX + dy * fY + dz * fZ;
					float across = fabsf(dx * rX + dz * rZ);
					float upward = fabsf(dx * uX + dy * uY + dz * uZ);
					visible = noViewTest || (depth >= 0.5f - cell->r && across - depth * tanH <= cell->r * kH && upward - depth * tanV <= cell->r * kV);
				}
			}
			if (visible) {
				// Index ranges of neighbouring units touch, so a visible run is one draw.
				if (runCount == 0) runStart = list[u].start[stream];
				runCount += list[u].count[stream];
			} else if (u == units || list[u].count[stream] != 0 || runCount != 0) {
				if (runCount != 0) {
					if (!bound) {
						C3D_BufInfo* buf = C3D_GetBufInfo();
						BufInfo_Init(buf);
						BufInfo_Add(buf, c->verts, VERT_SIZE, 2, 0x10);
						bound = true;
					}
					gpuCheckOffer(c->verts, c->indices[stream] + runStart, runCount, &mvp);
					C3D_DrawElements(GPU_TRIANGLES, runCount, C3D_UNSIGNED_SHORT, c->indices[stream] + runStart);
					trisSubmitted += runCount / 3;
					drawCalls++;
					runCount = 0;
				}
			}
		}
	}
	for (int i = 0; i < dynamicCount; i++) {
		const DynamicObject* d = &dynamicObjects[i];
		if (d->vertexCount == 0) continue;
		if (C3D_GetCmdBufUsage() > CMDBUF_LIMIT) {
			cut = true;
			break;
		}
		C3D_BufInfo* buf = C3D_GetBufInfo();
		BufInfo_Init(buf);
		BufInfo_Add(buf, d->vertices, VERT_SIZE, 2, 0x10);
		C3D_DrawArrays(GPU_TRIANGLES, 0, d->vertexCount);
	}
	drawPlayerMarker();
	drawNpcs(&mvp);
	drawTextOverlay();
	drawGameCursor();
	float used = C3D_GetCmdBufUsage();
	if (used > cmdPeak) cmdPeak = used;
	if (used > cmdPeakRecent) cmdPeakRecent = used;
	if (drawCalls > drawCallsPeak) drawCallsPeak = drawCalls;
	if (cut || used > CMDBUF_LIMIT) cmdFramesCut++;
}

typedef struct {
	bool running;
	FILE* out;
	int scene, radius, mode, frame;
	u64 phaseStart;
	double frameMs, frameMax, gpuMs, cpuMs, tris;
	int samples;
} Bench;
static Bench bench;
#define BENCH_WARMUP 30
#define BENCH_MS 5000.0

static void benchStart(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/rs3ds", 0777);
	bench.out = fopen("sdmc:/3ds/rs3ds/bench5.csv", "w");
	if (!bench.out) {
		snprintf(statusLine, sizeof(statusLine), "cannot write bench5.csv");
		return;
	}
	bool isNew = false;
	APT_CheckNew3DS(&isNew);
	fprintf(bench.out, "# new3ds=%d linear_free_mb=%lu\n", isNew, (unsigned long)(linearSpaceFree() >> 20));
	fprintf(bench.out, "scene,radius,mode,frames,avg_frame_ms,worst_frame_ms,avg_gpu_ms,avg_cpu_ms,avg_tris\n");
	fflush(bench.out);
	bench.running = true;
	bench.scene = bench.radius = bench.mode = bench.frame = 0;
	bench.samples = 0;
	loadScene(0);
	fprintf(bench.out, "# %s\n", loadReport);
	radiusIndex = 0;
	modeIndex = 0;
	pitch = 0.6f;
	zoom = 13.0f;
}

static void benchFrame(double frameMs) {
	bench.frame++;
	yaw += (float)(2.0 * M_PI * frameMs / BENCH_MS);
	if (bench.frame <= BENCH_WARMUP) {
		bench.phaseStart = osGetTime();
		bench.frameMs = bench.frameMax = bench.gpuMs = bench.cpuMs = bench.tris = 0;
		bench.samples = 0;
		return;
	}
	bench.frameMs += frameMs;
	if (frameMs > bench.frameMax) bench.frameMax = frameMs;
	bench.gpuMs += C3D_GetDrawingTime();
	bench.cpuMs += C3D_GetProcessingTime();
	bench.tris += trisSubmitted;
	bench.samples++;
	if (osGetTime() - bench.phaseStart < (u64)BENCH_MS) return;

	double n = bench.samples;
	fprintf(bench.out, "%s,%d,%s,%d,%.2f,%.2f,%.2f,%.2f,%.0f\n", SCENE_NAMES[bench.scene], RADII[bench.radius],
		MODE_NAMES[bench.mode], bench.samples, bench.frameMs / n, bench.frameMax, bench.gpuMs / n, bench.cpuMs / n, bench.tris / n);
	fflush(bench.out);

	bench.frame = 0;
	if (++bench.mode == MODE_COUNT) {
		bench.mode = 0;
		if (++bench.radius == RADIUS_COUNT) {
			bench.radius = 0;
			if (++bench.scene == SCENE_COUNT) {
				fclose(bench.out);
				bench.out = NULL;
				bench.running = false;
				snprintf(statusLine, sizeof(statusLine), "Benchmark finished. bench5.csv saved.");
				return;
			}
			if (!loadScene(bench.scene)) {
				fprintf(bench.out, "# load failed: %s\n", statusLine);
				fclose(bench.out);
				bench.out = NULL;
				bench.running = false;
				return;
			}
			fprintf(bench.out, "# %s\n", loadReport);
		}
	}
	radiusIndex = bench.radius;
	modeIndex = bench.mode;
}

#include "panel.inc"
#include "dialog.inc"

// Self-test, run only if sdmc:/3ds/rs3ds/autotest.txt exists. Renders each camera with the view test on and off.
// The images must match exactly: any difference means culling removed something visible.
#define FB_BYTES (240 * 400 * 3)
#define AUTOTEST_DIR "sdmc:/3ds/rs3ds/"

// A build for finding out whether the L2 cache plays a part in the glitch seen on real hardware. The clock comes
// from the title's settings in that build, because osSetSpeedupEnable can only turn both on together.
#if defined(RS3DS_SOAK)
#define DIAG_NOTE "diag_build soak"
#elif defined(RS3DS_NO_MODELS)
#define DIAG_NOTE "diag_build no_models"
#elif defined(RS3DS_SLOW_PANEL)
#define DIAG_NOTE "diag_build slow_panel"
#else
#define DIAG_NOTE "diag_build none"
#endif

#ifdef RS3DS_NO_L2
#define CPU_MODE_NOTE "cpu_mode l2=off speedup_call=0"
#else
#define CPU_MODE_NOTE "cpu_mode l2=on speedup_call=1"
#endif

static void renderGame(bool sceneReady) {
	C3D_RenderTarget* target = gameOnBottom ? targetBottom : targetTop;
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	applyPoolResets(); // beginning a frame waits for the GPU, so nothing is reading these now
	gpuCheckRead();
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
	C3D_FrameDrawOn(target);
	if (sceneReady) {
		drawScene();
		gpuCheckDraw();
	}
	C3D_FrameEnd(0);
}

// Returns once the GPU has finished, so memory it reads can safely change.
static void waitForGpu(void) {
	C3D_FrameBegin(0);
	C3D_FrameEnd(0);
}

static bool renderFrames(int count) {
	for (int i = 0; i < count; i++) {
		if (!aptMainLoop()) return false;
		renderGame(true);
	}
	return true;
}

static int gameBytes(void) {
	return (gameOnBottom ? 320 : 400) * 240 * 3;
}

static void captureTop(u8* out) {
	u8* fb = gfxGetFramebuffer(gameOnBottom ? GFX_BOTTOM : GFX_TOP, GFX_LEFT, NULL, NULL);
	GSPGPU_InvalidateDataCache(fb, (u32)gameBytes());
	memcpy(out, fb, (size_t)gameBytes());
}

static void writeBmp(const char* path, const u8* fb, int width) {
	static u8 row[1200];
	FILE* f = fopen(path, "wb");
	if (!f) return;
	u8 h[54] = {'B', 'M'};
	u32 imageSize = (u32)width * 240 * 3, size = 54 + imageSize, offset = 54, dib = 40, w = (u32)width, ht = 240, planesBits = 1 | (24 << 16);
	memcpy(h + 2, &size, 4);
	memcpy(h + 10, &offset, 4);
	memcpy(h + 14, &dib, 4);
	memcpy(h + 18, &w, 4);
	memcpy(h + 22, &ht, 4);
	memcpy(h + 26, &planesBits, 4);
	memcpy(h + 34, &imageSize, 4);
	fwrite(h, 1, 54, f);
	// The framebuffer is stored rotated: 240-pixel columns running up the screen.
	for (int y = 239; y >= 0; y--) {
		for (int x = 0; x < width; x++) memcpy(row + x * 3, fb + (x * 240 + (239 - y)) * 3, 3);
		fwrite(row, 1, (size_t)width * 3, f);
	}
	fclose(f);
}

static void runAutotest(void) {
	static const float PITCHES[] = {0.35f, 0.6f, 1.1f};
	static const float ZOOMS[] = {5.0f, 13.0f, 30.0f};
	FILE* out = fopen(AUTOTEST_DIR "autotest_result.txt", "w");
	if (!out) return;
	u8* culled = malloc(FB_BYTES);
	u8* full = malloc(FB_BYTES);
	if (!culled || !full) {
		fprintf(out, "FAILED no memory\n");
		fclose(out);
		return;
	}
	fprintf(out, "scene,yaw_deg,pitch,zoom,tris_culled,tris_all,diff_pixels\n");
	long cameras = 0, badCameras = 0, worst = 0;
	radiusIndex = 1;
	modeIndex = 0;
	for (int s = 0; s < SCENE_COUNT; s++) {
		if (!loadScene(s)) {
			fprintf(out, "FAILED load %s: %s\n", SCENE_NAMES[s], statusLine);
			fclose(out);
			return;
		}
		fprintf(out, "# %s\n", loadReport);
		for (int p = 0; p < 3; p++) for (int z = 0; z < 3; z++) for (int y = 0; y < 8; y++) {
			pitch = PITCHES[p];
			zoom = ZOOMS[z];
			yaw = y * (float)M_PI / 4.0f;
			noViewTest = false;
			if (!renderFrames(4)) goto end;
			u32 trisCulled = trisSubmitted;
			captureTop(culled);
			noViewTest = true;
			if (!renderFrames(4)) goto end;
			u32 trisAll = trisSubmitted;
			captureTop(full);
			noViewTest = false;
			long diff = 0;
			for (int i = 0; i < gameBytes(); i += 3) if (memcmp(culled + i, full + i, 3) != 0) diff++;
			fprintf(out, "%s,%d,%.2f,%.0f,%lu,%lu,%ld\n", SCENE_NAMES[s], y * 45, pitch, zoom, (unsigned long)trisCulled, (unsigned long)trisAll, diff);
			fflush(out);
			cameras++;
			if (diff) badCameras++;
			char path[80];
			if (diff > worst) {
				worst = diff;
				writeBmp(AUTOTEST_DIR "autotest_worst_culled.bmp", culled, gameOnBottom ? 320 : 400);
				writeBmp(AUTOTEST_DIR "autotest_worst_full.bmp", full, gameOnBottom ? 320 : 400);
			}
			if (p == 1 && z == 1 && y == 0) {
				snprintf(path, sizeof(path), AUTOTEST_DIR "autotest_%s.bmp", SCENE_NAMES[s]);
				writeBmp(path, culled, gameOnBottom ? 320 : 400);
			}
		}
	}
	fprintf(out, "DONE cameras=%ld bad_cameras=%ld worst_diff_pixels=%ld\n", cameras, badCameras, worst);
end:
	fclose(out);
	free(culled);
	free(full);
}

#define MENU_TOP 8
#include "input.inc"
#include "bag.inc"
#include "worn.inc"
#include "stats.inc"
#include "quests.inc"
#include "social.inc"
#include "tabs.inc"
#include "summon.inc"
#include "menu.inc"
#include "integrity.inc"
#include "changes.inc"
static void showMessage(const char* text);
#include "npcs.inc"
#include "guards.inc"
#include "heads.inc"
#include "ground.inc"
#include "overlay.inc"
#include "online.inc"

// One frame of everything except input.
#ifdef RS3DS_SLOW_PANEL
#define PANEL_MIN_MS 500
#else
#define PANEL_MIN_MS 50 // the panel is shown at most 20 times a second
#endif
#define GPU_STALL_MS 200.0 // a frame normally takes 20 to 40

// The system pausing the game looks just like a stuck GPU in the timings, so these are noted to tell them apart.
static aptHookCookie diagAptCookie;
static volatile int diagAptEvents[5], diagAptLogged[5];

static void diagAptHook(APT_HookType hook, void* param) {
	(void)param;
	if ((int)hook >= 0 && (int)hook < 5) diagAptEvents[hook]++;
}

static void diagReport(void) {
	static const char* const NAMES[5] = {"suspend", "restore", "sleep", "wakeup", "exit"};
	char line[120];
	for (int i = 0; i < 5; i++) {
		if (diagAptEvents[i] == diagAptLogged[i]) continue;
		diagAptLogged[i] = diagAptEvents[i];
		snprintf(line, sizeof(line), "system_event %s count=%d up_min=%d", NAMES[i], diagAptEvents[i], minutesSinceBoot());
		logLine(line);
	}
	double gpu = C3D_GetDrawingTime();
	if (gpu >= GPU_STALL_MS) {
		snprintf(line, sizeof(line), "gpu_stall ms=%.1f tab=%d swapped=%d menu=%d dialog=%d cmd=%.3f up_min=%d", gpu, panelTab, gameOnBottom, menuOpen, dialogActive(), C3D_GetCmdBufUsage(), minutesSinceBoot());
		logLine(line);
	}
}

static long panelLast = 0;

#ifdef RS3DS_SOAK
#define SOAK_SCENE_MS (5 * 60 * 1000)

// Keeps the GPU and the panel busy the way turning the camera does, with nobody at the console. It never connects,
// so nothing here can reach a server. Start finishes it, and the HOME Menu after that shows whether the glitch came.
static void soakTick(double frameMs, bool* sceneReady) {
	static long sceneAt = 0;
	static float phase = 0;
	static bool started = false;
	if (!started) {
		started = true;
		sceneAt = nowMs();
		logLine(DIAG_NOTE);
		snprintf(statusLine, sizeof(statusLine), "Soak test. Press Start to finish.");
	}
	phase += 0.01f;
	yaw += 0.03f;
	pitch = 0.75f + 0.25f * sinf(phase);
	zoom = 16.0f + 8.0f * sinf(phase * 0.37f);
	panelDirty = true;
	meshCheckStep();
	logHealth(frameMs);
	if ((u32)nowMs() - (u32)sceneAt >= SOAK_SCENE_MS) {
		sceneAt = nowMs();
		*sceneReady = loadScene((sceneIndex + 1) % SCENE_COUNT);
		logLine(*sceneReady ? "soak next area" : "soak area failed");
		gpuCheckReport("soak");
	}
}
#endif

static void frameTick(double frameMs, bool* sceneReady) {
	static int hudTick = 0;
	if (online) updateOnline(frameMs, sceneReady);
	diagReport();
	if (frameMs > frameMsPeak) frameMsPeak = frameMs;
	if (C3D_GetDrawingTime() > gpuMsPeak) gpuMsPeak = C3D_GetDrawingTime();
	// The menu always opens on the touch screen, covering the game if the game is there.
	if (menuOpen && gameOnBottom) gspWaitForVBlank();
	else renderGame(*sceneReady);
	hudFrameMs = frameMs;
	// Dragging and rotating ask for a redraw on every frame. Swapping this screen that often, on top of what
	// the GPU is doing for the other one, is a suspect for the glitching seen on real hardware.
	hudTick++;
	if ((hudTick >= 20 || panelDirty) && (u32)nowMs() - (u32)panelLast >= PANEL_MIN_MS) {
		hudTick = 0;
		panelDirty = false;
		panelLast = nowMs();
		panelsShown++;
		if (menuOpen) menuPresent();
		else panelPresent();
	}
}

#include "login.inc"
#include "guide.inc"
#include "store.inc"
#include "online_test.inc"

int main(void) {
#ifndef RS3DS_NO_L2
	osSetSpeedupEnable(true);
#endif
	gfxInitDefault();
	irrstInit();
	C3D_Init(CMDBUF_SIZE);
	aptHook(&diagAptCookie, diagAptHook, NULL);

	targetTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	targetBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(targetTop, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTargetSetOutput(targetBottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	sceneInit();

	showMessage("Loading the world...");
	int opened = world_open(&world, "sdmc:/3ds/rs3ds/cache", "sdmc:/3ds/rs3ds/xteas.json", 0.7);
	meshPool = linearAlloc(MESH_POOL_SIZE + GUARD_SIZE);
	dynamicPool = linearAlloc(DYNAMIC_POOL_SIZE + GUARD_SIZE);
	npcPool = linearAlloc(NPC_POOL_SIZE + GUARD_SIZE);
	guardsSet();
	vramWatchStart();
	gpuCheckStart();
	bool sceneReady = false;
	if (opened != 0) snprintf(statusLine, sizeof(statusLine), "Cache error %d: %.70s", opened, world.cache.error);
	else if (!meshPool) snprintf(statusLine, sizeof(statusLine), "Not enough GPU memory, %lu KB free", (unsigned long)(linearSpaceFree() >> 10));

	bool autotest = false, quitting = false;
	struct stat trigger;
	bool worldOk = opened == 0 && meshPool;
	if (worldOk && stat(ONLINE_TEST_FILE, &trigger) == 0) {
		autotest = true;
		FILE* f = fopen(ONLINE_TEST_FILE, "r");
		if (f) {
			if (fscanf(f, "%63s %15s %15s", serverHost, username, testFocus) < 2) serverHost[0] = 0;
			fclose(f);
		}
		strcpy(password, "password1");
		runOnlineTest(&sceneReady);
	} else if (worldOk && stat(AUTOTEST_DIR "autotest.txt", &trigger) != 0 && stat(AUTOTEST_DIR "offline.txt", &trigger) != 0) {
#ifdef RS3DS_SOAK
		sceneReady = loadScene(0); // no server and no login: it only looks at the world
#else
		readServerFile();
		quitting = !loginScreen("", &sceneReady);
#endif
	} else if (worldOk) {
		sceneReady = loadScene(0);
	}

	if (sceneReady && !autotest && stat(AUTOTEST_DIR "autotest.txt", &trigger) == 0) {
		if (stat(AUTOTEST_DIR "autotest_break.txt", &trigger) == 0) viewTestScale = 0.6f;
		if (stat(AUTOTEST_DIR "autotest_bottom.txt", &trigger) == 0) gameOnBottom = true;
		autotest = true;
		runAutotest();
	}

	u64 last = svcGetSystemTick();
	double frameMs = 16.7;
	while (!autotest && !quitting && aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;
		touchPosition touch;
		circlePosition pad;
		hidTouchRead(&touch);
		hidCircleRead(&pad);
		if (!bench.running) handleInput(down, held, touch, pad);

		if (!online && !bench.running && sceneReady) {
			float mx = pad.dx / 156.0f, my = pad.dy / 156.0f;
			if (fabsf(mx) < 0.1f) mx = 0;
			if (fabsf(my) < 0.1f) my = 0;
			targetX += (cosf(yaw) * mx - sinf(yaw) * my) * 0.25f;
			targetZ += (-sinf(yaw) * mx - cosf(yaw) * my) * 0.25f;
			if (held & KEY_X && zoom > 5.0f) zoom -= 0.2f;
			if (held & KEY_B && zoom < 30.0f) zoom += 0.2f;
			if (down & KEY_DUP && radiusIndex < RADIUS_COUNT - 1) radiusIndex++;
			if (down & KEY_DDOWN && radiusIndex > 0) radiusIndex--;
			if (down & KEY_DRIGHT && modeIndex < MODE_COUNT - 1) modeIndex++;
			if (down & KEY_DLEFT && modeIndex > 0) modeIndex--;
			if (down & KEY_A) sceneReady = loadScene((sceneIndex + 1) % SCENE_COUNT);
			if (down & KEY_Y) benchStart();
		}

#ifdef RS3DS_SOAK
		soakTick(frameMs, &sceneReady);
#endif
		bool wasOnline = online;
		frameTick(frameMs, &sceneReady);
		if (wasOnline && !online) {
			// Logged out or cut off: either way, back to the title.
			bool clean = net.loggedOut || logoutPressed;
			endSession();
			sceneReady = false;
			if (!loginScreen(clean ? "" : "Connection lost.", &sceneReady)) break;
			last = svcGetSystemTick();
		}

		u64 now = svcGetSystemTick();
		frameMs = (now - last) / (double)CPU_TICKS_PER_MSEC;
		last = now;
		if (bench.running) benchFrame(frameMs);
	}

	if (bench.out) fclose(bench.out);
	// Homebrew runs inside the launcher's process, so everything must be handed back or the launcher inherits the mess.
	shutDownNetwork();
	waitForGpu(); // nothing may still be running when the launcher takes the GPU back
	integrityFullCheck("exit");
	gpuCheckReport("exit");
	mcuHwcExit();
	logLine("exit clean");
	C3D_Fini();
	if (meshPool) linearFree(meshPool);
	if (dynamicPool) linearFree(dynamicPool);
	if (npcPool) linearFree(npcPool);
	irrstExit();
	gfxExit();
	return 0;
}
