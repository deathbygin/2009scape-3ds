// Watches what the GPU actually produces. The other checks read memory through the CPU, so they cannot see the GPU
// going wrong. Every few seconds this redraws a piece of the real world, with a frozen camera, into a hidden picture.
// The same commands should give the same pixels, so a difference means the GPU's result changed.
// A reference only counts once it has come out the same several times, and if it never settles the check stops,
// because a GPU that is not repeatable would give nothing but false alarms.

#define GPU_CHECK_SIZE 128
#define GPU_CHECK_FRAMES 150 // about every five seconds
#define GPU_CHECK_SETTLE 3   // matching results in a row before a reference is trusted
#define GPU_CHECK_TRIES 30
#define GPU_CHECK_LOG_LIMIT 40
#define GPU_CHECK_PIXELS (GPU_CHECK_SIZE * GPU_CHECK_SIZE)

enum { GPU_CHECK_CHOOSING, GPU_CHECK_SETTLING, GPU_CHECK_WATCHING, GPU_CHECK_OFF };

static C3D_RenderTarget* gpuCheckTarget = NULL;
static int gpuCheckState = GPU_CHECK_CHOOSING, gpuCheckFrames = 0, gpuCheckMatches = 0, gpuCheckTries = 0;
static bool gpuCheckPending = false;
static long gpuChecks = 0, gpuCheckMismatches = 0;
static int gpuCheckWorst = 0;
static u32 gpuCheckRef[GPU_CHECK_PIXELS], gpuCheckNow[GPU_CHECK_PIXELS];

// What gets redrawn: the largest run of the world drawn when the check began.
static const void* gpuCheckVerts = NULL;
static const u16* gpuCheckIndices = NULL;
static u32 gpuCheckCount = 0;
static C3D_Mtx gpuCheckMvp;

// The world has changed, on purpose, so what was seen before no longer applies.
static void gpuCheckReset(void) {
	if (gpuCheckState == GPU_CHECK_OFF) return;
	gpuCheckState = GPU_CHECK_CHOOSING;
	gpuCheckVerts = NULL;
	gpuCheckCount = 0;
	gpuCheckMatches = gpuCheckTries = 0;
	gpuCheckPending = false;
}

static void gpuCheckStart(void) {
	gpuCheckTarget = C3D_RenderTargetCreate(GPU_CHECK_SIZE, GPU_CHECK_SIZE, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!gpuCheckTarget) {
		gpuCheckState = GPU_CHECK_OFF;
		logLine("gpu_check off: no room for its picture");
	}
}

// Called for each run of the world as it is drawn.
static void gpuCheckOffer(const void* verts, const u16* indices, u32 count, const C3D_Mtx* mvp) {
	if (gpuCheckState != GPU_CHECK_CHOOSING || count <= gpuCheckCount) return;
	gpuCheckVerts = verts;
	gpuCheckIndices = indices;
	gpuCheckCount = count;
	gpuCheckMvp = *mvp;
}

// Called at the end of a frame's drawing.
static void gpuCheckDraw(void) {
	if (gpuCheckState == GPU_CHECK_OFF || !gpuCheckTarget) return;
	if (gpuCheckState == GPU_CHECK_CHOOSING) {
		if (gpuCheckCount == 0) return;
		gpuCheckState = GPU_CHECK_SETTLING;
	}
	if (++gpuCheckFrames < GPU_CHECK_FRAMES) return;
	gpuCheckFrames = 0;
	C3D_RenderTargetClear(gpuCheckTarget, C3D_CLEAR_ALL, 0, 0);
	C3D_FrameDrawOn(gpuCheckTarget);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_mvp, &gpuCheckMvp);
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, gpuCheckVerts, VERT_SIZE, 2, 0x10);
	C3D_DrawElements(GPU_TRIANGLES, gpuCheckCount, C3D_UNSIGNED_SHORT, gpuCheckIndices);
	gpuCheckPending = true;
}

// Called once the GPU has finished, which is when a new frame has begun.
static void gpuCheckRead(void) {
	if (!gpuCheckPending) return;
	gpuCheckPending = false;
	const volatile u32* picture = (const volatile u32*)gpuCheckTarget->frameBuf.colorBuf;
	int covered = 0;
	for (int i = 0; i < GPU_CHECK_PIXELS; i++) {
		gpuCheckNow[i] = picture[i];
		if (gpuCheckNow[i]) covered++;
	}
	char line[120];
	if (gpuCheckState == GPU_CHECK_SETTLING) {
		bool same = gpuCheckTries > 0 && memcmp(gpuCheckNow, gpuCheckRef, sizeof(gpuCheckRef)) == 0;
		gpuCheckMatches = same ? gpuCheckMatches + 1 : 0;
		gpuCheckTries++;
		memcpy(gpuCheckRef, gpuCheckNow, sizeof(gpuCheckRef));
		if (gpuCheckMatches >= GPU_CHECK_SETTLE && covered > 0) {
			gpuCheckState = GPU_CHECK_WATCHING;
			snprintf(line, sizeof(line), "gpu_check watching triangles=%lu covered=%d up_min=%d", (unsigned long)(gpuCheckCount / 3), covered, minutesSinceBoot());
			logLine(line);
		} else if (gpuCheckTries >= GPU_CHECK_TRIES) {
			snprintf(line, sizeof(line), "gpu_check off: never settled matches=%d covered=%d", gpuCheckMatches, covered);
			logLine(line);
			gpuCheckState = GPU_CHECK_OFF;
		}
		return;
	}
	if (gpuCheckState != GPU_CHECK_WATCHING) return;
	gpuChecks++;
	int different = 0;
	for (int i = 0; i < GPU_CHECK_PIXELS; i++) if (gpuCheckNow[i] != gpuCheckRef[i]) different++;
	if (different == 0) return;
	gpuCheckMismatches++;
	if (different > gpuCheckWorst) gpuCheckWorst = different;
	if (gpuCheckMismatches <= GPU_CHECK_LOG_LIMIT) {
		snprintf(line, sizeof(line), "gpu_check MISMATCH different=%d of %d covered=%d check=%ld up_min=%d", different, GPU_CHECK_PIXELS, covered, gpuChecks, minutesSinceBoot());
		logLine(line);
	}
}

static void gpuCheckReport(const char* when) {
	char line[120];
	snprintf(line, sizeof(line), "gpu_check %s state=%d checks=%ld mismatches=%ld worst=%d", when, gpuCheckState, gpuChecks, gpuCheckMismatches, gpuCheckWorst);
	logLine(line);
}
