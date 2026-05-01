# Driver script for a single C# runtime test.  Invoked via `cmake -P` from
# the ctest add_test command.  Required -D variables:
#
#   NAME              Test name (matches the .h / .cxx / .cs basename)
#   WORK_DIR          Per-test build directory (scratch)
#   DRIVER_CS         Absolute path to the test's driver .cs file
#   INTERROGATE_CS    Path to the interrogate_csharp pass-2 binary
#   DOTNET            Path to the dotnet binary
#   INTERROGATE_CORE_CSPROJ  Absolute path to Interrogate.Core.csproj
#   NATIVE_LIB_DIR    Directory containing libtest_<name>.so (added to LD_LIBRARY_PATH)
#   SUPPORT_LIB_DIR   Directory containing libinterrogate_support.so
#
# Pass 1 (interrogate) already ran at cmake build time via add_custom_command,
# producing <WORK_DIR>/<name>.gen.in which was then compiled into
# libtest_<name>.so.  Re-running pass 1 here would overwrite that generated
# supplemental with a different hash — the already-linked .so would then be
# stale and its exports wouldn't match the newly-generated pinvoke names.
#
# This script:
#   1. Runs interrogate_csharp pass 2 to populate <WORK_DIR>/cs/ from the
#      existing .gen.in (same names as the .so exports).
#   2. Writes a tiny driver .csproj that references Interrogate.Core and
#      compiles the generated cs/*.cs alongside DRIVER_CS.
#   3. Invokes `dotnet run`, with LD_LIBRARY_PATH set so the runtime can
#      find libtest_<name>.so and libinterrogate_support.so.
#
# Exit code 0 means pass; anything else fails the test.

cmake_policy(PUSH)
cmake_policy(SET CMP0007 NEW)

foreach(var NAME WORK_DIR DRIVER_CS INTERROGATE_CS DOTNET INTERROGATE_CORE_CSPROJ NATIVE_LIB_DIR SUPPORT_LIB_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "RunCSharpTest.cmake: ${var} is required")
  endif()
endforeach()

set(gen_in "${WORK_DIR}/${NAME}.gen.in")
if(NOT EXISTS "${gen_in}")
  message(FATAL_ERROR "${gen_in} missing — build the test_${NAME} target first")
endif()

set(ENV{SOURCE_DATE_EPOCH} 0)
set(cs_out "${WORK_DIR}/cs")
# Wipe previous run's generated bindings so stale files can't leak in.
file(REMOVE_RECURSE "${cs_out}")
file(MAKE_DIRECTORY "${cs_out}")

# Pass 2 only — final .cs files from the build-time .in database.
execute_process(
  COMMAND
    "${INTERROGATE_CS}"
    --ocs "${cs_out}"
    --module "${NAME}" --library "${NAME}"
    "${gen_in}"
  RESULT_VARIABLE rv)
if(NOT rv EQUAL 0)
  message(FATAL_ERROR "interrogate_csharp pass 2 failed (exit ${rv})")
endif()

# Emit a minimal csproj that compiles driver + generated bindings and runs.
set(project_file "${WORK_DIR}/${NAME}_driver.csproj")
file(WRITE "${project_file}" "<Project Sdk=\"Microsoft.NET.Sdk\">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <Nullable>enable</Nullable>
    <!-- Disable default Compile glob so we don't double-include generated
         .cs files from cs/, which live under the project directory. -->
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <NoWarn>\$(NoWarn);CS0108;CS8981</NoWarn>
  </PropertyGroup>
  <ItemGroup>
    <ProjectReference Include=\"${INTERROGATE_CORE_CSPROJ}\" />
  </ItemGroup>
  <ItemGroup>
    <Compile Include=\"${cs_out}/*.cs\" />
    <Compile Include=\"${DRIVER_CS}\" />
  </ItemGroup>
</Project>
")

# Run the driver.
if(WIN32)
  set(path_sep ";")
else()
  set(path_sep ":")
endif()
set(ENV{LD_LIBRARY_PATH} "${NATIVE_LIB_DIR}${path_sep}${SUPPORT_LIB_DIR}${path_sep}$ENV{LD_LIBRARY_PATH}")
set(ENV{DYLD_LIBRARY_PATH} "${NATIVE_LIB_DIR}${path_sep}${SUPPORT_LIB_DIR}${path_sep}$ENV{DYLD_LIBRARY_PATH}")

execute_process(
  COMMAND "${DOTNET}" run --project "${project_file}" -c Release
  WORKING_DIRECTORY "${WORK_DIR}"
  RESULT_VARIABLE rv)
if(NOT rv EQUAL 0)
  message(FATAL_ERROR "csharp test driver failed (exit ${rv})")
endif()

cmake_policy(POP)
