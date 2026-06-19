cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../esp-video-components-temp/esp_sccb_intf"
    "${CMAKE_CURRENT_LIST_DIR}/../esp-video-components-temp/esp_cam_sensor"
    "${CMAKE_CURRENT_LIST_DIR}/../esp-video-components-temp/esp_ipa"
    "${CMAKE_CURRENT_LIST_DIR}/../esp-video-components-temp/esp_video"
    "${CMAKE_CURRENT_LIST_DIR}/../hosted_probe/host_nimble_bleprph_host_only_vhci/managed_components/espressif__esp_serial_slave_link"
    "${CMAKE_CURRENT_LIST_DIR}/../hosted_probe/host_nimble_bleprph_host_only_vhci/managed_components/espressif__eppp_link"
    "${CMAKE_CURRENT_LIST_DIR}/../hosted_probe/host_nimble_bleprph_host_only_vhci/managed_components/espressif__esp_hosted"
    "${CMAKE_CURRENT_LIST_DIR}/../hosted_probe/host_nimble_bleprph_host_only_vhci/managed_components/espressif__esp_wifi_remote"
)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bodyguard_esp32_p4)
