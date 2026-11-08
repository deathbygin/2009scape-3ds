// The talking head beside dialogue. The server names the head and the expression to play, and the head is drawn
// in software on the panel, so it costs the GPU nothing. It only changes the picture, and sends nothing.

#define HEAD_SIZE 72

static SceneBase* headBase = NULL;
static HeadMesh headMesh;
static BoneTable* headBones = NULL;
static int32_t* headPose = NULL;
static u32 headPixels[HEAD_SIZE * HEAD_SIZE];
static long headVersion = 0;
static bool headReady = false;
static int headFrame, headDrawnFrame = -1, headSeq = -1;
static u32 headFrameAt;
static bool headMissing = false;

// The expression, if it is for this head.
static int headWantedSeq(void) {
	return net.headAnimComponent == net.headComponent ? net.headAnim : -1;
}

static void freeHead(void) {
	if (headBase) scene_free_base(headBase);
	headBase = NULL;
	head_free(&headMesh);
	if (headBones) anim_free_bones(headBones);
	headBones = NULL;
	free(headPose);
	headPose = NULL;
	headReady = false;
}

static void resetHead(void) {
	freeHead();
	headVersion = 0;
}

static void buildHead(void) {
	freeHead();
	headDrawnFrame = -1;
	headFrame = 0;
	headFrameAt = (u32)nowMs();
	headSeq = headWantedSeq();
	if (net.headKind == NET_HEAD_NPC) {
		const NpcDef* d = npcShown(net.headNpc);
		if (d) headBase = scene_build_npc_head(&scene, d);
	} else if (net.headKind == NET_HEAD_PLAYER) {
		const NetLook* l = &net.looks[NET_SELF_ID];
		if (l->valid) {
			PlayerLook look;
			lookOf(l, NULL, &look);
			headBase = scene_build_player_head(&scene, &look);
		}
	}
	scene_free(&scene);
	if (!headBase || head_prepare(&scene, world.palette, headBase, &headMesh) != 0) {
		freeHead();
		return;
	}
	headBones = anim_build_bones(headBase->vertexBones, headBase->vertexCount);
	headPose = malloc(sizeof(int32_t) * 3 * (size_t)(headBase->vertexCount + 1));
	headReady = headPose != NULL;
}

// Shows the head at a frame of its expression, loading the frame first if it has to be read from the SD card.
static void poseHead(const SeqDef* seq) {
	int count = headBase->vertexCount;
	int32_t *vx = headPose, *vy = headPose + count, *vz = headPose + count * 2;
	int missing = -1;
	if (seq && !anim_frame_ready(seq, headFrame, &missing)) {
		anim_load_frameset(missing);
		return;
	}
	memcpy(vx, headBase->vx, sizeof(int32_t) * (size_t)count);
	memcpy(vy, headBase->vy, sizeof(int32_t) * (size_t)count);
	memcpy(vz, headBase->vz, sizeof(int32_t) * (size_t)count);
	if (seq && headBones) anim_apply_pose(headBones, vx, vy, vz, NULL, 0, seq, headFrame);
	memset(headPixels, 0, sizeof(headPixels));
	head_draw(&headMesh, vx, vy, vz, count, headPixels, HEAD_SIZE);
	headDrawnFrame = headFrame;
	panelDirty = true;
}

// Draws an NPC's head once. Nothing is kept but the pixels, so it cannot disturb a head on show in dialogue.
static bool renderNpcHead(int npc, u32* pixels) {
	const NpcDef* d = npcShown(npc);
	SceneBase* base = d ? scene_build_npc_head(&scene, d) : NULL;
	scene_free(&scene);
	if (!base) return false;
	HeadMesh mesh;
	memset(&mesh, 0, sizeof(mesh));
	bool drawn = head_prepare(&scene, world.palette, base, &mesh) == 0;
	if (drawn) {
		memset(pixels, 0, sizeof(u32) * HEAD_SIZE * HEAD_SIZE);
		head_draw(&mesh, base->vx, base->vy, base->vz, base->vertexCount, pixels, HEAD_SIZE);
	}
	head_free(&mesh);
	scene_free_base(base);
	return drawn;
}

