#include "SvenTV.h"
#include "mmlib.h"
#include <iostream>
#include <ctime>

SvenTV::SvenTV(bool singleThreadMode) {
	threadShouldExit = false;
	lastTvThink = 0;
	this->singleThreadMode = singleThreadMode;

	netedicts = new netedict[MAX_EDICTS];
	debugEdict = new netedict[MAX_EDICTS];
	fileedicts = new netedict[MAX_EDICTS];

	playerinfos = new DemoPlayer[32];
	fileplayerinfos = new DemoPlayer[32];
	netmessages = new NetMessageData[MAX_NETMSG_FRAME];
	cmds = new CommandData[MAX_CMD_FRAME];

	memset(playerinfos, 0, 32 * sizeof(DemoPlayer));
	memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayer));

	deltaPacketBufferSz = 508; // max UDP payload before possible fragmentation
	deltaPacketBuffer = new char[deltaPacketBufferSz];

	// size of full delta on every edict + byte for each index delta + 2 bytes for each delta bits on edict
	int fullDeltaMaxSize = sizeof(netedict) + 4;
	int indexBytes = 1; // 2 for full index writes but on a max size update there will be no 255+ edict gaps
	int headerBytes = 1; // 1 for first full index
	fileDeltaBufferSize = headerBytes + (fullDeltaMaxSize + indexBytes) * MAX_EDICTS;
	fileDeltaBuffer = new char[fileDeltaBufferSize];

	// size of full delta on every player info
	filePlayerInfoBufferSize = (sizeof(DemoPlayer)+2) * 32;
	filePlayerInfoBuffer = new char[filePlayerInfoBufferSize];

	netmessagesBufferSize = sizeof(NetMessageData) * MAX_NETMSG_FRAME;
	netmessagesBuffer = new char[netmessagesBufferSize];

	cmdsBufferSize = sizeof(CommandData) * MAX_CMD_FRAME;
	cmdsBuffer = new char[cmdsBufferSize];

	if (!singleThreadMode) {
		edicts = new edict_t[MAX_EDICTS];
		edictCopyState.setValue(EDICT_COPY_REQUESTED);
		tv_thread = new thread(&SvenTV::think_tvThread, this);
	}
	else {
		socket = new Socket(SOCKET_UDP | SOCKET_NONBLOCKING, SVENTV_PORT);
		edicts = INDEXENT(0);
	}

	println("ZOMG SIZE: %d", (int)sizeof(netedict));
}

SvenTV::~SvenTV() {
	threadShouldExit = true;

	if (tv_thread) {
		tv_thread->join();
		delete tv_thread;
		delete[] edicts;
	}

	for (int i = 0; i < MAX_CLIENTS; i++) {
		delete[] clients[i].baselines;
	}

	delete[] deltaPacketBuffer;
	delete[] fileDeltaBuffer;
	delete[] netedicts;
	delete[] debugEdict;
	delete[] playerinfos;
	delete[] fileplayerinfos;
	delete[] netmessages;
	delete[] netmessagesBuffer;
	delete[] cmds;
	delete[] cmdsBuffer;

	if (replayFile) {
		fclose(replayFile);
	}
}

void SvenTV::think_mainThread() {
	uint64_t startMillis = getEpochMillis();

	if (singleThreadMode) {
		uint64_t now = getEpochMillis();
		if (TimeDifference(lastTvThink, now) >= 0.05f) {
			lastTvThink = now;

			for (int i = 0; i < MAX_EDICTS; i++) {
				netedicts[i].load(edicts[i]);
			}

			//handleClientPackets();
			broadcastEntityStates();
			updateId++;
		}
	}
	else { // multi-threaded mode. Main thread only needs to copy current edict states
		if (edictCopyState.getValue() == EDICT_COPY_REQUESTED) {
			memcpy(edicts, INDEXENT(0), sizeof(edict_t) * MAX_EDICTS);
			memcpy(playerinfos, g_demoplayers, gpGlobals->maxClients*sizeof(DemoPlayer));
			memcpy(netmessages, g_netmessages, g_netmessage_count*sizeof(NetMessageData));
			memcpy(cmds, g_cmds, g_command_count*sizeof(CommandData));
			netmessage_count = g_netmessage_count;
			cmds_count = g_command_count;
			serverFrameCount = g_server_frame_count;

			// clear the main thread buffers
			g_netmessage_count = 0;
			g_command_count = 0;

			int copySz = (sizeof(edict_t) * MAX_EDICTS) + (gpGlobals->maxClients * sizeof(DemoPlayer)) + (g_netmessage_count * sizeof(NetMessageData)) + (g_command_count * sizeof(CommandData));
			edictCopyState.setValue(EDICT_COPY_FINISHED);

			copyTime = getEpochMillis() - startMillis;
			//println("Copy %.1fKB in %lums", copySz / 1024.0f, copyTime);
		}
	}
}

int SvenTV::writePlayerDelta(mstream& writer, uint8_t playerIdx, const DemoPlayer& old, const DemoPlayer& now) {
	uint64_t startOffset = writer.tell();

	DemoPlayerDelta deltaBits; // flags which fields were changed
	memset(&deltaBits, 0, sizeof(DemoPlayerDelta));

	writer.skip(sizeof(DemoPlayerDelta)); // write delta bits later
	uint64_t deltaStartOffset = writer.tell();

	if (old.isConnected != now.isConnected) {
		deltaBits.isConnectedChanged = 1;
		writer.write((void*)&now.isConnected, 1);
	}
	if (now.isConnected) {
		if (strcmp(old.name, now.name) != 0) {
			deltaBits.nameChanged = 1;
			uint8_t len = strlen(now.name);
			writer.write((void*)&len, 1);
			writer.write((void*)&now.name, len);
		}
		if (strcmp(old.model, now.model) != 0) {
			deltaBits.modelChanged = 1;
			uint8_t len = strlen(now.model);
			writer.write((void*)&len, 1);
			writer.write((void*)&now.model, len);
		}
		if (old.steamid64 != now.steamid64) {
			deltaBits.steamIdChanged = 1;
			writer.write((void*)&now.steamid64, 8);
		}
		if (old.topColor != now.topColor || old.bottomColor != now.bottomColor) {
			deltaBits.colorsChanged = 1;
			writer.write((void*)&now.topColor, 1);
			writer.write((void*)&now.bottomColor, 1);
		}
		if (old.ping != now.ping) {
			deltaBits.pingChanged = 1;
			writer.write((void*)&now.ping, 2);
		}
		if (old.pmMoveCounter != now.pmMoveCounter) {
			deltaBits.pmMoveChanged = 1;
			uint8_t delta = clamp(now.pmMoveCounter - old.pmMoveCounter, 0, 255);
			writer.write((void*)&delta, 1);
		}
		if (old.flags != now.flags) {
			deltaBits.flagsChanged = 1;
			writer.write((void*)&now.flags, 1);
		}
		if (old.punchangle[0] != now.punchangle[0]) {
			deltaBits.punchAngleXChanged = 1;
			writer.write((void*)&now.punchangle[0], 2);
		}
		if (old.punchangle[1] != now.punchangle[1]) {
			deltaBits.punchAngleYChanged = 1;
			writer.write((void*)&now.punchangle[1], 2);
		}
		if (old.punchangle[2] != now.punchangle[2]) {
			deltaBits.punchAngleZChanged = 1;
			writer.write((void*)&now.punchangle[2], 2);
		}
		if (old.viewmodel != now.viewmodel) {
			deltaBits.viewmodelChanged = 1;
			writer.write((void*)&now.viewmodel, 2);
		}
		if (old.weaponmodel != now.weaponmodel) {
			deltaBits.weaponmodelChanged = 1;
			writer.write((void*)&now.weaponmodel, 2);
		}
		if (old.weaponanim != now.weaponanim) {
			deltaBits.weaponanimChanged = 1;
			writer.write((void*)&now.weaponanim, 2);
		}
		if (old.armorvalue != now.armorvalue) {
			deltaBits.armorvalueChanged = 1;
			writer.write((void*)&now.armorvalue, 2);
		}
		if (old.button != now.button) {
			deltaBits.buttonChanged = 1;
			writer.write((void*)&now.button, 2);
		}
		if (old.view_ofs != now.view_ofs) {
			deltaBits.view_ofsChanged = 1;
			writer.write((void*)&now.view_ofs, 2);
		}
		if (old.frags != now.frags) {
			deltaBits.fragsChanged = 1;
			writer.write((void*)&now.frags, 2);
		}
		if (old.fov != now.fov) {
			deltaBits.fovChanged = 1;
			writer.write((void*)&now.fov, 1);
		}
		if (old.clip != now.clip) {
			deltaBits.clipChanged = 1;
			writer.write((void*)&now.clip, 2);
		}
		if (old.clip2 != now.clip2) {
			deltaBits.clip2Changed = 1;
			writer.write((void*)&now.clip2, 2);
		}
		if (old.ammo != now.ammo) {
			deltaBits.ammoChanged = 1;
			writer.write((void*)&now.ammo, 2);
		}
		if (old.ammo2 != now.ammo2) {
			deltaBits.ammo2Changed = 1;
			writer.write((void*)&now.ammo2, 2);
		}
		if (old.observer != now.observer) {
			deltaBits.observerChanged = 1;
			writer.write((void*)&now.observer, 1);
		}
	}

	if (writer.eom()) {
		writer.seek(startOffset);
		return EDELTA_OVERFLOW;
	}

	uint64_t currentOffset = writer.tell();
	writer.seek(startOffset);

	if (currentOffset == deltaStartOffset) {
		return EDELTA_NONE;
	}

	writer.write((void*)&deltaBits, sizeof(DemoPlayerDelta));
	writer.seek(currentOffset);

	return EDELTA_WRITE;
}

