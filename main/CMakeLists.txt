idf_component_register(
    SRCS
        "main.cpp"
        "register_map.cpp"
        "modbus_slave.cpp"
        "api_server.cpp"
        "playback.cpp"
        "wifi_manager.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_http_server
        esp_wifi
        esp_netif
        nvs_flash
        json
        esp_timer
        vfs
        spiffs
    EMBED_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
)

# Gzip the web dashboard for embedding (ESP-IDF always has Python)
find_package(Python3 REQUIRED)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
    COMMAND ${Python3_EXECUTABLE} -c
        "import gzip,shutil; shutil.copyfileobj(open(r'${CMAKE_CURRENT_SOURCE_DIR}/web/index.html','rb'), gzip.open(r'${CMAKE_CURRENT_BINARY_DIR}/index.html.gz','wb'))"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    COMMENT "Gzipping web dashboard"
    VERBATIM
)
