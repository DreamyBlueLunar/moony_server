cmake_minimum_required(VERSION 3.10)
PROJECT(coroutine)

AUX_SOURCE_DIRECTORY(. SRC_LIST)

ADD_EXECUTABLE(test_co ${SRC_LIST})

target_link_libraries(test_co dl pthread)
