#include "main.h"
#include "mmlib.h"
#include <string>
#include "misc_utils.h"
#include "ThreadSafeInt.h"
#include "SvenTV.h"

// Description of plugin
plugin_info_t Plugin_info = {
	META_INTERFACE_VERSION,	// ifvers
	"SvenTV",	// name
	"1.0",	// version
	__DATE__,	// date
	"w00tguy",	// author
	"https://github.com/wootguy/",	// url
	"SVENTV",	// logtag, all caps please
	PT_ANYTIME,	// (when) loadable
	PT_ANYPAUSE,	// (when) unloadable
};

volatile bool g_plugin_exiting = false;
const bool singleThreadMode = false;

SvenTV* g_sventv = NULL;
DemoPlayer* g_demoPlayer = NULL;
DemoPlayerEnt* g_demoplayers = NULL;
NetMessageData* g_netmessages = NULL;
int g_netmessage_count = 0;
CommandData* g_cmds = NULL;
int g_command_count = 0;
DemoEventData* g_events = NULL;
int g_event_count = 0;
uint32_t g_server_frame_count = 0;
int g_copyTime = 0;
volatile int g_thinkTime = 0;
bool g_should_write_next_message = false;

// maps indexes to model names, for all models that were used so far in this map
map<int, string> g_indexToModel;
set<string> g_playerModels; // all player model names used during the game

cvar_t* g_auto_demo_file;
cvar_t* g_demo_file_path;

const char* stateFilePath = "svencoop/addons/metamod/store/sventv.txt";

DemoStats g_stats;
bool demoStatPlayers[33] = { false };

#define EVT_GAUSS_CHARGE 13

struct GaussChargeEvt {
	float time;
	int charge;
};

// gauss charging spams events too much. Use this to slow them down
GaussChargeEvt lastGaussCharge[33];

const char* te_names[TE_NAMES] = {
	"TE_BEAMPOINTS", "TE_BEAMENTPOINT", "TE_GUNSHOT", "TE_EXPLOSION",
	"TE_TAREXPLOSION", "TE_SMOKE", "TE_TRACER", "TE_LIGHTNING",
	"TE_BEAMENTS", "TE_SPARKS", "TE_LAVASPLASH", "TE_TELEPORT",
	"TE_EXPLOSION2", "TE_BSPDECAL", "TE_IMPLOSION", "TE_SPRITETRAIL", 0,
	"TE_SPRITE", "TE_BEAMSPRITE", "TE_BEAMTORUS", "TE_BEAMDISK",
	"TE_BEAMCYLINDER", "TE_BEAMFOLLOW", "TE_GLOWSPRITE", "TE_BEAMRING",
	"TE_STREAK_SPLASH", 0, "TE_DLIGHT", "TE_ELIGHT", "TE_TEXTMESSAGE",
	"TE_LINE", "TE_BOX",

	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,

	"TE_KILLBEAM", "TE_LARGEFUNNEL", "TE_BLOODSTREAM", "TE_SHOWLINE",
	"TE_BLOOD", "TE_DECAL", "TE_FIZZ", "TE_MODEL",
	"TE_EXPLODEMODEL", "TE_BREAKMODEL", "TE_GUNSHOTDECAL", "TE_SPRITE_SPRAY",
	"TE_ARMOR_RICOCHET", "TE_PLAYERDECAL", "TE_BUBBLES", "TE_BUBBLETRAIL",
	"TE_BLOODSPRITE", "TE_WORLDDECAL", "TE_WORLDDECALHIGH", "TE_DECALHIGH",
	"TE_PROJECTILE", "TE_SPRAY", "TE_PLAYERSPRITES", "TE_PARTICLEBURST",
	"TE_FIREFIELD", "TE_PLAYERATTACHMENT", "TE_KILLPLAYERATTACHMENTS",
	"TE_MULTIGUNSHOT", "TE_USERTRACER"
};

void ClientLeave(edict_t* ent) {
	DemoPlayerEnt& plr = g_demoplayers[ENTINDEX(ent) - 1];
	plr.flags = 0;
	demoStatPlayers[ENTINDEX(ent)] = false;
	RETURN_META(MRES_IGNORED);
}

void Changelevel() {
	if (g_sventv) {
		g_sventv->enableDemoFile = false;
		g_demoPlayer->stopReplay();
		g_server_frame_count = 0;
		g_netmessage_count = 0;
		g_server_frame_count = 0;
		g_indexToModel.clear();
		g_playerModels.clear();
	}
	remove(stateFilePath);
}

