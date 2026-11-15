idf_component_register(
    SRCS "comm_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash transport_stream
)
