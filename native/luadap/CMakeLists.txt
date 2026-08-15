cmake_minimum_required(VERSION 3.16)

# Embed debugger.lua / dkjson.lua as C byte arrays.
# Task 4 adds luadap.c and the SHARED luadap target.

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    find_program(Python3_EXECUTABLE NAMES python3 python)
endif()
if(NOT Python3_EXECUTABLE)
    message(FATAL_ERROR "Python 3 is required to embed Lua sources into luadap")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/embed_lua.cmake")

set(LUADAP_LUA_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/script/lua-runtime")
set(LUADAP_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(LUADAP_EMBED_DEBUGGER_C "${LUADAP_GEN_DIR}/embedded_debugger.c")
set(LUADAP_EMBED_DKJSON_C "${LUADAP_GEN_DIR}/embedded_dkjson.c")

luadap_embed_lua(
    "${LUADAP_LUA_RUNTIME_DIR}/debugger.lua"
    "${LUADAP_EMBED_DEBUGGER_C}"
    luadap_embedded_debugger)
luadap_embed_lua(
    "${LUADAP_LUA_RUNTIME_DIR}/dkjson.lua"
    "${LUADAP_EMBED_DKJSON_C}"
    luadap_embedded_dkjson)

set(LUADAP_EMBED_SOURCES
    "${LUADAP_EMBED_DEBUGGER_C}"
    "${LUADAP_EMBED_DKJSON_C}")

# Standalone target so the blobs can be generated before luadap.c exists.
add_custom_target(luadap_embed
    DEPENDS ${LUADAP_EMBED_SOURCES}
    COMMENT "Generate embedded Lua C blobs for luadap")
