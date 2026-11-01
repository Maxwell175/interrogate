# Runtime tests for the interrogate C# backend.
#
# Each test is a directory containing:
#   <name>.h      -- class declarations fed to interrogate
#   <name>.cxx    -- real C++ implementations compiled into a shared lib
#   <name>.cs     -- C# driver that exercises the generated bindings
#
# The test framework:
#   1. Compiles <name>.cxx + interrogate's generated supplemental into
#      libtest_<name>.so, linking against interrogate_support.
#   2. At ctest time, runs interrogate pass 1 + pass 2, writes a tiny csproj
#      that compiles the generated .cs files + the driver, and invokes
#      `dotnet run`.  The driver exits 0 on success.

find_program(DOTNET_EXECUTABLE dotnet)
if(NOT DOTNET_EXECUTABLE)
  message(STATUS "dotnet not found; skipping C# runtime tests")
  return()
endif()

set(CSHARP_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(INTERROGATE_CORE_CSPROJ "${PROJECT_SOURCE_DIR}/csharp/Interrogate.Core/Interrogate.Core.csproj")

# Internal helper: read "//FLAGS: ..." from the header's first line.
function(_csharp_extract_flags header outvar)
  file(STRINGS "${header}" lines REGEX "//\\s*FLAGS:\\s*(.+)")
  if(lines)
    string(REGEX MATCH "//\\s*FLAGS:\\s*(.+)" _ "" "${lines}")
    set(${outvar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  else()
    set(${outvar} "" PARENT_SCOPE)
  endif()
endfunction()

# Register a runtime C# test.  Sources expected at:
#   ${CMAKE_CURRENT_SOURCE_DIR}/<name>.h
#   ${CMAKE_CURRENT_SOURCE_DIR}/<name>.cxx
#   ${CMAKE_CURRENT_SOURCE_DIR}/<name>.cs
function(csharp_test name)
  set(hdr "${CSHARP_TEST_DIR}/${name}.h")
  set(cxx "${CSHARP_TEST_DIR}/${name}.cxx")
  set(driver_cs "${CSHARP_TEST_DIR}/${name}.cs")
  set(command_file "${CSHARP_TEST_DIR}/${name}.N")

  foreach(f "${hdr}" "${cxx}" "${driver_cs}")
    if(NOT EXISTS "${f}")
      message(FATAL_ERROR "csharp_test(${name}): missing ${f}")
    endif()
  endforeach()

  set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
  file(MAKE_DIRECTORY "${work_dir}")

  _csharp_extract_flags("${hdr}" flags)

  # Generate the pass-1 supplemental at configure time (so it's available at
  # build time for the shared lib).  The RunCSharpTest.cmake script regenerates
  # it at test time with the current interrogate binary — they don't conflict,
  # this first run just seeds the build dependency.
  set(gen_cxx "${work_dir}/${name}.gen.cxx")
  set(gen_in "${work_dir}/${name}.gen.in")
  set(cs_out "${work_dir}/cs")

  separate_arguments(extra_args UNIX_COMMAND "${flags}")
  set(pass1_deps interrogate "${hdr}")
  if(EXISTS "${command_file}")
    list(APPEND pass1_deps "${command_file}")
  endif()

  add_custom_command(
    OUTPUT "${gen_cxx}" "${gen_in}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${cs_out}"
    COMMAND
      $<TARGET_FILE:interrogate> --csharp
      -ocs "${cs_out}"
      -oc "${gen_cxx}"
      -od "${gen_in}"
      -module "${name}" -library "${name}"
      ${extra_args} -D__cplusplus
      "-S${PROJECT_SOURCE_DIR}/parser-inc"
      "${hdr}"
    DEPENDS ${pass1_deps}
    COMMENT "interrogate (pass 1) ${name}"
    VERBATIM)

  # Shared library: user impl + interrogate's generated wrappers + the support
  # stream bridge.  ctest's LD_LIBRARY_PATH picks it up at runtime.
  add_library(test_${name} SHARED "${cxx}" "${gen_cxx}" "${hdr}")
  target_include_directories(test_${name} PRIVATE
    "${CSHARP_TEST_DIR}"
    "${CSHARP_TEST_DIR}/support"
    "${PROJECT_SOURCE_DIR}/src/dtoolbase"
    "${PROJECT_SOURCE_DIR}/src/interrogatedb")
  target_link_libraries(test_${name} PRIVATE interrogate_support)
  set_target_properties(test_${name} PROPERTIES
    CXX_STANDARD 17
    OUTPUT_NAME "${name}"
    UNITY_BUILD OFF)
  # Silence -Wunknown-attributes for [[in::nullable]] etc. — interrogate
  # reads these but the C++ compiler doesn't know about them.
  target_compile_options(test_${name} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-unknown-attributes>)

  add_test(NAME csharp_${name}
    COMMAND ${CMAKE_COMMAND}
      "-DNAME=${name}"
      "-DWORK_DIR=${work_dir}"
      "-DDRIVER_CS=${driver_cs}"
      "-DINTERROGATE_CS=$<TARGET_FILE:interrogate_csharp>"
      "-DDOTNET=${DOTNET_EXECUTABLE}"
      "-DINTERROGATE_CORE_CSPROJ=${INTERROGATE_CORE_CSPROJ}"
      "-DNATIVE_LIB_DIR=$<TARGET_FILE_DIR:test_${name}>"
      "-DSUPPORT_LIB_DIR=$<TARGET_FILE_DIR:interrogate_support>"
      -P "${PROJECT_SOURCE_DIR}/cmake/scripts/RunCSharpTest.cmake")
  set_tests_properties(csharp_${name} PROPERTIES
    FIXTURES_REQUIRED "csharp_${name}_lib")

  # The shared library must be built before the test runs.  Encode that via
  # a fixture so `ctest -R csharp_streams` builds test_streams first.
  add_test(NAME csharp_${name}_build_lib
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_${name})
  set_tests_properties(csharp_${name}_build_lib PROPERTIES
    FIXTURES_SETUP "csharp_${name}_lib")
endfunction()

csharp_test(advanced_features)
csharp_test(basic_class)
csharp_test(collections)
csharp_test(keepalive)
csharp_test(memory)
csharp_test(nullability)
csharp_test(refcounting)
csharp_test(secondary_base_methods)
csharp_test(streams)

# Two libraries that both publish the same template instantiations each emit
# their per-type helpers.  Linked statically into one executable -- as a
# NativeAOT executable links every module's archive -- the two copies must be
# different symbols, and each library's C# bindings must name its own copy.  The
# test runs pass 2 for both libraries, takes the entry points their bindings
# declare for Box<int>'s delete helper and the reference-counted Counted<int>'s
# unref-delete and ref helpers, checks that a module emitting none of its own
# binds the most-core module's copy, and links and calls exactly those.
set(_si_dir "${CMAKE_CURRENT_BINARY_DIR}/shared_instantiation")
file(MAKE_DIRECTORY "${_si_dir}/db")
foreach(_lib alpha beta)
  set(_name "shared_instantiation_${_lib}")
  set(_hdr "${CSHARP_TEST_DIR}/${_name}.h")
  _csharp_extract_flags("${_hdr}" _flags)
  separate_arguments(_extra_args UNIX_COMMAND "${_flags}")
  add_custom_command(
    OUTPUT "${_si_dir}/${_name}.gen.cxx" "${_si_dir}/db/${_name}.in"
    BYPRODUCTS "${_si_dir}/db/${_name}.csharphelpers"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_si_dir}/pass1_${_lib}"
    COMMAND
      $<TARGET_FILE:interrogate> --csharp
      -ocs "${_si_dir}/pass1_${_lib}"
      -oc "${_si_dir}/${_name}.gen.cxx"
      -od "${_si_dir}/db/${_name}.in"
      -module "${_name}" -library "${_name}"
      ${_extra_args} -D__cplusplus
      "-S${PROJECT_SOURCE_DIR}/parser-inc"
      "${CSHARP_TEST_DIR}/shared_instantiation.h" "${_hdr}"
    DEPENDS interrogate "${_hdr}" "${CSHARP_TEST_DIR}/shared_instantiation.h"
    COMMENT "interrogate (pass 1) ${_name}"
    VERBATIM)

  add_library(test_${_name} STATIC EXCLUDE_FROM_ALL
    "${CSHARP_TEST_DIR}/${_name}.cxx"
    "${_si_dir}/${_name}.gen.cxx")
  target_include_directories(test_${_name} PRIVATE
    "${CSHARP_TEST_DIR}"
    "${CSHARP_TEST_DIR}/support"
    "${PROJECT_SOURCE_DIR}/src/dtoolbase"
    "${PROJECT_SOURCE_DIR}/src/interrogatedb")
  set_target_properties(test_${_name} PROPERTIES
    CXX_STANDARD 17
    UNITY_BUILD OFF)
endforeach()

add_executable(test_shared_instantiation EXCLUDE_FROM_ALL
  "${CSHARP_TEST_DIR}/shared_instantiation.cxx")
target_include_directories(test_shared_instantiation PRIVATE
  "${CSHARP_TEST_DIR}" "${_si_dir}")
target_link_libraries(test_shared_instantiation PRIVATE
  test_shared_instantiation_alpha test_shared_instantiation_beta interrogate_support)
set_target_properties(test_shared_instantiation PROPERTIES CXX_STANDARD 17)

add_test(NAME csharp_shared_instantiation
  COMMAND ${CMAKE_COMMAND}
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DWORK_DIR=${_si_dir}"
    "-DINTERROGATE_CS=$<TARGET_FILE:interrogate_csharp>"
    "-DEXE=$<TARGET_FILE:test_shared_instantiation>"
    -P "${PROJECT_SOURCE_DIR}/cmake/scripts/RunSharedInstantiationTest.cmake")