void SvenTV::handleDeltaAck(mstream& reader, NetClient& client) {
	uint16_t updateId;

	reader.read(&updateId, 2);

	int deltaIdx = -1;
	for (int i = 0; i < client.sentDeltas.size(); i++) {
		if (client.sentDeltas[i].updateId == updateId) {
			deltaIdx = i;
			break;
		}
	}

	if (deltaIdx == -1) {
		println("Got ack for too old update %d", (int)updateId);
		return;
	}

	DeltaUpdate& update = client.sentDeltas[deltaIdx];

	if (update.packets.empty()) {
		println("Ignoring duplicate ack");
		return;
	}

	int packetIdx = 0;
	for (int i = 0; i < update.packets.size(); i++) {
		int bit = reader.readBit();
		if (bit == -1) {
			println("Failed to read ack, invalid number of bits sent");
			return;
		}

		if (bit == 1) {
			client.applyDeltaToBaseline(update.packets[i], false);
		}
	}
	client.baselineId = updateId;

	// don't need any history older than the current baseline
	client.sentDeltas.erase(client.sentDeltas.begin(), client.sentDeltas.begin() + deltaIdx);
	
	//update.packets.clear();
	//println("OKIE DID ACK %d", updateId);
}

void SvenTV::handleClientPackets() {
	Packet packet;
	while (!threadShouldExit) {
		if (!socket->recv(packet)) {
			return;
		}

		if (packet.sz == 0) {
			println("Ignore 0 size packet");
			continue;
		}

		uint8_t packetType;
		mstream reader(packet.data, packet.sz);
		reader.read(&packetType, 1);

		int clientIdx = -1;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (!clients[i].isFree && clients[i].addr == packet.addr) {
				clientIdx = i;
				break;
			}
		}

		if (packetType == CLC_CONNECT) {
			if (clientIdx != -1) {
				println("Ignore connect packet for existing client");
				continue;
			}
			println("New client connected from %s", packet.addr.getString().c_str());

			bool foundFree = false;
			for (int i = 0; i < MAX_CLIENTS; i++) {
				if (clients[i].isFree) {
					clients[i].init(packet.addr);
					foundFree = true;
					break;
				}
			}

			if (!foundFree) {
				const char* fullStr = "sventv_full";
				socket->send(Packet(packet.addr, fullStr, strlen(fullStr)));
			}
			else {
				const char* mapName = STRING(gpGlobals->mapname);
				int welcomDatSz = 1 + strlen(mapName);
				static char welcomeDat[64];
				welcomeDat[0] = SVC_WELCOME;
				memcpy(welcomeDat + 1, mapName, welcomDatSz);

				socket->send(Packet(packet.addr, welcomeDat, welcomDatSz));
			}

			continue;
		}

		if (clientIdx == -1) {
			println("Ignore packet from unknown client");
			continue;
		}

		NetClient& client = clients[clientIdx];
		client.lastPacketTime = getEpochMillis();

		if (packetType == CLC_DELTA_RESET) {
			println("Client reset delta state to null");
			client.baselineId = 0;
			client.sentDeltas.clear();
			memset(client.baselines, 0, MAX_EDICTS * sizeof(netedict));
		}
		if (packetType == CLC_DELTA_ACK) {
			handleDeltaAck(reader, client);
		}
	}
	
}

