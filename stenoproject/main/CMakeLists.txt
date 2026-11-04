idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES mpr121 keypad ble_hid hid_kbd steno_app steno_core nvs_flash
)

# Make `idf.py flash` also write the dictionary into the "dict" partition.
set(DICT_BIN "${CMAKE_SOURCE_DIR}/dict/dict.bin")
if(EXISTS "${DICT_BIN}")
    esptool_py_flash_to_partition(flash "dict" "${DICT_BIN}")
else()
    message(WARNING "dict/dict.bin not found - run tools/convert_dict.py first (see README)")
endif()
