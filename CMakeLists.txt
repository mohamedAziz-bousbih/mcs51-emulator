cmake_minimum_required(VERSION 3.16)
project(mcs51_emulator VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(MCS51_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
elseif(MSVC)
  set(MCS51_WARNINGS /W4)
endif()

# --- emulator library -------------------------------------------------------
add_library(mcs51
  src/cpu.c
  src/periph.c
  src/opcodes.c
  src/disasm.c
  src/ihex.c
  src/xbus.c
  src/ppi8255.c
  src/board.c)
target_include_directories(mcs51 PUBLIC include)
target_compile_options(mcs51 PRIVATE ${MCS51_WARNINGS})

# --- command line tool ------------------------------------------------------
add_executable(mcs51_cli src/main.c)
set_target_properties(mcs51_cli PROPERTIES OUTPUT_NAME mcs51)
target_link_libraries(mcs51_cli PRIVATE mcs51)
target_compile_options(mcs51_cli PRIVATE ${MCS51_WARNINGS})

# --- tests --------------------------------------------------------------------
option(MCS51_BUILD_TESTS "Build the unit and end-to-end tests" ON)
if(MCS51_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