void SvenTV::broadcastEntityStates() {
	int totalEnts = 0;

	int clientCount = 0;

	const int maxPacketSz = 508; // 508 = max size before fragmentation is possible

	mstream buffer(deltaPacketBuffer, deltaPacketBufferSz);

	uint64_t now = getEpochMillis();

	if (abortEverything) {
		return;
	}

	bool debugMode = false;
	int debugFrag = -1;
	int debugEnt = -1;

	for (int k = 0; k < MAX_CLIENTS; k++) {
		if (clients[k].isFree && (k != 0 || !debugMode)) {
			continue;
		}

		if (clients[k].nextUpdateTime > now && debugFrag == -1) {
			continue;
		}

		uint64_t updateDelay = (1.0f / clients[k].updateRateFps) * 1000;
		clients[k].nextUpdateTime = now + updateDelay;

		const uint8_t packetType = SVC_DELTAPACKETENTITIES;

		// in case the update is split into multiple packets, the client needs to be able
		// to tell the server which packets it received.
		uint16_t fragmentId = 0;

		buffer.seek(0);
		buffer.write((void*)&packetType, 1);
		buffer.write((void*)&updateId, 2);
		buffer.write((void*)&clients[k].baselineId, 2);
		buffer.write((void*)&fragmentId, 2);
		if (debugMode) {
			println("Write delta %d frag %d", (int)updateId, (int)fragmentId);
		}
		fragmentId++;

		uint8_t offset = 0; // always write full index for first entity delta

		vector<Packet> deltaPackets;

		for (uint16_t i = 0; i < MAX_EDICTS; i++) {
			netedict& now = netedicts[i];

			uint64_t startOffset = buffer.tell();
			uint8_t initialOffset = offset;

			buffer.write(&offset, 1); // entity index the delta is for (offset from previous)
			if (offset == 0) {
				// last edict was 256+ slots away. Write full index.
				buffer.write(&i, 2);
			}

			uint64_t datOffset = buffer.tell();

			int ret = now.writeDeltas(buffer, clients[k].baselines[i]);

			if (ret == EDELTA_OVERFLOW) {
				Packet delta = Packet(clients[k].addr, deltaPacketBuffer, startOffset);
				deltaPackets.push_back(delta);
				
				// set up new packet
				buffer.seek(0);
				buffer.write((void*)&packetType, 1);
				buffer.write((void*)&updateId, 2);
				buffer.write((void*)&clients[k].baselineId, 2);
				buffer.write((void*)&fragmentId, 2);
				if (debugMode) {
					println("Write delta %d frag %d", (int)updateId, (int)fragmentId);
				}
				fragmentId++;
				offset = 0; // always write full index for first entity delta
				
				// redo this entity delta in the new packet
				i--;
				continue;
			}
			else if (ret == EDELTA_NONE) {
				// no differences
				buffer.seek(startOffset); // undo index write
				if (offset != 0) {
					offset += 1;
				}
			}
			else {
				// delta written
				int writeSz = (int)(buffer.tell() - startOffset);
				offset = 1;
				totalEnts++;
				if (debugMode) {
					uint32_t bits = *((uint32_t*)(buffer.getBuffer() + datOffset));
					println("Wrote ent idx %d (%d bytes) offset %d, bits %lu", (int)i, (int)(buffer.tell() - datOffset), (int)initialOffset, bits);
				}
				if (fragmentId - 1 == debugFrag && debugFrag != -1) {
					totalEnts--; totalEnts++; // set breakpoint here
				}
				//println("Write %d bytes for edict %d (%d total)", writeSz, i, totalBytes);
				//println("Write %d bytes for edict %s (%d total)", writeSz, STRING(edicts[i].v.classname), totalBytes);
			}
		}

		if (buffer.tell() > 7) { // more than just the header written
			deltaPackets.push_back(Packet(clients[k].addr, deltaPacketBuffer, buffer.tell()));
		}
		else {
			// TODO: Don't send empty updates (only here because clients get disconnected for lack of acks)
			deltaPackets.push_back(Packet(clients[k].addr, deltaPacketBuffer, buffer.tell()));
		}

		// assume client acked
		
		if (debugMode) {
			bool redoWrite = false;

			memcpy(debugEdict, clients[k].baselines, MAX_EDICTS * sizeof(netedict));

			for (int i = 0; i < deltaPackets.size(); i++) {
				Packet& deltaPacket = deltaPackets[i];
				debugEnt = clients[k].applyDeltaToBaseline(deltaPacket, debugMode);
				if (debugEnt != -1) {
					abortEverything = true;
					
					if (debugFrag != -1) {
						return;
					}

					redoWrite = true;
					debugFrag = i;
					break;
				}
			}

			if (redoWrite) {
				memcpy(clients[k].baselines, debugEdict, MAX_EDICTS * sizeof(netedict));
				println("OK DO IT AGAIN");
				k--;
				continue;
			}

			/*
			for (int i = 0; i < MAX_EDICTS; i++) {
				if (!clients[k].baselines[i].matches(netedicts[i])) {
					println("ZOMG BAD");
				}
			}
			*/
			
			//memcpy(clients[k].baselines, netedicts, MAX_EDICTS * sizeof(netedict));
		}

		
		int totalSz = 0;
		int numPackets = deltaPackets.size();
		for (int i = 0; i < deltaPackets.size(); i++) {
			Packet& deltaPacket = deltaPackets[i];
			if (!debugMode)
				socket->send(deltaPacket);		
			totalSz += deltaPacket.sz;
		}

		DeltaUpdate update;
		update.packets = deltaPackets;
		update.updateId = updateId;
		clients[k].sentDeltas.push_back(update);

		NetUsageDatapoint datapoint;
		datapoint.bytes = totalSz;
		datapoint.time = getEpochMillis();
		clients[k].sentBytesHistory.push_back(datapoint);
		float kbsec = clients[k].getBytesSentPerSecond() / 1024.0f;
		println("Send %s: %4d B, %d packets, update %d, baseline %d, %.1f KB/s", 
			clients[k].addr.getString().c_str(), totalSz, numPackets, (int)updateId, clients[k].baselineId, kbsec);

		if (clients[k].sentDeltas.size() > 100) {
			if (!debugMode)
				println("Client %d not acking!", k);
			clients[k].sentDeltas.erase(clients[k].sentDeltas.begin());
		}

		if (TimeDifference(clients[k].lastPacketTime, now) > timeoutSeconds && !debugMode) {
			println("Disconnecting unresponsive client");
			clients[k].isFree = true;
			clients[k].sentDeltas.clear();
			clients[k].sentBytesHistory.clear();
		}

		clientCount++;
	}

	//println("Send %d ents to %d clients (%d bytes each)", totalEnts, clientCount, sizeof(netedict));
}

bool SvenTV::openDemo(edict_t* plr, string path, float offsetSeconds, bool skipPrecache) {
	stopReplay();
	replayFile = fopen(path.c_str(), "rb");

	if (!replayFile) {
		ClientPrint(plr, HUD_PRINTTALK, UTIL_VarArgs("Failed to open demo file: %s\n", path.c_str()));
		return false;
	}

	if (!fread(&demoHeader, sizeof(DemoHeader), 1, replayFile)) {
		ClientPrint(plr, HUD_PRINTTALK, "Invalid demo file: EOF before header read\n");
		closeReplayFile();
		return false;
	}

	if (demoHeader.version != DEMO_VERSION) {
		ClientPrint(plr, HUD_PRINTTALK, UTIL_VarArgs("Invalid demo version: %d (expected %d)\n", demoHeader.version, DEMO_VERSION));
		closeReplayFile();
		return false;
	}

	string mapname = string(demoHeader.mapname, 64);
	if (!g_engfuncs.pfnIsMapValid((char*)mapname.c_str())) {
		ClientPrint(plr, HUD_PRINTTALK, UTIL_VarArgs("Invalid demo map: %s\n", mapname.c_str()));
		closeReplayFile();
		return false;
	}

	if (demoHeader.modelLen > 1024 * 1024 * 32 || demoHeader.soundLen > 1024 * 1024 * 32) {
		ClientPrint(plr, HUD_PRINTTALK, UTIL_VarArgs("Invalid demo file: %u + %u byte model/sound data (too big)\n", demoHeader.modelLen, demoHeader.soundLen));
		closeReplayFile();
		return false;
	}
	
	if (demoHeader.modelLen) {
		char* modelData = new char[demoHeader.modelLen];

		if (!fread(modelData, demoHeader.modelLen, 1, replayFile)) {
			ClientPrint(plr, HUD_PRINTTALK, "Invalid demo file: incomplete model data\n");
			closeReplayFile();
			return false;
		}

		precacheModels = splitString(string(modelData, demoHeader.modelLen), "\n");

		for (int i = 0; i < precacheModels.size(); i++) {
			int replayIdx = demoHeader.modelIdxStart + i;
			replayModelPath[replayIdx] = precacheModels[i];
			//println("Replay model %d: %s", replayIdx, precacheModels[i].c_str());
		}

		delete[] modelData;
	}
	if (demoHeader.soundLen) {
		char* soundData = new char[demoHeader.soundLen];

		if (!fread(soundData, demoHeader.soundLen, 1, replayFile)) {
			ClientPrint(plr, HUD_PRINTTALK, "Invalid demo file: incomplete sound data\n");
			closeReplayFile();
			return false;
		}

		precacheSounds = splitString(string(soundData, demoHeader.soundLen), "\n");

		delete[] soundData;
	}
	if (demoHeader.modelLen == 0) {
		println("WARNING: Demo has no model list. The plugin may have been reloaded before the demo started.");
	}

	ClientPrint(plr, HUD_PRINTCONSOLE, UTIL_VarArgs("\nfile       : %s\n", path.c_str()));
	{
		time_t rawtime;
		struct tm* timeinfo;
		char buffer[80];

		time(&rawtime);
		timeinfo = localtime(&rawtime);

		strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
		ClientPrint(plr, HUD_PRINTCONSOLE, UTIL_VarArgs("date       : %s\n", buffer));
	}
	{
		int duration = demoHeader.endTime ? TimeDifference(demoHeader.startTime, demoHeader.endTime) : 0;
		int hours = duration / (60 * 60);
		int minutes = (duration - (hours * 60 * 60)) / 60;
		int seconds = duration % 60;

		if (duration) {
			ClientPrint(plr, HUD_PRINTCONSOLE, UTIL_VarArgs("duration   : %02d:%02d:%02d\n", hours, minutes, seconds));
		}
		else {
			ClientPrint(plr, HUD_PRINTCONSOLE, "duration   : unknown\n");
		}
	}
	ClientPrint(plr, HUD_PRINTCONSOLE, UTIL_VarArgs("map        : %s\n", mapname.c_str()));
	ClientPrint(plr, HUD_PRINTCONSOLE, UTIL_VarArgs("maxplayers : %d\n", demoHeader.maxPlayers));
	ClientPrint(plr, HUD_PRINTCONSOLE, UTIL_VarArgs("models     : %d (offset %d)\n", precacheModels.size(), demoHeader.modelIdxStart));
	ClientPrint(plr, HUD_PRINTCONSOLE, UTIL_VarArgs("sounds     : %d\n", precacheSounds.size()));

	if (skipPrecache) {
		prepareDemo(offsetSeconds);
		return true;
	}

	if (toLowerCase(STRING(gpGlobals->mapname)) != toLowerCase(mapname)) {
		g_engfuncs.pfnServerCommand(UTIL_VarArgs("changelevel %s\n", mapname.c_str()));
		g_engfuncs.pfnServerExecute();
	}
}