void writeSvenTvState() {
	FILE* file = fopen(stateFilePath, "w");
	if (!file) {
		println("Failed to write sventv state file");
		return;
	}

	for (auto item : g_indexToModel) {
		string line = to_string(item.first) + "=" + item.second + "\n";
		fwrite(line.c_str(), line.size(), 1, file);
	}

	fclose(file);
}

void loadSvenTvState() {
	FILE* file = fopen(stateFilePath, "r");
	if (!file) {
		println("Failed to open sventv state file");
		return;
	}

	string line;
	bool parsingSounds = false;
	while (cgetline(file, line)) {
		if (line.empty()) {
			continue;
		}

		vector<string> parts = splitString(line, "=");

		if (parts.size() != 2) {
			continue;
		}

		g_indexToModel[atoi(parts[0].c_str())] = parts[1];
	}

	fclose(file);
}

void MapInit(edict_t* pEdictList, int edictCount, int maxClients) {
	if (!g_sventv) {
		g_sventv = new SvenTV(singleThreadMode);
		g_demoPlayer = new DemoPlayer();
	}
	g_demoPlayer->precacheDemo();
	remove(stateFilePath);
	memset(demoStatPlayers, 0, sizeof(demoStatPlayers));
	memset(lastGaussCharge, 0, sizeof(GaussChargeEvt) * MAX_PLAYERS);
	
	RETURN_META(MRES_IGNORED);
}

void MapInit_post(edict_t* pEdictList, int edictCount, int maxClients) {
	loadSoundCacheFile();

	writeSvenTvState();

	RETURN_META(MRES_IGNORED);
}

void loadPlayerInfo(edict_t* pEntity, char* infobuffer) {
	vector<string> parts = splitString(infobuffer, "\\");
	DemoPlayerEnt& plr = g_demoplayers[ENTINDEX(pEntity) - 1];

	for (int i = 1; i < parts.size() - 1; i += 2) {
		string key = parts[i];
		string value = parts[i + 1];

		if (key == "topcolor") {
			plr.topColor = atoi(value.c_str());
		}
		else if (key == "bottomcolor") {
			plr.bottomColor = atoi(value.c_str());
		}
		else if (key == "model") {
			strncpy(plr.model, value.c_str(), 22);
			plr.model[22] = 0;
		}
		else if (key == "name") {
			strncpy(plr.name, value.c_str(), 31);
			plr.name[31] = 0;
			g_playerModels.insert(value);
		}
	}
}

void ClientUserInfoChanged(edict_t* pEntity, char* infobuffer) {
	if (!isValidPlayer(pEntity)) {
		RETURN_META(MRES_IGNORED);
	}

	loadPlayerInfo(pEntity, infobuffer);

	RETURN_META(MRES_IGNORED);
}

float lastPingUpdate = 0;

// http://forum.tsgk.com/viewtopic.php?t=26238
uint64_t getSteamId64(edict_t* ent) {
	string steamid = g_engfuncs.pfnGetPlayerAuthId(ent);
	vector<string> parts = splitString(steamid, ":");
	if (steamid == "STEAM_ID_LAN") {
		return 1;
	}
	else if (steamid == "BOT") {
		return 2;
	}
	else if (parts.size() != 3) {
		return 0; // indicates invalid ID
	}
	else {
		uint64_t x = atoi(parts[1].c_str());
		uint64_t y = atoi(parts[2].c_str());
		return ((y * 2LLU) - x) + 76561197960265728LLU;
	}
}

