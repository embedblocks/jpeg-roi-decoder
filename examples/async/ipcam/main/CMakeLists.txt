idf_component_register(
    SRCS
        "main.c"
        "lcd_init.c"
        "lcd_output.c"
       
       
       
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_http_client
        esp-tls
        mbedtls
        esp_lcd
        jpeg-roi-decoder
        protocol_examples_common
        esp-lcd-ili9486
)
