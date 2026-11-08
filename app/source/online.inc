// Connected play: login, scenes from the server, the player marker and the follow camera.

#define SOC_BUFFER_SIZE 0x100000
#define SERVER_FILE AUTOTEST_DIR "server.txt"
#define ONLINE_TEST_FILE AUTOTEST_DIR "autotest_online.txt"
#define MARKER_VERTS 36
#define CURSOR_VERTS 6

static u8* markerBuffer = NULL;
static u8* cursorBuffer = NULL;
static int markerFrame = 0;
static bool markerHidden = false; // for close-ups in the self-test
static char serverHost[64] = "", username[16] = "", password[24] = "";
static int serverPort = 43595;

static void logLine(const char* text) {
	FILE* f = fopen(AUTOTEST_DIR "online_log.txt", "a");
	if (!f) return;
	fprintf(f, "%s\n", text);
	fclose(f);
}

static bool loginShowing = false; // the login screen owns both screens, so messages wait

static void showMessage(const char* text) {
	snprintf(statusLine, sizeof(statusLine), "%s", text);
	if (!loginShowing) panelPresent();
}

static bool askText(const char* hint, char* out, size_t size, bool secret) {
	SwkbdState keyboard;
	swkbdInit(&keyboard, SWKBD_TYPE_WESTERN, 2, (int)size - 1);
	swkbdSetHintText(&keyboard, hint);
	if (secret) swkbdSetPasswordMode(&keyboard, SWKBD_PASSWORD_HIDE_DELAY);
	if (out[0]) swkbdSetInitialText(&keyboard, out);
	return swkbdInputText(&keyboard, out, size) == SWKBD_BUTTON_CONFIRM && out[0];
}

// Reads the server address. Returns false when no server is configured, which means offline mode.
static bool readServerFile(void) {
	FILE* f = fopen(SERVER_FILE, "r");
	if (!f) return false;
	int fields = fscanf(f, "%63s %d %15s", serverHost, &serverPort, username);
	fclose(f);
	return fields >= 1;
}

static void placePlayerInstantly(void) {
	playerViewX = net.localX + 0.5f;
	playerViewZ = -(net.localY + 0.5f);
	playerViewH = groundHeight(net.plane, playerViewX, -playerViewZ);
	targetX = playerViewX;
	targetZ = playerViewZ;
	targetH = playerViewH;
}

static bool loadFromServer(void) {
	if (menuOpen) closeMenu(); // its entries point into the list about to be replaced
	waitForGpu(); // the old map is about to be overwritten, and the last frame may still be drawing it
	showMessage("Building the map...");
	net.rebuildPending = 0;
	char name[32];
	snprintf(name, sizeof(name), "zone_%d_%d", net.zoneX, net.zoneY);
	resetChanges();
	anim_clear_framesets(); // the scene builder needs the memory back
	bool ok = loadZone(net.zoneX, net.zoneY, (const int32_t (*)[4])net.keys, net.regionCount, net.localX, net.localY, net.plane, name);
	if (ok) placePlayerInstantly();
	walkFlagX = walkFlagY = -1;
	logLine(ok ? loadReport : statusLine);
	panelDirty = true;
	return ok;
}

// A plain blocking connect is what the 3DS handles reliably, so it runs on its own thread while this one
// keeps time. If the time runs out the thread is left behind, because closing the socket would wait for it.
typedef struct {
	int sock, size;
	struct sockaddr_in address;
	volatile int finished, result;
} ConnectJob;

static ConnectJob connectJob;

static void connectThread(void* arg) {
	ConnectJob* job = arg;
	job->result = connect(job->sock, (const struct sockaddr*)&job->address, (socklen_t)job->size);
	job->finished = 1;
}

static int connectOnThread(int sock, const void* address, int size, int timeoutMs) {
	connectJob.sock = sock;
	connectJob.size = size;
	memcpy(&connectJob.address, address, sizeof(connectJob.address));
	connectJob.finished = 0;
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	Thread thread = threadCreate(connectThread, &connectJob, 16 * 1024, priority - 1, -2, false);
	if (!thread) return connect(sock, (const struct sockaddr*)address, (socklen_t)size);
	for (int waited = 0; waited < timeoutMs && !connectJob.finished; waited += 50) svcSleepThread(50 * 1000000LL);
	if (!connectJob.finished) return -1;
	threadJoin(thread, U64_MAX);
	threadFree(thread);
	return connectJob.result;
}

