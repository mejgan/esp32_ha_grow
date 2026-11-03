# Build-time check of the Grow Box configuration (idf.py menuconfig -> Grow Box): every GPIO is used by one thing
# only, no pin is a flash pin, and no two I2C devices on the same bus share an address. The build stops with a
# list of the clashes.
#
# Included by main/CMakeLists.txt. test/config/check_configs.py runs it on its own:
#   cmake -DSDKCONFIG_CMAKE=build/config/sdkconfig.cmake -P main/check_config.cmake

if(DEFINED SDKCONFIG_CMAKE)
    include(${SDKCONFIG_CMAKE})
endif()

set(_gb_pins "")
set(_gb_pin_users "")
set(_gb_i2c "")
set(_gb_i2c_users "")
set(_gb_errors "")

# _gb_pin(<gpio> <what>): a negative or empty <gpio> means "not connected".
macro(_gb_pin pin what)
    set(_p "${pin}")
    if(NOT _p STREQUAL "")
        if(_p GREATER_EQUAL 0)
            list(FIND _gb_pins "${_p}" _i)
            if(_i GREATER_EQUAL 0)
                list(GET _gb_pin_users ${_i} _other)
                list(APPEND _gb_errors "GPIO${_p} is used by both ${_other} and ${what}")
            else()
                list(APPEND _gb_pins "${_p}")
                list(APPEND _gb_pin_users "${what}")
            endif()
            if(_p GREATER_EQUAL 24)
                list(APPEND _gb_errors "GPIO${_p} (${what}) is a flash pin: use GPIO0-23")
            endif()
        endif()
    endif()
endmacro()

# _gb_i2c(<bus 0 = main, 1 = LP> <address, decimal or 0x..> <what>)
macro(_gb_i2c bus addr what)
    math(EXPR _a "${addr}")
    math(EXPR _ahex "${_a}" OUTPUT_FORMAT HEXADECIMAL)
    if("${bus}" STREQUAL "1")
        set(_bus_name "LP")
    else()
        set(_bus_name "main")
    endif()
    set(_key "${_bus_name}:${_a}")
    list(FIND _gb_i2c "${_key}" _i)
    if(_i GREATER_EQUAL 0)
        list(GET _gb_i2c_users ${_i} _other)
        list(APPEND _gb_errors
             "${_other} and ${what} both use I2C address ${_ahex} on the ${_bus_name} bus: change an address or move one to the other bus")
    else()
        list(APPEND _gb_i2c "${_key}")
        list(APPEND _gb_i2c_users "${what}")
    endif()
endmacro()

# Fixed pins: the USB socket, and the UART0 console unless it is moved to USB Serial/JTAG.
_gb_pin(12 "USB D-")
_gb_pin(13 "USB D+")
if(CONFIG_ESP_CONSOLE_UART_DEFAULT)
    set(_hint "move it to USB: Component config -> ESP System Settings -> Channel for console output")
    _gb_pin(16 "the UART0 console TX (${_hint})")
    _gb_pin(17 "the UART0 console RX (${_hint})")
endif()

# I2C buses.
if(CONFIG_GROWBOX_I2C_MAIN_USED)
    _gb_pin("${CONFIG_GROWBOX_I2C_SDA_GPIO}" "main I2C SDA")
    _gb_pin("${CONFIG_GROWBOX_I2C_SCL_GPIO}" "main I2C SCL")
endif()
if(CONFIG_GROWBOX_I2C_LP_USED)
    _gb_pin(6 "LP I2C SDA")
    _gb_pin(7 "LP I2C SCL")
endif()

