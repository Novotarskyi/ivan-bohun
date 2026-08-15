if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR "main/secrets.h is missing - run tools/gen_secrets.sh")
endif()
# the page's compressed encodings are generated, never committed - README
# "Build your own" step 4 makes them from assets/personal.html
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/personal.html.gz")
    message(FATAL_ERROR "main/personal.html.gz is missing - run:\n"
        "  gzip -9 -n -c assets/personal.html > firmware/eth/main/personal.html.gz\n"
        "  brotli -f -q 11 assets/personal.html -o firmware/eth/main/personal.html.br")
endif()

idf_component_register(SRCS "eth_main.c" "serve.c" "swarm.c" "swarm_identity.c" "ota.c" "mask.c" "blackbox.c" "cpustat.c" "selftest.c" "vitals.c" "splice.c"
                       PRIV_REQUIRES esp_netif esp_eth esp_event esp_https_server esp_wifi nvs_flash esp_timer app_update esp_hw_support esp_partition esp_driver_tsens
                       INCLUDE_DIRS "."
                       EMBED_FILES "personal.html.gz" "personal.html.br" "favicon.ico" "favicon.ico.gz" "favicon.ico.br" "llms.txt" "robots.txt" "og.png"
                       EMBED_TXTFILES "certs/servercert.pem" "certs/prvtkey.pem")
target_compile_definitions(${COMPONENT_LIB} PRIVATE BBX_HAS_HTTPD)
