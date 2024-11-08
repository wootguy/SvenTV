#include "main.h"
#include "DemoWriter.h"
#include "DemoPlayer.h"
#include "CAmbientGeneric.h"
#include "lzma.h"
#include "SvenTV.h"

using namespace std;

DemoWriter::DemoWriter() {
	fileplayerinfos = new DemoPlayerEnt[32];
	fileedicts = new netedict[MAX_EDICTS];

	memset(fileedicts, 0, MAX_EDICTS * sizeof(netedict));
	memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayerEnt));

	// size of full delta on every edict + byte for each index delta + 2 bytes for each delta bits on edict
	int fullDeltaMaxSize = sizeof(netedict) + 4;
	int indexBytes = 1; // 2 for full index writes but on a max size update there will be no 255+ edict gaps
	int headerBytes = 1; // 1 for first full index
	fileDeltaBufferSize = headerBytes + (fullDeltaMaxSize + indexBytes) * MAX_EDICTS;
	fileDeltaBuffer = new char[fileDeltaBufferSize];

	// size of full delta on every player info
	filePlayerInfoBufferSize = sizeof(DemoPlayerEnt) * 32 + 2;
	filePlayerInfoBuffer = new char[filePlayerInfoBufferSize];

	netmessagesBufferSize = sizeof(NetMessageData) * MAX_NETMSG_FRAME + 2;
	netmessagesBuffer = new char[netmessagesBufferSize];

	cmdsBufferSize = sizeof(CommandData) * MAX_CMD_FRAME + 2;
	cmdsBuffer = new char[cmdsBufferSize];

	eventsBufferSize = sizeof(DemoEventData) * MAX_EVENT_FRAME + 2;
	eventsBuffer = new char[eventsBufferSize];
}

DemoWriter::~DemoWriter() {
	delete[] fileedicts;
	delete[] fileDeltaBuffer;
	delete[] fileplayerinfos;
	delete[] netmessagesBuffer;
	delete[] cmdsBuffer;
	delete[] eventsBuffer;

	closeDemoFile();

	if (compress_thread) {
		ALERT(at_console, "Waiting for demo compression to finish...\n", 0);
		compress_thread->join();
		delete compress_thread;
	}
}

