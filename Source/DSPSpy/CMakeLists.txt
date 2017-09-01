cmake_minimum_required(VERSION 3.5.0)

project(dspspy)

set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(main
  dsp_interface.cpp
  main_spy.cpp
  real_dsp.cpp
)

target_compile_definitions(main PRIVATE HW_RVL RVL GEKKO)
target_compile_options(main PRIVATE
  -mrvl -mcpu=750 -meabi -mhard-float
  -Wall -fno-exceptions -fno-strict-aliasing
)
target_include_directories(main PRIVATE ${LIBOGCDIR}/include ./)
link_libraries(${LIBOGCDIR}/lib/wii)
target_link_libraries(main PRIVATE
  asnd
  wiiuse
  bte
  fat
  ogc
  m
)

# Be nice, fail early and with a useful error message.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/dsp_code.h")
  message(FATAL_ERROR "Cannot find dsp_code.h. To generate one, run:\n"
                      "dsptool -h dsp_code [DSP assembler file]")
endif()
