# Driver for csharp_pass1_sidecars.  Invoked via `cmake -P` from ctest.
# Required -D variables:
#
#   INTERROGATE     Path to the interrogate pass-1 binary
#   PARSER_INC      interrogate's parser-inc directory
#   WORK_DIR        A scratch directory; emptied first
#   NONE_HEADER     A header that defines no collection
#   SOME_HEADER     A header that defines at least one collection
#
# A build declares each library's .csharpcoll and .csharphelpers sidecars as
# outputs of its pass 1, so pass 1 must write both for every library: a missing
# output makes the build rerun the pass on every build.  The library defining no
# collection starts with a stale .csharpcoll left by an earlier run; afterwards
# both of its sidecars must exist and the .csharpcoll must be empty.  The other
# library's .csharpcoll must list its collections.  Exit code 0 means pass.

foreach(var INTERROGATE PARSER_INC WORK_DIR NONE_HEADER SOME_HEADER)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "RunPass1SidecarTest.cmake: ${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/cs")
file(WRITE "${WORK_DIR}/none.csharpcoll" "Stale\n")

# Runs pass 1 over ${header} as library ${name}, with the flags its first line names.
function(pass1 name header)
  file(STRINGS "${header}" lines REGEX "//\\s*FLAGS:\\s*(.+)")
  set(flags "")
  if(lines)
    string(REGEX MATCH "//\\s*FLAGS:\\s*(.+)" _ "${lines}")
    separate_arguments(flags UNIX_COMMAND "${CMAKE_MATCH_1}")
  endif()
  execute_process(
    COMMAND "${INTERROGATE}" --csharp
      -ocs "${WORK_DIR}/cs"
      -oc "${WORK_DIR}/${name}.gen.cxx"
      -od "${WORK_DIR}/${name}.in"
      -module "${name}" -library "${name}"
      ${flags} -D__cplusplus
      "-S${PARSER_INC}"
      "${header}"
    RESULT_VARIABLE rv
    OUTPUT_QUIET)
  if(NOT rv EQUAL 0)
    message(FATAL_ERROR "interrogate pass 1 for ${name} failed (exit ${rv})")
  endif()
endfunction()

pass1(none "${NONE_HEADER}")
pass1(some "${SOME_HEADER}")

foreach(sidecar none.csharpcoll none.csharphelpers some.csharpcoll some.csharphelpers)
  if(NOT EXISTS "${WORK_DIR}/${sidecar}")
    message(FATAL_ERROR "pass 1 did not write ${sidecar}")
  endif()
endforeach()

file(READ "${WORK_DIR}/none.csharpcoll" none_coll)
if(NOT none_coll STREQUAL "")
  message(FATAL_ERROR "none.csharpcoll should be empty, holds: ${none_coll}")
endif()

file(READ "${WORK_DIR}/some.csharpcoll" some_coll)
if(some_coll STREQUAL "")
  message(FATAL_ERROR "some.csharpcoll is empty, but ${SOME_HEADER} defines collections")
endif()

message(STATUS "sidecars written for both libraries; some.csharpcoll lists: ${some_coll}")
