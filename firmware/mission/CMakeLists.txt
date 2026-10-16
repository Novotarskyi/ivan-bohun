cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# LVGL's assertion handler lives in main/lv_assert_hook.h and LVGL's own sources
# must find it (CONFIG_LV_ASSERT_HANDLER_INCLUDE) - a build property, set before
# project(), is the only include path a managed component sees.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/main" APPEND)
project(bohun_mission)
