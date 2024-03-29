#pragma once
#include "meta_init.h"
#include "mstream.h"
#include "DemoFile.h"
#include "NetClient.h"

// TODO:
// - shocktrooper beam
// - gonome/hgrunt gait
// - apache bullets
// - sentence mouth controller (scientist)
// - some sounds have no attn? (gib sound)
// - player footsteps (+swimming effects?)
// - save cvars to demo
// - charging wrench triggers constant animation resets (frame bytes)
// - do monsters ever reset sequences to a non-zero frame? (undo frame opt)
// - punchangle prediction
// - controller interp
// - capture SetView calls
// - NetMessage compression is WIP and broke replays or isn't working I forget

// optimize ideas:
// - 1-2byte deltaidx for players
// - init framerate to 16
// - simpler startSound message
// - fewer startsound messages for apache
// - 1-2byte delta for health

struct InterpInfo {
	Vector originStart;
	Vector originEnd;

	Vector anglesStart;
	Vector anglesEnd;

	float frameStart;
	float frameEnd; // used as a reference point for monsters
	float interpFrame; // for resetting when predicted frame is close enough to the real one

	float framerateEnt; // framerate modifier set in the entity
	float framerateSmd; // framerate of the model animation
	float groundspeed; // movement speed of the model animation on the ground
	float animTime; // world time when frame was set
	float lastMovementTime; // last time origin/angles were changed
	float estimatedUpdateDelay; // used to guess a framerate for origin/angle interpolation

	int sequenceStart;
	int sequenceEnd;
	bool sequenceLoops;
};

struct ReplayEntity {
	EHandle h_ent;
	InterpInfo interp;
};

class DemoPlayer {
public:
	const float demoFileFps = 60; // TODO: calculate this or smth
	bool clearMapForPlayback = true; // map may crash if entities are not cleared first
	bool useBots = false; // try to use bots for player entites
	float replaySpeed = 1.0f;
	int netmsgPlrIdx = 1; // player to replay network messages for

	DemoPlayer();
	~DemoPlayer();

	// validates and prepares demo file for playback
	// plr = player who opened the file
	bool openDemo(edict_t* plr, string path, float offsetSeconds, bool skipPrecache = false);

	// call this every frame to replay the demo
	void playDemo();

	// call in MapInit after loading a demo file
	void precacheDemo();

	void stopReplay();

	edict_t* getReplayEntity(int idx);

private:
	// vars for replaying a demo file
	FILE* replayFile = NULL;
	vector<string> precacheModels;
	vector<string> precacheSounds;
	uint64_t replayStartTime = 0;
	uint32_t replayFrame = 0;
	uint32_t nextFrameOffset = 0;
	uint64_t nextFrameTime = 0;
	uint32_t lastFrameDemoTime = 0;
	vector<ReplayEntity> replayEnts;
	map<int, string> replayModelPath; // maps model index in demo file to a path
	DemoHeader demoHeader;
	DemoFrame lastReplayFrame;

	DemoPlayerEnt* fileplayerinfos = NULL; // last infos read from file
	netedict* fileedicts = NULL; // last edicts read from file

	float offsetSeconds;
	float frameProgress; // for interpolation (0-1 progress to next frame)

	string oldPlayerNames[MAX_PLAYERS]; // for resetting conflicted names after the replay finishes

	void closeReplayFile();

	// returns true if more frames are needed to catch up with current playback time
	bool readDemoFrame();

	bool simulate(DemoFrame& header); // create entities and replay the demo through them
	bool readEntDeltas(mstream& reader);
	bool readPlayerDeltas(mstream& reader);
	bool readNetworkMessages(mstream& reader);
	bool readEvents(mstream& reader);
	bool readClientCommands(mstream& reader);
	
	// converts a simulated entity into a class best suited for the demo entity
	// i = index into replayEntities
	edict_t* convertEdictType(edict_t* ent, int i);

	// creates new generic demo entities until reaching count
	// returns false on fatal error
	bool createReplayEntities(int count);

	// set new interpolation start/end points for the given entity
	// i = index into replayEntities
	void setupInterpolation(edict_t* ent, int i);

	void interpolateEdicts();

	void decompressNetMessage(NetMessageData& msg);

	// update network message data to point to the proper entities
	// return false if message should not be sent
	bool processDemoNetMessage(NetMessageData& msg);

	// really like 127 different messages
	bool processTempEntityMessage(NetMessageData& msg);

	// clears existing map entities for playback
	void prepareDemo();

	// ent = replay entitiy to update pitch/gait angles
	// dt = seconds between current time and last time
	void updatePlayerModelGait(edict_t* ent, float dt);

	void updatePlayerModelPitchBlend(edict_t* ent);

	// convert a model index read from a demo file into a model path
	string getReplayModel(uint16_t modelIdx);

	// convert from a demo file entity index to an entity index in the current game
	void convReplayEntIdx(uint16_t& eidx);

	// convert from a demo file model idx to a model idx in the current game
	void convReplayModelIdx(uint16_t& modelIdx);

	// convert from a demo file sound idx to a sound idx idx in the current game
	void convReplaySoundIdx(uint16_t& soundIdx);

	bool validateEdicts(); // debug
};