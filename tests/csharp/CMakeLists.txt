function(csharp_test file)
  get_filename_component(name ${file} NAME_WE)
  set(extra_args)
  if(file MATCHES "[.][ch](pp|xx)$")
    set(extra_args -D__cplusplus)
  endif()

  file(STRINGS "${file}" matched_lines REGEX "//\\s*FLAGS:\\s*(.+)")
  if(matched_lines)
    string(REGEX MATCH "//\\s*FLAGS:\\s*(.+)" _ "" "${matched_lines}")
    set(extra_args "${extra_args} ${CMAKE_MATCH_1}")
  endif()

  set(cs_out_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}_cs")
  set(expected_dir "${CMAKE_CURRENT_SOURCE_DIR}/${name}_expected")

  add_test(NAME csharp_${name}
    COMMAND ${CMAKE_COMMAND}
            "-DOUTPUT_DIR=${cs_out_dir}"
            "-DEXPECTED_DIR=${expected_dir}"
            "-DCOMMAND=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/interrogate --csharp -ocs \"${cs_out_dir}\" -oc \"${CMAKE_CURRENT_BINARY_DIR}/${name}.gen.cxx\" -od \"${CMAKE_CURRENT_BINARY_DIR}/${name}.gen.in\" -module ${name} -library ${name} ${extra_args} \"-S${PROJECT_SOURCE_DIR}/parser-inc\" \"${CMAKE_CURRENT_SOURCE_DIR}/${file}\""
            -P "${PROJECT_SOURCE_DIR}/cmake/scripts/CompareCSharp.cmake"
  )
endfunction()

csharp_test(basic_class.h)
csharp_test(advanced_features.h)
csharp_test(nullability.h)
csharp_test(secondary_base_methods.h)
