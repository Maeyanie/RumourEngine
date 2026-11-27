cmake_minimum_required(VERSION 3.21)

project(
    RumourEngine
    VERSION 1.2.0
    DESCRIPTION "Group-level rumour mills for SkyrimNet"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(SOURCES
    src/Plugin.cpp
    src/SkyrimNet.cpp
    src/Settings.cpp
    src/PluginScan.cpp
    src/Ini.cpp
    src/Mills.cpp
    src/Database.cpp
    src/Session.cpp
    src/Api.cpp
    src/Papyrus.cpp
    src/Rumours.cpp
    src/Delivery.cpp
    src/Gossip.cpp
    src/Promote.cpp
    src/Quests.cpp
    src/GossipStarter.cpp
    src/Spread.cpp
    src/Retell.cpp
    src/Harvest.cpp
    src/Placements.cpp
    src/Runtime.cpp
    src/Text.cpp
)

# A plain DLL, not CommonLib's add_commonlibsse_plugin: that writes the SKSE
# plugin declaration itself and can't set the Address Library format 5 flag
# Skyrim 1.7 needs. src/Plugin.cpp declares the plugin instead.
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)
target_compile_definitions(
    ${PROJECT_NAME}
    PRIVATE
        RUMOURENGINE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        RUMOURENGINE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        RUMOURENGINE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
        ZLIB::ZLIB
)

target_precompile_headers(
    ${PROJECT_NAME}
    PRIVATE
        src/PCH.h
)

# FactionEngine needed this to dodge an MSVC internal compiler error (C1001)
# in CommonLibSSE's heavy templates; the same compiler builds this.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /d2ReducedOptimizeHugeFunctions /utf-8)
endif()

# Copy the DLL into the MO2 mod folder when SKYRIM_MODS_FOLDER is set.
if(DEFINED ENV{SKYRIM_MODS_FOLDER})
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/RumourEngine/SKSE/Plugins")
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PROJECT_NAME}>
            "${OUTPUT_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMENT "Copying ${PROJECT_NAME} to ${OUTPUT_FOLDER}"
    )
endif()