static int axisSpan(const int32_t* v, int count) {
	int low = INT_MAX, high = INT_MIN;
	for (int i = 0; i < count; i++) {
		if (v[i] < low) low = v[i];
		if (v[i] > high) high = v[i];
	}
	return count > 0 ? high - low : 0;
}

// Turns a flat model to face the viewer, along whichever axis it is thinnest. Returns false if it already does.
// Many of these are items lying down, which would otherwise show as a thin line and could not be told apart.
static bool faceViewer(SceneBase* base, int side) {
	int count = base->vertexCount;
	int spanX = axisSpan(base->vx, count), spanY = axisSpan(base->vy, count), spanZ = axisSpan(base->vz, count);
	if (spanZ <= spanX && spanZ <= spanY) return false;
	int32_t* other = spanY <= spanX ? base->vy : base->vx;
	for (int i = 0; i < count; i++) {
		int32_t kept = other[i];
		other[i] = side * base->vz[i];
		base->vz[i] = -side * kept;
	}
	return true;
}

// Draws a model and says how many pixels it covered, or -1.
static int drawModelSide(int model, int yaw, int side, bool* turned, u32* pixels) {
	SceneBase* base = scene_build_raw(&scene, model);
	scene_free(&scene);
	if (!base) return -1;
	*turned = faceViewer(base, side);
	HeadMesh mesh;
	memset(&mesh, 0, sizeof(mesh));
	int covered = -1;
	if (head_prepare(&scene, world.palette, base, &mesh) == 0) {
		memset(pixels, 0, sizeof(u32) * HEAD_SIZE * HEAD_SIZE);
		head_set_yaw(yaw);
		head_draw(&mesh, base->vx, base->vy, base->vz, base->vertexCount, pixels, HEAD_SIZE);
		head_set_yaw(-1);
		covered = 0;
		for (int i = 0; i < HEAD_SIZE * HEAD_SIZE; i++) if (pixels[i]) covered++;
	}
	head_free(&mesh);
	scene_free_base(base);
	return covered;
}

// Draws a model the server named into a picture. Like the head above, only the pixels are kept. One side of a
// turned model can have no faces at all, so both are tried and the fuller one kept.
static bool renderModelPicture(int model, int yaw, u32* pixels) {
	static u32 back[HEAD_SIZE * HEAD_SIZE];
	bool turned = false;
	int front = drawModelSide(model, yaw, 1, &turned, pixels);
	if (front < 0) return false;
	if (turned && drawModelSide(model, yaw, -1, &turned, back) > front) memcpy(pixels, back, sizeof(back));
	return true;
}

static void updateHead(void) {
	if (!dialogActive()) return;
	if (headVersion != net.headVersion) {
		headVersion = net.headVersion;
		buildHead();
	}
	if (!headReady) return;
	const SeqDef* seq = headSeq >= 0 ? anim_get_seq(headSeq) : NULL;
	if (headSeq != headWantedSeq()) {
		headSeq = headWantedSeq();
		headFrame = 0;
		headFrameAt = (u32)nowMs();
		headDrawnFrame = -1;
		seq = headSeq >= 0 ? anim_get_seq(headSeq) : NULL;
	}
	if (seq && seq->frameCount > 0 && (u32)nowMs() - headFrameAt >= (u32)seq->delays[headFrame] * 20) {
		headFrameAt = (u32)nowMs();
		headFrame++;
		if (headFrame >= seq->frameCount) headFrame = seq->replayoff >= 0 && seq->replayoff < seq->frameCount ? seq->replayoff : 0;
	}
	if (headDrawnFrame != headFrame) poseHead(seq);
}

static bool headShown(void) {
	return headReady && dialogLoaded && (net.headComponent >> 16 & 0xFFFF) == dialogIface.id;
}

static bool headOnRight(void) {
	return net.headKind == NET_HEAD_PLAYER;
}

static void drawHead(u8* fb, int width) {
	if (!headShown()) return;
	int left = headOnRight() ? width - HEAD_SIZE - 6 : 6;
	for (int y = 0; y < HEAD_SIZE; y++) {
		for (int x = 0; x < HEAD_SIZE; x++) {
			u32 p = headPixels[y * HEAD_SIZE + x];
			if (p) putPixel(fb, width, left + x, 6 + y, (u8)(p >> 16), (u8)(p >> 8), (u8)p);
		}
	}
}
