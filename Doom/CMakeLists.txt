cmake_minimum_required(VERSION 3.16)
# Sin EXTRA_COMPONENT_DIRS A PROPOSITO: esta app no usa el BSP de la board ni
# LVGL, asi que el proyecto es autocontenido y no arrastra componentes de fuera.
# esp_wifi_remote: su Kconfig hace orsource "Kconfig.idf_v$ESP_IDF_VERSION.in" y solo
# trae el fichero de major.minor -> normalizar "6.0.1" a "6.0" o ~60 CONFIG_WIFI_RMT_*
# desaparecen EN SILENCIO (el error sale 200 lineas despues, en WIFI_INIT_CONFIG_DEFAULT).
if(DEFINED ENV{ESP_IDF_VERSION})
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" IDF_MAJOR_MINOR "$ENV{ESP_IDF_VERSION}")
    if(IDF_MAJOR_MINOR AND NOT "$ENV{ESP_IDF_VERSION}" STREQUAL "${IDF_MAJOR_MINOR}")
        set(ENV{ESP_IDF_VERSION} "${IDF_MAJOR_MINOR}")
    endif()
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(app_doom)
