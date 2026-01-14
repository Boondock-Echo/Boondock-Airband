cmake_minimum_required (VERSION 3.1...3.18 FATAL_ERROR)
project (Boondock-Airband CXX)

execute_process(COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/find_version
   OUTPUT_VARIABLE BOONDOCK_AIRBAND_VERSION
   OUTPUT_STRIP_TRAILING_WHITESPACE
   ERROR_VARIABLE BOONDOCK_AIRBAND_VERSION_ERROR
   RESULT_VARIABLE BOONDOCK_AIRBAND_VERSION_RESULT
   ERROR_STRIP_TRAILING_WHITESPACE)

string(COMPARE EQUAL "${BOONDOCK_AIRBAND_VERSION}" "" BOONDOCK_AIRBAND_VERSION_UNSET)

if(BOONDOCK_AIRBAND_VERSION_UNSET)
   # Fallback to a default version if script fails
   set(BOONDOCK_AIRBAND_VERSION "0.0.0-unknown")
   message(WARNING "Failed to detect BOONDOCK_AIRBAND_VERSION from script (result: ${BOONDOCK_AIRBAND_VERSION_RESULT}, error: ${BOONDOCK_AIRBAND_VERSION_ERROR}), using default: ${BOONDOCK_AIRBAND_VERSION}")
endif()

set (CMAKE_CXX_STANDARD 11)
set (CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_COMPILE_WARNING_AS_ERROR ON)

if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE Release)
   message(STATUS "Build type not specified: defaulting to Release")
endif(NOT CMAKE_BUILD_TYPE)

# Additional Release build optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
   # Link-time optimization for better performance
   include(CheckCXXCompilerFlag)
   CHECK_CXX_COMPILER_FLAG(-flto CXX_HAS_LTO)
   if(CXX_HAS_LTO)
      add_compile_options(-flto)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
      message(STATUS "Link-time optimization (LTO) enabled")
   endif()
   
   # Ensure O3 optimization level
   set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
endif()

# TODO: flags to add: -Wfloat-equal -Wconversion -Wstrict-overflow=5 -Waggregate-return -Wpedantic -Wcast-align
# TODO: these could be added except for gtest: -Wswitch-enum -Wundef -Wswitch-default
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wshadow -Wdate-time -Wpointer-arith -Wwrite-strings -Wcast-qual  -Wunreachable-code -Werror")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Og -DDEBUG")

if(DEBUG_SQUELCH)
   set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG_SQUELCH")
endif()

add_subdirectory (src)
