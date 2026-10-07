set(srcs "simple_video_server_example.c")
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
