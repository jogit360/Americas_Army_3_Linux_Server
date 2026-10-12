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

# Encoder round-trip check against the tool's --rep-layout output (see the
# file header for how to produce the two layout files it takes).
add_executable(rep_roundtrip_test
    tests/rep_roundtrip_test.cpp
    src/network/rep_encode.cpp
    src/network/rpc_encode.cpp
    src/network/rpc_decode.cpp
    src/util/bits.cpp
)
target_include_directories(rep_roundtrip_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Client->server RPC decoder vs real retail bunches (see the file header).
add_executable(rpc_decode_pcap_test
    tests/rpc_decode_pcap_test.cpp
    src/network/rpc_decode.cpp
    src/network/rpc_encode.cpp
    src/network/rep_encode.cpp
    src/util/bits.cpp
)
target_include_directories(rpc_decode_pcap_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# LAN beacon wire checks against retail fixtures (tests/fixtures).
add_executable(lan_beacon_test
    tests/lan_beacon_test.cpp
    src/network/lan_beacon.cpp
    src/util/logging.cpp
)
target_include_directories(lan_beacon_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(lan_beacon_test PRIVATE z)

# Collision world check against retail player positions (doc sec 32).
add_executable(collision_test tests/collision_test.cpp src/game/collision.cpp src/util/logging.cpp)
target_include_directories(collision_test PRIVATE include)

add_executable(collision_parity_test tests/collision_parity_test.cpp src/game/collision.cpp
    src/engine/map_bundle.cpp src/engine/map_manifest.cpp src/engine/package_reader.cpp
    src/util/crc.cpp src/util/json.cpp src/util/logging.cpp)
target_include_directories(collision_parity_test PRIVATE include)
target_link_libraries(collision_parity_test PRIVATE ${LZO2_LIBRARY} ${ZSTD_LIBRARY} OpenSSL::Crypto z)

add_executable(package_extractor_test
    tests/package_extractor_test.cpp
    src/engine/map_bundle.cpp
    src/engine/map_manifest.cpp
    src/engine/package_extractor.cpp
    src/engine/package_props.cpp
    src/engine/package_reader.cpp
    src/game/collision.cpp
    src/util/crc.cpp
    src/util/json.cpp
    src/util/logging.cpp
)
target_include_directories(package_extractor_test PRIVATE include)
target_link_libraries(package_extractor_test PRIVATE ${LZO2_LIBRARY} ${ZSTD_LIBRARY} OpenSSL::Crypto z)

add_executable(collision_quant_test
    tests/collision_quant_test.cpp
    src/engine/package_extractor.cpp
    src/engine/package_props.cpp
    src/engine/package_reader.cpp
    src/game/collision.cpp
    src/util/logging.cpp
)
target_include_directories(collision_quant_test PRIVATE include)
target_link_libraries(collision_quant_test PRIVATE ${LZO2_LIBRARY} z)

add_executable(mission_extraction_test
    tests/mission_extraction_test.cpp
    src/engine/map_bundle.cpp
    src/engine/map_manifest.cpp
    src/engine/package_reader.cpp
    src/engine/package_extractor.cpp
    src/engine/package_props.cpp
    src/game/world.cpp
    src/game/weapons.cpp
    src/game/medic.cpp
    src/game/collision.cpp
    src/game/player_manager.cpp
    src/game/stats_export.cpp
    src/game/game_loop.cpp
    src/network/rpc_decode.cpp
    src/network/rpc_encode.cpp
    src/network/rep_encode.cpp
    src/util/bits.cpp
    src/util/crc.cpp
    src/util/logging.cpp
    src/util/json.cpp
)
target_include_directories(mission_extraction_test PRIVATE include ${STEAMWORKS_INCLUDE_DIR})
target_link_libraries(mission_extraction_test PRIVATE ${LZO2_LIBRARY} ${ZSTD_LIBRARY} z OpenSSL::Crypto pthread dl)

# Every baked bundle: class MaxIndex, NetIndex ranges, armed roles, spawn ground.
add_executable(map_bundle_validation_test
    tests/map_bundle_validation_test.cpp
    src/engine/map_bundle.cpp
    src/engine/map_manifest.cpp
    src/engine/mission_extractor.cpp
    src/engine/package_props.cpp
    src/engine/package_reader.cpp
    src/game/collision.cpp
    src/util/crc.cpp
    src/util/json.cpp
    src/util/logging.cpp
)
target_include_directories(map_bundle_validation_test PRIVATE include)
target_link_libraries(map_bundle_validation_test PRIVATE ${LZO2_LIBRARY} ${ZSTD_LIBRARY} z OpenSSL::Crypto pthread dl)

enable_testing()
add_test(NAME collision_parity_test COMMAND collision_parity_test)
add_test(NAME package_extractor_test COMMAND package_extractor_test)
add_test(NAME collision_quant_test COMMAND collision_quant_test)
add_test(NAME mission_extraction_test COMMAND mission_extraction_test)
add_test(NAME map_bundle_validation_test COMMAND map_bundle_validation_test)
set_tests_properties(collision_parity_test package_extractor_test collision_quant_test mission_extraction_test
    map_bundle_validation_test
    PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# Tests that read an AA3 install (packages or files generated from them)
# exit 77 without one: reported as skipped, not failed.
set_tests_properties(collision_parity_test package_extractor_test collision_quant_test
    PROPERTIES SKIP_RETURN_CODE 77
)

