# C foundation libraries: cjson, circle_buffer

add_library(cjson STATIC cjson/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cjson)
set_property(TARGET cjson PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET cjson PROPERTY FOLDER "native/base")

add_library(circle_buffer STATIC circle_buffer/circle_buffer.c)
target_include_directories(circle_buffer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/circle_buffer)
set_target_properties(circle_buffer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(circle_buffer PRIVATE /std:c11 /experimental:c11atomics)
endif()
set_property(TARGET circle_buffer PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET circle_buffer PROPERTY FOLDER "native/base")

add_executable(circle_buffer_test circle_buffer/circle_buffer_test.c)
target_link_libraries(circle_buffer_test PRIVATE circle_buffer)
set_property(TARGET circle_buffer_test PROPERTY FOLDER "native/base")
