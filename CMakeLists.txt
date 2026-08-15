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

# 多配置生成器（如 Visual Studio）
# foreach(CONFIG IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
#     string(TOUPPER ${CONFIG} _CONFIG)
#     set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
#     set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
#     set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
# endforeach()


if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# include(LuaSelect)

message(STATUS "LUA_VERSION: ${LUA_VERSION}")
if(LUA_VERSION STREQUAL "5.1")
    add_subdirectory(3rd/lua-5.1.5)
    set(LUA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/3rd/lua-5.1.5/src)
elseif(LUA_VERSION STREQUAL "5.2")
    add_subdirectory(3rd/lua-5.2.4)
    set(LUA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/3rd/lua-5.2.4/src)
elseif(LUA_VERSION STREQUAL "5.3")
    add_subdirectory(3rd/lua-5.3.6)
    set(LUA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/3rd/lua-5.3.6/src)
elseif(LUA_VERSION STREQUAL "5.4")
    add_subdirectory(3rd/lua-5.4.8)
    set(LUA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/3rd/lua-5.4.8/inc)
else()
    message(FATAL_ERROR "Unsupported Lua version: ${LUA_VERSION}")
endif()

# message(STATUS "LUA_INCLUDE_DIR: ${LUA_INCLUDE_DIR}")
include_directories(${LUA_INCLUDE_DIR})
include_directories(3rd/sol2/include)
include_directories(3rd/nlohmann/include)

link_directories(${OUTPUT_BIN_DIR})

add_subdirectory(native/common)
add_subdirectory(native/asyncsocket)
add_subdirectory(native/luadap)
add_subdirectory(tools/lua-runner)
add_subdirectory(main)
# 后续可以在这里添加库、子目录和测试
