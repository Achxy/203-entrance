idf_component_register(SRCS "lighting_session_wire.c" "lighting_session_idf.c"
                       INCLUDE_DIRS "." REQUIRES protocomm mbedtls espressif__cjson)
