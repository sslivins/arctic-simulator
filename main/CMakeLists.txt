# Web UI gzip compression
# Generate gzipped web file at configure time if source is newer
# Locate gzip helper script and Python
set(GZIP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_html.py")
if(Python3_EXECUTABLE)
    set(PYTHON_CMD "${Python3_EXECUTABLE}")
else()
    find_program(PYTHON_CMD NAMES python3 python)
endif()
if(NOT PYTHON_CMD)
    message(FATAL_ERROR "Python not found - required for gzip compression")
endif()

# Helper macro: gzip a web file at configure time if source is newer
macro(gzip_web_file SRC GZ LABEL)
    if(EXISTS "${SRC}")
        if(NOT EXISTS "${GZ}" OR "${SRC}" IS_NEWER_THAN "${GZ}")
            message(STATUS "Compressing ${LABEL}")
            execute_process(
                COMMAND "${PYTHON_CMD}" "${GZIP_SCRIPT}" "${SRC}" "${GZ}"
                RESULT_VARIABLE GZIP_RESULT
                ERROR_VARIABLE GZIP_ERROR
            )
            if(NOT GZIP_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to gzip ${LABEL} (exit ${GZIP_RESULT}: ${GZIP_ERROR})")
            endif()
        endif()
    endif()
endmacro()

# Dashboard
gzip_web_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html.gz"
    "web/index.html")

# Captive portal
gzip_web_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/web/portal.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/portal.html.gz"
    "web/portal.html")

idf_component_register(
    SRCS
        "main.cpp"
        "register_map.cpp"
        "modbus_slave.cpp"
        "api_server.cpp"
        "playback.cpp"
        "wifi_manager.cpp"
        "dns_server.cpp"
        "simulation.cpp"
        "display.cpp"
        "recorder.cpp"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "web/index.html.gz"
        "web/portal.html.gz"
    REQUIRES
        driver
        esp_app_format
        esp_http_server
        esp_wifi
        esp_netif
        nvs_flash
        json
        esp_timer
        vfs
        lwip
        esp_lcd
)
