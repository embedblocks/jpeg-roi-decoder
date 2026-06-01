idf_component_register(SRCS main.c
                    INCLUDE_DIRS .
                    
                   REQUIRES jpeg-roi-decoder driver
            )

# Embed JPEG file into firmware
target_add_binary_data(${COMPONENT_TARGET}
    "test_image.jpg"
    BINARY
)