void SvenTV::prepareDemo(float offsetSeconds) {
	for (int i = 1; i <= gpGlobals->maxClients; i++) {
		edict_t* ent = INDEXENT(i);
		CBasePlayer* plr = (CBasePlayer*)GET_PRIVATE(ent);
		if (isValidPlayer(ent) && plr) {
			ent->v.viewmodel = 0;
			ent->v.weaponmodel = 0;
			plr->m_iHideHUD = 1;
		}
	}
	for (int i = gpGlobals->maxClients; i < gpGlobals->maxEntities; i++) {
		edict_t* ent = INDEXENT(i);
		if (ent && strlen(STRING(ent->v.classname)) > 0) {
			REMOVE_ENTITY(ent);
		}
	}

	replayStartTime = getEpochMillis() - (uint64_t)(offsetSeconds*1000);
	replayFrame = 0;
	nextFrameOffset = ftell(replayFile);
	nextFrameTime = 0;
	enableDemoFile = false;
}

void SvenTV::precacheDemo() {
	if (!replayFile) {
		return;
	}

	for (int i = 0; i < precacheModels.size(); i++) {
		PrecacheModel(precacheModels[i]);
	}
	for (int i = 0; i < precacheSounds.size(); i++) {
		PrecacheSound(precacheSounds[i]);
	}
}

void SvenTV::stopReplay() {
	closeReplayFile();
	precacheModels.clear();
	precacheSounds.clear();
	replayModelPath.clear();
	replayEnts.clear();
}

void SvenTV::closeReplayFile() {
	if (replayFile) {
		fclose(replayFile);
		replayFile = NULL;
	}
}

bool SvenTV::readEntDeltas(mstream& reader) {
	uint16_t numEntDeltas = 0;
	reader.read(&numEntDeltas, 2);

	//println("Reading %d deltas", (int)numEntDeltas);

	int loop = -1;
	uint16_t fullIndex = 0;
	for (int i = 0; i < numEntDeltas; i++) {
		loop++;
		uint8_t offset = 0;
		reader.read(&offset, 1);

		if (offset == 0) {
			reader.read(&fullIndex, 2);
		}
		else {
			fullIndex += offset;
		}

		if (fullIndex >= MAX_EDICTS) {
			println("ERROR: Invalid delta wants to update edict %d at %d\n", (int)fullIndex, loop);
			closeReplayFile();
			return false;
		}

		uint64_t startPos = reader.tell();

		netedict* ed = &fileedicts[fullIndex];
		if (!ed->readDeltas(reader)) {
			//println("Skip free %d", fullIndex);
			continue;
		}

		//println("Read index %d (%d bytes)", (int)fullIndex, (int)(reader.tell() - startPos));

		if (reader.eom()) {
			println("ERROR: Invalid delta hit unexpected eom at %d\n", loop);
			closeReplayFile();
			return false;
		}
	}

	return true;
}

bool SvenTV::applyEntDeltas(DemoFrame& header) {
	int errorSprIdx = g_engfuncs.pfnModelIndex("sprites/error.spr");

	for (int i = 1; i < MAX_EDICTS; i++) {
		if (!fileedicts[i].edtype) {
			if (i < replayEnts.size()) {
				replayEnts[i].GetEdict()->v.effects |= EF_NODRAW;
			}
			continue;
		}

		// create ents until the client has enough to handle this demo frame
		while (i >= replayEnts.size()) {
			map<string, string> keys;
			keys["model"] = "sprites/error.spr";

			CBaseEntity* ent = CreateEntity("cycler", keys, true);
			ent->pev->solid = SOLID_NOT;
			ent->pev->effects |= EF_NODRAW;
			ent->pev->movetype = MOVETYPE_NONE;

			if (!ent) {
				println("Entity overflow at %d\n", (int)replayEnts.size());
				for (int i = 0; i < replayEnts.size(); i++) {
					REMOVE_ENTITY(replayEnts[i]);
				}
				closeReplayFile();
				return false;
			}

			replayEnts.push_back(ent);
		}

		edict_t* ent = replayEnts[i];
		int oldModelIdx = ent->v.modelindex;
		int oldSeq = ent->v.sequence;

		fileedicts[i].apply(ent, replayEnts);
		ent->v.framerate = 0.00001f;

		if (oldSeq != ent->v.sequence && (ent->v.flags & FL_MONSTER)) {
			CBaseMonster* anim = (CBaseMonster*)replayEnts[i].GetEntity();
			anim->m_Activity = ACT_RELOAD;

			void* pmodel = GET_MODEL_PTR(anim->edict());
			studiohdr_t* header = (studiohdr_t*)pmodel;
			if (!header || header->id != 1414743113 || header->version != 10) {
				println("Invalid studio model for edict %d (%s): %s", ENTINDEX(anim->edict()), STRING(ent->v.classname), STRING(ent->v.model));
			}
			else {
				anim->ResetSequenceInfo();
			}
		}

		// player entity
		if (i > 0 && i <= demoHeader.maxPlayers) {
			float dt = (header.demoTime - lastReplayFrame.demoTime) / 1000.0f;
			updatePlayerModelRotations(ent, dt);
		}


		if (oldModelIdx != ent->v.modelindex) {
			SET_MODEL(ent, getReplayModel(ent->v.modelindex).c_str());
		}

		if (ent->v.modelindex == errorSprIdx) {
			// prevent console flooding with invalid frame errors
			ent->v.frame = 0;
		}

		/*
		if (g_engfuncs.pfnModelIndex("sprites/lgtning.spr") == ent->v.modelindex || g_engfuncs.pfnModelIndex("sprites/error.spr") == ent->v.modelindex) {
			SET_MODEL(ent, "sprites/error.spr");
			println("OK LIGHTNING: %f %d", ent->v.scale, ent->v.movetype);
		}
		*/
	}

	return true;
}

