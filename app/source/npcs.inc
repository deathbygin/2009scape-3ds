// NPCs: models are built from the cache when first needed. Colours and triangles are stored once per model,
// and each animation frame adds only a list of positions, kept until the pool fills.

#define NPC_POOL_SIZE (6u * 1024 * 1024)
#define NPC_MODEL_REGION (2560u * 1024) // models get this much, poses the rest, so neither starves the other
#define NPC_MODEL_MAX 160
#define NPC_POSE_SLOTS 4096
#define NPC_WALK_SPEED 1.8f // tiles per second, about one tile per server tick
#define NPC_POSITION_SIZE 8
#define CLIENT_CYCLE_MS 20.0

typedef struct {
	u64 key;
	const NpcDef* def; // NULL for a player, who has no size scaling
	u16* indices;     // GPU memory
	u8* colours;      // GPU memory, four bytes per corner
	u8* restPose;     // GPU memory, the unanimated positions
	u32 indexCount, cornerCount;
	u16* cornerVertex; // which model vertex each corner came from
	int vertexCount;
	int32_t *vx, *vy, *vz;
	BoneTable* bones;
	int top;    // height above the feet, in game units
	int radius; // reach from the centre line, in game units
} NpcModel;

typedef struct {
	u32 keyA, keyB; // keyA is 0 when the slot is free
	u8* positions;
} NpcPoseSlot;

static NpcModel npcModels[NPC_MODEL_MAX];
static int npcModelCount = 0;
static NpcPoseSlot npcPoses[NPC_POSE_SLOTS];
static int npcPoseCount = 0;
static u8* npcPool = NULL;
static size_t npcPoolBottom = 0, npcPoolTop = NPC_POOL_SIZE; // models grow up from the start, poses down from the end
static long npcModelsBuilt = 0, npcPosesBuilt = 0, npcPoolResets = 0;
static int npcsDrawn = 0, npcLoadBudget = 0;
static double npcCycleMs = 0;
static int32_t* poseScratch = NULL;
static int poseScratchSize = 0;

// A full pool cannot be emptied while a frame is being built: draws already queued still read it, and the GPU
// reads them after the frame ends. So a reset waits here until the GPU has finished and no draw is queued.
static bool npcPoseResetWanted = false, npcModelResetWanted = false;

static const NpcDef* npcShown(int type) {
	if (!scene.cache) return NULL;
	return scene_shown_npc(&scene, scene_get_npc(&scene, type));
}

static const NpcModel* findNpcModel(u64 key) {
	for (int i = 0; i < npcModelCount; i++) if (npcModels[i].key == key) return &npcModels[i];
	return NULL;
}

static int npcModelTop(u64 key) {
	const NpcModel* m = findNpcModel(key);
	return m && m->top > 0 ? m->top : 200;
}

static void forgetPoses(void) {
	memset(npcPoses, 0, sizeof(npcPoses));
	npcPoseCount = 0;
	npcPoolTop = NPC_POOL_SIZE;
	for (int i = 0; i < net.npcCount; i++) net.npcs[i].pose = NULL;
	for (int i = 0; i < net.playerCount; i++) net.players[i].pose = NULL;
	net.self.pose = NULL;
	npcPoolResets++;
}

static void forgetModels(void) {
	for (int i = 0; i < npcModelCount; i++) {
		NpcModel* m = &npcModels[i];
		free(m->cornerVertex);
		free(m->vx);
		free(m->vy);
		free(m->vz);
		anim_free_bones(m->bones);
	}
	npcModelCount = 0;
	npcPoolBottom = 0;
	forgetPoses();
}

static u8* poolAllocModel(size_t bytes) {
	bytes = (bytes + 15) & ~(size_t)15;
	if (bytes > NPC_MODEL_REGION - npcPoolBottom) return NULL;
	u8* out = npcPool + npcPoolBottom;
	npcPoolBottom += bytes;
	return out;
}

static u8* poolAllocPose(size_t bytes) {
	bytes = (bytes + 15) & ~(size_t)15;
	if (bytes > npcPoolTop - NPC_MODEL_REGION) return NULL;
	npcPoolTop -= bytes;
	return npcPool + npcPoolTop;
}