# Floors. Types: climate 1-2 DHT, 3-7 I2C; pressure 8-9 I2C; soil 1-2 analog; light 1 analog, 2 I2C.
foreach(_n 1 2 3)
    set(_f "CONFIG_GROWBOX_F${_n}")
    _gb_pin("${${_f}_LIGHT_PWM_GPIO}" "floor ${_n} LED strip")

    set(_type "${${_f}_CLIMATE_TYPE}")
    if(_type EQUAL 1 OR _type EQUAL 2)
        _gb_pin("${${_f}_CLIMATE_GPIO}" "floor ${_n} DHT")
    elseif(_type GREATER_EQUAL 3)
        _gb_i2c("${${_f}_BUS}" "${${_f}_CLIMATE_ADDR}" "floor ${_n} temperature/humidity sensor")
    endif()

    set(_type "${${_f}_PRESSURE_TYPE}")
    if(_type GREATER_EQUAL 8)
        _gb_i2c("${${_f}_BUS}" "${${_f}_PRESSURE_ADDR}" "floor ${_n} pressure sensor")
    endif()

    set(_type "${${_f}_SOIL_TYPE}")
    if(_type GREATER_EQUAL 1)
        _gb_pin("${${_f}_SOIL_GPIO}" "floor ${_n} soil sensor")
        _gb_pin("${${_f}_SOIL_PWR_GPIO}" "floor ${_n} soil sensor power")
    endif()

    set(_type "${${_f}_LIGHT_TYPE}")
    if(_type EQUAL 1)
        _gb_pin("${${_f}_LIGHT_GPIO}" "floor ${_n} TEMT6000")
    elseif(_type EQUAL 2)
        _gb_i2c("${${_f}_BUS}" "${${_f}_LIGHT_ADDR}" "floor ${_n} BH1750")
    endif()
endforeach()

# Room sensor.
if(CONFIG_GROWBOX_ROOM_DHT)
    _gb_pin("${CONFIG_GROWBOX_ROOM_GPIO}" "room sensor DHT")
elseif(CONFIG_GROWBOX_ROOM_I2C)
    _gb_i2c("${CONFIG_GROWBOX_ROOM_BUS}" "${CONFIG_GROWBOX_ROOM_I2C_ADDR}" "room sensor")
endif()

# Display.
if(CONFIG_GROWBOX_DISPLAY_I2C AND NOT "${CONFIG_GROWBOX_DISPLAY_I2C_ADDR}" STREQUAL "")
    math(EXPR _a "${CONFIG_GROWBOX_DISPLAY_I2C_ADDR}")
    if(_a GREATER 0)
        if(CONFIG_GROWBOX_DISPLAY_I2C_BUS_LP)
            _gb_i2c(1 "${_a}" "display")
        else()
            _gb_i2c(0 "${_a}" "display")
        endif()
    endif()
endif()
if(CONFIG_GROWBOX_DISPLAY_TFT)
    _gb_pin("${CONFIG_GROWBOX_DISPLAY_SPI_SCLK_GPIO}" "TFT/SPI SCLK")
    _gb_pin("${CONFIG_GROWBOX_DISPLAY_SPI_MOSI_GPIO}" "TFT/SPI MOSI")
    _gb_pin("${CONFIG_GROWBOX_DISPLAY_SPI_CS_GPIO}" "TFT CS")
    _gb_pin("${CONFIG_GROWBOX_DISPLAY_SPI_DC_GPIO}" "TFT DC")
    _gb_pin("${CONFIG_GROWBOX_DISPLAY_SPI_RST_GPIO}" "TFT RST")
    _gb_pin("${CONFIG_GROWBOX_DISPLAY_BL_GPIO}" "TFT backlight")
endif()

# Button.
_gb_pin("${CONFIG_GROWBOX_BUTTON_GPIO}" "button")

# Cameras (they share SCLK/MOSI with a TFT).
if(NOT "${CONFIG_GROWBOX_CAMERA_COUNT}" STREQUAL "" AND CONFIG_GROWBOX_CAMERA_COUNT GREATER 0)
    if(NOT CONFIG_GROWBOX_DISPLAY_TFT)
        _gb_pin("${CONFIG_GROWBOX_CAM_SPI_SCLK_GPIO}" "camera SPI SCLK")
        _gb_pin("${CONFIG_GROWBOX_CAM_SPI_MOSI_GPIO}" "camera SPI MOSI")
    endif()
    _gb_pin("${CONFIG_GROWBOX_CAM_SPI_MISO_GPIO}" "camera SPI MISO")
    foreach(_n 1 2 3)
        if(CONFIG_GROWBOX_CAMERA_COUNT GREATER_EQUAL ${_n})
            _gb_pin("${CONFIG_GROWBOX_CAM${_n}_CS_GPIO}" "camera ${_n} CS")
        endif()
    endforeach()
endif()

if(_gb_errors)
    list(JOIN _gb_errors "\n  - " _gb_text)
    message(FATAL_ERROR "Grow Box configuration problem (fix it in idf.py menuconfig -> Grow Box):\n  - ${_gb_text}\n")
elseif(DEFINED SDKCONFIG_CMAKE)
    message(STATUS "Grow Box configuration OK")
endif()
