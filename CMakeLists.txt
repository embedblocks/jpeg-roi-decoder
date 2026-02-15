idf_component_register(
    SRCS 
        "core/jpeg_roi_decoder_core.c"
        "sys/tjpgd_sys.c"
        "third_party/tjpgd/tjpgd.c"
        "platform/esp/jpeg_roi_decoder_rtos.c"
    
    INCLUDE_DIRS 
        "include"
    
    PRIV_INCLUDE_DIRS
        "sys"
        "third_party/tjpgd"
    
    #REQUIRES 
     #   esp_event
)