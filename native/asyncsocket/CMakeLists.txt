cmake_minimum_required(VERSION 3.16)

# Requires (from root / 3rd):
#   LUA_INC_DIR — Lua headers
#   LUA_LIB_DIR — directory containing liblua (bin/lua5.x)

if(NOT LUA_INC_DIR)
    message(FATAL_ERROR "asyncsocket: LUA_INC_DIR is not set")
endif()
if(NOT LUA_LIB_DIR)
    message(FATAL_ERROR "asyncsocket: LUA_LIB_DIR is not set")
endif()

set(ASYNCSOCKET_SOURCES asyncsocket.c poll_loop.c)

function(asyncsocket_link_lua target)
    target_include_directories(${target} PRIVATE ${LUA_INC_DIR})
    target_link_directories(${target} PRIVATE ${LUA_LIB_DIR})
    if(TARGET liblua)
        target_link_libraries(${target} PRIVATE liblua)
    else()
        find_library(_AS_LUA_LIB NAMES liblua lua PATHS ${LUA_LIB_DIR} NO_DEFAULT_PATH)
        if(NOT _AS_LUA_LIB)
            message(FATAL_ERROR "asyncsocket: liblua not found in LUA_LIB_DIR=${LUA_LIB_DIR}")
        endif()
        target_link_libraries(${target} PRIVATE ${_AS_LUA_LIB})
    endif()
endfunction()

function(asyncsocket_configure target)
    target_include_directories(${target} PRIVATE
        ${LUA_INC_DIR}
        ${CMAKE_SOURCE_DIR}/native/lua_compat)
    target_link_directories(${target} PRIVATE ${LUA_LIB_DIR})
    target_link_libraries(${target} PRIVATE lua_compat)
    asyncsocket_link_lua(${target})

    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0600
            _CRT_SECURE_NO_WARNINGS)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endif()
endfunction()

add_library(asyncsocket_static STATIC ${ASYNCSOCKET_SOURCES})
target_compile_definitions(asyncsocket_static PRIVATE ASYNCSOCKET_STATIC)
asyncsocket_configure(asyncsocket_static)

add_library(asyncsocket SHARED ${ASYNCSOCKET_SOURCES})
set_target_properties(asyncsocket PROPERTIES
    OUTPUT_NAME "asyncsocket"
    PREFIX "")
asyncsocket_configure(asyncsocket)
