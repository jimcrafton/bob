cmake_minimum_required(VERSION 3.30)

if(POLICY CMP0184)
    cmake_policy(SET CMP0184 NEW)
endif()

project(bob VERSION 0.1.0 LANGUAGES C CXX ASM_MASM)

if(NOT MSVC)
    message(FATAL_ERROR "bob currently builds with MSVC (cl) on Windows x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BOB_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${BOB_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${BOB_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${BOB_OUTPUT_DIR}")

set(BOB_LLVM_ROOT "" CACHE PATH "Root of an LLVM install that contains lib/cmake/llvm (debug LLVM for Debug builds)")
find_package(LLVM CONFIG REQUIRED HINTS "${BOB_LLVM_ROOT}/lib/cmake/llvm")
message(STATUS "bob: using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

find_program(BOB_LLVM_DLLTOOL llvm-dlltool HINTS "${LLVM_TOOLS_BINARY_DIR}" "C:/Program Files/LLVM/bin")
find_program(BOB_LLD_LINK lld-link HINTS "${LLVM_TOOLS_BINARY_DIR}" "C:/Program Files/LLVM/bin")
if(NOT BOB_LLD_LINK)
    message(WARNING "bob: lld-link not found; bobc will need --linker=<path>")
endif()

add_subdirectory(src/runtime)
add_subdirectory(src/compiler)
add_subdirectory(src/tools/bobc)

option(BOB_BUILD_TESTS "Build the bobTests GoogleTest executable" ON)
if(BOB_BUILD_TESTS)
    add_subdirectory(tests)
endif()
