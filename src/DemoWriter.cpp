#include "DemoWriter.h"
#include "main.h"

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
}

void DemoWriter::initDemoFile() {
	time_t rawtime;
	struct tm* timeinfo;
	char buffer[80];

	time(&rawtime);
	timeinfo = localtime(&rawtime);

	strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", timeinfo);
	std::string fname(buffer);
	//fname = fname + "_" + STRING(gpGlobals->mapname) + ".demo";
	fname = string(STRING(gpGlobals->mapname)) + ".demo";
	string fpath = g_demo_file_path->string + fname;
	println("Open demo file: %s", fpath.c_str());

	demoFile = fopen(fpath.c_str(), "wb");
	memset(fileedicts, 0, MAX_EDICTS * sizeof(netedict));
	memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayerEnt));

	fileDeltaBuffer = new char[fileDeltaBufferSize];

	uint64_t now = getEpochMillis();

	DemoHeader header;
	strncpy(header.mapname, STRING(gpGlobals->mapname), 64);
	header.maxPlayers = gpGlobals->maxClients;
	header.startTime = now;
	header.endTime = 0; // will indicate server crash if stays 0
	header.version = DEMO_VERSION;

	nextDemoKeyframe = 0;
	demoStartTime = now;
	lastDemoFrameTime = now;

	string modelData;
	int maxModelIdx = 0;
	int minModelIdx = 0;
	for (pair<int, string> item : g_indexToModel) {
		if (item.first > maxModelIdx) {
			maxModelIdx = item.first;
		}
	}
	for (int i = 0; i <= maxModelIdx; i++) {
		if (g_indexToModel[i].empty() || g_indexToModel[i].size() > 1 && g_indexToModel[i][0] == '*') {
			minModelIdx = i + 1;
			continue;
		}
		modelData += g_indexToModel[i] + "\n";
	}

	string soundData;
	for (int i = 0; i < g_SoundCacheFiles.size(); i++) {
		soundData += g_SoundCacheFiles[i] + "\n";
	}

	header.modelIdxStart = minModelIdx;
	header.modelLen = modelData.size();
	header.soundLen = soundData.size();

	memset(&g_stats, 0, sizeof(DemoStats));
	g_stats.totalWriteSz = sizeof(DemoHeader) + header.modelLen + header.soundLen;

	fwrite(&header, sizeof(DemoHeader), 1, demoFile);

	if (modelData.size())
		fwrite(modelData.c_str(), modelData.size(), 1, demoFile);
	if (soundData.size())
		fwrite(soundData.c_str(), soundData.size(), 1, demoFile);
}

mstream DemoWriter::writeEntDeltas(FrameData& frame, uint16_t& numEntDeltas) {
	mstream entbuffer(fileDeltaBuffer, fileDeltaBufferSize);

	uint16_t offset = 0; // always write full index for first entity delta

	numEntDeltas = 0;
	uint32_t indexWriteSz = 0;
	for (uint16_t i = 0; i < MAX_EDICTS; i++) {
		netedict& now = frame.netedicts[i];

		uint64_t startOffset = entbuffer.tell();

		if (offset > 127) {
			// last edict was 256+ slots away. Write full index.
			uint16_t writeIdx = (i << 1) | FL_ENTIDX_LONG;
			entbuffer.write(&writeIdx, 2);
		}
		else {
			uint8_t writeIdx = offset << 1;
			entbuffer.write(&writeIdx, 1); // entity index the delta is for (offset from previous)
		}

		int ret = now.writeDeltas(entbuffer, fileedicts[i]);

		if (ret == EDELTA_OVERFLOW) {
			println("ERROR: Demo file entity delta buffer overflowed. Use a bigger buffer! The demo file is now broken");
			break;
		}
		else if (ret == EDELTA_NONE) {
			// no differences
			entbuffer.seek(startOffset); // undo index write
			offset += 1;
		}
		else {
			// delta written
			//println("Write edict %d offset %d bytes %d", i, (int)offset, (int)(entbuffer.tell() - startOffset));
			indexWriteSz += offset > 127 ? 2 : 1;
			offset = 1;
			numEntDeltas++;
		}
	}

	g_stats.entIndexTotalSz += indexWriteSz;

	return entbuffer;
}