// Positions for each corner, after the NPC's size scaling.
static void writePose(const NpcModel* m, int32_t* vx, int32_t* vy, int32_t* vz, u8* out) {
	if (m->def) scene_resize_npc(vx, vy, vz, m->vertexCount, m->def);
	for (u32 k = 0; k < m->cornerCount; k++) {
		int v = m->cornerVertex[k];
		s16 p[4] = {(s16)vx[v], (s16)vy[v], (s16)vz[v], 0};
		memcpy(out + k * NPC_POSITION_SIZE, p, NPC_POSITION_SIZE);
	}
	GSPGPU_FlushDataCache(out, m->cornerCount * NPC_POSITION_SIZE);
}

static bool copyToScratch(const NpcModel* m) {
	if (m->vertexCount > poseScratchSize) {
		int32_t* grown = realloc(poseScratch, (size_t)m->vertexCount * 3 * 4);
		if (!grown) return false;
		poseScratch = grown;
		poseScratchSize = m->vertexCount;
	}
	size_t bytes = (size_t)m->vertexCount * 4;
	memcpy(poseScratch, m->vx, bytes);
	memcpy(poseScratch + poseScratchSize, m->vy, bytes);
	memcpy(poseScratch + poseScratchSize * 2, m->vz, bytes);
	return true;
}

// Only for the start of a frame, when the GPU is idle.
static void applyPoolResets(void) {
	if (npcModelResetWanted) forgetModels();
	else if (npcPoseResetWanted) forgetPoses();
	npcModelResetWanted = npcPoseResetWanted = false;
}

static void buildBody(u64 key, const NpcDef* d, const PlayerLook* look, const ObjDef* item) {
	if (!npcPool) return;
	if (npcModelCount == NPC_MODEL_MAX) {
		npcModelResetWanted = true;
		return;
	}
	NpcModel m;
	memset(&m, 0, sizeof(m));
	m.key = key;
	m.def = d;
	SceneBase* b = item ? scene_build_item(&scene, item) : look ? scene_build_player(&scene, look) : scene_build_npc(&scene, d);
	if (b) {
		size_t corners = (size_t)b->triangleCount * 3;
		u16* cornerVertex = malloc(corners * 2 + 2);
		u16* indices = malloc(corners * 2 + 2);
		u8* colours = malloc(corners * 4 + 4);
		int count = cornerVertex && indices && colours ? mesh_index_model(&scene, world.palette, b, cornerVertex, colours, indices, &m.indexCount) : -1;
		if (count > 0) {
			size_t need = m.indexCount * 2 + (size_t)count * (4 + NPC_POSITION_SIZE) + 64;
			// Starting over drops people who have left. Those still here are rebuilt over later frames.
			if (need > NPC_MODEL_REGION - npcPoolBottom) {
				npcModelResetWanted = true;
			} else {
				m.indices = (u16*)poolAllocModel(m.indexCount * 2);
				m.colours = poolAllocModel((size_t)count * 4);
				m.restPose = poolAllocModel((size_t)count * NPC_POSITION_SIZE);
			}
		}
		if (m.indices && m.colours && m.restPose) {
			m.cornerCount = (u32)count;
			m.vertexCount = b->vertexCount;
			m.vx = b->vx;
			m.vy = b->vy;
			m.vz = b->vz;
			b->vx = b->vy = b->vz = NULL;
			m.bones = anim_build_bones(b->vertexBones, b->vertexCount);
			m.cornerVertex = cornerVertex;
			cornerVertex = NULL;
			memcpy(m.indices, indices, m.indexCount * 2);
			memcpy(m.colours, colours, (size_t)count * 4);
			GSPGPU_FlushDataCache(m.indices, m.indexCount * 2);
			GSPGPU_FlushDataCache(m.colours, (u32)count * 4);
			for (int v = 0; v < m.vertexCount; v++) {
				int reach = (int)sqrtf((float)(m.vx[v] * m.vx[v] + m.vz[v] * m.vz[v]));
				if (-m.vy[v] > m.top) m.top = -m.vy[v];
				if (reach > m.radius) m.radius = reach;
			}
			if (copyToScratch(&m)) writePose(&m, poseScratch, poseScratch + poseScratchSize, poseScratch + poseScratchSize * 2, m.restPose);
			npcModelsBuilt++;
		} else {
			m.indexCount = 0;
		}
		free(cornerVertex);
		free(indices);
		free(colours);
		scene_free_base(b);
	}
	scene_free(&scene); // drops the raw models it loaded
	npcModels[npcModelCount++] = m;
}

