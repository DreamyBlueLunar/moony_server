cmake_minimum_required(VERSION 3.10)
project(WebServer)

set(CMAKE_CXX_STANDARD 11)

add_subdirectory(src/moony_coroutine)

add_executable(WebServer main.cpp)