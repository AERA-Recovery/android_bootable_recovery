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
add_executable(ui_check ui_check.cpp ../about_scene.cpp ../aera_logo.cpp
  ../matrix_engraving.cpp
  ../home_scene.cpp ../file_manager.cpp
  ../fonts/aera_fallback.cpp ../i18n.cpp ../i18n/catalog_generated.cpp
  ../tool_scene.cpp ../sideload_scene.cpp ../wifi_scene.cpp ../picture_decode.cpp
  ../picture_viewer.cpp ../operation_scene.cpp ../decrypt_scene.cpp
  ../browser/web_scene.cpp ../browser/session.cpp ../browser/launcher.cpp)
target_include_directories(ui_check PRIVATE .. ../include)
target_compile_definitions(ui_check PRIVATE AERA_DEFAULT_LANGUAGE="en" AERA_EXTRA_LANGUAGES)
target_compile_definitions(ui_check PRIVATE
  AERA_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../fonts/assets"
  AERA_COMMON_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../fonts/assets")
target_compile_options(ui_check PRIVATE -UNDEBUG)
target_link_libraries(ui_check PRIVATE lvgl PNG::PNG Threads::Threads)
enable_testing()
add_test(NAME ui_confirmation_navigation_preferences
  COMMAND ui_check "${LVGL_DIR}/examples/libs/libpng/png_demo.png")
add_executable(display_transform_test display_transform_test.cpp)
target_include_directories(display_transform_test PRIVATE ../include)
add_test(NAME adaptive_display_transform COMMAND display_transform_test)
add_executable(plugin_api_session_test plugin_api_session_test.cpp
  ../plugin_api/session.cpp)
target_include_directories(plugin_api_session_test PRIVATE ..)
target_compile_options(plugin_api_session_test PRIVATE -UNDEBUG)
add_test(NAME plugin_api_negotiation_and_limits COMMAND plugin_api_session_test)
add_executable(sideload_ui_test sideload_ui_test.cpp
  ../fonts/aera_fallback.cpp ../i18n.cpp ../i18n/catalog_generated.cpp
  ../sideload_scene.cpp ../operation_scene.cpp)
target_include_directories(sideload_ui_test PRIVATE .. ../include)
target_compile_definitions(sideload_ui_test PRIVATE AERA_DEFAULT_LANGUAGE="en")
target_compile_definitions(sideload_ui_test PRIVATE
  AERA_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../fonts/assets"
  AERA_COMMON_FONT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../fonts/assets")
target_compile_options(sideload_ui_test PRIVATE -UNDEBUG)
target_link_libraries(sideload_ui_test PRIVATE lvgl Threads::Threads)
add_test(NAME sideload_scene_and_progress COMMAND sideload_ui_test)
add_executable(file_manager_test file_manager_test.cpp ../file_manager.cpp)
target_include_directories(file_manager_test PRIVATE ..)
target_compile_options(file_manager_test PRIVATE -UNDEBUG)
target_link_libraries(file_manager_test PRIVATE Threads::Threads)
add_test(NAME file_manager_operations COMMAND file_manager_test)
