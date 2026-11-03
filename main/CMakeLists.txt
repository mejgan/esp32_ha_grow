idf_component_register(SRC_DIRS "."
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_rmt
                                     esp_driver_spi esp_lcd esp_adc esp_timer app_update
                                     esp_wifi esp_netif esp_event esp_http_client esp_coex mbedtls
                                     growbox_logic growbox_ui)

# Zigbee OTA file version from PROJECT_VER "major.minor.patch" (top-level CMakeLists.txt): 0xMMmmpp00.
# tools/make_ota_image.py derives the same number, so the OTA file and the running firmware always agree.
idf_build_get_property(project_ver PROJECT_VER)
if(NOT project_ver MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "PROJECT_VER '${project_ver}' must start with major.minor.patch, e.g. 2.0.0")
endif()
math(EXPR ota_file_version "(${CMAKE_MATCH_1} << 24) | (${CMAKE_MATCH_2} << 16) | (${CMAKE_MATCH_3} << 8)"
     OUTPUT_FORMAT HEXADECIMAL)
target_compile_definitions(${COMPONENT_LIB} PRIVATE GROWBOX_OTA_FILE_VERSION=${ota_file_version})
