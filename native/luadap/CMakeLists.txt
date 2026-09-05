cmake_minimum_required(VERSION 3.16)

set(LUADAP_SOURCES
    luadap.c
    coro_registry.c
    state_registry.c
    dap_framing.c
    dap_json.c
    dap_sync.c
    dap_session.c
    lua_debug.c)

function(luadap_configure target)
    target_include_directories(${target} PRIVATE
        ${LUA_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/native/base/cjson
        ${CMAKE_SOURCE_DIR}/native/lua_compat
        ${CMAKE_SOURCE_DIR}/native/asyncsocket
        ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0600
            _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

add_library(luadap SHARED ${LUADAP_SOURCES})
luadap_configure(luadap)
target_link_libraries(luadap PRIVATE asyncsocket_static lua_compat cjson liblua)
if(WIN32)
    target_link_libraries(luadap PRIVATE ws2_32)
endif()
set_target_properties(luadap PROPERTIES
    OUTPUT_NAME "luadap"
    PREFIX "")

add_library(luadap_static STATIC ${LUADAP_SOURCES})
target_compile_definitions(luadap_static PUBLIC LUADAP_STATIC)
luadap_configure(luadap_static)
target_link_libraries(luadap_static PUBLIC asyncsocket_static lua_compat cjson liblua)
if(WIN32)
    target_link_libraries(luadap_static PUBLIC ws2_32)
endif()

add_executable(coro_registry_test coro_registry_test.c)
luadap_configure(coro_registry_test)
target_link_libraries(coro_registry_test PRIVATE luadap_static liblua)
if(WIN32)
    target_link_libraries(coro_registry_test PRIVATE ws2_32)
endif()
