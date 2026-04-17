# SPDX-License-Identifier: BSD-3-Clause
# cmake/embed_resources.cmake — embed SPA assets into the binary at compile time

function(embed_resource target file varname)
    get_filename_component(abs_file ${file} ABSOLUTE)
    set(out_c "${CMAKE_CURRENT_BINARY_DIR}/${varname}.c")
    set(out_h "${CMAKE_CURRENT_BINARY_DIR}/${varname}.h")

    add_custom_command(
        OUTPUT ${out_c} ${out_h}
        COMMAND xxd -i ${abs_file} > ${out_c}
        DEPENDS ${abs_file}
        COMMENT "Embedding ${file} as ${varname}"
    )

    target_sources(${target} PRIVATE ${out_c})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endfunction()