string SvenTV::getReplayModel(uint16_t modelIdx) {
	static set<uint16_t> unknownSpam;

	if (replayModelPath.count(modelIdx)) {
		// Demo file model path
		return replayModelPath[modelIdx];
	}
	else if (g_indexToModel.count(modelIdx)) {
		// BSP model or whatever the game has loaded for this idx
		return g_indexToModel[modelIdx];
	}
	else {
		if (!unknownSpam.count(modelIdx)) {
			println("Unknown model idx %d", modelIdx);
			unknownSpam.insert(modelIdx);
		}
		
		return "sprites/error.spr";
	}
}

void SvenTV::convReplayEntIdx(uint16_t& eidx) {
	if (eidx >= replayEnts.size()) {
		println("Invalid replay ent idx %d", (int)eidx);
		eidx = 0;
	}
	eidx = ENTINDEX(replayEnts[eidx]);
}

void SvenTV::convReplayModelIdx(uint16_t& modelIdx) {
	modelIdx = g_engfuncs.pfnModelIndex(getReplayModel(modelIdx).c_str());
}

void SvenTV::convReplaySoundIdx(uint16_t& soundIdx) {
	if (soundIdx >= precacheSounds.size()) {
		println("Invalid replay sound idx %d", (int)soundIdx);
		soundIdx = 0;
		return;
	}

	string path = precacheSounds[soundIdx];
	if (g_SoundCache.find(path) == g_SoundCache.end()) {
		println("Invalid replay sound: %s", path.c_str());
		soundIdx = 0;
		return;
	}

	soundIdx = g_SoundCache[path];
}

bool SvenTV::readPlayerDeltas(mstream& reader) {
	uint32_t includedPlayers = 0;
	reader.read(&includedPlayers, 4);

	int numPlayerDeltas = 0;
	for (int i = 0; i < gpGlobals->maxClients; i++) {
		if ((includedPlayers & (1 << i)) == 0) {
			continue;
		}

		DemoPlayerDelta deltaBits;
		reader.read(&deltaBits, sizeof(DemoPlayerDelta));

		DemoPlayer& info = fileplayerinfos[i];
		static char strBuffer[256];

		if (deltaBits.isConnectedChanged) {
			reader.read(&info.isConnected, 1);
		}
		if (!info.isConnected) {
			continue;
		}
		if (deltaBits.nameChanged) {
			uint8_t len;
			reader.read(&len, 1);
			if (len > 31) {
				println("Invalid name length %d", (int)len);
				len = 31;
			}
			reader.read(strBuffer, len);
			strBuffer[len] = '\0';
			memcpy(info.name, strBuffer, len+1);
		}
		if (deltaBits.modelChanged) {
			uint8_t len;
			reader.read(&len, 1);
			if (len > 22) {
				println("Invalid name length %d", (int)len);
				len = 22;
			}
			reader.read(strBuffer, len);
			strBuffer[len] = '\0';
			memcpy(info.model, strBuffer, len+1);
		}
		if (deltaBits.steamIdChanged) {
			reader.read(&info.steamid64, 8);
		}
		if (deltaBits.colorsChanged) {
			reader.read(&info.topColor, 1);
			reader.read(&info.bottomColor, 1);
		}
		if (deltaBits.pingChanged) {
			reader.read(&info.ping, 2);
		}
		if (deltaBits.pmMoveChanged) {
			reader.read(&info.pmMoveCounter, 1);
		}
		if (deltaBits.flagsChanged) {
			reader.read(&info.flags, 1);
		}
		if (deltaBits.punchAngleXChanged) {
			reader.read(&info.punchangle[0], 2);
		}
		if (deltaBits.punchAngleYChanged) {
			reader.read(&info.punchangle[1], 2);
		}
		if (deltaBits.punchAngleZChanged) {
			reader.read(&info.punchangle[2], 2);
		}
		if (deltaBits.viewmodelChanged) {
			reader.read(&info.viewmodel, 2);
		}
		if (deltaBits.weaponmodelChanged) {
			reader.read(&info.weaponmodel, 2);
		}
		if (deltaBits.weaponanimChanged) {
			reader.read(&info.weaponanim, 2);
		}
		if (deltaBits.armorvalueChanged) {
			reader.read(&info.armorvalue, 2);
		}
		if (deltaBits.buttonChanged) {
			reader.read(&info.button, 2);
		}
		if (deltaBits.view_ofsChanged) {
			reader.read(&info.view_ofs, 2);
		}
		if (deltaBits.fragsChanged) {
			reader.read(&info.frags, 2);
		}
		if (deltaBits.fovChanged) {
			reader.read(&info.fov, 1);
		}
		if (deltaBits.clipChanged) {
			reader.read(&info.clip, 2);
		}
		if (deltaBits.clip2Changed) {
			reader.read(&info.clip2, 2);
		}
		if (deltaBits.ammoChanged) {
			reader.read(&info.ammo, 2);
		}
		if (deltaBits.ammo2Changed) {
			reader.read(&info.ammo2, 2);
		}
		if (deltaBits.observerChanged) {
			reader.read(&info.observer, 1);
		}

		numPlayerDeltas++;
	}

	//println("Got %d player deltas", numPlayerDeltas);

	return !reader.eom();
}

bool SvenTV::processTempEntityMessage(DemoNetMessage& msg, const float* origin, uint16_t& edictIdx, byte* data) {
	uint8_t type = data[0];

	byte* args = data + 1;
	uint16_t* args16 = (uint16_t*)args;
	float* fargs = (float*)args;

	switch (type) {
	case TE_BEAMPOINTS:
	case TE_BEAMDISK:
	case TE_BEAMCYLINDER:
	case TE_BEAMTORUS:
	case TE_SPRITETRAIL:
	case TE_SPRITE_SPRAY:
	case TE_SPRAY:
	case TE_PROJECTILE:
		convReplayModelIdx(args16[12]);
		//args16[12] = g_engfuncs.pfnModelIndex("sprites/laserbeam.spr");
		break;
	case TE_BEAMENTPOINT:
		convReplayEntIdx(args16[0]);
		convReplayModelIdx(args16[7]);
		break;
	case TE_KILLBEAM:
		convReplayEntIdx(args16[0]);
		break;
	case TE_BEAMENTS:
	case TE_BEAMRING:
		convReplayEntIdx(args16[0]);
		convReplayEntIdx(args16[1]);
		convReplayModelIdx(args16[2]);
		break;
	case TE_LIGHTNING: {
		uint16_t& arg = *(uint16_t*)(args + 27);
		convReplayModelIdx(arg);
		break;
	}
	case TE_BEAMSPRITE:
		convReplayModelIdx(args16[12]);
		convReplayModelIdx(args16[13]);
		break;
	case TE_BEAMFOLLOW:
	case TE_FIZZ:
		convReplayEntIdx(args16[0]);
		convReplayModelIdx(args16[1]);
		break;
	case TE_PLAYERSPRITES:
		args16[0] = 1; // TODO: Use bots so player attachments work
		convReplayModelIdx(args16[1]);
		break;
	case TE_EXPLOSION:
	case TE_SMOKE:
	case TE_SPRITE:
	case TE_GLOWSPRITE:
	case TE_LARGEFUNNEL:
		convReplayModelIdx(args16[6]);
		break;
	case TE_PLAYERATTACHMENT: {
		uint16_t entIdx = args[0];
		uint16_t& modelIdx = *(uint16_t*)(args + 5);
		convReplayEntIdx(entIdx);
		convReplayModelIdx(modelIdx);
		entIdx = 1; // TODO: Use bots so player attachments work
		args[0] = (uint8_t)entIdx;
		break;
	}
	case TE_KILLPLAYERATTACHMENTS:
	case TE_PLAYERDECAL: {
		uint16_t entIdx = args[0];
		convReplayEntIdx(entIdx);
		entIdx = 1; // TODO: Use bots so player attachments work
		args[0] = (uint8_t)entIdx;
		break;
	}
	case TE_BUBBLETRAIL:
	case TE_BUBBLES:
		convReplayModelIdx(args16[14]);
		break;
	case TE_BLOODSPRITE:
		convReplayModelIdx(args16[6]);
		convReplayModelIdx(args16[7]);
		break;
	case TE_FIREFIELD:
		convReplayModelIdx(args16[7]);
		break;
	case TE_EXPLODEMODEL:
		convReplayModelIdx(args16[8]);
		break;
	case TE_MODEL: {
		uint16_t& modelIdx = *(uint16_t*)(args + 25);
		convReplayModelIdx(modelIdx);
		break;
	}
	case TE_BREAKMODEL: {
		uint16_t& modelIdx = *(uint16_t*)(args + 37);
		convReplayModelIdx(modelIdx);
		break;
	}
	case TE_DECAL:
	case TE_GUNSHOTDECAL:
	case TE_DECALHIGH:
	{
		uint16_t& modelIdx = *(uint16_t*)(args + 13);
		convReplayModelIdx(modelIdx);
		break;
	}
	case TE_BSPDECAL:
		if (args16[7]) { // not world entity?
			convReplayEntIdx(args16[7]);
			//convReplayModelIdx(args16[5]); // (BSP model idx in demo should always match the game)
		}
		break;
	default:
		break;
	}

	//println("Play temp ent %d", (int)type);

	return true;
}