// Returns the positions for a pose, building them if needed. NULL means not ready yet.
static const u8* npcPose(const NpcModel* m, const SeqDef* primary, int primaryFrame, const SeqDef* secondary, int secondaryFrame) {
	if (!primary && !secondary) return m->restPose;
	if (!m->bones) return m->restPose;
	int slot = (int)(m - npcModels);
	u32 keyA = (u32)(slot + 1) << 16 | (u32)(primary ? primary->id + 1 : 0);
	u32 keyB = (u32)(secondaryFrame & 0xFF) << 24 | (u32)(secondary ? secondary->id + 1 : 0) << 8 | (u32)(primaryFrame & 0xFF);
	u32 at = (keyA * 2654435761u ^ keyB * 40503u) % NPC_POSE_SLOTS;
	while (npcPoses[at].keyA) {
		if (npcPoses[at].keyA == keyA && npcPoses[at].keyB == keyB) return npcPoses[at].positions;
		at = (at + 1) % NPC_POSE_SLOTS;
	}

	// Each frameset is an SD card read, so only one is loaded per frame.
	int missing = -1;
	while (!anim_frame_ready(primary, primaryFrame, &missing) || !anim_frame_ready(secondary, secondaryFrame, &missing)) {
		if (npcLoadBudget <= 0) return NULL;
		npcLoadBudget--;
		anim_load_frameset(missing);
	}

	size_t bytes = (size_t)m->cornerCount * NPC_POSITION_SIZE;
	if (npcPoseCount >= NPC_POSE_SLOTS * 3 / 4 || bytes + 16 > npcPoolTop - NPC_MODEL_REGION) {
		npcPoseResetWanted = true; // the entity keeps its last pose for this one frame
		return NULL;
	}
	u8* out = poolAllocPose(bytes);
	if (!out || !copyToScratch(m)) return NULL;
	int32_t *vx = poseScratch, *vy = poseScratch + poseScratchSize, *vz = poseScratch + poseScratchSize * 2;
	anim_apply_pose(m->bones, vx, vy, vz, primary, primaryFrame, secondary, secondaryFrame);
	writePose(m, vx, vy, vz, out);
	npcPoses[at] = (NpcPoseSlot){keyA, keyB, out};
	npcPoseCount++;
	npcPosesBuilt++;
	return out;
}

// Protocol.animateNpc: a new action replaces the current one unless the current one matters more.
static void startSeq(NetNpc* n, int id, int delay) {
	const SeqDef* next = id == -1 ? NULL : anim_get_seq(id);
	if (n->seqId == id && id != -1) {
		if (next && next->exactmove == 1) {
			n->seqFrame = 0;
			n->seqClock = 0;
			n->seqReplay = 0;
			n->seqDelay = delay;
		} else if (next && next->exactmove == 2) {
			n->seqReplay = 0;
		}
		return;
	}
	const SeqDef* current = n->seqId == -1 ? NULL : anim_get_seq(n->seqId);
	if (id == -1 || n->seqId == -1 || !current || (next && next->priority >= current->priority)) {
		n->seqId = id;
		n->seqFrame = 0;
		n->seqClock = 0;
		n->seqReplay = 0;
		n->seqDelay = delay;
	}
}

