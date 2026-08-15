cmake_minimum_required(VERSION 3.16)

project(lua-dap-debugger LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default 5.4 host artifacts stay in source bin/. Other LUA_VERSION / LUA_ROOT
# builds write to the build tree so they cannot overwrite the 5.4 lua.exe.
set(LUA_VERSION "5.4" CACHE STRING "Lua version: 5.1 5.2 5.3 5.4")
set(LUA_ROOT "" CACHE PATH "Optional external Lua prefix or source tree")
if(LUA_VERSION STREQUAL "5.4" AND LUA_ROOT STREQUAL "")
    set(OUTPUT_BIN_DIR ${CMAKE_SOURCE_DIR}/bin)
else()
    set(OUTPUT_BIN_DIR ${CMAKE_BINARY_DIR}/bin)
endif()
# 单配置生成器
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


if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# set(PROJECT_SOURCES
#     src/main.cpp
# )

# add_executable(lua-dap-debugger ${PROJECT_SOURCES})

# target_include_directories(lua-dap-debugger
#     PRIVATE
#         ${CMAKE_CURRENT_SOURCE_DIR}/include
# )

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LuaSelect)

include_directories(${LUA_INCLUDE_DIR})
include_directories(3rd/sol2/include)
include_directories(3rd/nlohmann/include)

link_directories(bin)

add_subdirectory(native/common)
add_subdirectory(native/asyncsocket)
add_subdirectory(native/luadap)
add_subdirectory(tools/lua-runner)

# main (sol2) and luasocket are 5.4-only. sol2/luasocket live under gitignored
# /3rd. Skip them when absent so CI can configure lua + asyncsocket + luadap
# via FetchContent without a vendored tree.
if(LUA_VERSION STREQUAL "5.4")
    if(EXISTS "${CMAKE_SOURCE_DIR}/3rd/luasocket/CMakeLists.txt")
        add_subdirectory(3rd/luasocket)

        # luasocket's CMake omits luasocket.c, so socket.dll has no luaopen_*.
        # Add the module entry and install it where require("socket.core") looks
        # (package.cpath bin/?.dll → bin/socket/core.dll).
        if(TARGET luasocket_shared)
            target_sources(luasocket_shared PRIVATE
                ${CMAKE_SOURCE_DIR}/3rd/luasocket/src/luasocket.c
            )
            add_custom_target(luasocket_core_install ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_BIN_DIR}/socket"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:luasocket_shared>"
                    "${OUTPUT_BIN_DIR}/socket/core.dll"
                DEPENDS luasocket_shared
                COMMENT "Install socket.core.dll for require(\"socket.core\")"
            )
        endif()
    else()
        message(STATUS "Skipping luasocket: 3rd/luasocket not present")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/3rd/sol2/include")
        add_subdirectory(main)
    else()
        message(STATUS "Skipping main: 3rd/sol2 not present")
    endif()
endif()

# 后续可以在这里添加库、子目录和测试
