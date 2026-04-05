cmake_minimum_required(VERSION 3.16)

# Tell ESP-IDF where external components live
set(EXTRA_COMPONENT_DIRS
    ../../../
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sdcard_example)


            