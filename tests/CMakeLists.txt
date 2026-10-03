# Unit tests of the emulator core and end-to-end tests of the example
# programs. Every test is a small self-checking C program (see testkit.h).

add_library(testkit STATIC testkit.c)
target_link_libraries(testkit PUBLIC mcs51)
target_include_directories(testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Tests load example and vector files from the source tree.
target_compile_definitions(testkit PUBLIC MCS51_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(testkit PRIVATE ${MCS51_WARNINGS})

set(MCS51_TESTS
  test_opcode_table
  test_alu
  test_instructions
  test_interrupts
  test_timers
  test_serial
  test_xbus
  test_ihex
  test_disasm
  test_examples)

foreach(name IN LISTS MCS51_TESTS)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE testkit)
  target_compile_options(${name} PRIVATE ${MCS51_WARNINGS})
  add_test(NAME ${name} COMMAND ${name})
endforeach()

# Command line: the UART example prints its greeting on stdout, and the
# cycle count of the delay example is reported exactly.
add_test(NAME cli_uart_hello
         COMMAND mcs51_cli run ${PROJECT_SOURCE_DIR}/examples/uart_hello.hex)
set_tests_properties(cli_uart_hello PROPERTIES PASS_REGULAR_EXPRESSION "Hello, 8051!")

add_test(NAME cli_delay_cycles
         COMMAND mcs51_cli run ${PROJECT_SOURCE_DIR}/examples/delay.hex --clock 12000000)
set_tests_properties(cli_delay_cycles PROPERTIES
  PASS_REGULAR_EXPRESSION "halted after 10003 machine cycles \\(0\\.010003 s at 12\\.000000 MHz\\)")

add_test(NAME cli_max_cycles
         COMMAND mcs51_cli run ${PROJECT_SOURCE_DIR}/examples/timer0_blink.hex --max-cycles 1000)
set_tests_properties(cli_max_cycles PROPERTIES PASS_REGULAR_EXPRESSION "cycle limit after 100[0-9] machine cycles")

add_test(NAME cli_rejects_bad_file
         COMMAND mcs51_cli run ${PROJECT_SOURCE_DIR}/examples/no-such-file.hex)
set_tests_properties(cli_rejects_bad_file PROPERTIES PASS_REGULAR_EXPRESSION "no-such-file.hex: cannot open file")

add_test(NAME cli_memory_map COMMAND mcs51_cli map)
set_tests_properties(cli_memory_map PROPERTIES
  PASS_REGULAR_EXPRESSION "C000h-DFFFh  PPI 8255 +110xxxxxxxxxxxaa  2048")

# The reserved opcode A5h is reported instead of being executed.
add_test(NAME cli_reserved_opcode
         COMMAND mcs51_cli run ${PROJECT_SOURCE_DIR}/tests/vectors/reserved_opcode.hex)
set_tests_properties(cli_reserved_opcode PROPERTIES
  PASS_REGULAR_EXPRESSION "reserved opcode A5h at 0000h")
