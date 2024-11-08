#pragma once
#include "DemoFile.h"
#include "netedict.h"

struct DemoDataTest;

class DemoWriter {
public:
	int showStats = 0;
	std::string fpath;

	DemoWriter();
	~DemoWriter();

	void initDemoFile();

	// return true if more data requested
	bool writeDemoFile(FrameData& frame);

	bool shouldWriteDemoFrame();

	void closeDemoFile();

	bool isFileOpen();

	bool validateEdicts(); // debug

	void compressDemo(std::string inPath, std::string outPath);

private:
	std::thread* compress_thread = NULL;
	void compressNetMessage(FrameData& frame, NetMessageData& msg);

	mstream writeEntDeltas(FrameData& frame, uint16_t& numEntDeltas, DemoDataTest* testData);
	mstream writePlrDeltas(FrameData& frame, uint32_t& numEntDeltas);
	mstream writeMsgDeltas(FrameData& frame, DemoDataTest* testData);
	mstream writeCmdDeltas(FrameData& frame);
	mstream writeEvtDeltas(FrameData& frame);

	// vars for writing a demo file
	uint64_t nextDemoUpdate = 0;
	float demoFileFps = 60;
	FILE* demoFile = NULL;
	uint64_t nextDemoKeyframe = 0;
	uint32_t lastServerFrameCount = 0;
	uint64_t demoStartTime = 0;
	uint64_t lastDemoFrameTime = 0;
	uint32_t stringPoolOffset = 0; // location of the string pool in the file
	uint32_t lastStringPoolIdx = 0; // for detecting if the string pool needs updating in the file

	DemoPlayerEnt* fileplayerinfos = NULL;
	char* filePlayerInfoBuffer = NULL;
	int filePlayerInfoBufferSize = -1;

	netedict* fileedicts = NULL; // last edicts written to file
	char* fileDeltaBuffer = NULL;
	int fileDeltaBufferSize = -1;

	char* netmessagesBuffer = NULL;
	int netmessagesBufferSize = -1;

	char* cmdsBuffer = NULL;
	int cmdsBufferSize = -1;

	char* eventsBuffer = NULL;
	int eventsBufferSize = -1;
};