// NpcList.processAnimations, for the movement and action animations.
static void tickAnimations(NetNpc* n) {
	if (n->moveSeq != -1) {
		const SeqDef* s = anim_get_seq(n->moveSeq);
		if (!s || !s->frames) {
			n->moveSeq = -1;
		} else {
			n->moveClock++;
			if (n->moveFrame < s->frameCount && n->moveClock > s->delays[n->moveFrame]) {
				n->moveClock = 1;
				n->moveFrame++;
			}
			if (n->moveFrame >= s->frameCount) {
				n->moveFrame = 0;
				n->moveClock = 0;
			}
		}
	}
	if (n->seqId != -1 && n->seqDelay == 0) {
		const SeqDef* s = anim_get_seq(n->seqId);
		if (!s || !s->frames) {
			n->seqId = -1;
		} else {
			n->seqClock++;
			if (n->seqFrame < s->frameCount && n->seqClock > s->delays[n->seqFrame]) {
				n->seqClock = 1;
				n->seqFrame++;
			}
			if (n->seqFrame >= s->frameCount) {
				n->seqFrame -= s->replayoff;
				n->seqReplay++;
				if (n->seqReplay >= s->replaycount || n->seqFrame < 0 || n->seqFrame >= s->frameCount) n->seqId = -1;
			}
		}
	}
	if (n->seqDelay > 0) n->seqDelay--;
}

static int movementSeq(const NetNpc* n, const BasDef* b, bool moving, bool fast) {
	if (!moving) return b->idle;
	int difference = (n->angle - (int)n->viewAngle) & 2047;
	if (difference > 1024) difference -= 2048;
	int walk = b->walkTurn, run = b->runTurn;
	if (difference >= -256 && difference <= 256) {
		walk = b->walk;
		run = b->run;
	} else if (difference >= 256 && difference < 768) {
		walk = b->walkCW;
		run = b->runCW;
	} else if (difference >= -768 && difference <= -256) {
		walk = b->walkCCW;
		run = b->runCCW;
	}
	if (walk == -1) walk = b->walk;
	if (fast && b->run != -1) return run != -1 ? run : b->run;
	return walk;
}

static int angleTowards(float fromX, float fromY, float toX, float toY) {
	return (int)(atan2f(fromX - toX, fromY - toY) * 325.949f) & 2047;
}

// NpcList.processFacing, without the smooth acceleration a few large NPCs use.
static void tickFacing(NetNpc* n, const BasDef* b, int speed, bool moving) {
	if (speed == 0) return;
	if (n->faceEntity >= 32768) {
		int id = n->faceEntity - 32768;
		const NetNpc* other = id == net.selfId ? &net.self : NULL;
		for (int i = 0; i < net.playerCount && !other; i++) if (net.players[i].index == id) other = &net.players[i];
		if (other && (other->viewX != n->viewX || other->viewY != n->viewY)) n->angle = angleTowards(n->viewX, n->viewY, other->viewX, other->viewY);
	} else if (n->faceEntity != -1) {
		for (int i = 0; i < net.npcCount; i++) {
			const NetNpc* other = &net.npcs[i];
			if (other->index != n->faceEntity) continue;
			if (other->viewX != n->viewX || other->viewY != n->viewY) n->angle = angleTowards(n->viewX, n->viewY, other->viewX, other->viewY);
			break;
		}
	}
	if ((n->faceX != 0 || n->faceY != 0) && !moving) {
		float x = (n->faceX - (net.zoneX - 6) * 16) * 0.5f, y = (n->faceY - (net.zoneY - 6) * 16) * 0.5f;
		if (x != n->viewX || y != n->viewY) n->angle = angleTowards(n->viewX, n->viewY, x, y);
		n->faceX = n->faceY = 0;
	}
	int current = (int)n->viewAngle, difference = (n->angle - current) & 2047;
	if (difference == 0) {
		n->turnCounter = 0;
		return;
	}
	n->turnCounter++;
	bool turning = true, left = difference > 1024;
	current += left ? -speed : speed;
	if (difference < speed || difference > 2048 - speed) {
		current = n->angle;
		turning = false;
	}
	n->viewAngle = (float)(current & 2047);
	if (b->idle == n->moveSeq && (n->turnCounter > 25 || turning)) {
		int turn = left ? b->standCCW : b->standCW;
		n->moveSeq = turn != -1 ? turn : b->walk;
	}
}