void DemoWriter::initDemoFile() {
	bool autoMode = g_auto_demo_file->value;

	fpath = "";
	std::string rootPath = g_demo_file_path->string;

	if (!folderExists(rootPath)) {
		createFolder(rootPath);
		if (!folderExists(rootPath)) {
			ALERT(at_error, "Failed to create demo folder: %s\n", rootPath.c_str());
			g_can_autostart_demo = false;
			g_sventv->enableDemoFile = false;
			return;
		}
	}

	if (autoMode) {
		uint64_t bytesLeft = getFreeSpace(rootPath);
		uint32_t megaBytesLeft = (uint32_t)(bytesLeft / (1024ULL * 1024ULL));

		if (megaBytesLeft < g_min_storage_megabytes->value) {
			ALERT(at_error, "Not enough disk space to begin recording (%u < %d)\n",
				megaBytesLeft, (int)g_min_storage_megabytes->value);
			g_can_autostart_demo = false;
			g_sventv->enableDemoFile = false;
			return;
		}

		time_t rawtime;
		struct tm* timeinfo;
		char bufferday[80];

		time(&rawtime);
		timeinfo = localtime(&rawtime);
		strftime(bufferday, sizeof(bufferday), "%Y-%m-%d", timeinfo);
		
		std::string dayPath = rootPath + std::string(bufferday) + "/";
		if (!folderExists(dayPath)) {
			createFolder(dayPath);
			if (!folderExists(dayPath)) {
				ALERT(at_error, "Failed to create demo folder: %s\n", dayPath.c_str());
				g_can_autostart_demo = false;
				g_sventv->enableDemoFile = false;
				return;
			}
		}

		int suffix = 2;
		fpath = dayPath + STRING(gpGlobals->mapname) + ".demo";

		while (fileExists(fpath.c_str()) || fileExists((fpath + ".xz").c_str())) {
			fpath = dayPath + STRING(gpGlobals->mapname) + "__" + to_string(suffix++) + ".demo";
		}
	}
	else {
		fpath = rootPath + string(STRING(gpGlobals->mapname)) + ".demo";
	}

	g_engfuncs.pfnServerPrint(UTIL_VarArgs("Open demo file: %s\n", fpath.c_str()));

	demoFile = fopen(fpath.c_str(), "wb+");

	if (!demoFile) {
		ALERT(at_error, "Failed to open demo file: %s\n", fpath.c_str());
		g_can_autostart_demo = false;
		g_sventv->enableDemoFile = false;
		return;
	}

	memset(fileedicts, 0, MAX_EDICTS * sizeof(netedict));
	memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayerEnt));

	fileDeltaBuffer = new char[fileDeltaBufferSize];

	uint64_t now = getEpochMillis();

	DemoHeader header;
	strcpy_safe(header.mapname, STRING(gpGlobals->mapname), 64);
	header.maxPlayers = gpGlobals->maxClients;
	header.startTime = now;
	header.endTime = 0; // will indicate server crash if stays 0
	header.version = DEMO_VERSION;

	nextDemoKeyframe = 0;
	demoStartTime = now;
	lastDemoFrameTime = now;

	const int max_indexes = 8192;
	string modelData;
	uint16_t modelCount = 0;
	uint16_t soundCount = 0;
	uint16_t* modelIndexes = new uint16_t[max_indexes];
	uint16_t* soundIndexes = new uint16_t[max_indexes];
	for (const string& item : g_precachedModels) {
		if (item.empty()) {
			continue;
		}
		modelData += item + "\n";
		modelIndexes[modelCount++] = MODEL_INDEX(item.c_str());
	}

	string soundData;
	for (const std::string& sound : g_precachedSounds) {
		soundData += sound + "\n";
		soundIndexes[soundCount++] = SOUND_INDEX(sound.c_str());
	}

	header.modelCount = modelCount;
	header.soundCount = soundCount;
	header.modelLen = modelData.size();
	header.soundLen = soundData.size();
	header.stringPoolSize = STRING_POOL_SIZE;

	memset(&g_stats, 0, sizeof(DemoStats));
	g_stats.totalWriteSz = sizeof(DemoHeader) + header.modelLen + header.soundLen;

	fwrite(&header, sizeof(DemoHeader), 1, demoFile);

	stringPoolOffset = ftell(demoFile);
	fwrite(g_stringpool, STRING_POOL_SIZE, 1, demoFile); // empty for now

	if (modelData.size()) {
		fwrite(modelIndexes, modelCount*sizeof(uint16_t), 1, demoFile);
		fwrite(modelData.c_str(), modelData.size(), 1, demoFile);
	}
	if (soundData.size()) {
		fwrite(soundIndexes, soundCount * sizeof(uint16_t), 1, demoFile);
		fwrite(soundData.c_str(), soundData.size(), 1, demoFile);
	}

	delete[] modelIndexes;
	delete[] soundIndexes;
}

mstream DemoWriter::writeEntDeltas(FrameData& frame, uint16_t& numEntDeltas, DemoDataTest* testData) {
	mstream entbuffer(fileDeltaBuffer, fileDeltaBufferSize);

	uint16_t offset = 0; // always write full index for first entity delta

	numEntDeltas = 0;
	uint32_t indexWriteSz = 0;
	for (uint16_t i = 0; i < MAX_EDICTS; i++) {
		netedict& now = frame.netedicts[i];

		uint64_t startOffset = entbuffer.tellBits();
		uint8_t indexBits = 0;

		if (offset >= 256) {
			entbuffer.writeBits(3, 2);
			entbuffer.writeBits(i, ENTINDEX_BITS);
			indexBits = ENTINDEX_BITS + 2;
		}
		else if (offset >= 16) {
			entbuffer.writeBits(2, 2);
			entbuffer.writeBits(offset, 8);
			indexBits = 8 + 2;
		}
		else if (offset >= 2 || offset != 1) {
			entbuffer.writeBits(1, 2);
			entbuffer.writeBits(offset, 4);
			indexBits = 4 + 2;
		}
		else {
			entbuffer.writeBits(0, 2);
			indexBits = 2;
		}

		int ret = now.writeDeltas(entbuffer, fileedicts[i]);

		if (ret == EDELTA_OVERFLOW) {
			ALERT(at_console, "ERROR: Demo file entity delta buffer overflowed. Use a bigger buffer! The demo file is now broken\n");
			break;
		}
		else if (ret == EDELTA_NONE) {
			// no differences
			entbuffer.seekBits(startOffset); // undo index write
			offset += 1;
		}
		else {
			// delta written
			//ALERT(at_console, "Write edict %d offset %d bytes %d\n", i, (int)offset, (int)(entbuffer.tell() - startOffset));
			indexWriteSz += indexBits;
			offset = 1;
			numEntDeltas++;
			g_stats.entUpdateCount++;

			if (testData) {
				int sz = entbuffer.tellBits() - startOffset;
				testData->entDeltaSz[i] = sz;
			}
		}
	}

	entbuffer.endBitWriting();

	g_stats.entIndexTotalSz += indexWriteSz;

	return entbuffer;
}

