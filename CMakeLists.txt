cmake_minimum_required(VERSION 3.16)
project(lab9 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-pthread")
include_directories(.)

add_executable(lab9
        proxy.c
        csapp.c
        )
add_executable(sbuf sbuf_test.c sbuf.c csapp.c)