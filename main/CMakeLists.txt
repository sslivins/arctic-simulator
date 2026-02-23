# Web UI gzip compression
# Generate gzipped web file at configure time if source is newer
set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html.gz")
set(GZIP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_html.py")

if(EXISTS "${WEB_SRC}")
    if(NOT EXISTS "${WEB_GZ}" OR "${WEB_SRC}" IS_NEWER_THAN "${WEB_GZ}")
        message(STATUS "Compressing web/index.html -> index.html.gz")
        if(Python3_EXECUTABLE)
            set(PYTHON_CMD "${Python3_EXECUTABLE}")
        else()
            find_program(PYTHON_CMD NAMES python3 python)
        endif()
        if(NOT PYTHON_CMD)
            message(FATAL_ERROR "Python not found - required for gzip compression")
        endif()
        execute_process(
            COMMAND "${PYTHON_CMD}" "${GZIP_SCRIPT}" "${WEB_SRC}" "${WEB_GZ}"
            RESULT_VARIABLE GZIP_RESULT
            ERROR_VARIABLE GZIP_ERROR
        )
        if(NOT GZIP_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to gzip web/index.html (exit code: ${GZIP_RESULT}, error: ${GZIP_ERROR})")
        endif()
    endif()
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "register_map.cpp"
        "modbus_slave.cpp"
        "api_server.cpp"
        "playback.cpp"
        "wifi_manager.cpp"
        "simulation.cpp"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "web/index.html.gz"
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
)
