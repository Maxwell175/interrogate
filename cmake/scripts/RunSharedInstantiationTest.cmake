# Driver for csharp_shared_instantiation.  Invoked via `cmake -P` from ctest.
# Required -D variables:
#
#   BUILD_DIR       The build tree holding the test_shared_instantiation targets
#   WORK_DIR        The test's directory: <lib>.gen.cxx and db/<lib>.in per library
#   INTERROGATE_CS  Path to the interrogate_csharp pass-2 binary
#   EXE             The linked test executable
#
# For each of the two libraries: builds its archive (running pass 1), runs
# pass 2 over its database with both libraries' databases on the search path,
# as a project build does, and reads the entry points its bindings declare for
# the three per-type helper families: Box<int>'s delete helper, and Counted<int>'s
# unref-delete and ref helpers (Counted is reference-counted).  For each family
# the two libraries' entry points must differ, each must be declared against its
# own library and defined by its own supplemental alone.  Then pass 2 is run
# again for a module whose own library emits none of them, which must bind the
# copy of the most-core module that does.  Last, links both archives into one
# executable that calls exactly the entry points the two libraries declared,
# and runs it.  Exit code 0 means pass.

foreach(var BUILD_DIR WORK_DIR INTERROGATE_CS EXE)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "RunSharedInstantiationTest.cmake: ${var} is required")
  endif()
endforeach()

execute_process(
  COMMAND ${CMAKE_COMMAND} --build "${BUILD_DIR}"
    --target test_shared_instantiation_alpha test_shared_instantiation_beta
  RESULT_VARIABLE rv)
if(NOT rv EQUAL 0)
  message(FATAL_ERROR "building the two libraries' archives failed (exit ${rv})")
endif()

# The three helper families, each with the type it is checked for.
set(families BOX_DESTR COUNTED_UNREF COUNTED_REF)
set(prefix_BOX_DESTR "_inCSDestr_")
set(type_BOX_DESTR "Box__int__")
set(prefix_COUNTED_UNREF "_inCSUnrefDestr_")
set(type_COUNTED_UNREF "Counted__int__")
set(prefix_COUNTED_REF "_inCSRef_")
set(type_COUNTED_REF "Counted__int__")