void StartFrame() {
	handleThreadPrints();
	g_Scheduler.Think();

	g_server_frame_count++;

	if (!g_sventv->enableDemoFile && g_auto_demo_file->value > 0 && gpGlobals->time > 1.0f) {
		g_sventv->enableDemoFile = true;
	}

	for (int i = 1; i <= gpGlobals->maxClients; i++) {
		edict_t* ent = INDEXENT(i);
		if (demoStatPlayers[i]) {
			g_stats.showStats(ent);
		}
	}

	g_demoPlayer->playDemo();

	if (!g_sventv->enableDemoFile && !g_sventv->enableServer) {
		RETURN_META(MRES_IGNORED);
	}

	for (int i = 1; i <= gpGlobals->maxClients; i++) {
		edict_t* ent = INDEXENT(i);
		CBasePlayer* plr = (CBasePlayer*)GET_PRIVATE(ent);

		if (!isValidPlayer(ent) || !plr) {
			continue;
		}

		CBasePlayerWeapon* wep = (CBasePlayerWeapon*)plr->m_hActiveItem.GetEntity();
		DemoPlayerEnt& dplr = g_demoplayers[i - 1];
		dplr.button |= (plr->m_afButtonLast | plr->m_afButtonPressed | plr->m_afButtonReleased | ent->v.button) & 0xffff;

		if (wep) {
			int pammo = wep->m_iPrimaryAmmoType;
			int sammo = wep->m_iSecondaryAmmoType;
			dplr.clip = clamp(wep->m_iClip, 0, 65535);
			dplr.clip2 = clamp(wep->m_iClip2, 0, 65535);
			dplr.ammo = pammo >= 0 && pammo < 64 ? clamp(plr->m_rgAmmo[pammo], 0, 65535) : 0;
			dplr.ammo2 = sammo >= 0 && sammo < 64 ? clamp(plr->m_rgAmmo[sammo], 0, 65535) : 0;
		}
	}

	if (g_engfuncs.pfnTime() - lastPingUpdate > 1.0f) {
		lastPingUpdate = g_engfuncs.pfnTime();

		for (int i = 1; i <= gpGlobals->maxClients; i++) {
			DemoPlayerEnt& plr = g_demoplayers[i - 1];
			edict_t* ent = INDEXENT(i);
			
			if (!isValidPlayer(ent)) {
				plr.flags = 0;
				continue;
			}
			
			int ping;
			int loss;
			g_engfuncs.pfnGetPlayerStats(ent, &ping, &loss);
			plr.ping = clamp(ping, 0, 65535);

			if (plr.steamid64 == 0) {
				// plugin reloaded mid-map
				plr.flags |= PLR_FL_CONNECTED;
				plr.steamid64 = getSteamId64(ent);
				char* infobuffer = g_engfuncs.pfnGetInfoKeyBuffer(ent);
				loadPlayerInfo(ent, infobuffer);
			}
		}
	}

	if (g_sventv) {
		g_sventv->think_mainThread();
	}

	RETURN_META(MRES_IGNORED);
}

void ClientJoin(edict_t* ent) {
	DemoPlayerEnt& plr = g_demoplayers[ENTINDEX(ent) - 1];
	plr.steamid64 = getSteamId64(ent);
	plr.flags |= PLR_FL_CONNECTED;
	
	RETURN_META(MRES_IGNORED);
}

void MessageBegin(int msg_dest, int msg_type, const float* pOrigin, edict_t* ed) {
	//println("NET MESG: %d", msg_type);
	if (!g_sventv->enableDemoFile && !g_sventv->enableServer) {
		g_should_write_next_message = false;
		RETURN_META(MRES_IGNORED);
	}

	g_should_write_next_message = true;

	NetMessageData& msg = g_netmessages[g_netmessage_count];
	msg.header.dest = msg_dest;
	msg.header.type = msg_type;
	if (pOrigin) {
		msg.header.hasOrigin = 1;
		if (abs(pOrigin[0]) > INT16_MAX || abs(pOrigin[1]) > INT16_MAX || abs(pOrigin[2]) > INT16_MAX) {
			msg.header.hasLongOrigin = 1;
			msg.origin[0] = FLOAT_TO_FIXED(pOrigin[0], 19, 5);
			msg.origin[1] = FLOAT_TO_FIXED(pOrigin[1], 19, 5);
			msg.origin[2] = FLOAT_TO_FIXED(pOrigin[2], 19, 5);
		}
		else {
			msg.header.hasLongOrigin = 0;
			msg.origin[0] = (int16_t)pOrigin[0];
			msg.origin[1] = (int16_t)pOrigin[1];
			msg.origin[2] = (int16_t)pOrigin[2];
		}
	}
	else {
		msg.header.hasOrigin = 0;
	}
	if (ed) {
		msg.header.hasEdict = 1;
		msg.eidx = ENTINDEX(ed);
	}
	else {
		msg.header.hasEdict = 0;
	}
	msg.header.sz = 0;
	msg.sz = 0;

	RETURN_META(MRES_IGNORED);
}

void MessageEnd() {
	if (g_should_write_next_message) {
		g_netmessage_count++;

		if (g_netmessage_count >= MAX_NETMSG_FRAME) {
			g_netmessage_count--; // overwrite last message
			println("[SvenTV] Network message capture overflow!");
		}
	}
	RETURN_META(MRES_IGNORED);
}

