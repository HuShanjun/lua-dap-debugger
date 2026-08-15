# Embed a Lua source file as a C byte-array in the build tree.
#
#   luadap_embed_lua(<lua_abs_path> <output_c> <symbol_prefix>)
#
# Requires Python3_EXECUTABLE (Interpreter). Regenerates when the .lua
# source or gen_embed.py changes.

set(LUADAP_GEN_EMBED_PY "${CMAKE_CURRENT_LIST_DIR}/gen_embed.py")

function(luadap_embed_lua lua_file output_c symbol_prefix)
    if(NOT Python3_EXECUTABLE)
        message(FATAL_ERROR "luadap_embed_lua: Python3_EXECUTABLE is not set")
    endif()
    if(NOT EXISTS "${LUADAP_GEN_EMBED_PY}")
        message(FATAL_ERROR "luadap_embed_lua: missing ${LUADAP_GEN_EMBED_PY}")
    endif()

    get_filename_component(_out_dir "${output_c}" DIRECTORY)
    add_custom_command(
        OUTPUT "${output_c}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
        COMMAND ${Python3_EXECUTABLE}
            "${LUADAP_GEN_EMBED_PY}"
            "${lua_file}"
            "${output_c}"
            "${symbol_prefix}"
        DEPENDS
            "${lua_file}"
            "${LUADAP_GEN_EMBED_PY}"
        COMMENT "Embedding ${lua_file} -> ${output_c}"
        VERBATIM
    )
endfunction()
