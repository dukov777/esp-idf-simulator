idf_component_register(
    SRCS "pingpong.c"
    INCLUDE_DIRS "."
    REQUIRES freertos
    PRIV_REQUIRES esp_system
)
