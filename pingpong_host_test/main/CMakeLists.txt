idf_component_register(
    SRCS "uart_echo.c"
    INCLUDE_DIRS "."
    REQUIRES freertos esp_driver_uart
    PRIV_REQUIRES esp_system
)
