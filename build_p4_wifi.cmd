@echo off
if "%IDF_EXPORT_BAT%"=="" set IDF_EXPORT_BAT=D:\Espressif\frameworks\esp-idf-v5.5.3\export.bat
call "%IDF_EXPORT_BAT%"
set IDF_COMPONENT_MANAGER=0
set IDF_CCACHE_ENABLE=0
cd /d "%~dp0bodyguard_esp32_p4"
idf.py -B build_wifi_cmd -D SDKCONFIG=sdkconfig.hosted build
