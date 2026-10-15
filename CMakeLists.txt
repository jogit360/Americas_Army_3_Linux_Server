cmake_minimum_required(VERSION 3.16)
project(AA3LinuxServer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -fPIC")

# Steamworks SDK paths.
# Default points at the in-tree SDK shipped one directory up. Override on the
# command line with -DSTEAMWORKS_SDK_DIR=<sdk root>.
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/../steamworks_sdk_164/sdk"
    CACHE PATH "Path to Steamworks SDK root containing public/ and redistributable_bin/")
set(STEAMWORKS_LIB_DIR     "${STEAMWORKS_SDK_DIR}/redistributable_bin/linux64")
set(STEAMWORKS_INCLUDE_DIR "${STEAMWORKS_SDK_DIR}/public")

# SDK 1.64 ships only libsteam_api.so on Linux; GameServer APIs are folded in.
if(NOT EXISTS "${STEAMWORKS_LIB_DIR}/libsteam_api.so")
    message(FATAL_ERROR
        "libsteam_api.so not found at ${STEAMWORKS_LIB_DIR}.\n"
        "Set -DSTEAMWORKS_SDK_DIR=<sdk root> to a directory containing "
        "public/steam/ and redistributable_bin/linux64/libsteam_api.so.")
endif()

set(SERVER_SOURCES
    src/main.cpp
    src/engine/engine_init.cpp
    src/engine/engine_config.cpp
    src/engine/package_reader.cpp
    src/engine/map_manifest.cpp
    src/engine/map_bundle.cpp
    src/auth/steam_auth.cpp
    src/network/network_layer.cpp
    src/network/socket_manager.cpp
    src/network/protocol_handler.cpp
    src/network/rep_encode.cpp
    src/network/rpc_encode.cpp
    src/network/rpc_decode.cpp
    src/network/lan_beacon.cpp
    src/network/a2s_info.cpp
    src/game/game_loop.cpp
    src/game/player_manager.cpp
    src/game/stats_export.cpp
    src/game/world.cpp
    src/game/weapons.cpp
    src/game/medic.cpp
    src/game/objective.cpp
    src/game/reinforcements.cpp
    src/game/npc.cpp
    src/game/navgrid.cpp
    src/game/collision.cpp
    src/util/bits.cpp
    src/util/crc.cpp
    src/util/logging.cpp
    src/util/json.cpp
)

set(SERVER_HEADERS
    include/engine/engine.h
    include/engine/package_reader.h
    include/engine/map_manifest.h
    include/engine/map_bundle.h
    include/auth/steam_auth.h
    include/network/network.h
    include/network/protocol_handler.h
    include/network/a2s_info.h
    include/game/game.h
    include/game/server_actors.h
    include/game/world.h
    include/util/bits.h
    include/util/crc.h
    include/util/logging.h
)

# liblzo2 is required for fully-compressed UE3 package bodies.
find_library(LZO2_LIBRARY NAMES lzo2)
if(NOT LZO2_LIBRARY)
    message(FATAL_ERROR "liblzo2 not found. Install liblzo2-dev (Debian/Ubuntu)\n"
                        "or set -DLZO2_LIBRARY=<path> on the cmake line.")
endif()

# libzstd reads the shipped map bundles (data/maps/<map>.aa3map.zst).
find_library(ZSTD_LIBRARY NAMES zstd)
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "libzstd not found. Install libzstd-dev (Debian/Ubuntu)\n"
                        "or set -DZSTD_LIBRARY=<path> on the cmake line.")
endif()

# OpenSSL provides SHA256 for manifest source hashing.
find_package(OpenSSL REQUIRED)

add_executable(aa3_server ${SERVER_SOURCES})

target_include_directories(aa3_server PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${STEAMWORKS_INCLUDE_DIR}
)

target_link_libraries(aa3_server PRIVATE
    z
    ${STEAMWORKS_LIB_DIR}/libsteam_api.so
    ${LZO2_LIBRARY}
    ${ZSTD_LIBRARY}
    OpenSSL::Crypto
    pthread
    dl
)

set_target_properties(aa3_server PROPERTIES
    BUILD_RPATH "${STEAMWORKS_LIB_DIR}"
    INSTALL_RPATH "${STEAMWORKS_LIB_DIR}"
)

# steam_appid.txt must sit next to the binary at runtime, otherwise
# SteamGameServer_Init fails with "no app id".
configure_file(
    "${CMAKE_SOURCE_DIR}/config/steam_appid.txt"
    "${CMAKE_BINARY_DIR}/steam_appid.txt"
    COPYONLY
)

install(TARGETS aa3_server DESTINATION bin)
install(FILES config/steam_appid.txt DESTINATION bin)

# Offline manifest builder: same package-reader code, no Steam.
add_executable(aa3_build_manifests
    tools/build_manifests/main.cpp
    src/engine/package_reader.cpp
    src/engine/map_manifest.cpp
)
target_include_directories(aa3_build_manifests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(aa3_build_manifests PRIVATE
    ${LZO2_LIBRARY}
    OpenSSL::Crypto
    pthread
    dl
)
install(TARGETS aa3_build_manifests DESTINATION bin)

# Offline map bundler: packages all server map data into .aa3map bundles.
add_executable(aa3_mapbake
    tools/map_bake/main.cpp
    src/engine/mission_extractor.cpp
    src/engine/map_bundle.cpp
    src/engine/map_manifest.cpp
    src/engine/package_reader.cpp
    src/engine/package_extractor.cpp
    src/engine/package_props.cpp
    src/game/collision.cpp
    src/util/crc.cpp
    src/util/json.cpp
    src/util/logging.cpp
)
target_include_directories(aa3_mapbake PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(aa3_mapbake PRIVATE
    ${LZO2_LIBRARY}
    ${ZSTD_LIBRARY}
    OpenSSL::Crypto
    pthread
    dl
)
install(TARGETS aa3_mapbake DESTINATION bin)

# Walkable routes over a bundle's collision (bots; the server's NPCs use
# game/navgrid.cpp too).
add_executable(aa3_navgrid
    tools/navgrid/main.cpp
    src/game/navgrid.cpp
    src/game/collision.cpp
    src/engine/map_bundle.cpp
    src/engine/map_manifest.cpp
    src/engine/package_reader.cpp
    src/util/crc.cpp
    src/util/json.cpp
    src/util/logging.cpp
)
target_include_directories(aa3_navgrid PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(aa3_navgrid PRIVATE ${LZO2_LIBRARY} ${ZSTD_LIBRARY} OpenSSL::Crypto z pthread dl)
