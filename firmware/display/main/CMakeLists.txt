if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../eth/main/secrets.h")
    message(FATAL_ERROR "eth/main/secrets.h is missing - run tools/gen_secrets.sh")
endif()

# reuses the swarm control-plane sources from the node firmware (no divergence)
idf_component_register(
    SRCS "display_main.c" "tft.c" "led.c"
         "../../eth/main/swarm.c"
         "../../eth/main/swarm_identity.c"
         "../../eth/main/mask.c"
         "../../eth/main/blackbox.c"
         "../../eth/main/vitals.c"
         "../../eth/main/ota.c"
         "../../eth/main/ota_window.c"
    INCLUDE_DIRS "." "../../eth/main"
    PRIV_REQUIRES esp_netif esp_eth esp_wifi nvs_flash esp_timer esp_hw_support esp_partition esp_driver_tsens
                  esp_lcd driver esp_lvgl_port esp_https_server app_update
    EMBED_TXTFILES "../../eth/main/certs/servercert.pem" "../../eth/main/certs/prvtkey.pem"
)