# Runs pass 2 for ${name} into ${cs_out} with ${ARGN} as extra flags, and sets
# <family>_dll and <family>_entry in the caller for each family, from the
# LibraryImport the bindings declare for it.
function(bind_helpers name cs_out search_dir)
  file(REMOVE_RECURSE "${cs_out}")
  file(MAKE_DIRECTORY "${cs_out}")
  execute_process(
    COMMAND "${INTERROGATE_CS}"
      --ocs "${cs_out}"
      --library "${name}" --dllname "${name}"
      --search-dir "${search_dir}"
      ${ARGN}
      "${search_dir}/${name}.in"
    RESULT_VARIABLE rv)
  if(NOT rv EQUAL 0)
    message(FATAL_ERROR "interrogate_csharp pass 2 for ${name} failed (exit ${rv})")
  endif()

  file(GLOB native_methods "${cs_out}/NativeMethods*.cs")
  set(text "")
  foreach(f ${native_methods})
    file(READ "${f}" one)
    string(APPEND text "${one}")
  endforeach()
  set(bound_text "${text}" PARENT_SCOPE)

  foreach(family ${families})
    string(REGEX MATCH
      "LibraryImport\\(\"([^\"]*)\", EntryPoint = \"(${prefix_${family}}[A-Za-z0-9_]*${type_${family}})\"\\)"
      decl "${text}")
    if(NOT decl)
      message(FATAL_ERROR "${name}'s bindings declare no ${prefix_${family}} helper for ${type_${family}}: the libraries no longer share the instantiation, so this test would link nothing shared")
    endif()
    set(${family}_dll "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(${family}_entry "${CMAKE_MATCH_2}" PARENT_SCOPE)
  endforeach()
endfunction()

set(libs alpha beta)
foreach(lib ${libs})
  set(name "shared_instantiation_${lib}")
  bind_helpers("${name}" "${WORK_DIR}/cs_${lib}" "${WORK_DIR}/db" --module "${name}")
  foreach(family ${families})
    set(${family}_entry_${lib} "${${family}_entry}")
    message(STATUS "${name} binds ${${family}_entry} from ${${family}_dll}")
    if(NOT ${family}_dll STREQUAL name)
      message(FATAL_ERROR "${name} binds ${type_${family}}'s ${prefix_${family}} helper from ${${family}_dll}, not from its own library")
    endif()
  endforeach()

  # And the delete helper of the library's own class, which only its own
  # supplemental defines: calling it pulls that object into the link, as the
  # rest of a module's bindings do, so the two libraries' helpers meet there.
  string(SUBSTRING "${lib}" 0 1 first)
  string(TOUPPER "${first}" first)
  string(SUBSTRING "${lib}" 1 -1 rest)
  set(class "${first}${rest}")
  string(REGEX MATCH "EntryPoint = \"(_inCSDestr_[A-Za-z0-9_]*${class})\"" decl "${bound_text}")
  if(NOT decl)
    message(FATAL_ERROR "${name}'s bindings declare no delete helper for ${class}")
  endif()
  set(own_${lib} "${CMAKE_MATCH_1}")
endforeach()

foreach(family ${families})
  if(${family}_entry_alpha STREQUAL ${family}_entry_beta)
    message(FATAL_ERROR "both libraries bind ${type_${family}}'s ${prefix_${family}} helper as ${${family}_entry_alpha}: linked together, their two definitions of it collide")
  endif()

  # Each entry point must be defined by its own library's supplemental, and only there.
  foreach(lib ${libs})
    foreach(other ${libs})
      file(READ "${WORK_DIR}/shared_instantiation_${other}.gen.cxx" text)
      string(FIND "${text}" " ${${family}_entry_${lib}}(" at)
      if(lib STREQUAL other AND at EQUAL -1)
        message(FATAL_ERROR "${${family}_entry_${lib}} is bound by shared_instantiation_${lib} but its supplemental does not define it")
      elseif(NOT lib STREQUAL other AND NOT at EQUAL -1)
        message(FATAL_ERROR "${${family}_entry_${lib}} is defined by shared_instantiation_${other} too")
      endif()
    endforeach()
  endforeach()
endforeach()

# A module that owns the instantiations but whose own library emits no helpers
# for them binds the copy of the most-core module that does, which is in a
# library every module above it links.  Reproduced with the sidecars a build
# leaves: alpha's lists nothing, beta's lists what beta emits, and a third
# library, gamma, lists the same.  Given the module graph twice, beta most core
# and then gamma, alpha must bind beta's names and then names that are neither
# beta's nor its own.
set(fallback "${WORK_DIR}/fallback")
file(REMOVE_RECURSE "${fallback}")
file(MAKE_DIRECTORY "${fallback}")
file(COPY "${WORK_DIR}/db/shared_instantiation_alpha.in" "${WORK_DIR}/db/shared_instantiation_beta.in"
  DESTINATION "${fallback}")
file(READ "${WORK_DIR}/db/shared_instantiation_beta.csharphelpers" beta_helpers)
if(NOT beta_helpers MATCHES "Box< ?int ?>" OR NOT beta_helpers MATCHES "Counted< ?int ?>")
  message(FATAL_ERROR "beta's .csharphelpers does not list Box<int> and Counted<int>:\n${beta_helpers}")
endif()
file(WRITE "${fallback}/shared_instantiation_alpha.csharphelpers" "")
file(WRITE "${fallback}/shared_instantiation_beta.csharphelpers" "${beta_helpers}")
file(WRITE "${fallback}/shared_instantiation_gamma.csharphelpers" "${beta_helpers}")
set(module_map
  --module-map shared_instantiation_alpha=mod_alpha
  --module-map shared_instantiation_beta=mod_beta
  --module-map shared_instantiation_gamma=mod_gamma)

bind_helpers(shared_instantiation_alpha "${WORK_DIR}/cs_fallback_beta" "${fallback}"
  --module mod_alpha ${module_map}
  --module-depends mod_alpha=mod_beta,mod_gamma --module-depends mod_gamma=mod_beta)
foreach(family ${families})
  message(STATUS "with beta most core, alpha binds ${${family}_entry}")
  if(NOT ${family}_entry STREQUAL ${family}_entry_beta)
    message(FATAL_ERROR "alpha, emitting no ${prefix_${family}} helper of its own, binds ${${family}_entry} rather than the most-core module's ${${family}_entry_beta}")
  endif()
endforeach()

bind_helpers(shared_instantiation_alpha "${WORK_DIR}/cs_fallback_gamma" "${fallback}"
  --module mod_alpha ${module_map}
  --module-depends mod_alpha=mod_beta,mod_gamma --module-depends mod_beta=mod_gamma)
foreach(family ${families})
  message(STATUS "with gamma most core, alpha binds ${${family}_entry}")
  if(${family}_entry STREQUAL ${family}_entry_beta OR ${family}_entry STREQUAL ${family}_entry_alpha)
    message(FATAL_ERROR "alpha, emitting no ${prefix_${family}} helper of its own, binds ${${family}_entry} rather than gamma's, the most-core module's")
  endif()
endforeach()

file(WRITE "${WORK_DIR}/shared_instantiation_entry_points.h.new" "")
foreach(family ${families})
  foreach(lib ${libs})
    string(TOUPPER "${lib}" LIB)
    file(APPEND "${WORK_DIR}/shared_instantiation_entry_points.h.new"
      "#define ${LIB}_${family} ${${family}_entry_${lib}}\n")
  endforeach()
endforeach()
file(APPEND "${WORK_DIR}/shared_instantiation_entry_points.h.new"
  "#define ALPHA_DESTR ${own_alpha}\n#define BETA_DESTR ${own_beta}\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${WORK_DIR}/shared_instantiation_entry_points.h.new"
  "${WORK_DIR}/shared_instantiation_entry_points.h")

execute_process(
  COMMAND ${CMAKE_COMMAND} --build "${BUILD_DIR}" --target test_shared_instantiation
  RESULT_VARIABLE rv)
if(NOT rv EQUAL 0)
  message(FATAL_ERROR "linking both archives into one executable failed (exit ${rv})")
endif()

execute_process(COMMAND "${EXE}" RESULT_VARIABLE rv)
if(NOT rv EQUAL 0)
  message(FATAL_ERROR "${EXE} exited ${rv}")
endif()