static void updateGround(bool* built);
static void drawGroundItems(const C3D_Mtx* mvp, float limit);
static void drawModelAt(const NpcModel* m, const void* pose, float x, float y, float angleUnits, const C3D_Mtx* mvp, float limit);

// Moves someone towards their tile and advances their animations.
// The local player's position is smoothed by the follow camera, so only their animation is handled here.
static void advanceEntity(NetNpc* n, const BasDef* bas, float half, int speed, float seconds, int cycles, bool self) {
	if (n->pendingSeq != NET_NO_SEQ) {
		startSeq(n, n->pendingSeq, n->pendingDelay);
		n->pendingSeq = NET_NO_SEQ;
	}
	const SeqDef* action = n->seqId != -1 && n->seqDelay == 0 ? anim_get_seq(n->seqId) : NULL;
	float wantX = n->x + half, wantY = n->y + half;
	float dx = wantX - n->viewX, dy = wantY - n->viewY, distance = sqrtf(dx * dx + dy * dy);
	bool fast = n->running || distance > 2.2f, placed = n->placed, moving;
	if (self) {
		moving = distance > 0.02f;
	} else {
		// Some actions hold someone still until they finish.
		bool held = action && action->movetype == 0;
		float step = (fast ? NPC_WALK_SPEED * 2.0f : NPC_WALK_SPEED) * seconds;
		if (!placed || distance > 8.0f || (!held && distance <= step)) {
			n->viewX = wantX;
			n->viewY = wantY;
		} else if (!held) {
			n->viewX += dx / distance * step;
			n->viewY += dy / distance * step;
		}
		moving = n->viewX != wantX || n->viewY != wantY;
	}
	if (!placed) n->viewAngle = speed == 0 ? 0 : (float)n->angle;
	n->placed = 1;
	for (int c = 0; c < cycles; c++) {
		n->moveSeq = movementSeq(n, bas, moving, fast);
		tickFacing(n, bas, speed, moving);
		tickAnimations(n);
	}
}

static void poseEntity(NetNpc* n, const NpcModel* m, const BasDef* bas) {
	const SeqDef* action = n->seqId != -1 && n->seqDelay == 0 ? anim_get_seq(n->seqId) : NULL;
	const SeqDef* motion = n->moveSeq == -1 || (n->moveSeq == bas->idle && action) ? NULL : anim_get_seq(n->moveSeq);
	const u8* pose = npcPose(m, motion, n->moveFrame, action, n->seqFrame);
	if (pose) {
		n->pose = pose;
		n->poseKey = m->key;
	}
}

// The server names worn items by position in a list of every wearable item.
static int32_t* wearIds = NULL;
static int wearCount = 0;

static void lookOf(const NetLook* l, const SeqDef* action, PlayerLook* out) {
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < 12; i++) {
		int v = l->slots[i];
		if (v >= 32768) out->identikit[i] = v - 32768 < wearCount ? wearIds[v - 32768] | 0x40000000 : 0;
		else if (v != 0) out->identikit[i] = (int32_t)((u32)(v - 256) | 0x80000000u);
	}
	// Some actions show a different item in the hands while they play.
	if (action && action->mainhand >= 0) out->identikit[5] = action->mainhand == 65535 ? 0 : action->mainhand | 0x40000000;
	if (action && action->offhand >= 0) out->identikit[3] = action->offhand == 65535 ? 0 : action->offhand | 0x40000000;
	memcpy(out->colours, l->colours, 5);
	out->female = l->female;
	out->basId = l->basId;
}

static u64 lookKey(const PlayerLook* look) {
	u64 h = 0xCBF29CE484222325ULL;
	const u8* p = (const u8*)look;
	for (size_t i = 0; i < sizeof(*look); i++) h = (h ^ p[i]) * 0x100000001B3ULL;
	return h | 1ULL << 63;
}

#define PLAYER_BUILD_GAP_MS 100

