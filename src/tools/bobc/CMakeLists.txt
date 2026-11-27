add_executable(bobc main.cpp)
target_link_libraries(bobc PRIVATE bobCompiler)
add_dependencies(bobc bobrt bobImportLibs)
