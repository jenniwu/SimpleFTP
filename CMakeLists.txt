cmake_minimum_required(VERSION 2.8.4)
project(a3_b5u0b_j1o0b)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu99")

add_custom_target(a3_b5u0b_j1o0b COMMAND make -C ${a3_b5u0b_j1o0b_SOURCE_DIR}
        CLION_EXE_DIR=${PROJECT_BINARY_DIR})