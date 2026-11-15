idf_component_register(SRCS "probe.c" INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer bt protocomm session)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROBE_STAGE=${PROBE_STAGE})
