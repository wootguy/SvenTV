cmake_minimum_required(VERSION 3.6)

project(SvenTV)

set(SOURCE_FILES
	src/main.h src/main.cpp
	src/SvenTV.h src/SvenTV.cpp
	src/DemoWriter.h src/DemoWriter.cpp
	src/DemoPlayer.h src/DemoPlayer.cpp
	src/DemoPlayerEnt.h src/DemoPlayerEnt.cpp
	src/DemoStats.h src/DemoStats.cpp
	src/NetClient.h src/NetClient.cpp
	src/netedict.h src/netedict.cpp
	src/DemoFile.h src/DemoFile.cpp
)

add_library(${PROJECT_NAME} SHARED)
add_subdirectory(mmlib)
mmlib_default_setup()
