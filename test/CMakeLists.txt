cmake_minimum_required(VERSION 3.16)

# Host links the same liblua as the project (ABI match) and depends on the
# shared luadap module, which it loads at runtime via package.cpath (bin/?.dll).
add_executable(multi_state_dap_host multi_state_dap_host.c)
target_include_directories(multi_state_dap_host PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(multi_state_dap_host PRIVATE luadap liblua)
add_dependencies(multi_state_dap_host luadap)
target_compile_definitions(multi_state_dap_host PRIVATE
    "LUADAP_BIN_DIR=\"${OUTPUT_BIN_DIR}\""
    "TEST_SCRIPT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
if(WIN32)
    target_compile_definitions(multi_state_dap_host PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0600
        _CRT_SECURE_NO_WARNINGS)
endif()
set_property(TARGET multi_state_dap_host PROPERTY FOLDER "test")
