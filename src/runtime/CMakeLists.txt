string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

add_library(bobrt STATIC
    crt/crtSupport.cpp
    platform/win64/win64Console.cpp
    platform/win64/win64Entry.cpp
    platform/win64/chkstk.asm
)

target_include_directories(bobrt
    PUBLIC include
    PRIVATE "${PROJECT_SOURCE_DIR}/src/shared" platform/win64
)

# Freestanding: no CRT, no security cookies, no RTTI/EH, no runtime checks,
# no thread-safe statics (those call CRT helpers).
set_target_properties(bobrt PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    MSVC_RUNTIME_CHECKS ""
)
target_compile_options(bobrt PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/GS- /Zl /GR- /EHs-c- /Oi /Zc:threadSafeInit- /W4 /permissive- /utf-8>
)
target_compile_definitions(bobrt PRIVATE
    _HAS_EXCEPTIONS=0 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)

set(BOB_KERNEL32_DEF "${CMAKE_CURRENT_SOURCE_DIR}/platform/win64/kernel32.def")
set(BOB_KERNEL32_LIB "${BOB_OUTPUT_DIR}/kernel32.lib")
if(BOB_LLVM_DLLTOOL)
    add_custom_command(
        OUTPUT "${BOB_KERNEL32_LIB}"
        COMMAND "${BOB_LLVM_DLLTOOL}" -m i386:x86-64 -d "${BOB_KERNEL32_DEF}" -l "${BOB_KERNEL32_LIB}"
        DEPENDS "${BOB_KERNEL32_DEF}"
        COMMENT "Generating kernel32.lib from kernel32.def"
        VERBATIM
    )
    add_custom_target(bobImportLibs ALL DEPENDS "${BOB_KERNEL32_LIB}")
else()
    message(WARNING "bob: llvm-dlltool not found; bobc will fall back to the Windows SDK kernel32.lib")
    add_custom_target(bobImportLibs)
endif()
add_dependencies(bobrt bobImportLibs)
