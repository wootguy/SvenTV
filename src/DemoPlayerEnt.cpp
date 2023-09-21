#include "DemoPlayerEnt.h"
#include "mmlib.h"
#include "DemoFile.h"
#include "main.h"

using namespace std;

#undef read
#undef write

#define READ_DELTA(reader, deltaBits, deltaFlag, field, sz) \
	if (deltaBits & deltaFlag) { \
		g_stats.plrDeltaSz[bitoffset(deltaFlag)] += sz; \
		reader.read((void*)&field, sz); \
	}

#define WRITE_DELTA(writer, deltaBits, deltaFlag, field, sz) \
	if (old.field != field) { \
		deltaBits |= deltaFlag; \
		g_stats.plrDeltaSz[bitoffset(deltaFlag)] += sz; \
		writer.write((void*)&field, sz); \
	}

#define READ_DELTA_STR(reader, deltaBits, deltaFlag, field) \
	if (deltaBits & deltaFlag) { \
		uint8_t len; \
		reader.read(&len, 1); \
		if (len >= sizeof(field)) { \
			println("Invalid ##field length %d", (int)len); \
			len = sizeof(field)-1; \
		} \
		reader.read(strBuffer, len); \
		strBuffer[len] = '\0'; \
		memcpy(field, strBuffer, len + 1); \
		g_stats.plrDeltaSz[bitoffset(deltaFlag)] += len+1; \
	}

#define WRITE_DELTA_STR(writer, deltaBits, deltaFlag, field) \
	if (strcmp(old.field, field)) { \
		deltaBits |= deltaFlag; \
		uint8_t len = strlen(field); \
		g_stats.plrDeltaSz[bitoffset(deltaFlag)] += len+1; \
		writer.write((void*)&len, 1); \
		writer.write((void*)field, len); \
	}

int DemoPlayerEnt::writeDeltas(mstream& writer, const DemoPlayerEnt& old) {
	uint64_t startOffset = writer.tell();

	uint32_t deltaBits = 0; // flags which fields were changed

	writer.skip(3); // write delta bits later
	uint64_t deltaStartOffset = writer.tell();

	WRITE_DELTA(writer, deltaBits, FL_DELTA_CONNECTED, isConnected, 1);

	if (isConnected) {
		if (!old.isConnected) {
			// new player. Start from a fresh state
			memset((void*)(&old), 0, sizeof(DemoPlayerEnt));
		}

		WRITE_DELTA_STR(writer, deltaBits, FL_DELTA_NAME, name);
		WRITE_DELTA_STR(writer, deltaBits, FL_DELTA_MODEL, model);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_STEAMID, steamid64, 8);
		if (old.topColor != topColor || old.bottomColor != bottomColor) {
			deltaBits |= FL_DELTA_COLORS;
			writer.write((void*)&topColor, 1);
			writer.write((void*)&bottomColor, 1);
		}
		WRITE_DELTA(writer, deltaBits, FL_DELTA_PING, ping, 2);
		if (old.pmMoveCounter != pmMoveCounter) {
			deltaBits |= FL_DELTA_PMMOVE;
			uint8_t delta = clamp(pmMoveCounter - old.pmMoveCounter, 0, 255);
			g_stats.plrDeltaSz[bitoffset(FL_DELTA_PMMOVE)] += 1; \
			writer.write((void*)&delta, 1);
		}
		WRITE_DELTA(writer, deltaBits, FL_DELTA_FLAGS, flags, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_PUNCHANGLE_X, punchangle[0], 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_PUNCHANGLE_Y, punchangle[1], 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_PUNCHANGLE_Z, punchangle[2], 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_VIEWMODEL, viewmodel, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_WEAPONMODEL, weaponmodel, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_WEAPONANIM, weaponanim, 1);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_ARMORVALUE, armorvalue, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_BUTTON, button, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_VIEWOFS, view_ofs, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_FRAGS, frags, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_FOV, fov, 1);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_CLIP, clip, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_CLIP2, clip2, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_AMMO, ammo, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_AMMO2, ammo2, 2);
		WRITE_DELTA(writer, deltaBits, FL_DELTA_OBSERVER, observer, 1);
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

	writer.write((void*)&deltaBits, 3);
	writer.seek(currentOffset);

	return EDELTA_WRITE;
}

uint32_t DemoPlayerEnt::readDeltas(mstream& reader) {
	uint32_t deltaBits;
	reader.read(&deltaBits, 3);
	static char strBuffer[256];

	bool oldConnected = isConnected;
	bool newConnected = isConnected;

	READ_DELTA(reader, deltaBits, FL_DELTA_CONNECTED, newConnected, 1);

	if (!oldConnected && newConnected) {
		// new player joined. Start from a fresh state.
		memset(this, 0, sizeof(DemoPlayerEnt));
	}
	isConnected = newConnected;

	if (!isConnected) {
		return deltaBits;
	}

	READ_DELTA_STR(reader, deltaBits, FL_DELTA_NAME, name);
	READ_DELTA_STR(reader, deltaBits, FL_DELTA_MODEL, model);
	READ_DELTA(reader, deltaBits, FL_DELTA_STEAMID, steamid64, 8);
	if (deltaBits & FL_DELTA_COLORS) {
		reader.read(&topColor, 1);
		reader.read(&bottomColor, 1);
	}
	READ_DELTA(reader, deltaBits, FL_DELTA_PING, ping, 1);
	READ_DELTA(reader, deltaBits, FL_DELTA_PMMOVE, pmMoveCounter, 1);
	READ_DELTA(reader, deltaBits, FL_DELTA_FLAGS, flags, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_PUNCHANGLE_X, punchangle[0], 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_PUNCHANGLE_Y, punchangle[1], 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_PUNCHANGLE_Z, punchangle[2], 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_VIEWMODEL, viewmodel, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_WEAPONMODEL, weaponmodel, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_WEAPONANIM, weaponanim, 1);
	READ_DELTA(reader, deltaBits, FL_DELTA_ARMORVALUE, weaponanim, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_BUTTON, button, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_VIEWOFS, view_ofs, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_FRAGS, frags, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_FOV, fov, 1);
	READ_DELTA(reader, deltaBits, FL_DELTA_CLIP, clip, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_CLIP2, clip2, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_AMMO, ammo, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_AMMO2, ammo2, 2);
	READ_DELTA(reader, deltaBits, FL_DELTA_OBSERVER, observer, 1);

	return deltaBits;
}