mstream DemoWriter::writePlrDeltas(FrameData& frame, uint32_t& plrDeltaBits) {
	plrDeltaBits = 0;
	mstream plrbuffer(filePlayerInfoBuffer, filePlayerInfoBufferSize);
	for (int i = 0; i < gpGlobals->maxClients; i++) {
		int ret = frame.playerinfos[i].writeDeltas(plrbuffer, fileplayerinfos[i]);

		if (ret == EDELTA_OVERFLOW) {
			ALERT(at_console, "ERROR: Demo file player delta buffer overflowed. Use a bigger buffer! The demo file is now broken\n");
			break;
		}
		else if (ret == EDELTA_NONE) {
			// no differences
		}
		else {
			// delta written
			plrDeltaBits |= 1 << i;
		}
	}

	return plrbuffer;
}

void DemoWriter::compressNetMessage(FrameData& frame, NetMessageData& msg) {
	if (!g_compressMessages) {
		return;
	}
	
	switch (msg.header.type) {
	case SVC_TEMPENTITY: {
		uint8_t type = msg.data[0];

		switch (type) {
		case TE_BEAMPOINTS:
		case TE_BEAMDISK:
		case TE_BEAMCYLINDER:
		case TE_BEAMTORUS:
		case TE_LIGHTNING:
		case TE_BEAMSPRITE:
		case TE_SPRITETRAIL:
		case TE_BUBBLETRAIL:
		case TE_SPRITE_SPRAY:
		case TE_SPRAY:
		case TE_LINE:
		case TE_SHOWLINE:
		case TE_BOX:
		case TE_BLOODSTREAM:
		case TE_BLOOD:
		case TE_MODEL:
		case TE_PROJECTILE:
		case TE_TRACER:
		case TE_STREAK_SPLASH:
		case TE_USERTRACER:
			msg.compressCoords(1, 6);
			break;
		case TE_BEAMENTPOINT:
			msg.compressCoords(3, 3);
			break;
		case TE_EXPLOSION:
		case TE_SMOKE:
		case TE_SPARKS:
		case TE_SPRITE:
		case TE_GLOWSPRITE:
		case TE_ARMOR_RICOCHET:
		case TE_DLIGHT:
		case TE_LARGEFUNNEL:
		case TE_BLOODSPRITE:
		case TE_FIREFIELD:
		case TE_GUNSHOT:
		case TE_TAREXPLOSION:
		case TE_EXPLOSION2:
		case TE_PARTICLEBURST:
		case TE_LAVASPLASH:
		case TE_TELEPORT:
		case TE_IMPLOSION:
		case TE_DECAL:
		case TE_GUNSHOTDECAL:
		case TE_DECALHIGH:
		case TE_WORLDDECAL:
		case TE_WORLDDECALHIGH:
		case TE_BSPDECAL:
			msg.compressCoords(1, 3);
			break;
		case TE_ELIGHT:
			msg.compressCoords(23, 1);
			msg.compressCoords(3, 4);
			break;
		case TE_PLAYERATTACHMENT:
			msg.compressCoords(2, 1, true);
			break;
		case TE_BUBBLES:
			msg.compressCoords(1, 7);
			break;
		case TE_EXPLODEMODEL:
			msg.compressCoords(1, 4);
			break;
		case TE_BREAKMODEL:
			msg.compressCoords(1, 9);
			break;
		case TE_PLAYERDECAL:
			msg.compressCoords(2, 3);
			break;
		case TE_MULTIGUNSHOT:
			msg.compressCoords(25, 2);
			msg.compressCoords(13, 3, true);
			msg.compressCoords(1, 3);			
			break;
		default:
			break;
		}
		break;
	}
#ifndef HLCOOP_BUILD
	case MSG_TracerDecal: {
		msg.compressCoords(0, 6);
		break;
	}
	case MSG_StartSound: {
		uint16_t flags = *(uint16_t*)msg.data;
		if ((flags & SND_ORIGIN) == 0) {
			return;
		}

		int oriOffset = 2;
		if (flags & SND_ENT) {
			oriOffset += 2;
		}
		if (flags & SND_VOLUME) {
			oriOffset += 1;
		}
		if (flags & SND_PITCH) {
			oriOffset += 1;
		}
		if (flags & SND_ATTENUATION) {
			oriOffset += 1;
		}

		// WIP: origin delete code
		// this isn't reliable because entities can be created/destroyed/invisible at any frame,
		// so the client might not know which ent the index is referring to. I guess that's
		// why an origin is written in addition to an entity attachment. It's uncommon for
		// errors to happen so I think it's worth doing this anyway. The savings are significant.
		if ((flags & SND_ENT)) {
			uint16_t entidx = *(uint16_t*)(msg.data + 2);
			if (entidx == 0) {
				// why use the world as an attachment? That will always be 0,0,0
				// So, delete the ent index
				byte* entPtr = (byte*)(msg.data + 2);
				int moveSz = msg.sz - (2 + sizeof(uint16_t));
				memmove(entPtr, entPtr + sizeof(uint16_t), moveSz);
				msg.sz -= 2;
				*(uint16_t*)msg.data = flags & ~SND_ENT;
			}
			else if (entidx < MAX_EDICTS) {
				netedict& ed = frame.netedicts[entidx];
				uint16_t soundIdx = *(uint16_t*)(msg.data + (msg.sz - 2));

				if ((ed.effects & EF_NODRAW) == 0 && ed.modelindex
					&& g_indexToModel.find(ed.modelindex) != g_indexToModel.end()
					&& g_indexToModel[ed.modelindex][0] != '*') {
					// no need for both an entity attachment and origin if the client knows where the ent is.
					// So, delete the origin from the message.
					// BSP models need origins because clients don't know mins/maxs (though that can be guessed)
					byte* originPtr = (byte*)(msg.data + oriOffset);
					int oldOriginSz = sizeof(int32_t) * 3;
					int moveSz = msg.sz - (oriOffset + oldOriginSz);
					memmove(originPtr, originPtr + oldOriginSz, moveSz);
					msg.sz -= oldOriginSz;
					*(uint16_t*)msg.data = flags & ~SND_ORIGIN;
					return;
				}
			}
		}

		// chop fractional part of origin off. No one will notice 1-unit differences in sound origins
		msg.compressCoords(oriOffset, 3);
		break;
	}
#endif
	default:
		break;
	}
}

