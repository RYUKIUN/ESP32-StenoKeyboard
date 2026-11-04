idf_component_register(
    SRCS "steno_app.c"
    INCLUDE_DIRS "include"
    REQUIRES steno_core esp_partition freertos
)
