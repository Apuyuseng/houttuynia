cmake_minimum_required(VERSION 3.19)
project(Houttuynia LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Network LinguistTools)
qt_standard_project_setup()

qt_add_executable(HouttuyniaApp
    src/main.cpp
    src/backend.cpp
)

# 翻译：源文即中文（默认语言），英文经 Linguist 工具链产出 en.qm 并自动嵌入
# 资源 :/i18n/<lang>.qm（main.cpp 从该路径加载；注意：一旦传 QM_FILES_OUTPUT_VARIABLE
# 就不再自动嵌入，需自行 qt_add_resources）
file(GLOB TS_FILES i18n/*.ts)
qt_add_translations(HouttuyniaApp TS_FILES ${TS_FILES})
# lupdate 源（手动刷新翻译条目：cmake --build build --target update_ts）
qt_add_lupdate(HouttuyniaApp TS_FILES i18n/en.ts SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/qml)

qt_add_qml_module(HouttuyniaApp
    URI Houttuynia
    VERSION 1.0
    SOURCES
        src/theme.hpp
        src/theme.cpp
    QML_FILES
        qml/RButton.qml
        qml/RTextField.qml
        qml/RComboBox.qml
        qml/RSpinBox.qml
        qml/RCheckBox.qml
        qml/RGroupBox.qml
        qml/RSlider.qml
        qml/Main.qml
        qml/OverviewPage.qml
        qml/AppsPage.qml
        qml/StoreWindow.qml
        qml/ImagesPage.qml
        qml/DevicesPage.qml
        qml/TerminalPage.qml
        qml/KeysPage.qml
        qml/CapturePage.qml
        qml/SettingsPage.qml
        qml/AboutPage.qml
        qml/images/logo-appicon.png
        qml/images/icons/overview.svg
        qml/images/icons/apps.svg
        qml/images/icons/devices.svg
        qml/images/icons/cpu.svg
        qml/images/icons/ram.svg
        qml/images/icons/install.svg
        qml/images/icons/download.svg
        qml/images/icons/images.svg
        qml/images/icons/terminal.svg
        qml/images/icons/keys.svg
        qml/images/icons/capture.svg
        qml/images/icons/settings.svg
        qml/images/icons/info.svg
)

target_include_directories(HouttuyniaApp PRIVATE src)
target_link_libraries(HouttuyniaApp PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network)
