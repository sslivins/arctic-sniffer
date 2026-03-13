# Gzip the web dashboard during configure
execute_process(
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_html.py
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html.gz
)

idf_component_register(
    SRCS "main.cpp"
         "modbus_sniffer.cpp"
         "arctic_registers.cpp"
         "recorder.cpp"
         "api_server.cpp"
         "wifi_manager.cpp"
         "display.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html.gz"
    REQUIRES driver esp_app_format esp_http_server esp_wifi esp_netif
             nvs_flash json esp_timer esp_lcd
)
