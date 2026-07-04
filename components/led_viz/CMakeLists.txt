# LED Visualizer component
# References source files from the led_visualizer library at ../../../led_visualizer/

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../led_visualizer/esp32/led_viz_esp32.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../led_visualizer/include/led_viz_sdk.c"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../led_visualizer/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../led_visualizer/esp32"
    REQUIRES
        led_strip
        esp_timer
        esp_adc
        driver
)
