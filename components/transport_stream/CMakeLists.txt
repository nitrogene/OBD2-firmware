idf_component_register(
    SRCS "transport_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_ringbuf esp_timer
)
