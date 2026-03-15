idf_component_register(
    SRCS "src/uart_linux.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_ringbuf log freertos
)
