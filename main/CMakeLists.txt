set(gateway_sources
    "app_main.c" "gateway_poll.c" "modbus_tcp.c" "ats_model.c"
    "gateway_bacnet.c" "bip_port.c" "custom_map.c" "gateway_config.c"
    "gateway_web.c" "gateway_storage.c")
set(gateway_dependencies bacnet_stack esp_eth esp_netif esp_event esp_timer
    esp_http_server nvs_flash json lwip esp_app_format esp_partition spi_flash
    app_update bootloader_support esp_https_server esp_system mbedtls)
set(gateway_embeds "web/index.html" "web/template.csv")
get_filename_component(project_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(CONFIG_SECURE_BOOT OR CONFIG_SECURE_FLASH_ENC_ENABLED OR
   CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK OR CONFIG_SECURE_DISABLE_ROM_DL_MODE OR
   CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE)
    message(FATAL_ERROR "Hardware security/eFuse provisioning is outside this project's policy")
endif()
if(CONFIG_GW_OTA_ENABLED)
    if(NOT CMAKE_BUILD_EARLY_EXPANSION AND
       (NOT CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT OR
        NOT CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME OR
        NOT CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES OR
        NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE OR
        NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions.ota.csv"))
        message(FATAL_ERROR "HTTPS OTA requires signed RSA-3072 output, rollback and the dual-slot partition layout")
    endif()
    if(CONFIG_LWIP_MAX_SOCKETS LESS 16)
        message(FATAL_ERROR "HTTPS and HTTP redirect require CONFIG_LWIP_MAX_SOCKETS >= 16")
    endif()
    set(ota_secrets_dir "${project_root}/secrets")
    if(DEFINED ENV{ESP32_P4_OTA_SECRETS_DIR})
        set(ota_secrets_dir "$ENV{ESP32_P4_OTA_SECRETS_DIR}")
    endif()
    set(ota_private_key "${ota_secrets_dir}/ota_server_key.pem")
    set(ota_token_file "${ota_secrets_dir}/ota_token.txt")
    set(ota_viewer_token_file "${ota_secrets_dir}/ota_viewer_token.txt")
    foreach(secret_file IN ITEMS "${ota_private_key}" "${ota_token_file}" "${ota_viewer_token_file}")
        if(NOT EXISTS "${secret_file}")
            message(FATAL_ERROR "Missing OTA credential ${secret_file}; supply existing credentials through ESP32_P4_OTA_SECRETS_DIR")
        endif()
    endforeach()
    get_filename_component(signing_private_key "${CONFIG_SECURE_BOOT_SIGNING_KEY}"
                           ABSOLUTE BASE_DIR "${project_root}")
    add_custom_target(validate_signing_key ALL
        COMMAND "${PYTHON}" "${project_root}/tools/firmware_signing.py" check-key
            --private-key "${signing_private_key}"
            --public-key "${CMAKE_CURRENT_LIST_DIR}/ota_signing_public_key.pem"
        COMMENT "Checking host signing key against pinned public key" VERBATIM)
    add_custom_target(validate_ota_credentials ALL
        COMMAND "${PYTHON}" "${project_root}/tools/validate_ota_credentials.py"
            --certificate "${CMAKE_CURRENT_LIST_DIR}/ota_server_cert.pem"
            --private-key "${ota_private_key}" --token-file "${ota_token_file}"
            --viewer-token-file "${ota_viewer_token_file}"
        COMMENT "Validating HTTPS OTA credentials" VERBATIM)
    list(APPEND gateway_sources "gateway_ota.c" "ota_auth.c" "ota_health.c" "dashboard_redirect.c")
    list(APPEND gateway_embeds "ota_server_cert.pem" "${ota_private_key}"
         "${ota_token_file}" "${ota_viewer_token_file}")
endif()
idf_component_register(SRCS ${gateway_sources} INCLUDE_DIRS "."
    PRIV_REQUIRES ${gateway_dependencies} EMBED_TXTFILES ${gateway_embeds})
if(CONFIG_GW_OTA_ENABLED)
    add_dependencies(${COMPONENT_LIB} validate_ota_credentials validate_signing_key)
endif()
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY "${project_root}"
    OUTPUT_VARIABLE project_git_revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(project_git_revision STREQUAL "")
    set(project_git_revision "unknown")
else()
    execute_process(COMMAND git diff --quiet --ignore-submodules HEAD
        WORKING_DIRECTORY "${project_root}" RESULT_VARIABLE dirty)
    if(NOT dirty EQUAL 0)
        string(APPEND project_git_revision "-dirty")
    endif()
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROJECT_GIT_REVISION="${project_git_revision}" GATEWAY_SOURCE_REVISION="${project_git_revision}")
file(WRITE "${CMAKE_BINARY_DIR}/project_git_revision.txt" "${project_git_revision}\n")