mstream DemoWriter::writeMsgDeltas(FrameData& frame, DemoDataTest* testData) {
	mstream msgbuffer(netmessagesBuffer, netmessagesBufferSize);
	for (int i = 0; i < (int)frame.netmessage_count; i++) {
		NetMessageData& dat = frame.netmessages[i];

		if (testData) {
			if (!dat.header.hasOrigin) {
				memset(dat.origin, 0, 3 * sizeof(uint32_t));
			}
			memcpy(&testData->expectedMsg[i], &dat, sizeof(NetMessageData));
		}

		compressNetMessage(frame, dat);

		dat.header.sz = dat.sz & 0xff;
		dat.header.szHighBit = (dat.sz & 0x100) != 0;

		if (testData) {
			testData->expectedMsg[i].header.sz = dat.header.sz;
			testData->expectedMsg[i].header.szHighBit = dat.header.szHighBit;
			testData->expectedMsg[i].sz = (dat.header.szHighBit << 8) | dat.header.sz;
		}

		if (dat.header.type == SVC_BAD)
			ALERT(at_console, "Wrote SVC_BAD!!\n", 0);

		msgbuffer.write(&dat.header, sizeof(DemoNetMessage));

		g_stats.msgSz[dat.header.type] += dat.sz;
		if (dat.header.type == SVC_TEMPENTITY) {
			g_stats.msgSz[256 + dat.data[0]] += dat.sz;
		}

		if (dat.header.hasOrigin) {
			int sz = dat.header.hasLongOrigin ? 3 : 2;
			msgbuffer.write(&dat.origin[0], sz);
			msgbuffer.write(&dat.origin[1], sz);
			msgbuffer.write(&dat.origin[2], sz);
		}

		if (dat.header.hasEdict) {
			msgbuffer.write(&dat.eidx, sizeof(uint16_t));
		}
		msgbuffer.write(dat.data, dat.sz);
	}
	if (msgbuffer.eom()) {
		ALERT(at_console, "ERROR: Demo file network message buffer overflowed (%d > %d). Use a bigger buffer!\n", frame.netmessage_count, MAX_NETMSG_FRAME);
	}

	return msgbuffer;
}

