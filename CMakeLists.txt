# EDK60 (ESP32-C6-MINI-1-N4) 多工程 Demo
#
# 工程根目录 = 仓库根目录。所有 demo 通过 menuconfig 选择（见 main/Kconfig.projbuild）。
#
# VS Code (ESP-IDF 插件) 使用说明见 docs/vscode-setup.md。
#
# 注意：IDF_TARGET 的校验放在 main/CMakeLists.txt 中做 —— 在 IDF 6.1 里
# `idf_build_get_property(IDF_TARGET)` 只有到组件处理阶段（idf_build_process）才有值，
# 写在 project() 之前会读到空值。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(edk60_demos)
