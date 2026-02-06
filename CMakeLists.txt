idf_component_register(
    SRCS 
        "src/jpeg_roi_decoder.c"
        "sys/tjpgd_sys.c"
        "third_party/tjpgd/tjpgd.c"
    
    INCLUDE_DIRS 
        "include"
    
    PRIV_INCLUDE_DIRS
        "sys"
        "third_party/tjpgd"
    
    #REQUIRES 
     #   esp_event
)