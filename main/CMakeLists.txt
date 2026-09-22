idf_component_register(
    SRCS "app_main.c" "gateway_poll.c" "modbus_tcp.c" "ats_model.c"
         "gateway_bacnet.c" "bip_port.c" "custom_map.c"
         "gateway_config.c" "gateway_web.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bacnet_stack esp_eth esp_netif esp_event esp_timer
                  esp_http_server nvs_flash json lwip esp_app_format
    EMBED_TXTFILES "web/index.html" "web/template.csv"
)
