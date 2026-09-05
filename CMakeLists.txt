cmake_minimum_required(VERSION 3.16)

project(lua-dap-debugger LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 单配置生成器
set(OUTPUT_BIN_DIR ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_BIN_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_BIN_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${OUTPUT_BIN_DIR})

# 多配置生成器（如 Visual Studio）
foreach(CONFIG IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${CONFIG} _CONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
endforeach()

message(STATUS "OUTPUT_BIN_DIR: ${OUTPUT_BIN_DIR}")

if(MSVC)
    add_compile_options(/W4 /wd4334)
    add_compile_options(/D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Lua: build selected version under 3rd/ → bin/lua5.x
# Override with -DLUA_INC_DIR=... -DLUA_LIB_DIR=... to use an external Lua
# (skip vendored build when both are already set and LUA_SKIP_VENDORD=ON).
message(STATUS "LUA_VERSION: ${LUA_VERSION}")
if(LUA_SKIP_VENDOR AND LUA_INC_DIR AND LUA_LIB_DIR)
    message(STATUS "Using external Lua: INC=${LUA_INC_DIR} LIB=${LUA_LIB_DIR}")
else()
    add_subdirectory(3rd)
endif()
# Backward-compatible alias used by sample / test / lua-runner
set(LUA_INCLUDE_DIR ${LUA_INC_DIR})

include_directories(${LUA_INC_DIR})
include_directories(3rd/sol2/include)
include_directories(3rd/nlohmann/include)

link_directories(${OUTPUT_BIN_DIR})
link_directories(${LUA_LIB_DIR})

add_subdirectory(native/base)
add_subdirectory(native/lua_compat)
add_subdirectory(native/asyncsocket)
add_subdirectory(native/luadap)
add_subdirectory(native/lua-runner)
add_subdirectory(sample)
add_subdirectory(test)
