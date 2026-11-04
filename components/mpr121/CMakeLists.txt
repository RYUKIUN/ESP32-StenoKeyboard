idf_component_register(
    SRCS "mpr121.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio
    PRIV_REQUIRES esp_timer
)
