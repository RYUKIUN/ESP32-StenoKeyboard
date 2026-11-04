idf_component_register(
    SRCS "hid_keys.c" "hid_kbd.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer
)