void WriteAngle(float angle) {
	NetMessageData& msg = g_netmessages[g_netmessage_count];
	if (msg.sz + sizeof(byte) < MAX_NETMSG_DATA) {
		byte dat = (int64)(fmod((double)angle, 360.0) * 256.0 / 360.0) & 0xFF;
		memcpy(msg.data + msg.sz, &angle, sizeof(byte));
		msg.sz += sizeof(byte);
	}
	RETURN_META(MRES_IGNORED);
}

void WriteByte(int b) {
	NetMessageData& msg = g_netmessages[g_netmessage_count];
	if (msg.sz + sizeof(byte) < MAX_NETMSG_DATA) {
		byte dat = b;
		memcpy(msg.data + msg.sz, &dat, sizeof(byte));
		msg.sz += sizeof(byte);
	}
	RETURN_META(MRES_IGNORED);
}

void WriteChar(int c) {
	NetMessageData& msg = g_netmessages[g_netmessage_count];
	if (msg.sz + sizeof(byte) < MAX_NETMSG_DATA) {
		byte dat = c;
		memcpy(msg.data + msg.sz, &dat, sizeof(byte));
		msg.sz += sizeof(byte);
	}
	RETURN_META(MRES_IGNORED);
}

void WriteCoord(float coord) {
	NetMessageData& msg = g_netmessages[g_netmessage_count];
	if (msg.sz + sizeof(float) < MAX_NETMSG_DATA) {
		int32_t arg = coord * 8;
		memcpy(msg.data + msg.sz, &arg, sizeof(int32_t));
		msg.sz += sizeof(int32_t);
	}
	RETURN_META(MRES_IGNORED);
}

void WriteEntity(int ent) {
	NetMessageData& msg = g_netmessages[g_netmessage_count];
	if (msg.sz + sizeof(uint16_t) < MAX_NETMSG_DATA) {
		uint16_t dat = ent;
		memcpy(msg.data + msg.sz, &dat, sizeof(uint16_t));
		msg.sz += sizeof(uint16_t);
	}
	RETURN_META(MRES_IGNORED);
}

void WriteLong(int val) {
	NetMessageData& msg = g_netmessages[g_netmessage_count];
	if (msg.sz + sizeof(int) < MAX_NETMSG_DATA) {
		memcpy(msg.data + msg.sz, &val, sizeof(int));
		msg.sz += sizeof(int);
	}
	RETURN_META(MRES_IGNORED);
}

void WriteShort(int val) {
	NetMessageData& msg = g_netmessages[g_netmessage_count];
	if (msg.sz + sizeof(int16_t) < MAX_NETMSG_DATA) {
		int16_t dat = val;
		memcpy(msg.data + msg.sz, &dat, sizeof(int16_t));
		msg.sz += sizeof(int16_t);
	}
	RETURN_META(MRES_IGNORED);
}

void WriteString(const char* s) {
	NetMessageData& msg = g_netmessages[g_netmessage_count];
	int len = strlen(s)+1;
	if (msg.sz + len < MAX_NETMSG_DATA) {
		memcpy(msg.data + msg.sz, s, len);
		msg.sz += len;
	}
	RETURN_META(MRES_IGNORED);
}

