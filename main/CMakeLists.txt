set(srcs "simple_video_server_example.c" "lens_calibration.c" "imx708_modes.c" "isp_ipa_hook.c" "isp_debug_api.c" "stream_stats.c")
set(html_files "../frontend/gzipped/index.html.gz"
               "../frontend/gzipped/loading.jpg.gz"
               "../frontend/gzipped/favicon.ico.gz"
               "../frontend/gzipped/assets/index.js.gz"
               "../frontend/gzipped/assets/index.css.gz")
# Binary assets: embedded without the trailing NUL that EMBED_TXTFILES appends, so end - start is the exact size.
set(overlay_files "../frontend/gzipped/overlays/Master_overlay.png.gz"
                  "../frontend/gzipped/overlays/Blank_overlay.png.gz")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS . "../managed_components/espressif__esp_video/private_include"
                       EMBED_TXTFILES ${html_files}
                       EMBED_FILES ${overlay_files})

# esp_ipa only prints (message(FETAL_ERROR ...)) when an IPA JSON is missing, which would
# leave the IMX708 without AE/AWB. Fail the build instead.
idf_build_get_property(imx708_ipa_json IMX708_IPA_JSON_RESOLVED)
idf_build_get_property(project_dir PROJECT_DIR)
if(imx708_ipa_json)
    get_filename_component(imx708_ipa_abs "${imx708_ipa_json}" ABSOLUTE BASE_DIR "${project_dir}")
    if(NOT EXISTS "${imx708_ipa_abs}")
        message(FATAL_ERROR "IMX708 ISP tuning config not found at ${imx708_ipa_abs}")
    endif()
endif()

# Every esp_ipa_pipeline_process() call esp_video makes goes through
# __wrap_esp_ipa_pipeline_process() in isp_ipa_hook.c (IPA snapshot for the debug
# API, debug overrides, IMX708 colour pipeline corrections).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ipa_pipeline_process"
                                                "-Wl,--wrap=esp_ipa_pipeline_init")
if(imx708_ipa_json)
    get_filename_component(imx708_ipa_name "${imx708_ipa_json}" NAME)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ISP_TUNING_JSON_NAME="${imx708_ipa_name}")
endif()
