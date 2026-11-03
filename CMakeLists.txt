cmake_minimum_required(VERSION 3.16)

# Firmware version; bump it for every release that should be offered as a Zigbee OTA update.
set(PROJECT_VER "2.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(growbox)

# build/growbox.ota: the app wrapped as a Zigbee OTA upgrade file for Zigbee2MQTT (see README).
idf_build_get_property(python PYTHON)
idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(project_bin PROJECT_BIN)
idf_build_get_property(sdkconfig_cmake SDKCONFIG_CMAKE)
include(${sdkconfig_cmake})
add_custom_target(ota_image ALL
    COMMAND ${python} ${CMAKE_SOURCE_DIR}/tools/make_ota_image.py
            --version ${PROJECT_VER}
            --manufacturer ${CONFIG_GROWBOX_OTA_MANUF_CODE}
            --image-type ${CONFIG_GROWBOX_OTA_IMAGE_TYPE}
            --index ${build_dir}/growbox_ota_index.json
            ${build_dir}/${project_bin} ${build_dir}/growbox.ota
    BYPRODUCTS ${build_dir}/growbox.ota ${build_dir}/growbox_ota_index.json
    COMMENT "Creating Zigbee OTA image build/growbox.ota"
    VERBATIM)
add_dependencies(ota_image gen_project_binary)
