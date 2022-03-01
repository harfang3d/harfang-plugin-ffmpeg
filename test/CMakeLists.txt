# Copyright (c) NWNC HARFANG and contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.
cmake_minimum_required(VERSION 3.19)

project(hg_vid_play VERSION 0.0.1)

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

set(HG_CPPSDK_PATH CACHE PATH "Path to the Harfang C++ SDK")
get_filename_component(HG_CPPSDK_PATH ${HG_CPPSDK_PATH} ABSOLUTE)

set(HG_ASSETC_PATH CACHE PATH "Path to the Harfang Assets Compiler" )
get_filename_component(HG_ASSETC_PATH ${HG_ASSETC_PATH} ABSOLUTE)

find_package(harfang REQUIRED
	COMPONENTS cppsdk
	PATHS "${HG_CPPSDK_PATH}/cmake" "${HG_CPPSDK_PATH}"
	NO_DEFAULT_PATH
)

set(HG_VIDEO_STREAM_PLUGIN CACHE FILEPATH "Path to the VideoStream plugin")

add_subdirectory(assets)

add_executable(hg_vid_play main.cpp)
target_link_libraries(hg_vid_play PUBLIC hg::engine hg::foundation hg::platform)

add_dependencies(hg_vid_play assets)

if(MSVC)
	set_target_properties(hg_vid_play PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX})
	target_link_options(hg_vid_play PRIVATE /DEBUG)
endif()

install(FILES ${HG_VIDEO_STREAM_PLUGIN} DESTINATION ${CMAKE_INSTALL_PREFIX})
install(TARGETS hg_vid_play DESTINATION ${CMAKE_INSTALL_PREFIX})
install_cppsdk_dependencies(${CMAKE_INSTALL_PREFIX} hg_vid_play)