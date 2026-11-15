idf_component_register(SRCS "wiz_protocol.c" "wiz_capabilities.c" "wiz_crypto.c"
    "wiz_driver.c" "wiz_esp_idf.c" INCLUDE_DIRS "include"
    REQUIRES mbedtls espressif__cjson esp_netif esp_wifi esp_timer lwip)
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_11)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
