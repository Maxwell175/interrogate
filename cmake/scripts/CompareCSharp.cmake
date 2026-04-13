cmake_policy(PUSH)
cmake_policy(SET CMP0007 NEW)

if(NOT DEFINED OUTPUT_DIR OR NOT DEFINED EXPECTED_DIR OR NOT DEFINED COMMAND)
  message(FATAL_ERROR "Usage: cmake -DOUTPUT_DIR=... -DEXPECTED_DIR=... -DCOMMAND=\"cmd\" -P CompareCSharp.cmake")
endif()

set(ENV{SOURCE_DATE_EPOCH} 0)
file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
separate_arguments(COMMAND_LIST UNIX_COMMAND "${COMMAND}")
execute_process(COMMAND ${COMMAND_LIST} RESULT_VARIABLE run_result)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "Command failed (${run_result}): ${COMMAND}")
endif()

file(GLOB expected_files "${EXPECTED_DIR}/*.cs")
list(LENGTH expected_files num_expected)
if(num_expected EQUAL 0)
  message(FATAL_ERROR "No expected .cs files found in ${EXPECTED_DIR}")
endif()

set(failures "")

foreach(expected_file ${expected_files})
  get_filename_component(filename ${expected_file} NAME)
  set(generated_file "${OUTPUT_DIR}/${filename}")

  if(NOT EXISTS "${generated_file}")
    list(APPEND failures "MISSING: ${filename}")
    continue()
  endif()

  execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files "${generated_file}" "${expected_file}"
    RESULT_VARIABLE cmp_result
  )
  if(NOT cmp_result EQUAL 0)
    list(APPEND failures "${filename}")
    execute_process(
      COMMAND diff -u "${expected_file}" "${generated_file}"
      OUTPUT_VARIABLE diff_out ERROR_VARIABLE diff_err
    )
    message("--- ${filename} ---\n${diff_out}${diff_err}")
  endif()
endforeach()

file(GLOB generated_files "${OUTPUT_DIR}/*.cs")
foreach(generated_file ${generated_files})
  get_filename_component(filename ${generated_file} NAME)
  set(expected_file "${EXPECTED_DIR}/${filename}")
  if(NOT EXISTS "${expected_file}")
    list(APPEND failures "UNEXPECTED: ${filename}")
  endif()
endforeach()

list(LENGTH failures num_failures)
if(num_failures GREATER 0)
  string(REPLACE ";" "\n  " failure_list "${failures}")
  message(FATAL_ERROR "C# test failed:\n  ${failure_list}")
endif()

cmake_policy(POP)
