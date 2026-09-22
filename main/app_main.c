#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "gateway_bacnet.h"
#include "gateway_poll.h"

static const char *TAG = "ats_gateway";
static SemaphoreHandle_t lock;
static esp_netif_t *ethernet;
static bool network_up;
static uint32_t network_generation;
static esp_netif_ip_info_t network_info;
static gateway_poll_t published;
static gateway_bacnet_stats_t published_bacnet;

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        network_info = ((ip_event_got_ip_t *)data)->ip_info;
        network_up = true;
        ++network_generation;
        ESP_LOGI(TAG, "IPv4 " IPSTR "; BACnet device %d, UDP %d", IP2STR(&network_info.ip),
                 CONFIG_GW_DEVICE_INSTANCE, CONFIG_GW_BACNET_PORT);
    } else if ((base == ETH_EVENT && (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP)) ||
               (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP)) {
        network_up = false;
        ++network_generation;
        ESP_LOGW(TAG, "Ethernet unavailable");
    }
    xSemaphoreGive(lock);
}

static void poll_task(void *arg)
{
    (void)arg;
    static gateway_poll_t poller;
    gateway_poll_init(&poller);
    gateway_poll_config_t config = {
        .host = CONFIG_GW_MODBUS_HOST, .port = CONFIG_GW_MODBUS_PORT,
        .unit = CONFIG_GW_MODBUS_UNIT, .expected_firmware = CONFIG_GW_EXPECTED_FIRMWARE,
        .expected_mac_fragment = CONFIG_GW_EXPECTED_MAC_FRAGMENT,
    };
    bool previous_up = false;
    uint32_t generation = 0;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bool up = network_up;
        uint32_t changed = network_generation;
        xSemaphoreGive(lock);
        if ((previous_up && !up) || (up && changed != generation)) {
            gateway_poll_offline(&poller, now_ms());
            poller.next_request_ms = now_ms();
        }
        if (up && gateway_poll_step(&poller, &config, now_ms())) {
            if (poller.consecutive_failures == 1) {
                ESP_LOGW(TAG, "ATS offset %u: %s", poller.last_offset,
                         mb_error_string(poller.last_result.error));
            }
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        if (!network_up || network_generation != changed) {
            gateway_poll_offline(&poller, now_ms());
            poller.next_request_ms = now_ms();
        }
        published = poller;
        xSemaphoreGive(lock);
        previous_up = up;
        generation = changed;
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void bacnet_task(void *arg)
{
    (void)arg;
    static ats_model_t model;
    static ats_value_t values[ATS_POINT_COUNT];
    bool started = false;
    uint32_t generation = UINT32_MAX;
    uint64_t next_update = 0;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bool up = network_up;
        uint32_t changed = network_generation;
        esp_netif_ip_info_t info = network_info;
        model = published.model;
        xSemaphoreGive(lock);
        uint64_t now = now_ms();
        if (!up) {
            for (size_t i = 0; i < ATS_BLOCK_COUNT; ++i)
                ats_model_fail_block(&model, ats_scan_blocks[i].offset,
                                     ats_scan_blocks[i].count, now);
        }
        if (!started && up) {
            gateway_bacnet_config_t config = {
                .device_instance = CONFIG_GW_DEVICE_INSTANCE,
                .device_name = CONFIG_GW_DEVICE_NAME,
                .firmware_version = esp_app_get_description()->version,
                .location = "Automatic transfer switch",
                .vendor_id = CONFIG_GW_VENDOR_ID,
                .local_ip = info.ip.addr, .netmask = info.netmask.addr,
                .gateway = info.gw.addr, .udp_port = CONFIG_GW_BACNET_PORT,
                .dhcp_enabled = CONFIG_GW_STATIC_IP[0] == '\0',
            };
            const char *peers[] = {CONFIG_GW_IAM_PEER1, CONFIG_GW_IAM_PEER2};
            for (size_t i = 0; i < 2; ++i) {
                struct in_addr peer;
                if (peers[i][0] && inet_pton(AF_INET, peers[i], &peer) == 1) {
                    config.peers[config.peer_count++] = (gateway_bacnet_peer_t){
                        .ip = peer.s_addr, .port = CONFIG_GW_BACNET_PORT};
                }
            }
            started = gateway_bacnet_init(&config, now);
            if (!started) ESP_LOGE(TAG, "BACnet initialization failed; retrying");
            generation = changed;
        }
        if (started) {
            if (generation != changed) {
                if (!gateway_bacnet_network_update(info.ip.addr, info.netmask.addr,
                        info.gw.addr, up, now)) {
                    ESP_LOGW(TAG, "BACnet network rebind pending");
                } else generation = changed;
            }
            if (now >= next_update) {
                ats_model_decode_all(&model, now, values);
                gateway_bacnet_update(values);
                next_update = now + 200;
            }
            gateway_bacnet_tick(now);
            gateway_bacnet_poll(10);
            gateway_bacnet_stats_t stats;
            gateway_bacnet_stats(&stats);
            xSemaphoreTake(lock, portMAX_DELAY);
            published_bacnet = stats;
            xSemaphoreGive(lock);
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(started ? 10 : 500));
    }
}

static esp_err_t json_reply(httpd_req_t *request, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static esp_err_t status_handler(httpd_req_t *request)
{
    gateway_bacnet_stats_t stats;
    uint32_t requests, successes, failures;
    uint64_t last;
    bool up, profile;
    const char *reason;
    xSemaphoreTake(lock, portMAX_DELAY);
    stats = published_bacnet;
    requests = published.requests; successes = published.successes; failures = published.failures;
    last = published.last_success_ms; up = network_up;
    profile = published.profile_valid; reason = published.profile_status;
    xSemaphoreGive(lock);
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "board", "Waveshare ESP32-P4-POE-ETH");
    cJSON_AddNumberToObject(j, "uptime_seconds", now_ms() / 1000.0);
    cJSON_AddBoolToObject(j, "ethernet_up", up);
    cJSON_AddBoolToObject(j, "profile_verified", profile);
    cJSON_AddStringToObject(j, "profile_status", reason ? reason : "Initializing");
    cJSON_AddStringToObject(j, "modbus_host", CONFIG_GW_MODBUS_HOST);
    cJSON_AddNumberToObject(j, "modbus_unit", CONFIG_GW_MODBUS_UNIT);
    cJSON_AddNumberToObject(j, "modbus_requests", requests);
    cJSON_AddNumberToObject(j, "modbus_successful_responses", successes);
    cJSON_AddNumberToObject(j, "modbus_failures", failures);
    if (last) cJSON_AddNumberToObject(j, "last_modbus_response_age_seconds", (now_ms() - last) / 1000.0);
    else cJSON_AddNullToObject(j, "last_modbus_response_age_seconds");
    cJSON_AddNumberToObject(j, "bacnet_device_instance", CONFIG_GW_DEVICE_INSTANCE);
    cJSON_AddNumberToObject(j, "bacnet_received_packets", stats.received_packets);
    cJSON_AddNumberToObject(j, "good_points", stats.good_points);
    cJSON_AddNumberToObject(j, "fault_points", stats.fault_points);
    cJSON_AddBoolToObject(j, "read_only", true);
    return json_reply(request, j);
}

static esp_err_t points_handler(httpd_req_t *request)
{
    ats_model_t model;
    ats_value_t *values = calloc(ATS_POINT_COUNT, sizeof(*values));
    if (!values) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    xSemaphoreTake(lock, portMAX_DELAY);
    model = published.model;
    bool up = network_up;
    xSemaphoreGive(lock);
    if (!up) {
        for (size_t i = 0; i < ATS_BLOCK_COUNT; ++i)
            ats_model_fail_block(&model, ats_scan_blocks[i].offset,
                                 ats_scan_blocks[i].count, now_ms());
    }
    ats_model_decode_all(&model, now_ms(), values);
    cJSON *j = cJSON_CreateArray();
    if (!j) { free(values); return ESP_ERR_NO_MEM; }
    for (size_t i = 0; i < ATS_POINT_COUNT; ++i) {
        cJSON *p = cJSON_CreateObject();
        if (!p) { free(values); cJSON_Delete(j); return ESP_ERR_NO_MEM; }
        cJSON_AddStringToObject(p, "name", ats_points[i].name);
        cJSON_AddNumberToObject(p, "object_type", ats_points[i].object_type);
        cJSON_AddNumberToObject(p, "instance", ats_points[i].instance);
        cJSON_AddNumberToObject(p, "modbus_offset", ats_points[i].offset);
        cJSON_AddStringToObject(p, "quality", ats_quality_name(values[i].quality));
        cJSON_AddStringToObject(p, "quality_reason", values[i].quality_reason);
        if (ats_points[i].object_type == 40) cJSON_AddStringToObject(p, "value", values[i].text);
        else if (isfinite(values[i].numeric)) cJSON_AddNumberToObject(p, "value", values[i].numeric);
        else cJSON_AddNullToObject(p, "value");
        cJSON_AddItemToArray(j, p);
    }
    free(values);
    return json_reply(request, j);
}

void app_main(void)
{
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
    gateway_poll_init(&published);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    ethernet = esp_netif_new(&nc);
    configASSERT(ethernet);
    ESP_ERROR_CHECK(esp_netif_set_hostname(ethernet, CONFIG_GW_HOSTNAME));
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = 31;
    emac.smi_gpio.mdio_num = 52;
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 51;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    configASSERT(mac && phy);
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t driver;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &driver));
    ESP_ERROR_CHECK(esp_netif_attach(ethernet, esp_eth_new_netif_glue(driver)));
    if (CONFIG_GW_STATIC_IP[0]) {
        esp_netif_ip_info_t info = {0};
        esp_err_t stop = esp_netif_dhcpc_stop(ethernet);
        if (stop != ESP_OK && stop != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
            ESP_ERROR_CHECK(stop);
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_GW_STATIC_IP, &info.ip));
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_GW_NETMASK, &info.netmask));
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_GW_ROUTER, &info.gw));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(ethernet, &info));
    }
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, network_event, NULL));
    configASSERT(xTaskCreate(poll_task, "ats_modbus", 8192, NULL, 4, NULL) == pdPASS);
    configASSERT(xTaskCreate(bacnet_task, "ats_bacnet", 12288, NULL, 5, NULL) == pdPASS);
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.stack_size = 8192;
    http_config.max_open_sockets = 3;
    http_config.lru_purge_enable = true;
    httpd_handle_t http;
    ESP_ERROR_CHECK(httpd_start(&http, &http_config));
    const httpd_uri_t handlers[] = {
        {.uri="/", .method=HTTP_GET, .handler=status_handler},
        {.uri="/api/status", .method=HTTP_GET, .handler=status_handler},
        {.uri="/api/points", .method=HTTP_GET, .handler=points_handler},
    };
    for (size_t i=0; i<sizeof(handlers)/sizeof(handlers[0]); ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(http, &handlers[i]));
    ESP_ERROR_CHECK(esp_eth_start(driver));
    ESP_LOGI(TAG, "Read-only ATS gateway started; no GPIO field wiring required");
}
