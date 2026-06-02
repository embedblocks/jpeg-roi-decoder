cmake_minimum_required(VERSION 3.16)

# Tell ESP-IDF where external components live
set(EXTRA_COMPONENT_DIRS
    ../../../
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
)

set(JPEG_DECODER_THREADING "sync")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ipcam_example)


            
