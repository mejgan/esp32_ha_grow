idf_component_register(SRC_DIRS "."
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_ledc
                                     esp_adc esp_timer growbox_logic)
