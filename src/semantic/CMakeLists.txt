# bobSemantic: name resolution, types and checking. Like bobSyntax it has no
# LLVM dependency, so tools (language servers, linters) can use the semantic
# model too.
add_library(bobSemantic STATIC
    types.cpp
    semanticModel.cpp
    analyzer.cpp
    analyzerTypes.cpp
    analyzerContainers.cpp
    analyzerReflection.cpp
    analyzerPointers.cpp
    analyzerHardware.cpp
    analyzerConstruction.cpp
    analyzerEscape.cpp
    analyzerCompileTime.cpp
    analyzerModules.cpp
    interfaceWriter.cpp
    layout.cpp
    explainDispatch.cpp
)

target_include_directories(bobSemantic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bobSemantic PUBLIC bobSyntax)