static void* socBuffer = NULL;
static bool socReady = false;

static void shutDownNetwork(void) {
	if (net.sock > 0 && net.connected) net_close(&net);
	// A connect that never finished still holds the service, and asking it to stop would wait forever.
	if (socReady && (connectJob.finished || connectJob.size == 0)) {
		socExit();
		free(socBuffer);
		socReady = false;
	}
}

static long statsSeen = -1;

// What every session shares, done the first time only.
static bool startNetwork(void) {
	if (socReady) return true;
	socBuffer = memalign(0x1000, SOC_BUFFER_SIZE);
	Result rc = socBuffer ? socInit(socBuffer, SOC_BUFFER_SIZE) : -1;
	if (R_FAILED(rc)) {
		snprintf(statusLine, sizeof(statusLine), "Network start failed: %08lX", (unsigned long)rc);
		return false;
	}
	markerBuffer = linearAlloc(MARKER_VERTS * VERT_SIZE * 2);
	prepareWearTable();
	loadTabSprites();
	loadWornTab();
	loadStatsTab();
	socReady = true;
	srand((unsigned)osGetTime());
	return true;
}

static bool goOnline(void) {
	if (!startNetwork()) return false;
	char line[96];
	snprintf(line, sizeof(line), "Connecting to %s:%d...", serverHost, serverPort);
	showMessage(line);
	net_connect_hook = connectOnThread;
	if (net_login(&net, serverHost, serverPort, username, password) != 0) {
		snprintf(statusLine, sizeof(statusLine), "Login failed: %.70s", net.error);
		logLine(statusLine);
		if (net.sock > 0) net_close(&net);
		return false;
	}
	memset(password, 0, sizeof(password));
	resetQuests();
	resetTabs();
	resetSocial();
	loadChatCodec();
	loadFonts();
	online = true;
	return true;
}

#define HEALTH_LOG_MS 5000

// Written every few seconds so that the last lines before a crash say what was happening.
static void logHealth(double frameMs) {
	static long last = 0;
	long now = nowMs();
	if ((u32)now - (u32)last < HEALTH_LOG_MS) return;
	last = now;
	struct mallinfo info = mallinfo();
	char line[300];
	snprintf(line, sizeof(line), "health up_min=%d temp=%d linear_kb=%lu cmd=%.2f cut=%ld draws=%lu frame_max=%.1f gpu_max=%.1f panels=%ld corrupt=%ld checks=%ld tab=%d swapped=%d view=%d heap_kb=%lu npcs=%d players=%d ground=%d dynamic=%d hidden=%d poses=%ld resets=%ld",
		minutesSinceBoot(), consoleTemperature(), (unsigned long)(linearSpaceFree() >> 10), cmdPeakRecent, cmdFramesCut, (unsigned long)drawCallsPeak, frameMsPeak, gpuMsPeak, panelsShown, meshCorrupt, meshChecks, panelTab, gameOnBottom, RADII[radiusIndex], (unsigned long)(info.uordblks >> 10),
		net.npcCount, net.playerCount, net.groundCount, dynamicCount, hiddenCount, npcPosesBuilt, npcPoolResets);
	logLine(line);
	integrityQuickCheck();
	cmdPeakRecent = 0;
	frameMsPeak = gpuMsPeak = 0;
	panelsShown = 0;
}

