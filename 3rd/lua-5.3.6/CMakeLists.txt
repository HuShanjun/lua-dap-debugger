cmake_minimum_required(VERSION 3.20)
project(lua-5.3.6 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LUA_CORE_SRC
  src/lapi.c src/lcode.c src/lctype.c src/ldebug.c src/ldo.c src/ldump.c
  src/lfunc.c src/lgc.c src/llex.c src/lmem.c src/lobject.c src/lopcodes.c
  src/lparser.c src/lstate.c src/lstring.c src/ltable.c src/ltm.c
  src/lundump.c src/lvm.c src/lzio.c
)

set(LUA_LIB_SRC
  src/lauxlib.c src/lbaselib.c src/lbitlib.c src/lcorolib.c src/ldblib.c
  src/liolib.c src/lmathlib.c src/loslib.c src/lstrlib.c src/ltablib.c
  src/lutf8lib.c src/loadlib.c src/linit.c
)

add_library(liblua STATIC ${LUA_CORE_SRC} ${LUA_LIB_SRC})
target_include_directories(liblua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(liblua PUBLIC LUA_COMPAT_5_2)
set_property(TARGET liblua PROPERTY FOLDER "lua")

add_executable(lua src/lua.c)
target_link_libraries(lua PRIVATE liblua)
set_property(TARGET lua PROPERTY FOLDER "lua")

add_executable(luac src/luac.c)
target_link_libraries(luac PRIVATE liblua)
set_property(TARGET luac PROPERTY FOLDER "lua")

if(NOT WIN32)
  target_compile_options(liblua PRIVATE -fPIC)
endif()

if(WIN32)
  # no LUA_USE_*
elseif(APPLE)
  target_compile_definitions(liblua PUBLIC LUA_USE_MACOSX)
  target_link_libraries(liblua PUBLIC m)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(liblua PUBLIC LUA_USE_LINUX)
  target_link_libraries(liblua PUBLIC m dl)
else()
  target_compile_definitions(liblua PUBLIC LUA_USE_POSIX)
  target_link_libraries(liblua PUBLIC m)
endif()
