idf_component_register(SRCS "gateway_record.c" "gateway_storage.c" "gateway_nvs.c"
    "gateway_service.c" "../core/lighting_authority.c"
    INCLUDE_DIRS "." "../core" REQUIRES wiz nvs_flash espressif__cjson)
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_11)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