// Runs once per frame while connected. Returns false when the connection has ended.
static bool updateOnline(double frameMs, bool* sceneReady) {
	if (net_poll(&net, nowMs()) != 0) {
		snprintf(statusLine, sizeof(statusLine), "Disconnected: %.70s", net.error);
		logLine(statusLine);
		online = false;
		panelDirty = true;
		return false;
	}
	if (net.rebuildPending) *sceneReady = loadFromServer();
	else {
		if (net.changeCount > 0) waitForGpu();
		applyChanges();
	}
	if (statsSeen != net.statVersion) {
		statsSeen = net.statVersion;
		if (panelTab == TAB_STATS) panelDirty = true;
	}
	updateDialog();
	updateBag();
	updateQuests();
	updateTabs();
	updateSocial();
	updateChat();
	updateGuide();
	updateHead();
	updateStore();
	updatePrompt();
	updateOverlay();

	float wantX = net.localX + 0.5f, wantZ = -(net.localY + 0.5f);
	float step = (float)(frameMs / 1000.0) * (runMode ? 3.6f : 1.8f);
	float dx = wantX - playerViewX, dz = wantZ - playerViewZ, distance = sqrtf(dx * dx + dz * dz);
	if (distance > 8.0f || distance <= step) {
		playerViewX = wantX;
		playerViewZ = wantZ;
	} else {
		playerViewX += dx / distance * step;
		playerViewZ += dz / distance * step;
	}
	if (net.localX == walkFlagX && net.localY == walkFlagY) {
		walkFlagX = walkFlagY = -1;
		panelDirty = true;
	}
	float wantH = groundHeight(net.plane, playerViewX, -playerViewZ);
	playerViewH += (wantH - playerViewH) * 0.2f;
	targetX = playerViewX;
	targetZ = playerViewZ;
	targetH = playerViewH;
	updateNpcs(frameMs);
	meshCheckStep();
	logHealth(frameMs);
	return true;
}

// A plain box stands in for the player until their model is ready.
static void drawPlayerMarker(void) {
	if (!online || !markerBuffer || markerHidden || selfHidden || net.self.hasModel) return;
	static const s8 CORNERS[8][3] = {{-1,0,-1},{1,0,-1},{1,0,1},{-1,0,1},{-1,1,-1},{1,1,-1},{1,1,1},{-1,1,1}};
	static const u8 FACES[12][3] = {{0,1,2},{0,2,3},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
	u8* v = markerBuffer + markerFrame * MARKER_VERTS * VERT_SIZE;
	markerFrame ^= 1;
	int baseX = (int)(playerViewX * 128.0f), baseZ = (int)(-playerViewZ * 128.0f), baseY = (int)(-playerViewH * 128.0f);
	for (int f = 0; f < 12; f++) {
		for (int k = 0; k < 3; k++) {
			const s8* c = CORNERS[FACES[f][k]];
			s16 p[4] = {(s16)(baseX + c[0] * 40), (s16)(baseY - c[1] * 200), (s16)(baseZ + c[2] * 40), 0};
			u8* out = v + (f * 3 + k) * VERT_SIZE;
			memcpy(out, p, 8);
			out[8] = (u8)(f < 2 ? 200 : 255);
			out[9] = (u8)(f < 4 ? 220 : 120);
			out[10] = 0;
			out[11] = 255;
		}
	}
	GSPGPU_FlushDataCache(v, MARKER_VERTS * VERT_SIZE);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, v, VERT_SIZE, 2, 0x10);
	C3D_DrawArrays(GPU_TRIANGLES, 0, MARKER_VERTS);
	C3D_CullFace(GPU_CULL_BACK_CCW);
}

// The cursor over the game view, when the game is on the top screen.
static void drawGameCursor(void) {
	if (!online || gameOnBottom) return;
	if (!cursorBuffer) cursorBuffer = linearAlloc(CURSOR_VERTS * VERT_SIZE * 2);
	if (!cursorBuffer) return;
	static int frame = 0;
	u8* v = cursorBuffer + frame * CURSOR_VERTS * VERT_SIZE;
	frame ^= 1;
	static const s8 SHAPE[CURSOR_VERTS][2] = {{-1,-1},{13,10},{-1,14},{0,0},{10,9},{0,11}};
	for (int i = 0; i < CURSOR_VERTS; i++) {
		s16 p[4] = {(s16)((int)cursorX + SHAPE[i][0]), (s16)((int)cursorY + SHAPE[i][1]), 0, 0};
		u8* out = v + i * VERT_SIZE;
		u8 shade = i < 3 ? 0 : 255;
		memcpy(out, p, 8);
		out[8] = out[9] = out[10] = shade;
		out[11] = 255;
	}
	GSPGPU_FlushDataCache(v, CURSOR_VERTS * VERT_SIZE);
	C3D_Mtx ortho;
	Mtx_OrthoTilt(&ortho, 0.0f, 400.0f, 240.0f, 0.0f, -1.0f, 1.0f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_mvp, &ortho);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, v, VERT_SIZE, 2, 0x10);
	C3D_DrawArrays(GPU_TRIANGLES, 0, CURSOR_VERTS);
	C3D_CullFace(GPU_CULL_BACK_CCW);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}