// Building a player reads and merges many models, so they are spaced out to keep the game moving.
static void updatePlayer(NetNpc* n, const NetLook* l, float seconds, int cycles, bool self, bool* built) {
	static long lastBuild = 0;
	if (!l->valid) {
		advanceEntity(n, anim_get_bas(-1), 0.5f, 32, seconds, cycles, self);
		n->hasModel = 0;
		return;
	}
	const BasDef* bas = anim_get_bas(l->basId);
	advanceEntity(n, bas, l->size * 0.5f, 32, seconds, cycles, self);
	const NpcDef* disguise = l->npcId != -1 ? npcShown(l->npcId) : NULL;
	const SeqDef* action = n->seqId != -1 && n->seqDelay == 0 ? anim_get_seq(n->seqId) : NULL;
	PlayerLook look;
	u64 key;
	if (disguise) {
		key = (u64)disguise->id;
	} else {
		lookOf(l, action, &look);
		key = lookKey(&look);
	}
	const NpcModel* m = findNpcModel(key);
	if (!m && !*built && (self || (u32)nowMs() - (u32)lastBuild >= PLAYER_BUILD_GAP_MS)) {
		buildBody(key, disguise, disguise ? NULL : &look, NULL);
		*built = true;
		lastBuild = nowMs();
		m = findNpcModel(key);
	}
	if (!m || !m->indexCount) return; // keeps showing the last model until the new one is ready
	n->hasModel = 1;
	n->modelKey = key;
	poseEntity(n, m, bas);
}

// At most one new model and one animation are read from the SD card per frame, to avoid a long stall.
static void updateNpcs(double frameMs) {
	float seconds = (float)(frameMs / 1000.0);
	bool built = false;
	npcLoadBudget = 1;
	npcCycleMs += frameMs;
	int cycles = (int)(npcCycleMs / CLIENT_CYCLE_MS);
	npcCycleMs -= cycles * CLIENT_CYCLE_MS;
	if (cycles > 10) cycles = 10;

	net.self.viewX = playerViewX;
	net.self.viewY = -playerViewZ;
	net.self.placed = 1;
	updatePlayer(&net.self, &net.looks[NET_SELF_ID], seconds, cycles, true, &built);

	for (int i = 0; i < net.npcCount; i++) {
		NetNpc* n = &net.npcs[i];
		const NpcDef* d = npcShown(n->type);
		const BasDef* bas = anim_get_bas(d ? d->basId : -1);
		advanceEntity(n, bas, (d ? d->size : 1) * 0.5f, d ? d->rotationSpeed : 32, seconds, cycles, false);
		n->hasModel = 0;
		if (!d) continue;
		if (!built && !findNpcModel((u64)d->id)) {
			buildBody((u64)d->id, d, NULL, NULL);
			built = true;
		}
		const NpcModel* m = findNpcModel((u64)d->id);
		if (!m || !m->indexCount) continue;
		n->hasModel = 1;
		n->modelKey = (u64)d->id;
		poseEntity(n, m, bas);
	}

	for (int i = 0; i < net.playerCount; i++) updatePlayer(&net.players[i], &net.looks[net.players[i].index], seconds, cycles, false, &built);
	updateGround(&built);
}

static int selfCombat(void) {
	return net.looks[NET_SELF_ID].combat;
}

static bool sphereVisible(float x, float y, float z, float r) {
	float dx = x - viewTest.eyeX, dy = y - viewTest.eyeY, dz = z - viewTest.eyeZ;
	float depth = dx * viewTest.fX + dy * viewTest.fY + dz * viewTest.fZ;
	float across = fabsf(dx * viewTest.rX + dz * viewTest.rZ);
	float upward = fabsf(dx * viewTest.uX + dy * viewTest.uY + dz * viewTest.uZ);
	return depth >= 0.5f - r && across - depth * viewTest.tanH <= r * viewTest.kH && upward - depth * viewTest.tanV <= r * viewTest.kV;
}