bool doCommand(edict_t* plr) {
	bool isAdmin = AdminLevel(plr) >= ADMIN_YES;
	CommandArgs args = CommandArgs();
	args.loadArgs();
	string lowerArg = toLowerCase(args.ArgV(0));

	if (!isAdmin) {
		return false;
	}

	if (args.ArgC() > 0 && lowerArg == ".replay") {
		if (args.ArgC() > 1 || true) {
			string path = g_demo_file_path->string + args.ArgV(1);
			if (args.ArgV(1).empty()) {
				path += string(STRING(gpGlobals->mapname)) + ".demo";
			}
			float offsetSeconds = args.ArgC() > 2 ? atof(args.ArgV(2).c_str()) : 0;
			g_demoPlayer->openDemo(plr, path, offsetSeconds, true);
			g_sventv->enableDemoFile = false;
		}
		else {
			ClientPrint(plr, HUD_PRINTTALK, "Usage: .demo [demo file path]\n");
		}

		return true;
	}
	if (args.ArgC() > 0 && lowerArg == ".demo") {
		g_sventv->enableDemoFile = !g_sventv->enableDemoFile;
		return true;
	}
	if (args.ArgC() > 0 && lowerArg == ".demostats") {
		demoStatPlayers[ENTINDEX(plr)] = true;
		return true;
	}
	/*
	if (args.ArgC() > 0 && lowerArg == ".bot") {
		edict_t* bot = g_engfuncs.pfnCreateFakeClient("botguy");
		if (bot) {
			gpGamedllFuncs->dllapi_table->pfnClientPutInServer(bot);
			char* infoBuffer = g_engfuncs.pfnGetInfoKeyBuffer(bot);
			g_engfuncs.pfnSetClientKeyValue(ENTINDEX(bot), infoBuffer, "model", "player");
			//g_Scheduler.SetInterval(updateBot, 0.1, -1, EHandle(plr), EHandle(bot));
			h_plr = plr;
			h_bot = bot;
		}
		return true;
	}
	*/
	if (args.ArgC() > 0 && lowerArg == ".kick") {
		for (int i = 1; i <= 32; i++) {
			edict_t* ent = INDEXENT(i);
			if (ent) {
				int userid = g_engfuncs.pfnGetPlayerUserId(ent);
				g_engfuncs.pfnServerCommand(UTIL_VarArgs("kick #%d\n", userid));
			}
		}
		g_engfuncs.pfnServerExecute();
		return true;
	}
	return false;
}

void ClientCommand(edict_t* pEntity) {
	META_RES ret = doCommand(pEntity) ? MRES_SUPERCEDE : MRES_IGNORED;

	if (g_sventv->enableDemoFile || g_sventv->enableServer) {
		string lowerArg0 = toLowerCase(CMD_ARGV(0));
		bool isConsoleCmd = lowerArg0 != "say" && lowerArg0 != "say_team";
		string cmd = CMD_ARGC() > 1 ? CMD_ARGS() : "";
		cmd = CMD_ARGV(0) + string(" ") + cmd;

		CommandData& dat = g_cmds[g_command_count];
		dat.idx = ENTINDEX(pEntity);
		dat.len = cmd.size();
		memcpy(dat.data, cmd.c_str(), cmd.size());

		g_command_count++;
		if (g_command_count >= MAX_CMD_FRAME) {
			println("[SvenTV] Command capture overflow!");
			g_command_count--; // overwrite last command
		}
	}	

	RETURN_META(ret);
}

// maps BSP model indexes
void SetModel(edict_t* e, const char* m) {
	int index = MODEL_INDEX(m);
	g_indexToModel[index] = m;
}

