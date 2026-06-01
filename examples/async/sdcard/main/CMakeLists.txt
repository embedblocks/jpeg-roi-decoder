idf_component_register(SRCS main.c sd_mount.c
                    INCLUDE_DIRS .
                   PRIV_REQUIRES jpeg-roi-decoder 
                                driver 
                                fatfs    
                                sdmmc    
            )

# Embed JPEG file into firmware
#target_add_binary_data(${COMPONENT_TARGET}
#    "flower.jpg"
#    BINARY
#)
