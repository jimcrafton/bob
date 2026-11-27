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
    lexerTests.cpp
    parserTests.cpp
    runProgramTests.cpp
    semanticTests.cpp
    syntaxCorpusTests.cpp
)
target_link_libraries(bobTests PRIVATE bobCompiler GTest::gtest_main)
target_compile_definitions(bobTests PRIVATE
    BOB_TEST_RUNTIME_DIR="${BOB_OUTPUT_DIR}"
    BOB_TEST_PROGRAMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/programs"
)
add_dependencies(bobTests bobrt bobImportLibs)