mstream DemoWriter::writePlrDeltas(FrameData& frame, uint32_t& plrDeltaBits) {
	plrDeltaBits = 0;
	mstream plrbuffer(filePlayerInfoBuffer, filePlayerInfoBufferSize);
	for (int i = 0; i < gpGlobals->maxClients; i++) {
		int ret = frame.playerinfos[i].writeDeltas(plrbuffer, fileplayerinfos[i]);

		if (ret == EDELTA_OVERFLOW) {
			println("ERROR: Demo file player delta buffer overflowed. Use a bigger buffer! The demo file is now broken");
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
	default:
		break;
	}
}

mstream DemoWriter::writeMsgDeltas(FrameData& frame) {
	mstream msgbuffer(netmessagesBuffer, netmessagesBufferSize);
	for (int i = 0; i < frame.netmessage_count; i++) {
		NetMessageData& dat = frame.netmessages[i];

		compressNetMessage(frame, dat);

		dat.header.sz = dat.sz & 0xff;
		dat.header.szHighBit = (dat.sz & 0x100) != 0;

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
		println("ERROR: Demo file network message buffer overflowed (%d > %d). Use a bigger buffer!", frame.netmessage_count, MAX_NETMSG_FRAME);
	}

	return msgbuffer;
}

mstream DemoWriter::writeCmdDeltas(FrameData& frame) {
	mstream cmdbuffer(cmdsBuffer, cmdsBufferSize);
	for (int i = 0; i < frame.cmds_count; i++) {
		CommandData& dat = frame.cmds[i];
		DemoCommand cmd;

		cmd.idx = dat.idx;
		cmd.len = dat.len;
		cmdbuffer.write(&cmd, sizeof(DemoCommand));
		cmdbuffer.write(dat.data, dat.len);
	}
	if (cmdbuffer.eom()) {
		println("ERROR: Demo file command buffer overflowed. Use a bigger buffer!");
	}

	return cmdbuffer;
}

mstream DemoWriter::writeEvtDeltas(FrameData& frame) {
	mstream evbuffer(eventsBuffer, eventsBufferSize);
	for (int i = 0; i < frame.event_count; i++) {
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
		println("ERROR: Demo file event buffer overflowed (%d > %d). Use a bigger buffer!", frame.event_count, MAX_EVENT_FRAME);
	}

	return evbuffer;
}

bool DemoWriter::writeDemoFile(FrameData& frame) {
	if (!demoFile) {
		initDemoFile();
	}

	uint64_t now = getEpochMillis();
	if (now < nextDemoUpdate) {
		return false;
	}
	uint64_t updateDelay = (1.0f / demoFileFps) * 1000;
	uint32_t frameTimeDelta = now - lastDemoFrameTime;
	nextDemoUpdate = now + updateDelay;

	bool isKeyframe = now >= nextDemoKeyframe;

	if (isKeyframe) {
		nextDemoKeyframe = now + (1000ULL * KEYFRAME_INTERVAL);
		memset(fileedicts, 0, MAX_EDICTS * sizeof(netedict));
		memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayerEnt));
	}

	uint16_t numEntDeltas = 0;
	uint32_t plrDeltaBits = 0;

	mstream entbuffer = writeEntDeltas(frame, numEntDeltas);
	mstream plrbuffer = writePlrDeltas(frame, plrDeltaBits);
	mstream msgbuffer = writeMsgDeltas(frame);
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

	uint8_t frameCountDelta = clamp(frame.serverFrameCount - lastServerFrameCount, 0, 255);
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
		return true;
	}

	lastDemoFrameTime = now;

	fwrite(&header, sizeof(DemoFrame), 1, demoFile);

	if (header.isGiantFrame) {
		g_stats.currentWriteSz += sizeof(uint32_t) * 2;
		uint32_t demoTime = now - demoStartTime;
		uint32_t frameSize = g_stats.currentWriteSz;
		fwrite(&demoTime, sizeof(uint32_t), 1, demoFile);
		fwrite(&frameSize, sizeof(uint32_t), 1, demoFile);
		g_stats.giantFrameCount++;
	}
	else if (header.isBigFrame) {
		g_stats.currentWriteSz += sizeof(uint16_t) + sizeof(uint8_t);
		uint8_t demoTimeDelta = frameTimeDelta;
		uint16_t frameSize = g_stats.currentWriteSz;
		fwrite(&demoTimeDelta, sizeof(uint8_t), 1, demoFile);
		fwrite(&frameSize, sizeof(uint16_t), 1, demoFile);
		g_stats.bigFrameCount++;
	}
	else {
		g_stats.currentWriteSz += sizeof(uint8_t) + sizeof(uint8_t);
		uint8_t demoTimeDelta = frameTimeDelta;
		uint16_t frameSize = g_stats.currentWriteSz;
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

	return true;
}

void DemoWriter::closeDemoFile() {
	if (!demoFile) {
		return;
	}
	println("Close demo file");

	uint64_t endTime = getEpochMillis();
	fseek(demoFile, offsetof(DemoHeader, endTime), SEEK_SET);
	fwrite(&lastDemoFrameTime, sizeof(uint64_t), 1, demoFile);

	fclose(demoFile);
	demoFile = NULL;
}

bool DemoWriter::isFileOpen() {
	return demoFile != NULL;
}

bool DemoWriter::validateEdicts() {
	for (int i = 0; i < MAX_EDICTS; i++) {
		if (fileedicts[i].edflags && !(fileedicts[i].edflags & EDFLAG_BEAM) && fileedicts[i].aiment > 8192) {
			println("Invalid edict %d has %d", i, (int)fileedicts[i].aiment);
			return false;
		}
	}
	return true;
}