mstream DemoWriter::writeCmdDeltas(FrameData& frame) {
	mstream cmdbuffer(cmdsBuffer, cmdsBufferSize);
	for (int i = 0; i < (int)frame.cmds_count; i++) {
		CommandData& dat = frame.cmds[i];
		DemoCommand cmd;

		cmd.idx = dat.idx;
		cmd.len = dat.len;
		cmdbuffer.write(&cmd, sizeof(DemoCommand));
		cmdbuffer.write(dat.data, dat.len);
	}
	if (cmdbuffer.eom()) {
		ALERT(at_console, "ERROR: Demo file command buffer overflowed. Use a bigger buffer!\n");
	}

	return cmdbuffer;
}

mstream DemoWriter::writeEvtDeltas(FrameData& frame) {
	mstream evbuffer(eventsBuffer, eventsBufferSize);
	for (int i = 0; i < (int)frame.event_count; i++) {
		DemoEventData& dat = frame.events[i];
		evbuffer.write(&dat.header, sizeof(DemoEvent));

		if (dat.header.hasOrigin) {
			evbuffer.write(&dat.origin[0], 3);
			evbuffer.write(&dat.origin[1], 3);
			evbuffer.write(&dat.origin[2], 3);
		}
		if (dat.header.hasAngles) {
			evbuffer.write(&dat.angles[0], 2);
			evbuffer.write(&dat.angles[1], 2);
			evbuffer.write(&dat.angles[2], 2);
		}
		if (dat.header.hasFparam1) {
			evbuffer.write(&dat.fparam1, 3);
		}
		if (dat.header.hasFparam2) {
			evbuffer.write(&dat.fparam2, 3);
		}
		if (dat.header.hasIparam1) {
			evbuffer.write(&dat.iparam1, 2);
		}
		if (dat.header.hasIparam2) {
			evbuffer.write(&dat.iparam2, 2);
		}
	}
	if (evbuffer.eom()) {
		ALERT(at_console, "ERROR: Demo file event buffer overflowed (%d > %d). Use a bigger buffer!\n", frame.event_count, MAX_EVENT_FRAME);
	}

	return evbuffer;
}

#define ASSERT_FRAME(desc, expect, actual) \
	if (expect != actual) { ALERT(at_console, "Read unexpected %s! %d != %d\n", desc, expect, actual); valid = false; }

void initAmbientSounds() {
	edict_t* ent = NULL;

	while (!FNullEnt(ent = FIND_ENTITY_BY_CLASSNAME(ent, "ambient_generic"))) {
		CAmbientGeneric* ambient = (CAmbientGeneric*)GET_PRIVATE(ent);
		if (ambient && ambient->m_isWav && ambient->m_fActive && ambient->m_fLooping) {
			std::string ambientSound = toLowerCase(STRING(ambient->pev->message));
			if (!g_precachedSounds.count(ambientSound)) {
				ALERT(at_console, "Ambient using sound which isn't precached: %s\n", ambientSound.c_str());
				continue;
			}

			MessageBegin(MSG_BROADCAST, SVC_SPAWNSTATICSOUND, NULL, NULL);
			WriteCoord(ambient->pev->origin[0]);
			WriteCoord(ambient->pev->origin[1]);
			WriteCoord(ambient->pev->origin[2]);

			WriteShort(SOUND_INDEX(STRING(ambient->pev->message)));
			WriteByte(ambient->m_dpv.vol * 255.0);
			WriteByte(ambient->m_flAttenuation * 64.0);
			WriteShort(ambient->entindex());
			WriteByte(ambient->m_dpv.pitch);
			WriteByte(SND_CHANGE_VOL);
			MessageEnd();
		}
	}

	ent = NULL;
	while (!FNullEnt(ent = FIND_ENTITY_BY_CLASSNAME(ent, "ambient_music"))) {
		CAmbientGeneric* ambient = (CAmbientGeneric*)GET_PRIVATE(ent);
		if (ambient) {
			std::string ambientSound = toLowerCase(STRING(ambient->pev->message));
			if (!g_precachedSounds.count(ambientSound)) {
				ALERT(at_console, "Ambient using sound which isn't precached: %s\n", ambientSound.c_str());
				continue;
			}

			MessageBegin(MSG_BROADCAST, SVC_SPAWNSTATICSOUND, NULL, NULL);
			WriteCoord(ambient->pev->origin[0]);
			WriteCoord(ambient->pev->origin[1]);
			WriteCoord(ambient->pev->origin[2]);

			WriteShort(SOUND_INDEX(STRING(ambient->pev->message)));
			WriteByte(ambient->m_dpv.vol * 255.0);
			WriteByte(ambient->m_flAttenuation * 64.0);
			WriteShort(ambient->entindex());
			WriteByte(ambient->m_dpv.pitch);
			WriteByte(SND_CHANGE_VOL);
			MessageEnd();
		}
	}

	if (g_mp3Command.size()) {
		// start global music
		MessageBegin(MSG_BROADCAST, SVC_STUFFTEXT, NULL, NULL);
		WriteString(g_mp3Command.c_str());
		MessageEnd();
	}
}