// maps .mdl indexes
int PrecacheModel_post(char* m) {
	int index = MODEL_INDEX(m);
	g_indexToModel[index] = m;
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

void PlaybackEvent(int flags, const edict_t* pInvoker, unsigned short eventindex, float delay, 
	float* origin, float* angles, float fparam1, float fparam2,
	int iparam1, int iparam2, int bparam1, int bparam2) {
	
	if (pInvoker && ENTINDEX(pInvoker) <= gpGlobals->maxClients && eventindex == EVT_GAUSS_CHARGE) {
		GaussChargeEvt& lastEvent = lastGaussCharge[ENTINDEX(pInvoker)];
		float dtime = gpGlobals->time - lastEvent.time;
		if ((lastEvent.charge != iparam1 && dtime > 0.1f) || dtime > 0.2f) {
			lastEvent.time = gpGlobals->time;
			lastEvent.charge = iparam1;
		}
		else {
			// reduce event spam from gauss charging
			RETURN_META(MRES_IGNORED);
		}
	}
	/*
	println("RECORD EVT: %d %d %d %f (%.1f %.1f %.1f) (%.1f %.1f %.1f) %f %f %d %d %d %d",
		flags, pInvoker ? ENTINDEX(pInvoker) : 0, (int)eventindex, delay,
		origin[0], origin[1], origin[2], angles[0], angles[1], angles[2],
		fparam1, fparam2, iparam1, iparam2, bparam1, bparam2);
		*/
	DemoEventData& ev = g_events[g_event_count];
	memset(&ev, 0, (int)sizeof(DemoEventData));
	
	ev.header.eventindex = eventindex;
	ev.header.entindex = pInvoker ? ENTINDEX(pInvoker) : 0;
	ev.header.flags = flags & (~FEV_NOTHOST); // don't skip sending this to listen server host player

	if (origin[0] != 0 || origin[1] != 0 || origin[2] != 0) {
		ev.header.hasOrigin = 1;
		ev.origin[0] = FLOAT_TO_FIXED(origin[0], 21, 3);
		ev.origin[1] = FLOAT_TO_FIXED(origin[1], 21, 3);
		ev.origin[2] = FLOAT_TO_FIXED(origin[2], 21, 3);
	}
	if (angles[0] != 0 || angles[1] != 0 || angles[2] != 0) {
		ev.header.hasAngles = 1;
		ev.angles[0] = angles[0] * 8;
		ev.angles[1] = angles[1] * 8;
		ev.angles[2] = angles[2] * 8;
	}
	if (fparam1 != 0) {
		ev.header.hasFparam1 = 1;
		ev.fparam1 = fparam1 * 128;
	}
	if (fparam2 != 0) {
		ev.header.hasFparam2 = 1;
		ev.fparam2 = fparam2 * 128;
	}
	if (iparam1 != 0) {
		ev.header.hasIparam1 = 1;
		ev.iparam1 = iparam1;
	}
	if (iparam2 != 0) {
		ev.header.hasIparam2 = 1;
		ev.iparam2 = iparam2;
	}
	ev.header.bparam1 = bparam1;
	ev.header.bparam2 = bparam2;

	g_event_count++;
	if (g_event_count >= MAX_EVENT_FRAME) {
		println("[SvenTV] Event capture overflow!");
		g_event_count--; // overwrite last event
	}

	RETURN_META(MRES_IGNORED);
}

void PluginInit() {
	g_plugin_exiting = false;

	g_dll_hooks.pfnServerActivate = MapInit;
	g_dll_hooks_post.pfnServerActivate = MapInit_post;
	g_dll_hooks.pfnServerDeactivate = Changelevel;
	g_dll_hooks.pfnStartFrame = StartFrame;
	g_dll_hooks.pfnClientDisconnect = ClientLeave;
	g_dll_hooks.pfnClientUserInfoChanged = ClientUserInfoChanged;
	g_dll_hooks.pfnClientPutInServer = ClientJoin;
	g_dll_hooks.pfnClientCommand = ClientCommand;

	g_engine_hooks.pfnMessageBegin = MessageBegin;
	g_engine_hooks.pfnWriteAngle = WriteAngle;
	g_engine_hooks.pfnWriteByte = WriteByte;
	g_engine_hooks.pfnWriteChar = WriteChar;
	g_engine_hooks.pfnWriteCoord = WriteCoord;
	g_engine_hooks.pfnWriteEntity = WriteEntity;
	g_engine_hooks.pfnWriteLong = WriteLong;
	g_engine_hooks.pfnWriteShort = WriteShort;
	g_engine_hooks.pfnWriteString = WriteString;
	g_engine_hooks.pfnMessageEnd = MessageEnd;

	g_engine_hooks.pfnPlaybackEvent = PlaybackEvent;

	g_engine_hooks_post.pfnSetModel = SetModel;
	g_engine_hooks_post.pfnPrecacheModel = PrecacheModel_post;

	const char* stringPoolStart = gpGlobals->pStringBase;

	g_main_thread_id = std::this_thread::get_id();

	// start writing demo file automatically when map starts
	g_auto_demo_file = RegisterCVar("sventv.autodemofile", "0", 0, 0);

	g_demo_file_path = RegisterCVar("sventv.demofilepath", "svencoop_addon/scripts/plugins/metamod/SvenTV/", 0, 0);

	if (gpGlobals->time > 3.0f) {
		g_sventv = new SvenTV(singleThreadMode);
		g_demoPlayer = new DemoPlayer();
		loadSoundCacheFile();
		loadSvenTvState();
	}

	g_demoplayers = new DemoPlayerEnt[32];
	g_netmessages = new NetMessageData[MAX_NETMSG_FRAME];
	g_cmds = new CommandData[MAX_CMD_FRAME];
	g_events = new DemoEventData[MAX_EVENT_FRAME];
	memset(g_demoplayers, 0, 32*sizeof(DemoPlayerEnt));
	memset(lastGaussCharge, 0, sizeof(GaussChargeEvt) * MAX_PLAYERS);
}

void PluginExit() {
	writeSvenTvState();
	if (g_sventv) delete g_sventv;
	if (g_demoPlayer) delete g_demoPlayer;
	delete[] g_demoplayers;
	delete[] g_netmessages;
	delete[] g_cmds;
	delete[] g_events;

	println("Plugin exit finish");
}
