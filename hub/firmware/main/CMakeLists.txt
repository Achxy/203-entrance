idf_component_register(SRCS "hub.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_usb_serial_jtag esp_wifi esp_netif esp_event nvs_flash
             esp_timer esp_system lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror=return-type)
