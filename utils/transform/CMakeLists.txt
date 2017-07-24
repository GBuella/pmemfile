cmake_minimum_required(VERSION 3.0)
project(transform C)

# needs:
# libtinfo-dev
# zlib1g-dev

#find_package(LLVM CONFIG)
#find_package(Clang CONFIG)
include(FindLibClang.cmake)

#set_target_properties(clang PROPERTIES
#	INTERFACE_INCLUDE_DIRECTORIES "${LLVM_INCLUDE_DIRS}")

add_executable(transform transform.c)
#target_link_libraries(transform PRIVATE clangTooling clangBasic clangASTMatchers)
target_link_libraries(transform PRIVATE clang)
