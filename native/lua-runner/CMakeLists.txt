cmake_minimum_required(VERSION 3.16)

add_executable(lua-runner lua_runner.c)
target_include_directories(lua-runner PRIVATE
  ${LUA_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/native/luadap
  ${CMAKE_SOURCE_DIR}/native/common
  ${CMAKE_SOURCE_DIR}/3rd/cJSON)
target_link_libraries(lua-runner PRIVATE luadap_static liblua)
# WIN32: ws2_32 already via luadap_static transitive or add explicitly
if(WIN32)
    target_link_libraries(lua-runner PRIVATE ws2_32)
    target_compile_definitions(lua-runner PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0600
        _CRT_SECURE_NO_WARNINGS)
    add_custom_command(TARGET lua-runner POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_SOURCE_DIR}/vscode-extension/bin/win32-x64"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:lua-runner>"
            "${CMAKE_SOURCE_DIR}/vscode-extension/bin/win32-x64/lua-runner.exe"
        COMMENT "Copy lua-runner to vscode-extension/bin/win32-x64")
endif()

