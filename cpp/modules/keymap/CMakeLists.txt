# mw_keymap ← crates/mwine-keymap（按键映射引擎 + 模板）
file(GLOB MW_KEYMAP_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_keymap STATIC ${MW_KEYMAP_SRC})
target_include_directories(mw_keymap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mw_keymap PUBLIC mw_common)