bool SvenTV::processDemoNetMessage(DemoNetMessage& msg, const float* origin, uint16_t& edictIdx, byte* data) {
	byte* args = data;
	uint16_t* args16 = (uint16_t*)args;

	switch (msg.type) {
	case SVC_TEMPENTITY:
		return processTempEntityMessage(msg, origin, edictIdx, data);
	case MSG_StartSound: {
		uint16_t& soundIdx = *(uint16_t*)(args + (msg.sz-2));
		convReplaySoundIdx(soundIdx);
		return true;
	}
	default:
		println("Unhandled netmsg %d", (int)msg.type);
		return false;
	}
}

bool SvenTV::readNetworkMessages(mstream& reader) {
	uint16_t numMessages;
	reader.read(&numMessages, 2);

	if (numMessages > MAX_NETMSG_FRAME) {
		println("Invalid net msg count %d", (int)numMessages);
		return false;
	}

	for (int i = 0; i < numMessages; i++) {
		DemoNetMessage msg;
		float origin[3];
		uint16_t edictIdx = 0;
		static byte msg_data[MAX_NETMSG_DATA];

		reader.read(&msg, sizeof(DemoNetMessage));
		if (msg.hasOrigin) {
			reader.read(&origin, sizeof(float)*3);
		}
		if (msg.hasEdict) {
			reader.read(&edictIdx, 2);
		}
		if (msg.sz > MAX_NETMSG_DATA) {
			println("Invalid net msg size %d", (int)msg.sz);
			return false; // data corrupted, abort before SVC_BAD
		}
		reader.read(msg_data, msg.sz);

		if (edictIdx >= replayEnts.size()) {
			println("Invalid msg ent %d", (int)edictIdx);
			continue;
		}

		const float* ori = msg.hasOrigin ? origin : NULL;
		edict_t* ent = msg.hasEdict ? replayEnts[edictIdx].GetEdict() : NULL;
		if (!processDemoNetMessage(msg, ori, edictIdx, msg_data)) {
			continue;
		}

		MESSAGE_BEGIN(msg.dest, msg.type, ori, ent);

		int numLongs = msg.sz / 4;
		int numBytes = msg.sz % 4;

		for (int i = 0; i < numLongs; i++) {
			WRITE_LONG(((uint32_t*)msg_data)[i]);
		}

		int byteOffset = numLongs*4;
		for (int i = byteOffset; i < byteOffset + numBytes; i++) {
			WRITE_BYTE(msg_data[i]);
		}
		
		MESSAGE_END();

		//println("Read msg %d", (int)msg.type);
	}

	return true;
}

bool SvenTV::readClientCommands(mstream& reader) {
	uint16_t numCommands;
	reader.read(&numCommands, 2);

	static char commandChars[MAX_CMD_LENGTH + 1];

	for (int i = 0; i < numCommands; i++) {
		DemoCommand cmd;
		reader.read(&cmd, sizeof(DemoCommand));

		if (cmd.len > MAX_CMD_LENGTH) {
			println("Invalid cmd len %d", (int)cmd.len);
			return false;
		}

		reader.read(commandChars, cmd.len);
		commandChars[cmd.len] = '\0';

		if (cmd.idx > demoHeader.maxPlayers) {
			println("Invalid command player %d / %d", (int)cmd.idx, (int)demoHeader.maxPlayers);
			continue;
		}
		if (cmd.idx == 0) {
			println("[SvenTV][Cmd][Server] %s", commandChars);
			continue;
		}

		DemoPlayer& plr = fileplayerinfos[cmd.idx-1];
		
		const char* legacySteamId = "STEAM_ID_INVALID";
		if (plr.steamid64 == 1ULL) {
			legacySteamId = "STEAM_ID_LAN";
		}
		else if (plr.steamid64 == 2ULL) {
			legacySteamId = "BOT";
		}
		else if (plr.steamid64 > 0ULL) {
			uint32_t accountIdLowBit = plr.steamid64 & 1;
			uint32_t accountIdHighBits = (plr.steamid64 >> 1) & 0x7FFFFFF;
			legacySteamId = UTIL_VarArgs("STEAM_0:%u:%u", accountIdLowBit, accountIdHighBits);
		}

		println("[SvenTV][Cmd][%s][%s] %s", legacySteamId, plr.name, commandChars);
	}

	return true;
}

bool SvenTV::readDemoFrame() {
	uint32_t t = getEpochMillis() - replayStartTime;

	fseek(replayFile, nextFrameOffset, SEEK_SET);

	DemoFrame header;
	if (!fread(&header, sizeof(DemoFrame), 1, replayFile)) {
		ClientPrintAll(HUD_PRINTTALK, "[SvenTV] Unexpected EOF\n");
		closeReplayFile();
		return false;
	}
	if (header.demoTime > t) {
		//println("Wait %u > %u", header.demoTime, t);
		return false;
	}
	if (header.frameSize > 1024 * 1024*32 || header.frameSize == 0) {
		ClientPrintAll(HUD_PRINTTALK, "[SvenTV] Invalid frame size\n");
		closeReplayFile();
		return false;
	}
	nextFrameOffset = ftell(replayFile) + (header.frameSize - sizeof(DemoFrame));

	char* frameData = new char[header.frameSize];
	if (!fread(frameData, header.frameSize, 1, replayFile)) {
		delete[] frameData;
		ClientPrintAll(HUD_PRINTTALK, "[SvenTV] Unexpected EOF\n");
		closeReplayFile();
		return false;
	}

	mstream reader = mstream(frameData, header.frameSize);

	if (header.isKeyFrame) {
		memset(fileedicts, 0, MAX_EDICTS * sizeof(netedict));
		memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayer));
	}

	if (header.hasEntityDeltas && (!readEntDeltas(reader) || !applyEntDeltas(header))) {
		delete[] frameData;
		return false;
	}

	if (header.hasPlayerDeltas && !readPlayerDeltas(reader)) {
		delete[] frameData;
		return false;
	}

	if (header.hasNetworkMessages && !readNetworkMessages(reader)) {
		delete[] frameData;
		return false;
	}

	if (header.hasCommands && !readClientCommands(reader)) {
		delete[] frameData;
		return false;
	}
	
	replayFrame++;
	//println("Frame %d, Time: %.1f", replayFrame, (float)TimeDifference(0, header.demoTime));

	memcpy(&lastReplayFrame, &header, sizeof(DemoFrame));
	delete[] frameData;

	return true;
}

