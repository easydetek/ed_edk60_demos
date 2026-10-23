# EDK60 多工程 Demo —— 应用组件（各 demo 实现 + 编译期分派）
#
# 设计说明（含踩坑记录，改本文件前请先读完）
# ---------------------------------------------------------------------------
# 1) 为什么 demo 不放在 main/ ？
#    因为 main 组件在 IDF 里被展开两次：早期展开阶段若拿不到源文件，
#    idf_component_register() 会把 __idf_main 建成 INTERFACE 库；等到
#    真正注册时再 add_library(STATIC) 就会因重名报错。用独立组件可以规避。
#
# 2) 为什么不做「只编译选中的那一个 demo」？
#    IDF 6.1 的组件必须在 __component_get_requirements()（早期展开）阶段就给出
#    SRCS，而该阶段**读不到 CONFIG_***（build_properties.temp.cmake 里没有配置变量，
#    idf_build_get_config() 在该脚本里也未定义）。因此无法在这一阶段按配置裁剪源文件。
#    结论：本组件编译全部 demo 源文件，由预处理器在 demo_entry.c 中分派到选中的那一个。
#    - 未选中的 demo 代码不会被调用，链接期 --gc-sections 会丢弃；
#    - 代价只是编译时间，换来确定性（任何 Kconfig 组合都能构建）。
#
# 3) 依赖策略：REQUIRES 里把各组 demo 需要的组件都列上。
#    这些组件不会被裁剪（它们本来就是 IDF 内置组件，未使用的代码由链接器丢弃），
#    但可以保证「选任何 demo 都能编译通过」。

set(EDK60_DEMO_SRCS)
set(EDK60_DEMO_SRCS
    "src/demo_get_started_hello_board.c"
    "src/demo_get_started_blink.c"
    "src/demo_get_started_button.c"
    "src/demo_get_started_uart_echo.c"
    "src/demo_get_started_adc_read.c"
    "src/demo_get_started_wifi_sta.c"
    "src/demo_get_started_wifi_softap.c"
    "src/demo_get_started_ble_uart.c"
    "src/demo_get_started_ble_scan.c"
    "src/demo_get_started_radar_power.c"
    "src/demo_get_started_led_sequence.c"
    "src/demo_get_started_i2c_scan.c"
    "src/demo_get_started_deep_sleep.c"
    "src/demo_get_started_nvs_rw.c"
    "src/demo_get_started_usb_serial_bridge.c"
    "src/demo_get_started_radar_bridge.c"
    "src/demo_get_started_radar_bridge_edv11p.c"
    "src/demo_get_started_radar_bridge_edv163.c"
    "src/demo_get_started_radar_bridge_edv163_gesture.c"
)

# 与 EDK60_DEMO_SRCS 的一致性自检：漏一个源文件就会在链接期报 undefined
list(LENGTH EDK60_DEMO_SRCS _edk60_n)
message(STATUS "[EDK60] 应用组件将编译 ${_edk60_n} 个 demo 源文件（分派由预处理器完成）")

idf_component_register(
    SRCS
        "src/demo_dispatch.c"
        ${EDK60_DEMO_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        edk60_board
        esp_adc
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_event
        esp_hal_i2c          # i2c_addr_bit_len_t / I2C_ADDR_BIT_LEN_7（i2c_scan 直接用到）
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
        bt
)
