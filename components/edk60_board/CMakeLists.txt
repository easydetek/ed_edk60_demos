# EDK60 板级支持包（BSP）
#
# 所有 demo 共用。只封装「板上真实存在的资源」，不含任何 demo 业务逻辑。
#
# 依赖说明：
#   * REQUIRES  —— 本组件的公开头文件（include/edk60_board.h）直接用到的组件
#   * PRIV_REQUIRES —— 仅实现文件（.c）用到的组件
idf_component_register(
    SRCS "edk60_board.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_adc
        esp_hal_ana_conv
        esp_hal_gpio
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_usb_serial_jtag
        esp_hw_support
        esp_app_format
        spi_flash
)
