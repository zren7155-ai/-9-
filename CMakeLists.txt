##
# @file CMakeLists.txt
# @brief 
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRCS
set(APP_SRCS
    ${APP_PATH}/src/tuya_app_main.c
    ${APP_PATH}/src/log.c
    ${APP_PATH}/main/main.c
    ${APP_PATH}/main/ble_client.c
    ${APP_PATH}/main/ui.c
    ${APP_PATH}/main/alert.c
    ${APP_PATH}/main/app_state.c
)

# APP_INC
set(APP_INC
    ${APP_PATH}
    ${APP_PATH}/include
    ${APP_PATH}/src
    ${APP_PATH}/main
    ${APP_PATH}/ai_components/assets/include
)

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

add_subdirectory(${APP_PATH}/ai_components ${CMAKE_BINARY_DIR}/ai_components)

if (TARGET tal_bluetooth)
    target_compile_definitions(tal_bluetooth PRIVATE TY_HS_BLE_ROLE_CENTRAL=1)
endif()
