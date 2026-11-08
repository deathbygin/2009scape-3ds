// Animation data and skeletal posing, ported from the revision 530 client. Positions only:
// frames that change colour or transparency are not applied, and frames are not blended.
#ifndef ANIM_H
#define ANIM_H

#include "js5.h"

typedef struct {
	int id, frameCount;
	int32_t* frames; // frameset id << 16 | frame number
	int32_t* delays; // in 20 ms client cycles
	uint8_t* framegroup; // 256 entries, NULL if the sequence does not mask body parts
	int replayoff, priority, looptype, movetype, exactmove, replaycount;
	int mainhand, offhand; // an item shown instead while this plays, -1 if none, 65535 for empty
} SeqDef;

// The set of movement animations an NPC or player uses. -1 means none.
typedef struct {
	int idle, walk, walkTurn, walkCW, walkCCW, run, runTurn, runCW, runCCW, standCW, standCCW;
	unsigned transformsUsed; // bit i marks transforms[i] as used
	int16_t transforms[12][6];
} BasDef;

// Which vertices belong to each bone label.
typedef struct {
	int boneCount;
	uint32_t* start; // boneCount + 1 offsets into vertices
	uint16_t* vertices;
} BoneTable;

void anim_init(Js5Cache* cache);

// Definitions stay cached for the life of the program. A NULL result means the id is unknown.
const SeqDef* anim_get_seq(int id);
const BasDef* anim_get_bas(int id);

// Framesets are loaded separately because each one is a read from the SD card.
int anim_frameset_loaded(int id);
int anim_load_frameset(int id);
void anim_clear_framesets(void);

// True if everything needed to show this frame is in memory. Otherwise *missing is the frameset to load.
int anim_frame_ready(const SeqDef* seq, int frame, int* missing);

BoneTable* anim_build_bones(const int32_t* vertexBones, int vertexCount);
void anim_free_bones(BoneTable* bones);

// Moves vertices to a pose, as NpcType.getBodyModel does. Either sequence may be NULL.
// primary is the movement animation and secondary the action played over it.
void anim_apply_pose(const BoneTable* bones, int32_t* vx, int32_t* vy, int32_t* vz,
	const SeqDef* primary, int primaryFrame, const SeqDef* secondary, int secondaryFrame);

#endif
