idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        jpeg-roi-decoder
        driver
        protocol_examples_common
        esp_netif
        esp_http_client
        esp-tls 
        nvs_flash
    )