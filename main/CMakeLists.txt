# EDK60 Demo —— 应用入口组件
#
# 本组件只做一件事：调用 edk60_app 里被 menuconfig 选中的 demo。
# 各 demo 的实现与分派逻辑在 components/edk60_app 中。
#
# 注意：这里不能在组件层加 if(CONFIG_...) 条件（原因见 components/edk60_app/CMakeLists.txt
# 的“踩坑记录”），因此 main 的依赖固定列出，实际用不到的代码由链接器丢弃。

idf_component_register(
    SRCS "src/app_main.c"
    REQUIRES edk60_app edk60_board
)
