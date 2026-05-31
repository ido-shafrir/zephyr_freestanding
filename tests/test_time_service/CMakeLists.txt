cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test_time_service)

target_sources(app PRIVATE
    src/main.c
    src/stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/time_service.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
