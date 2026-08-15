cmake_minimum_required(VERSION 3.16)

add_library(luadap SHARED
    luadap.c
    coro_registry.c
    dap_framing.c
    dap_json.c
    dap_session.c
    lua_debug.c
    ${CMAKE_SOURCE_DIR}/3rd/cJSON/cJSON.c)

target_include_directories(luadap PRIVATE
    ${LUA_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/3rd/cJSON
    ${CMAKE_SOURCE_DIR}/native/asyncsocket
    ${CMAKE_SOURCE_DIR}/native/common
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(luadap PRIVATE asyncsocket_static lua_compat liblua)
if(WIN32)
    target_link_libraries(luadap PRIVATE ws2_32)
    target_compile_definitions(luadap PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0600
        _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(luadap PROPERTIES
    OUTPUT_NAME "luadap"
    PREFIX "")