void SvenTV::updatePlayerModelRotations(edict_t* ent, float dt) {
	// blending and pitch calculations (best guess)
	{
		float pitch = normalizeRangef(-ent->v.angles.x, -180.0f, 180.0f);

		if (pitch > 35) {
			ent->v.angles.x = (pitch - 35) * 0.5f;
		}
		else if (pitch < -45) {
			ent->v.angles.x = (pitch - -45) * 0.5f;
		}
		else {
			ent->v.angles.x = 0;
		}

		uint8_t blend = 127 + (pitch * 1.4f);
		ent->v.blending[0] = blend;
	}

	// TODO: calculate demoFileFps
	Vector gaitspeed = Vector(ent->v.velocity[0], ent->v.velocity[1], 0) * demoFileFps;
	float& gaityaw = ent->v.fuser1;
	
	float dtScale = 1.0f / dt;
	const float PI = 3.1415f;

	// gait calculations from the HLSDK
	if (gaitspeed.Length() < 5)
	{
		// standing still. Rotate legs back to forward position
		float flYawDiff = ent->v.angles.y - gaityaw;
		flYawDiff = flYawDiff - (int)(flYawDiff / 360) * 360;
		if (flYawDiff > 180)
			flYawDiff -= 360;
		if (flYawDiff < -180)
			flYawDiff += 360;

		if (dt < 0.25)
			flYawDiff *= dt * 4;
		else
			flYawDiff *= dt;

		gaityaw += flYawDiff;
		gaityaw -= (int)(gaityaw / 360) * 360;
	}
	else
	{
		// moving. rotate legs towards movement direction
		Vector doot = gaitspeed.Normalize();
		gaityaw = (atan2(gaitspeed.y, gaitspeed.x) * 180 / PI);
		if (gaityaw > 180)
			gaityaw = 180;
		if (gaityaw < -180)
			gaityaw = -180;
	}

	float flYaw = ent->v.angles.y - gaityaw;
	flYaw = flYaw - (int)(flYaw / 360) * 360;
	if (flYaw < -180)
		flYaw = flYaw + 360;
	if (flYaw > 180)
		flYaw = flYaw - 360;

	if (flYaw > 120)
	{
		gaityaw = gaityaw - 180;
		flYaw = flYaw - 180;
	}
	else if (flYaw < -120)
	{
		gaityaw = gaityaw + 180;
		flYaw = flYaw + 180;
	}

	// adjust torso
	uint8_t torso = ((flYaw / 4.0) + 30) / (60.0 / 255.0);
	memset(ent->v.controller, torso, 4);
	ent->v.angles.y = gaityaw;

	static uint8_t crouching_anims[256] = {
		0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, // 0-15
		0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, // 16-31
		0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, // 32-47
		0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, // 48-63
		0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, // 64-79
		0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, // 80-95
		1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, // 96-111
		0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, // 112-127
		1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, // 128-143
		1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, // 144-159
		0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, // 160-175
		1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 176-191
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 192-207
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 208-223
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 224-239
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 240-255
	};

	if (crouching_anims[clamp(ent->v.gaitsequence, 0, 255)]) {
		ent->v.origin.z += 18;
	}
}

void SvenTV::playDemo() {
	if (!replayFile) {
		return;
	}	

	while (readDemoFrame());
}

void SvenTV::initDemoFile() {
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
	memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayer));

	fileDeltaBuffer = new char[fileDeltaBufferSize];
	numFileDeltas = 0;

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
			minModelIdx = i+1;
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

	fwrite(&header, sizeof(DemoHeader), 1, demoFile);

	if (modelData.size())
		fwrite(modelData.c_str(), modelData.size(), 1, demoFile);
	if (soundData.size())
		fwrite(soundData.c_str(), soundData.size(), 1, demoFile);
}

