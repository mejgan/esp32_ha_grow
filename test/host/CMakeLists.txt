# Host-side unit tests for the hardware-independent components:
#   cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(growbox_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Same warnings as the ESP-IDF firmware build (-Werror there); -Og enables its format-truncation checks.
add_compile_options(-Og -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_library(growbox_core STATIC
    ${ROOT}/components/growbox_logic/growbox_logic.c
    ${ROOT}/components/growbox_ui/growbox_ui.c
    ${ROOT}/components/growbox_ui/font5x7.c)
target_include_directories(growbox_core PUBLIC
    ${ROOT}/components/growbox_logic/include
    ${ROOT}/components/growbox_ui/include)
target_link_libraries(growbox_core PUBLIC m)

enable_testing()

add_executable(test_logic test_logic.c)
target_link_libraries(test_logic growbox_core)
add_test(NAME logic COMMAND test_logic)

add_executable(test_ui test_ui.c)
target_link_libraries(test_ui growbox_core)
add_test(NAME ui COMMAND test_ui)

# Writes PPM previews of every page for each supported display geometry (not a test).
add_executable(ui_preview ui_preview.c)
target_link_libraries(ui_preview growbox_core)

# tools/make_ota_image.py output must parse with the firmware's OTA parser and carry the app unchanged.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_executable(ota_check ota_check.c)
    target_link_libraries(ota_check growbox_core)
    set(OTA_WORK ${CMAKE_CURRENT_BINARY_DIR}/ota_test)
    add_test(NAME ota_tool
        COMMAND sh -c "mkdir -p '${OTA_WORK}' \
            && '$<TARGET_FILE:ota_check>' gen '${OTA_WORK}/app.bin' \
            && '${Python3_EXECUTABLE}' '${ROOT}/tools/make_ota_image.py' --version 2.1.3 \
                   --manufacturer 0x131B --image-type 0x4742 --index '${OTA_WORK}/index.json' \
                   '${OTA_WORK}/app.bin' '${OTA_WORK}/app.ota' \
            && '$<TARGET_FILE:ota_check>' verify '${OTA_WORK}/app.bin' '${OTA_WORK}/app.ota' 0x131B 0x4742 0x02010300 \
            && grep -q '\"fileVersion\": 33620736' '${OTA_WORK}/index.json'")
endif()
