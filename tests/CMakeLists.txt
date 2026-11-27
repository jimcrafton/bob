include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(bobTests
    astTests.cpp
    driverOptionsTests.cpp
    endToEndTests.cpp
    layoutTests.cpp
    lexerTests.cpp
    parserTests.cpp
    reflectionTests.cpp
    runProgramTests.cpp
    semanticTests.cpp
    syntaxCorpusTests.cpp
    unicodeTests.cpp
    "${PROJECT_SOURCE_DIR}/src/runtime/core/unicode.cpp"
)
target_include_directories(bobTests PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/core")
target_link_libraries(bobTests PRIVATE bobCompiler GTest::gtest_main)
# As for bobCompiler: C4702 fires inside inlined LLVM headers (llvm/ADT/bit.h).
target_compile_options(bobTests PRIVATE /wd4702)
target_compile_definitions(bobTests PRIVATE
    BOB_TEST_RUNTIME_DIR="${BOB_OUTPUT_DIR}"
    BOB_TEST_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs"
)
add_dependencies(bobTests bobrt bobImportLibs)
