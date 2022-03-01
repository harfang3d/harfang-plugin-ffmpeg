# Copyright (c) NWNC HARFANG and contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.

add_custom_target(assets ALL
	COMMAND ${HG_ASSETC_PATH}/assetc ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/assets
	WORKING_DIRECTORY ${HG_ASSETC_PATH}
	COMMENT "Build assets"
)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/assets/
	DESTINATION assets
)