static void drawModelAt(const NpcModel* m, const void* pose, float x, float y, float angleUnits, const C3D_Mtx* mvp, float limit) {
	if (!m->indexCount || C3D_GetCmdBufUsage() > CMDBUF_LIMIT) return;
	float px = x - targetX, pz = -y - targetZ;
	if (sqrtf(px * px + pz * pz) > limit) return;
	float ground = groundHeight(net.plane, x, y);
	// Animation can reach beyond the resting shape.
	float top = m->top / 128.0f, reach = m->radius / 128.0f * 1.5f;
	if (!sphereVisible(x, ground + top * 0.5f, -y, sqrtf(reach * reach + top * top * 0.25f))) return;

	// Same rotation as the client: x' = x cos + z sin, z' = z cos - x sin.
	float angle = angleUnits * (2.0f * (float)M_PI / 2048.0f), s = sinf(angle), c = cosf(angle);
	C3D_Mtx place, out;
	Mtx_Identity(&place);
	place.r[0].x = c;
	place.r[0].z = s;
	place.r[0].w = x * 128.0f;
	place.r[1].w = -ground * 128.0f;
	place.r[2].x = -s;
	place.r[2].z = c;
	place.r[2].w = y * 128.0f;
	Mtx_Multiply(&out, mvp, &place);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_mvp, &out);
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, pose, NPC_POSITION_SIZE, 1, 0x0);
	BufInfo_Add(buf, m->colours, 4, 1, 0x1);
	C3D_DrawElements(GPU_TRIANGLES, m->indexCount, C3D_UNSIGNED_SHORT, m->indices);
	trisSubmitted += m->indexCount / 3;
	drawCalls++;
	npcsDrawn++;
}

static void drawEntity(const NetNpc* n, const C3D_Mtx* mvp, float limit) {
	const NpcModel* m = n->hasModel ? findNpcModel(n->modelKey) : NULL;
	if (!m || !n->placed) return;
	drawModelAt(m, n->pose && n->poseKey == m->key ? n->pose : m->restPose, n->viewX, n->viewY, n->viewAngle, mvp, limit);
}

static void drawNpcs(const C3D_Mtx* mvp) {
#ifdef RS3DS_NO_MODELS
	return;
#endif
	npcsDrawn = 0;
	if (!online) return;
	float limit = (float)RADII[radiusIndex];
	for (int i = 0; i < net.npcCount; i++) drawEntity(&net.npcs[i], mvp, limit);
	for (int i = 0; i < net.playerCount; i++) drawEntity(&net.players[i], mvp, limit);
	if (!selfHidden) drawEntity(&net.self, mvp, limit);
	drawGroundItems(mvp, limit);
}

// Loads the list of wearable items, building it on the first run since that reads every item.
static void prepareWearTable(void) {
	const char* path = AUTOTEST_DIR "wear.bin";
	scene.cache = &world.cache;
	scene.types = &world.types;
	anim_init(&world.cache);
	if (js5_load_index(&world.cache, 19) != 0) return;
	u32 stamp = 0;
	const Js5Index* index = &world.cache.index[19];
	for (u32 i = 0; i < index->capacity; i++) stamp = stamp * 31 + index->groups[i].crc;
	FILE* f = fopen(path, "rb");
	if (f) {
		u32 saved = 0, count = 0;
		if (fread(&saved, 4, 1, f) == 1 && fread(&count, 4, 1, f) == 1 && saved == stamp && count > 0 && count < 65536) {
			wearIds = malloc(count * 4);
			if (wearIds && fread(wearIds, 4, count, f) == count) wearCount = (int)count;
		}
		fclose(f);
		if (wearCount) return;
		free(wearIds);
		wearIds = NULL;
	}
	showMessage("Preparing items (first run only)...");
	int count = scene_wear_table(&scene, &wearIds);
	if (count <= 0) return;
	wearCount = count;
	f = fopen(path, "wb");
	if (!f) return;
	u32 size = (u32)count;
	fwrite(&stamp, 4, 1, f);
	fwrite(&size, 4, 1, f);
	fwrite(wearIds, 4, size, f);
	fclose(f);
}
