# in subcomponent CMakeLists.txt
if(NOT DEFINED JPEG_DECODER_THREADING)
    set(JPEG_DECODER_THREADING "async")
endif()

if(JPEG_DECODER_THREADING STREQUAL "async")
    set(ADAPTER_SRC "adapter/async/jpeg_roi_decoder_rtos.c")
else()
    set(ADAPTER_SRC "adapter/sync/jpeg_roi_decoder_rt_sync.c")
endif()

idf_component_register(
    SRCS 
        "core/jpeg_roi_decoder_core.c"
       # "core/jpeg_roi_decoder_helpers.c"
        "sys/tjpgd_sys.c"
        "third_party/tjpgd/tjpgd.c"
        ${ADAPTER_SRC}
    
    INCLUDE_DIRS 
        "include"
    
    PRIV_INCLUDE_DIRS
        "sys"
        "priv_include"
        "third_party/tjpgd"
    
    #REQUIRES 
     #   esp_event
)