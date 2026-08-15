if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../eth/main/secrets.h")
    message(FATAL_ERROR "eth/main/secrets.h is missing - run tools/gen_secrets.sh")
endif()

# reuses the swarm control-plane sources from the node firmware (no divergence),
# exactly like the 2.8" display project does
idf_component_register(
    SRCS "mission_main.c" "panel.c" "ui.c"
         "../../eth/main/swarm.c"
         "../../eth/main/swarm_identity.c"
         "../../eth/main/mask.c"
         "../../eth/main/blackbox.c"
         "../../eth/main/vitals.c"
         "../../eth/main/ota.c"
         "../../eth/main/ota_window.c"
    INCLUDE_DIRS "." "../../eth/main"
    PRIV_REQUIRES esp_netif esp_eth esp_wifi nvs_flash esp_timer esp_hw_support esp_partition esp_driver_tsens
                  esp_lcd esp_driver_i2c esp_driver_gpio esp_lvgl_port esp_https_server app_update
    EMBED_FILES "arms_84x104.bin"
    EMBED_TXTFILES "../../eth/main/certs/servercert.pem" "../../eth/main/certs/prvtkey.pem"
)
# page_strip.bin = the WHOLE served page baked to RGB565 (render_page_asset.py, which
# also generates page_strip.h). It lives in the dedicated "page" partition - NOT in the
# app image - so SPIRAM_RODATA doesn't drag 6MB into PSRAM and starve the frame buffers.
# idf.py flash writes it via the registration below; regenerate after personal.html edits.
# The .bin is generated and never committed: build works without it, flash needs it.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/page_strip.bin")
    message(WARNING "page_strip.bin missing - run firmware/mission/render_page_asset.py before idf.py flash (the page tile shows 'partition empty' until then)")
endif()
esptool_py_flash_to_partition(flash "page" "${CMAKE_CURRENT_SOURCE_DIR}/page_strip.bin")
# arms_84x104.bin = Herb Viyska Zaporozkoho baked small (render_arms_asset.py) - tiny, rides rodata