bool DemoWriter::shouldWriteDemoFrame() {
	uint64_t now = getEpochMillis();
	if (now < nextDemoUpdate) {
		return false;
	}
	return true;
}

bool DemoWriter::writeDemoFile(FrameData& frame) {
	if (!demoFile) {
		initDemoFile();
	}
	if (!demoFile) {
		return false;
	}

	if (ftell(demoFile)> g_max_demo_megabytes->value * 1024*1024) {
		ALERT(at_error, "Max demo file size exceeded (%.2f MB)!\n", (float)(g_max_demo_megabytes->value*1024*1024));
		g_can_autostart_demo = false;
		if (g_sventv)
			g_sventv->enableDemoFile = false;
		closeDemoFile();
		return false;
	}

	if (!shouldWriteDemoFrame()) {
		return false;
	}

	uint64_t now = getEpochMillis();
	uint64_t updateDelay = (1.0f / demoFileFps) * 1000;
	uint32_t frameTimeDelta = now - lastDemoFrameTime;
	nextDemoUpdate = now + updateDelay;

	if (g_stats.frameCount == 5) {
		initAmbientSounds();
	}

	bool isKeyframe = now >= nextDemoKeyframe;

	if (isKeyframe) {
		nextDemoKeyframe = now + (1000ULL * KEYFRAME_INTERVAL);
		memset(fileedicts, 0, MAX_EDICTS * sizeof(netedict));
		memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayerEnt));
	}

	uint16_t numEntDeltas = 0;
	uint32_t plrDeltaBits = 0;

	bool validateOutput = g_validate_output->value;

	DemoDataTest* testData = NULL;
	DemoPlayer* testPlayer = NULL;

	if (validateOutput) {
		testData = new DemoDataTest;
		testPlayer = new DemoPlayer();
		memset(testData, 0, sizeof(DemoDataTest));
		memcpy(testData->oldEntState, fileedicts, sizeof(netedict) * MAX_EDICTS);
		memcpy(testPlayer->fileedicts, fileedicts, sizeof(netedict) * MAX_EDICTS);
	}

	mstream entbuffer = writeEntDeltas(frame, numEntDeltas, testData);
	mstream plrbuffer = writePlrDeltas(frame, plrDeltaBits);
	mstream msgbuffer = writeMsgDeltas(frame, testData);
	mstream cmdbuffer = writeCmdDeltas(frame);
	mstream evtbuffer = writeEvtDeltas(frame);

	memcpy(fileedicts, frame.netedicts, MAX_EDICTS * sizeof(netedict));
	memcpy(fileplayerinfos, frame.playerinfos, 32 * sizeof(DemoPlayerEnt));

	g_stats.entDeltaCurrentSz = entbuffer.tell() + (entbuffer.tell() ? 2 : 0);
	g_stats.plrDeltaCurrentSz = plrbuffer.tell() + (plrbuffer.tell() ? 4 : 0);
	g_stats.msgCurrentSz = msgbuffer.tell() + (msgbuffer.tell() ? 2 : 0);
	g_stats.cmdCurrentSz = cmdbuffer.tell() + (cmdbuffer.tell() ? 1 : 0);
	g_stats.eventCurrentSz = evtbuffer.tell() + (evtbuffer.tell() ? 1 : 0);
	g_stats.msgCount += frame.netmessage_count;
	g_stats.cmdCount += frame.cmds_count;
	g_stats.eventCount += frame.event_count;
	
	g_stats.calcFrameSize();

	uint8_t frameCountDelta = V_min(frame.serverFrameCount - lastServerFrameCount, 255);
	lastServerFrameCount = frame.serverFrameCount;

	DemoFrame header;
	header.deltaFrames = frameCountDelta;
	header.hasEntityDeltas = numEntDeltas > 0;
	header.hasNetworkMessages = frame.netmessage_count > 0;
	header.hasEvents = frame.event_count > 0;
	header.hasPlayerDeltas = plrDeltaBits > 0;
	header.hasCommands = frame.cmds_count > 0;
	header.isKeyFrame = isKeyframe;
	header.isGiantFrame = frameTimeDelta > 255 || g_stats.currentWriteSz > (65535-3) || isKeyframe;
	header.isBigFrame = frameTimeDelta > 255 || g_stats.currentWriteSz > (255 - 3) || isKeyframe;
	if (header.isGiantFrame) {
		header.isBigFrame = 0;
	}

	bool hasAnyDeltas = header.hasEntityDeltas + header.hasPlayerDeltas + header.hasNetworkMessages + header.hasEvents + header.hasCommands;
	if (!hasAnyDeltas) {
		if (validateOutput) {
			delete testData;
			delete testPlayer;
		}
		return true;
	}

	lastDemoFrameTime = now;

	size_t frameStartOffset = ftell(demoFile);
	uint32_t expectedDemoTime = 0;

	fwrite(&header, sizeof(DemoFrame), 1, demoFile);

	if (header.isGiantFrame) {
		g_stats.currentWriteSz += sizeof(uint32_t) * 2;
		uint32_t demoTime = now - demoStartTime;
		uint32_t frameSize = g_stats.currentWriteSz;
		expectedDemoTime = demoTime;
		fwrite(&demoTime, sizeof(uint32_t), 1, demoFile);
		fwrite(&frameSize, sizeof(uint32_t), 1, demoFile);
		g_stats.giantFrameCount++;
	}
	else if (header.isBigFrame) {
		g_stats.currentWriteSz += sizeof(uint16_t) + sizeof(uint8_t);
		uint8_t demoTimeDelta = frameTimeDelta;
		uint16_t frameSize = g_stats.currentWriteSz;
		expectedDemoTime = demoTimeDelta;
		fwrite(&demoTimeDelta, sizeof(uint8_t), 1, demoFile);
		fwrite(&frameSize, sizeof(uint16_t), 1, demoFile);
		g_stats.bigFrameCount++;
	}
	else {
		g_stats.currentWriteSz += sizeof(uint8_t) + sizeof(uint8_t);
		uint8_t demoTimeDelta = frameTimeDelta;
		uint16_t frameSize = g_stats.currentWriteSz;
		expectedDemoTime = demoTimeDelta;
		fwrite(&demoTimeDelta, sizeof(uint8_t), 1, demoFile);
		fwrite(&frameSize, sizeof(uint8_t), 1, demoFile);
	}

	g_stats.incTotals();

	if (header.hasEntityDeltas) {
		fwrite(&numEntDeltas, sizeof(uint16_t), 1, demoFile);
		fwrite(entbuffer.getBuffer(), entbuffer.tell(), 1, demoFile);
	}
	if (header.hasPlayerDeltas) {
		fwrite(&plrDeltaBits, sizeof(uint32_t), 1, demoFile);
		fwrite(plrbuffer.getBuffer(), plrbuffer.tell(), 1, demoFile);
	}
	if (header.hasNetworkMessages) {
		fwrite(&frame.netmessage_count, sizeof(uint16_t), 1, demoFile);
		fwrite(msgbuffer.getBuffer(), msgbuffer.tell(), 1, demoFile);
	}
	if (header.hasEvents) {
		fwrite(&frame.event_count, sizeof(uint8_t), 1, demoFile);
		fwrite(evtbuffer.getBuffer(), evtbuffer.tell(), 1, demoFile);
	}
	if (header.hasCommands) {
		fwrite(&frame.cmds_count, sizeof(uint8_t), 1, demoFile);
		fwrite(cmdbuffer.getBuffer(), cmdbuffer.tell(), 1, demoFile);
	}

	if (lastStringPoolIdx != g_stringpool_idx) {
		uint32_t currentOffset = ftell(demoFile);
		fseek(demoFile, stringPoolOffset, SEEK_SET);
		fwrite(g_stringpool, g_stringpool_idx, 1, demoFile);
		fseek(demoFile, currentOffset, SEEK_SET);
	}

	if (validateOutput) {
		size_t oldOffset = ftell(demoFile);
		
		uint32_t expectedEntSz = g_stats.entDeltaCurrentSz;
		uint32_t expectedPlrSz = g_stats.plrDeltaCurrentSz;
		uint32_t expectedNetSz = g_stats.msgCurrentSz;
		uint32_t expectedEvtSz = g_stats.eventCurrentSz;
		uint32_t expectedCmdSz = g_stats.cmdCurrentSz;
		uint32_t expectedFrameSz = g_stats.currentWriteSz;

		DemoStats oldStats = g_stats;
		memset(&g_stats, 0, sizeof(DemoStats));

		memcpy(testData->newEntState, frame.netedicts, sizeof(netedict) * MAX_EDICTS);

		DemoDataStream testStream(demoFile);
		testStream.seek(frameStartOffset);

		testPlayer->validateFrame(testStream, testData);
		g_stats.calcFrameSize();

		bool valid = testData->success;

		if (memcmp(&testData->header, &header, sizeof(DemoFrame))) {
			ALERT(at_console, "Read unexpected demo header!\n", 0);
		}

		ASSERT_FRAME("demo time delta", testData->demoTime, expectedDemoTime);

		ASSERT_FRAME("ent count", testData->numEntDeltas, numEntDeltas);
		ASSERT_FRAME("ent bytes", g_stats.entDeltaCurrentSz, expectedEntSz);

		ASSERT_FRAME("plr bits", testData->playerDeltaBits, plrDeltaBits);
		ASSERT_FRAME("plr bytes", g_stats.plrDeltaCurrentSz, expectedPlrSz);

		ASSERT_FRAME("msg count", testData->msgCount, frame.netmessage_count);
		ASSERT_FRAME("msg bytes", g_stats.msgCurrentSz, expectedNetSz);

		ASSERT_FRAME("evt count", testData->evtCount, frame.event_count);
		ASSERT_FRAME("evt bytes", g_stats.eventCurrentSz, expectedEvtSz);

		ASSERT_FRAME("evt count", testData->cmdCount, frame.cmds_count);
		ASSERT_FRAME("evt bytes", g_stats.cmdCurrentSz, expectedCmdSz);

		ASSERT_FRAME("frame bytes", g_stats.currentWriteSz, expectedFrameSz);

		if (!valid) {
			ALERT(at_console, "WROTE BAD FRAME %d!\n\n", oldStats.frameCount);
			memset(&g_stats, 0, sizeof(DemoStats));
			testStream.seek(frameStartOffset);
			testPlayer->validateFrame(testStream, testData); // now debug it!
		}

		testStream.seek(oldOffset);
		g_stats = oldStats;
		delete testData;
		delete testPlayer;
	}

	return true;
}

