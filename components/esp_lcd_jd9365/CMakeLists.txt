idf_component_register(
    SRCS "esp_lcd_jd9365.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "esp_driver_gpio"
    REQUIRES "esp_lcd"
)
