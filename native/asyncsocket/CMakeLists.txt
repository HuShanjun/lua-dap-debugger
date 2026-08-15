cmake_minimum_required(VERSION 3.16)

set(ASYNCSOCKET_SOURCES asyncsocket.c poll_loop.c)

function(asyncsocket_configure target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/3rd/lua-5.4.8/inc)

    if(TARGET liblua)
        target_link_libraries(${target} PRIVATE liblua)
    endif()

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