void DemoWriter::compressDemo(std::string inPath, std::string outPath) {
	if (lzmaCompress(inPath, outPath, 9)) {
		remove(inPath.c_str());
		g_engfuncs.pfnServerPrint(UTIL_VarArgs("Compressed demo file: %s\n", outPath.c_str()));
	}
	else {
		ALERT(at_error, "Failed to compress %s\n", inPath.c_str());
	}
}

void DemoWriter::closeDemoFile() {
	if (!demoFile) {
		return;
	}

	fseek(demoFile, offsetof(DemoHeader, endTime), SEEK_SET);
	fwrite(&lastDemoFrameTime, sizeof(uint64_t), 1, demoFile);
	fclose(demoFile);
	demoFile = NULL;

	g_engfuncs.pfnServerPrint(UTIL_VarArgs("Closed demo file: %s\n", fpath.c_str()));

	if (g_auto_demo_file->value && g_compress_demos->value && fileExists(fpath.c_str())) {
		if (compress_thread) {
			ALERT(at_console, "Waiting for previous compression to finish...\n", 0);
			compress_thread->join();
			delete compress_thread;
		}
		compress_thread = new thread(&DemoWriter::compressDemo, this, fpath, fpath + ".xz");
	}
}

bool DemoWriter::isFileOpen() {
	return demoFile != NULL;
}

bool DemoWriter::validateEdicts() {
	for (int i = 0; i < MAX_EDICTS; i++) {
		if (fileedicts[i].edflags && !(fileedicts[i].edflags & EDFLAG_BEAM) && fileedicts[i].aiment > 8192) {
			ALERT(at_console, "Invalid edict %d has %d\n", i, (int)fileedicts[i].aiment);
			return false;
		}
	}
	return true;
}