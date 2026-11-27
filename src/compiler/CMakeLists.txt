llvm_map_components_to_libnames(bobLlvmLibs
    core irreader support target mc
    x86codegen x86asmparser x86desc x86info
)

add_library(bobCompiler STATIC
    driver/driver.cpp
    driver/driverOptions.cpp
    driver/linker.cpp
    driver/objectEmitter.cpp
    driver/toolchain.cpp
)

target_include_directories(bobCompiler PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src/shared"
)
target_include_directories(bobCompiler SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})

separate_arguments(bobLlvmDefinitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(bobCompiler
    PUBLIC ${bobLlvmDefinitions}
    PRIVATE BOB_VERSION_STRING="${PROJECT_VERSION}"
)
if(BOB_LLD_LINK)
    target_compile_definitions(bobCompiler PRIVATE BOB_DEFAULT_LINKER="${BOB_LLD_LINK}")
endif()

target_compile_options(bobCompiler PUBLIC /W4 /permissive- /utf-8 /Zc:__cplusplus)
target_link_libraries(bobCompiler PUBLIC ${bobLlvmLibs})
