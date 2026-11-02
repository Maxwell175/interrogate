# Driver for csharp_interface_order.  Invoked via `cmake -P` from ctest.
# Required -D variables:
#
#   INTERROGATE     Path to the interrogate pass-1 binary
#   INTERROGATE_CS  Path to the interrogate_csharp pass-2 binary
#   PARSER_INC      interrogate's parser-inc directory
#   WORK_DIR        A scratch directory; emptied first
#   HEADER          interface_order.h
#
# Runs both passes over HEADER and reads OrderDerived's generated class.  Its
# secondary bases must appear in interface-name order (Alpha, Bravo, Charlie,
# Leaf) in each of the four places the generator lists them: the interface list,
# the pointer fields, the explicit NativeHandle implementations and the
# constructor's upcasts.  OrderStem, on OrderLeaf's primary chain, must share
# OrderLeaf's pointer.  Exit code 0 means pass.

foreach(var INTERROGATE INTERROGATE_CS PARSER_INC WORK_DIR HEADER)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "RunInterfaceOrderTest.cmake: ${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/pass1" "${WORK_DIR}/cs")

file(STRINGS "${HEADER}" lines REGEX "//\\s*FLAGS:\\s*(.+)")
set(flags "")
if(lines)
  string(REGEX MATCH "//\\s*FLAGS:\\s*(.+)" _ "${lines}")
  separate_arguments(flags UNIX_COMMAND "${CMAKE_MATCH_1}")
endif()

execute_process(
  COMMAND "${INTERROGATE}" --csharp
    -ocs "${WORK_DIR}/pass1"
    -oc "${WORK_DIR}/interface_order.gen.cxx"
    -od "${WORK_DIR}/interface_order.in"
    -module interface_order -library interface_order
    ${flags} -D__cplusplus
    "-S${PARSER_INC}"
    "${HEADER}"
  RESULT_VARIABLE rv
  OUTPUT_QUIET)
if(NOT rv EQUAL 0)
  message(FATAL_ERROR "interrogate pass 1 failed (exit ${rv})")
endif()

execute_process(
  COMMAND "${INTERROGATE_CS}"
    --ocs "${WORK_DIR}/cs"
    --module interface_order --library interface_order
    "${WORK_DIR}/interface_order.in"
  RESULT_VARIABLE rv
  OUTPUT_QUIET)
if(NOT rv EQUAL 0)
  message(FATAL_ERROR "interrogate_csharp pass 2 failed (exit ${rv})")
endif()

file(GLOB_RECURSE derived_files "${WORK_DIR}/cs/OrderDerived.cs")
if(NOT derived_files)
  message(FATAL_ERROR "pass 2 wrote no OrderDerived.cs")
endif()
list(GET derived_files 0 derived_file)
file(READ "${derived_file}" text)

# Fails unless every argument after the first occurs in ${text}, each after the one before it.
function(expect_in_order what)
  set(pos -1)
  set(seen "")
  foreach(needle IN LISTS ARGN)
    string(FIND "${text}" "${needle}" at)
    if(at EQUAL -1)
      message(FATAL_ERROR "${what}: '${needle}' is missing from ${derived_file}")
    endif()
    if(NOT at GREATER pos)
      message(FATAL_ERROR "${what}: '${needle}' comes before '${seen}'; expected the order ${ARGN}")
    endif()
    set(pos ${at})
    set(seen "${needle}")
  endforeach()
endfunction()

string(REGEX MATCH "partial class OrderDerived : [^{]*" class_line "${text}")
if(class_line STREQUAL "")
  message(FATAL_ERROR "no class declaration for OrderDerived in ${derived_file}")
endif()
set(saved_text "${text}")
set(text "${class_line}")
expect_in_order("interface list" "IOrderAlpha" "IOrderBravo" "IOrderCharlie" "IOrderLeaf" "IOrderStem")
set(text "${saved_text}")

expect_in_order("pointer fields"
  "private IntPtr _orderAlphaPtr" "private IntPtr _orderBravoPtr"
  "private IntPtr _orderCharliePtr" "private IntPtr _orderLeafPtr")
expect_in_order("NativeHandle implementations"
  "IOrderAlpha.NativeHandle =>" "IOrderBravo.NativeHandle =>"
  "IOrderCharlie.NativeHandle =>" "IOrderLeaf.NativeHandle =>"
  "IOrderStem.NativeHandle =>")
expect_in_order("constructor upcasts"
  "_orderAlphaPtr = " "_orderBravoPtr = " "_orderCharliePtr = " "_orderLeafPtr = ")

string(FIND "${text}" "IOrderStem.NativeHandle => _orderLeafPtr;" at)
if(at EQUAL -1)
  string(REGEX MATCH "IOrderStem\\.NativeHandle => [^;]*;" stem_line "${text}")
  message(FATAL_ERROR "OrderStem must share OrderLeaf's pointer; found '${stem_line}'")
endif()

message(STATUS "OrderDerived lists its secondary bases in interface-name order")
