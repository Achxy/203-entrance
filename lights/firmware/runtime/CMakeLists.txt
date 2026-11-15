idf_component_register(SRCS "lighting_runtime_queue.c" "lighting_runtime_recovery.c" "lighting_runtime.c"
    "lighting_local_time.c" "lighting_local_time_idf.c"
    INCLUDE_DIRS "." REQUIRES gateway session ble wiz nvs_flash esp_timer mbedtls esp_wifi esp_netif lwip)
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_11)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