bool SvenTV::writeDemoFile() {
	if (!demoFile) {
		initDemoFile();
	}

	uint64_t now = getEpochMillis();
	if (now < nextDemoUpdate) {
		return false;
	}
	uint64_t updateDelay = (1.0f / demoFileFps) * 1000;
	nextDemoUpdate = now + updateDelay;
	lastDemoFrameTime = now;

	bool isKeyframe = now >= nextDemoKeyframe;

	if (isKeyframe) {
		nextDemoKeyframe = now + (1000ULL * KEYFRAME_INTERVAL);
		memset(fileedicts, 0, MAX_EDICTS * sizeof(netedict));
		memset(fileplayerinfos, 0, 32 * sizeof(DemoPlayer));
	}

	mstream entbuffer(fileDeltaBuffer, fileDeltaBufferSize);

	uint8_t offset = 0; // always write full index for first entity delta

	uint16_t numEntDeltas = 0;
	for (uint16_t i = 0; i < MAX_EDICTS; i++) {
		netedict& now = netedicts[i];

		uint64_t startOffset = entbuffer.tell();

		entbuffer.write(&offset, 1); // entity index the delta is for (offset from previous)
		if (offset == 0) {
			// last edict was 256+ slots away. Write full index.
			entbuffer.write(&i, 2);
		}

		int ret = now.writeDeltas(entbuffer, fileedicts[i]);

		if (ret == EDELTA_OVERFLOW) {
			println("ERROR: Demo file entity delta buffer overflowed. Use a bigger buffer! The demo file is now broken");
			break;
		}
		else if (ret == EDELTA_NONE) {
			// no differences
			entbuffer.seek(startOffset); // undo index write
			if (offset != 0) {
				offset += 1;
			}
		}
		else {
			// delta written
			//println("Write edict %d offset %d bytes %d", i, (int)offset, (int)(entbuffer.tell() - startOffset));
			offset = 1;
			numEntDeltas++;
		}
	}

	int numPlayerDeltas = 0;
	uint32_t plrDeltaBits = 0;
	mstream plrbuffer(filePlayerInfoBuffer, filePlayerInfoBufferSize);
	for (int i = 0; i < gpGlobals->maxClients; i++) {
		int ret = writePlayerDelta(plrbuffer, i, fileplayerinfos[i], playerinfos[i]);

		if (ret == EDELTA_OVERFLOW) {
			println("ERROR: Demo file player delta buffer overflowed. Use a bigger buffer! The demo file is now broken");
			break;
		}
		else if (ret == EDELTA_NONE) {
			// no differences
		}
		else {
			// delta written
			offset = 1;
			plrDeltaBits |= 1 << i;
			numPlayerDeltas++;
		}
	}

	mstream msgbuffer(netmessagesBuffer, netmessagesBufferSize);
	for (int i = 0; i < netmessage_count; i++) {
		NetMessageData& dat = netmessages[i];
		DemoNetMessage msg;

		msg.type = dat.type;
		msg.dest = dat.dest;
		msg.sz = dat.sz;
		msg.hasEdict = dat.hasEdict;
		msg.hasOrigin = dat.hasOrigin;
		
		msgbuffer.write(&msg, sizeof(DemoNetMessage));

		if (msg.hasOrigin) {
			msgbuffer.write(dat.origin, sizeof(float) * 3);
		}
		if (msg.hasEdict) {
			msgbuffer.write(&dat.eidx, sizeof(uint16_t));
		}
		msgbuffer.write(dat.data, dat.sz);
	}
	if (msgbuffer.eom()) {
		println("ERROR: Demo file network message buffer overflowed. Use a bigger buffer! The demo file is now broken");
	}

	mstream cmdbuffer(cmdsBuffer, cmdsBufferSize);
	for (int i = 0; i < cmds_count; i++) {
		CommandData& dat = cmds[i];
		DemoCommand cmd;

		cmd.idx = dat.idx;
		cmd.len = dat.len;
		cmdbuffer.write(&cmd, sizeof(DemoCommand));
		cmdbuffer.write(dat.data, dat.len);
	}
	if (cmdbuffer.eom()) {
		println("ERROR: Demo file command buffer overflowed. Use a bigger buffer! The demo file is now broken");
	}

	memcpy(fileedicts, netedicts, MAX_EDICTS * sizeof(netedict));
	memcpy(fileplayerinfos, playerinfos, 32 * sizeof(DemoPlayer));
	
	int entDeltaSz = entbuffer.tell() + (entbuffer.tell() ? 2 : 0);
	int plrDeltaSz = plrbuffer.tell() + (plrbuffer.tell() ? 4 : 0);
	int msgSz = msgbuffer.tell() + (msgbuffer.tell() ? 2 : 0);
	int cmdSz = cmdbuffer.tell() + (cmdbuffer.tell() ? 2 : 0);
	int totalSz = entDeltaSz + plrDeltaSz + msgSz + cmdSz + sizeof(DemoFrame);
	numFileDeltas++;
	deltaWriteSz += (uint64_t)totalSz;

	uint8_t frameCountDelta = clamp(serverFrameCount - lastServerFrameCount, 0, 255);
	lastServerFrameCount = serverFrameCount;

	DemoFrame header;
	header.deltaFrames = frameCountDelta;
	header.demoTime = now - demoStartTime;
	header.hasEntityDeltas = numEntDeltas > 0;
	header.hasNetworkMessages = netmessage_count > 0;
	header.hasPlayerDeltas = plrDeltaBits > 0;
	header.hasCommands = cmds_count > 0;
	header.isKeyFrame = isKeyframe;
	header.frameSize = totalSz;

	fwrite(&header, sizeof(DemoFrame), 1, demoFile);

	if (header.hasEntityDeltas) {
		fwrite(&numEntDeltas, sizeof(uint16_t), 1, demoFile);
		fwrite(entbuffer.getBuffer(), entbuffer.tell(), 1, demoFile);
	}
	if (header.hasPlayerDeltas) {
		fwrite(&plrDeltaBits, sizeof(uint32_t), 1, demoFile);
		fwrite(plrbuffer.getBuffer(), plrbuffer.tell(), 1, demoFile);
	}
	if (header.hasNetworkMessages) {
		fwrite(&netmessage_count, sizeof(uint16_t), 1, demoFile);
		fwrite(msgbuffer.getBuffer(), msgbuffer.tell(), 1, demoFile);
	}
	if (header.hasCommands) {
		fwrite(&cmds_count, sizeof(uint16_t), 1, demoFile);
		fwrite(cmdbuffer.getBuffer(), cmdbuffer.tell(), 1, demoFile);
	}

	println("%4de %2dp %4dm %4dc    |  %4d + %4d + %4d + %4d = %4d B  |  %.1f MB file  |  %dms copy, %dms think",
		numEntDeltas, numPlayerDeltas, netmessage_count, cmds_count,
		entDeltaSz, plrDeltaSz, msgSz, cmdSz, totalSz,
		(float)((double)deltaWriteSz / (1024.0*1024.0)),
		copyTime, thinkTime);

	return true;
}

void SvenTV::closeDemoFile() {
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

void SvenTV::think_tvThread() {
	bool loadNewData = true;

	while (!threadShouldExit) {
		while (edictCopyState.getValue() != EDICT_COPY_FINISHED && !threadShouldExit) {
			if (demoFile && !enableDemoFile) {
				closeDemoFile();
			}
			this_thread::sleep_for(chrono::milliseconds(1));
		}
		uint64_t startMillis = getEpochMillis();

		if (loadNewData) {
			for (int i = 0; i < MAX_EDICTS; i++) {
				netedicts[i].load(edicts[i]);
			}
			for (int i = 1; i <= gpGlobals->maxClients; i++) {
				DemoPlayer& dplr = playerinfos[i - 1];
				edict_t* ent = INDEXENT(i);

				if (!isValidPlayer(ent)) {
					continue;
				}

				dplr.armorvalue = clamp(ent->v.armorvalue + 0.5f, 0, UINT16_MAX);
				dplr.button = ent->v.button & 0xffff;
				dplr.fov = clamp(ent->v.fov + 0.5f, 0, 255);
				dplr.frags = clamp(ent->v.frags, 0, UINT16_MAX);
				dplr.punchangle[0] = clamp(ent->v.punchangle[0] * 8, INT16_MIN, INT16_MAX);
				dplr.punchangle[1] = clamp(ent->v.punchangle[1] * 8, INT16_MIN, INT16_MAX);
				dplr.punchangle[2] = clamp(ent->v.punchangle[2] * 8, INT16_MIN, INT16_MAX);
				dplr.viewmodel = ent->v.viewmodel;
				dplr.weaponmodel = ent->v.weaponmodel;
				dplr.weaponanim = ent->v.weaponanim;
				dplr.view_ofs = clamp(ent->v.view_ofs[2] * 16, INT16_MIN, INT16_MAX);
				dplr.observer = ((uint8_t)ent->v.iuser2 << 6) | ((ent->v.iuser1 & 0x3) << 1) | (ent->v.deadflag != DEAD_NO);

				int fl = ent->v.flags;
				dplr.flags = (fl & FL_INWATER ? PLR_FL_INWATER : 0)
					| (fl & FL_NOTARGET ? PLR_FL_NOTARGET : 0)
					| (fl & (FL_ONGROUND|FL_PARTIALGROUND) ? PLR_FL_ONGROUND : 0)
					| (fl & FL_WATERJUMP ? PLR_FL_WATERJUMP : 0)
					| (fl & FL_FROZEN ? PLR_FL_FROZEN : 0)
					| (fl & FL_DUCKING ? PLR_FL_DUCKING : 0)
					| (fl & FL_NOWEAPONS ? PLR_FL_NOWEAPONS : 0);
			}
			loadNewData = false;
		}

		bool wantMoreData = false;

		if (enableDemoFile) {
			wantMoreData = wantMoreData || writeDemoFile();
		}
		else if (demoFile) {
			closeDemoFile();
		}

		if (enableServer && !socket) {
			socket = new Socket(SOCKET_UDP | SOCKET_NONBLOCKING, SVENTV_PORT);
		}
		if (socket) {
			wantMoreData = true;
			handleClientPackets();
			broadcastEntityStates();
			updateId++;
		}

		if (wantMoreData) {
			edictCopyState.setValue(EDICT_COPY_REQUESTED);
			loadNewData = true;
			thinkTime = getEpochMillis() - startMillis;
		}
	}

	if (demoFile) {
		closeDemoFile();
	}

	delete socket;
}
