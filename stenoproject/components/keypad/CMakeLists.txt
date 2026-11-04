idf_component_register(
    SRCS "keypad.c"
    INCLUDE_DIRS "include"
    REQUIRES mpr121
    PRIV_REQUIRES esp_timer
)
