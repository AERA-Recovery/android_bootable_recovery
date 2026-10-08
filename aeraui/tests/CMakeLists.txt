cmake_minimum_required(VERSION 3.16)
project(aera_ui_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(LVGL_DIR "" CACHE PATH "Recovery's external/lvgl directory")
if(NOT EXISTS "${LVGL_DIR}/lv_conf.h")
  message(FATAL_ERROR "Set LVGL_DIR to the recovery tree's configured external/lvgl directory")
endif()
set(LV_BUILD_CONF_PATH "${LVGL_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_DIR}" lvgl)
find_package(Freetype REQUIRED)
find_package(PNG REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(lvgl PUBLIC Freetype::Freetype)
add_executable(ui_check ui_check.cpp
  ../scenes/about_scene.cpp ../components/aera_logo.cpp
  ../components/matrix_engraving.cpp
  ../scenes/home_scene.cpp ../features/files/file_manager.cpp
  ../localization/fonts/aera_fallback.cpp ../localization/i18n.cpp
  ../localization/catalog/catalog_generated.cpp
  ../scenes/tool_scene.cpp ../scenes/sideload_scene.cpp
  ../scenes/wifi_scene.cpp ../components/picture_decode.cpp
  ../components/picture_viewer.cpp ../components/wallpaper.cpp
  ../scenes/operation_scene.cpp
  ../scenes/decrypt_scene.cpp ../features/browser/web_scene.cpp
  ../features/browser/session.cpp ../features/browser/launcher.cpp)
target_include_directories(ui_check PRIVATE .. ../include ../components
  ../features ../features/files ../localization ../localization/fonts
  ../scenes)
target_compile_definitions(ui_check PRIVATE AERA_DEFAULT_LANGUAGE="en" AERA_EXTRA_LANGUAGES)
target_compile_definitions(ui_check PRIVATE
  AERA_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../localization/fonts/assets"
  AERA_COMMON_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../localization/fonts/assets")
target_compile_options(ui_check PRIVATE -UNDEBUG)
target_link_libraries(ui_check PRIVATE lvgl PNG::PNG Threads::Threads)
enable_testing()
add_executable(navigation_dock_test navigation_dock_test.cpp)
target_include_directories(navigation_dock_test PRIVATE .. ../include
  ../components ../localization ../scenes ../features)
target_compile_options(navigation_dock_test PRIVATE -UNDEBUG)
target_link_libraries(navigation_dock_test PRIVATE lvgl PNG::PNG)
add_test(NAME navigation_dock_layouts COMMAND navigation_dock_test)
add_executable(edge_swipe_test edge_swipe_test.cpp)
target_include_directories(edge_swipe_test PRIVATE ../core)
target_compile_options(edge_swipe_test PRIVATE -UNDEBUG)
add_test(NAME edge_swipe_cancel COMMAND edge_swipe_test)
add_executable(button_cancel_test button_cancel_test.cpp)
target_compile_options(button_cancel_test PRIVATE -UNDEBUG)
target_link_libraries(button_cancel_test PRIVATE lvgl)
add_test(NAME button_drag_cancel COMMAND button_cancel_test)
add_test(NAME ui_confirmation_navigation_preferences
  COMMAND ui_check "${LVGL_DIR}/examples/libs/libpng/png_demo.png")
add_executable(display_transform_test display_transform_test.cpp)
target_include_directories(display_transform_test PRIVATE ../include)
add_test(NAME adaptive_display_transform COMMAND display_transform_test)
add_executable(plugin_api_session_test plugin_api_session_test.cpp
  ../features/plugin_api/session.cpp)
target_include_directories(plugin_api_session_test PRIVATE ..
  ../features)
target_compile_options(plugin_api_session_test PRIVATE -UNDEBUG)
add_test(NAME plugin_api_negotiation_and_limits COMMAND plugin_api_session_test)
add_executable(sideload_ui_test sideload_ui_test.cpp
  ../localization/fonts/aera_fallback.cpp ../localization/i18n.cpp
  ../localization/catalog/catalog_generated.cpp
  ../scenes/sideload_scene.cpp ../scenes/operation_scene.cpp)
target_include_directories(sideload_ui_test PRIVATE .. ../include ../components
  ../localization ../localization/fonts ../scenes)
target_compile_definitions(sideload_ui_test PRIVATE AERA_DEFAULT_LANGUAGE="en")
target_compile_definitions(sideload_ui_test PRIVATE
  AERA_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../localization/fonts/assets"
  AERA_COMMON_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../localization/fonts/assets")
target_compile_options(sideload_ui_test PRIVATE -UNDEBUG)
target_link_libraries(sideload_ui_test PRIVATE lvgl Threads::Threads)
add_test(NAME sideload_scene_and_progress COMMAND sideload_ui_test)
add_executable(file_manager_test file_manager_test.cpp
  ../features/files/file_manager.cpp)
target_include_directories(file_manager_test PRIVATE .. ../features/files)
target_compile_options(file_manager_test PRIVATE -UNDEBUG)
target_link_libraries(file_manager_test PRIVATE Threads::Threads)
add_test(NAME file_manager_operations COMMAND file_manager_test)

set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_DIR}/../jsoncpp" jsoncpp)
add_executable(smb_browser_test smb_browser_test.cpp ../features/nas/smb_browser.cpp)
target_include_directories(smb_browser_test PRIVATE ../include ../features)
target_compile_options(smb_browser_test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_options(smb_browser_test PRIVATE -Wl,--wrap=posix_spawn)
target_link_libraries(smb_browser_test PRIVATE jsoncpp_static Threads::Threads)
add_test(NAME smb_browse_paths_and_cancellation COMMAND smb_browser_test)

add_executable(nas_ui_test nas_ui_test.cpp ../scenes/nas_scene.cpp
  ../localization/fonts/aera_fallback.cpp ../localization/i18n.cpp
  ../localization/catalog/catalog_generated.cpp)
target_include_directories(nas_ui_test PRIVATE .. ../include ../components
  ../features ../localization ../localization/fonts ../scenes)
target_compile_definitions(nas_ui_test PRIVATE AERA_DEFAULT_LANGUAGE="en" AERA_EXTRA_LANGUAGES)
target_compile_definitions(nas_ui_test PRIVATE
  AERA_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../localization/fonts/assets"
  AERA_COMMON_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../localization/fonts/assets")
target_compile_options(nas_ui_test PRIVATE -UNDEBUG)
target_link_libraries(nas_ui_test PRIVATE lvgl Threads::Threads PNG::PNG)
add_test(NAME nas_picker_navigation_and_layout COMMAND nas_ui_test)
