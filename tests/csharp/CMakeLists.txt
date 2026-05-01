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
    DEPENDS interrogate "${hdr}"
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
csharp_test(memory)
csharp_test(nullability)
csharp_test(secondary_base_methods)
csharp_test(streams)
