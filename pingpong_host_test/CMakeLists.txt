cmake_minimum_required(VERSION 3.16)

# Only pull in what we need — keeps the build minimal
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pingpong_host_test)
