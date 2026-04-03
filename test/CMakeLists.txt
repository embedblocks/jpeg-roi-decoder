idf_component_register(SRCS "test_jpeg_roi.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES unity jpeg-roi-decoder)


# Embed JPEG file into firmware
target_add_binary_data(${COMPONENT_TARGET} "test_16x16.jpg" BINARY)
target_add_binary_data(${COMPONENT_TARGET} "test_320x240.jpg" BINARY)