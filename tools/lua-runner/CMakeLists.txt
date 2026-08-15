cmake_minimum_required(VERSION 3.16)

add_executable(lua-runner lua_runner.c)
target_include_directories(lua-runner PRIVATE
  ${LUA_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/native/luadap
  ${CMAKE_SOURCE_DIR}/native/common)
target_link_libraries(lua-runner PRIVATE luadap_static liblua)
# WIN32: ws2_32 already via luadap_static transitive or add explicitly
if(WIN32)
    target_link_libraries(lua-runner PRIVATE ws2_32)
    target_compile_definitions(lua-runner PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0600
        _CRT_SECURE_NO_WARNINGS)
endif()
