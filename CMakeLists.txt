cmake_minimum_required(VERSION 3.10)
project(StaticAnalyzer)

# Find LLVM and Clang packages
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

# Add LLVM/Clang include directories
include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Create the executable
add_executable(analyzer CFGBuilder.cpp)

# Link all required Clang and LLVM libraries
target_link_libraries(analyzer PRIVATE
    clangTooling
    clangFrontend
    clangSerialization
    clangDriver
    clangParse
    clangSema
    clangAnalysis
    clangEdit
    clangAST
    clangLex
    clangBasic
)