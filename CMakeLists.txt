# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ISP tuning (esp_ipa) for the IMX708: the project's own file, derived from
# mushbraindave/esp_cam_sensor_imx 0.4.0 cfg/imx708_default.json (a verbatim copy of that is
# kept next to it) and written for main/isp_ipa_hook.c's IMX708 colour pipeline - see
# main/tuning/imx708_b0310.json and docs/camera-formats.md. esp_ipa reads this property
# while its own CMakeLists is processed, so it must be set here (after project.cmake,
# before project()); the OV5647 config is added by esp_cam_sensor itself.
# Path is relative to the project dir and is checked in main/CMakeLists.txt.
# -DIMX708_IPA_JSON=<path> builds with another file (e.g. the upstream copy, for A/B comparisons).
if(NOT IMX708_IPA_JSON)
    set(IMX708_IPA_JSON "main/tuning/imx708_b0310.json")
endif()
idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${IMX708_IPA_JSON}" APPEND)
idf_build_set_property(IMX708_IPA_JSON_RESOLVED "${IMX708_IPA_JSON}")

# ABI fix for the prebuilt esp_ipa library: its esp_ipa_stats_t carries the 5x5 AWB
# sub-window grid (the v5.5 library reads ae_stats at offset 428), but ESP-IDF only
# defines ISP_AWB_WINDOW_X/Y_NUM from v5.4.4 / v5.5.3. Without them esp_ipa_types.h
# collapses awb_subwin to [0][0], so esp_video writes ae_stats/af_stats 400 bytes
# earlier than the library reads them and the IPA's AE (used by the IMX708) runs on
# garbage - it pins exposure and gain at their maximum on a bright scene. Defining the
# macros restores the layout; esp_video keeps its AWB sub-window feature off (it is
# keyed on the IDF version). Same workaround as mushbraindave/esp_cam_sensor_imx's
# examples. Global, because esp_video, the generated IPA config and the library must agree.
if((IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR EQUAL 4 AND IDF_VERSION_PATCH LESS 4) OR
   (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR EQUAL 5 AND IDF_VERSION_PATCH LESS 3))
    idf_build_set_property(COMPILE_DEFINITIONS "ISP_AWB_WINDOW_X_NUM=5" APPEND)
    idf_build_set_property(COMPILE_DEFINITIONS "ISP_AWB_WINDOW_Y_NUM=5" APPEND)
endif()
project(ov5647_ethernet_stream)
