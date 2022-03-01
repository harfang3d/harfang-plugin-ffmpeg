# Copyright (c) NWNC HARFANG and contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.
cmake_minimum_required(VERSION 3.19)

project(hg_ffmpeg VERSION 0.0.1)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(GetPrerequisites)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

IF(UNIX)
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
	if(NOT APPLE)
		set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
		set(CMAKE_SKIP_BUILD_RPATH  FALSE)
		set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
		set(CMAKE_INSTALL_RPATH $ORIGIN)
	endif()
endif()

if(MSVC AND NOT FFMPEG_ROOT)
	message(ERROR "FFMPEG_ROOT is not set")
endif()
find_package(FFMPEG REQUIRED COMPONENTS avutil avcodec avformat avutil avdevice swscale)

set(HG_FFMPEG_SOURCES 
	main.cpp
)

set(HG_FFMPEG_HEADERS
	video_stream_interface.h 
)

add_library(hg_ffmpeg SHARED ${HG_FFMPEG_SOURCES} ${HG_FFMPEG_HEADERS})
target_link_libraries(hg_ffmpeg PRIVATE FFMPEG::avutil FFMPEG::avcodec FFMPEG::avformat FFMPEG::avutil FFMPEG::avdevice FFMPEG::swscale)
set_property(TARGET hg_ffmpeg PROPERTY PREFIX "")
install(TARGETS hg_ffmpeg
	RUNTIME DESTINATION $<CONFIG>
	LIBRARY DESTINATION $<CONFIG>
)
