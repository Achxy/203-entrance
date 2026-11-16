idf_build_get_property(ENTRANCE_LIGHTS ENTRANCE_LIGHTS)
set(lights_requirements "")
set(lights_sources "")
if(ENTRANCE_LIGHTS)
    set(lights_requirements runtime esp-tls)
    set(lights_sources relay_ingress.c)
endif()
idf_component_register(
    SRCS "core.c" "protocol.c" "auth.c" "settings_policy.c" "store.c" "network.c" "diagnostics.c" "ota.c" "health.c" "api.c" "release.c" "media.c" "display.c" "display_config.c" "builtin_loop.c" "captions.c" "arcade_core.c" "arcade.c" "arcade_identity.c" "relay.c" "relay_transport.c" "lights_integration.c" ${lights_sources}
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_app_format esp_http_server esp_https_server esp_http_client
             esp_wifi esp_netif nvs_flash esp_timer esp_driver_gpio esp_system esp_event
             mbedtls espressif__cjson espressif__esp_websocket_client tcp_transport lwip spi_flash esp_partition esp_rom esp_driver_spi ${lights_requirements}
    EMBED_FILES "../../web/index.html" "../../web/app.js" "../../web/style.css" "../../web/media.js" "../../web/display.js" "../../web/replay.js" "../../assets/neko/neko.ecl" "../../assets/study/study.ecl" "../../player/play.html" "../../player/play.js" "../../player/relay.js" "../../player/play.css" "../../keys/arcade-pass-public.hex"
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u entrance_descriptor")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_transport_connect")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror=return-type)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ENTRANCE_TEST_FAULT=${ENTRANCE_TEST_FAULT})
if(ENTRANCE_LIGHTS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ENTRANCE_LIGHTS=1)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_tls_server_session_create,--wrap=esp_tls_conn_new_sync")
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ENTRANCE_LIGHTS=0)
endif()
