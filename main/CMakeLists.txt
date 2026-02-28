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
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html.gz"
)
