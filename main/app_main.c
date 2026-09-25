#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "gateway_bacnet.h"
#include "gateway_poll.h"
#include "gateway_web.h"
#include "error_history.h"
#if CONFIG_GW_OTA_ENABLED
#include "gateway_ota.h"
#endif

static const char *TAG = "ats_gateway";
static SemaphoreHandle_t lock;
static esp_netif_t *ethernet;
static bool network_up;
#if CONFIG_GW_OTA_ENABLED
static bool web_started;
static uint64_t poll_heartbeat_ms, bacnet_heartbeat_ms;
#endif
static uint32_t network_generation;
static uint32_t published_generation;
static esp_netif_ip_info_t network_info;
static gateway_poll_t published;
static gateway_bacnet_stats_t published_bacnet;
static gateway_config_t settings;
static custom_map_t custom_map;
static custom_poll_t published_custom;
static char config_error[192];
static const ats_point_def_t *active_points;
static size_t active_count;
static atomic_bool clock_synchronized;
static bool sntp_initialized;

/* Configuration and catalog remain immutable until the requested reboot. */
static bool is_custom(void) { return settings.profile == GW_PROFILE_CUSTOM; }

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

static void time_synchronized(struct timeval *tv)
{
    atomic_store(&clock_synchronized, tv && tv->tv_sec >= 1704067200);
}

static int64_t utc_ms(void)
{
    struct timeval tv;
    if (!atomic_load(&clock_synchronized) || gettimeofday(&tv, NULL) != 0 ||
        tv.tv_sec < 1704067200) return 0;
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static cJSON *errors_json(void)
{
    return error_history_json(atomic_load(&clock_synchronized));
}

static void record_failure(uint8_t function, uint16_t offset, uint16_t quantity,
                           uint16_t transaction_id, const char *phase,
                           const mb_result_t *result)
{
    error_history_request_t request = {
        .port = settings.modbus_port, .unit = settings.modbus_unit,
        .function = function, .offset = offset, .quantity = quantity,
        .transaction_id = transaction_id, .config_revision = settings.revision,
    };
    snprintf(request.host, sizeof(request.host), "%s", settings.modbus_host);
    snprintf(request.profile, sizeof(request.profile), "%s", gateway_profile_id(settings.profile));
    snprintf(request.phase, sizeof(request.phase), "%s", phase);
    /* Capture completion time and details before the next request replaces them.
     * Failure counters already exclude the expected old-map identity exception. */
    error_history_record(&request, result, now_ms(), utc_ms());
}

#if CONFIG_GW_OTA_ENABLED
static bool startup_healthy(void)
{
    const uint64_t now = now_ms();
    xSemaphoreTake(lock, portMAX_DELAY);
    bool healthy = network_up && web_started && gateway_ota_ready() &&
        !config_error[0] && published_bacnet.initialized && published_bacnet.link_up &&
        poll_heartbeat_ms && bacnet_heartbeat_ms &&
        now - poll_heartbeat_ms <= 2500 && now - bacnet_heartbeat_ms <= 2500;
    xSemaphoreGive(lock);
    return healthy;
}
#endif

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        network_info = ((ip_event_got_ip_t *)data)->ip_info;
        network_up = true;
        ++network_generation;
        ESP_LOGI(TAG, "IPv4 " IPSTR "; BACnet device %d, UDP %d", IP2STR(&network_info.ip),
                 (int)settings.device_instance, settings.bacnet_port);
    } else if ((base == ETH_EVENT && (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP)) ||
               (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP)) {
        network_up = false;
        ++network_generation;
        ESP_LOGW(TAG, "Ethernet unavailable");
    }
    xSemaphoreGive(lock);
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP && sntp_initialized) {
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) ESP_LOGW(TAG, "SNTP start: %s", esp_err_to_name(err));
    }
}

static void poll_task(void *arg)
{
    (void)arg;
    static gateway_poll_t poller;
    static custom_poll_t custom;
    gateway_poll_init(&poller);
    custom_poll_init(&custom, &custom_map);
    gateway_poll_config_t config = {
        .host = settings.modbus_host, .port = settings.modbus_port,
        .unit = settings.modbus_unit, .expected_firmware = settings.expected_firmware,
        .expected_mac_fragment = settings.expected_mac_fragment,
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
            custom_poll_offline(&custom, &custom_map, now_ms());
            poller.next_request_ms = custom.next_request_ms = now_ms();
        }
        if (up && !config_error[0] && is_custom()) {
            uint32_t before = custom.failures;
            custom_poll_step(&custom, &custom_map, settings.modbus_host,
                             settings.modbus_port, settings.modbus_unit, now_ms());
            if (custom.failures != before)
                record_failure(custom.last_function, custom.last_offset, custom.last_quantity,
                               custom.transaction_id, "data", &custom.last_result);
        } else if (up && !config_error[0]) {
            uint32_t before = poller.failures;
            gateway_poll_step(&poller, &config, now_ms());
            if (poller.failures != before) {
                record_failure(3, poller.last_offset, poller.last_quantity, poller.transaction_id,
                               poller.last_checking ? "identity" : "data", &poller.last_result);
                ESP_LOGW(TAG, "ATS offset %u: %s", poller.last_offset,
                         mb_error_string(poller.last_result.error));
            }
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        if (!network_up || network_generation != changed) {
            gateway_poll_offline(&poller, now_ms());
            custom_poll_offline(&custom, &custom_map, now_ms());
            poller.next_request_ms = custom.next_request_ms = now_ms();
        }
        published = poller;
        published_custom = custom;
        published_generation = changed;
#if CONFIG_GW_OTA_ENABLED
        poll_heartbeat_ms = now_ms();
#endif
        xSemaphoreGive(lock);
        previous_up = up;
        generation = changed;
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

/* Decode while holding the small shared snapshot lock; no I/O or allocation.
 * ATS electrical preset uses the same full decoder/qualification dependencies. */
static void snapshot_values(ats_value_t *values)
{
    uint64_t now = now_ms();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (is_custom()) custom_poll_snapshot(&published_custom, &custom_map, now, values);
    else ats_model_decode_all(&published.model, now, values);
    bool up = network_up && published_generation == network_generation;
    xSemaphoreGive(lock);
    if (!up || config_error[0]) for (size_t i = 0; i < active_count; ++i) {
        values[i].quality = ATS_QUALITY_COMM;
        values[i].quality_reason = config_error[0] ? "Configuration requires repair" : "Ethernet unavailable";
    }
}

static void bacnet_task(void *arg)
{
    (void)arg;
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
        xSemaphoreGive(lock);
        uint64_t now = now_ms();
        if (!started && up) {
            gateway_bacnet_config_t config = {
                .device_instance = settings.device_instance,
                .device_name = settings.device_name,
                .firmware_version = esp_app_get_description()->version,
                .location = "Modbus TCP gateway",
                .points = active_points, .point_count = active_count,
                .model_name = "ESP32-P4 Modbus BACnet",
                .description = is_custom() ? "Read-only custom CSV map" : "Read-only Kohler MPAC1500 ATS",
                .database_revision = settings.revision,
                .vendor_id = CONFIG_GW_VENDOR_ID,
                .local_ip = info.ip.addr, .netmask = info.netmask.addr,
                .gateway = info.gw.addr, .udp_port = settings.bacnet_port,
                .dhcp_enabled = CONFIG_GW_STATIC_IP[0] == '\0',
            };
            const char *peers[] = {CONFIG_GW_IAM_PEER1, CONFIG_GW_IAM_PEER2};
            for (size_t i = 0; i < 2; ++i) {
                struct in_addr peer;
                if (peers[i][0] && inet_pton(AF_INET, peers[i], &peer) == 1) {
                    config.peers[config.peer_count++] = (gateway_bacnet_peer_t){
                        .ip = peer.s_addr, .port = settings.bacnet_port};
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
                snapshot_values(values);
                gateway_bacnet_update(values);
                next_update = now + 200;
            }
            gateway_bacnet_tick(now);
            gateway_bacnet_poll(10);
            gateway_bacnet_stats_t stats;
            gateway_bacnet_stats(&stats);
            xSemaphoreTake(lock, portMAX_DELAY);
            published_bacnet = stats;
#if CONFIG_GW_OTA_ENABLED
            bacnet_heartbeat_ms = now_ms();
#endif
            xSemaphoreGive(lock);
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(started ? 10 : 500));
    }
}

static cJSON *status_json(void)
{
    gateway_bacnet_stats_t stats;
    uint32_t requests, successes, failures;
    uint64_t last;
    bool up, profile;
    const char *reason;
    xSemaphoreTake(lock, portMAX_DELAY);
    stats = published_bacnet;
    if (is_custom()) {
        requests = published_custom.requests; successes = published_custom.successes;
        failures = published_custom.failures; last = published_custom.last_success_ms;
        profile = false; reason = "User-defined CSV; controller identity is not checked";
    } else {
        requests = published.requests; successes = published.successes; failures = published.failures;
        last = published.last_success_ms; profile = published.profile_valid; reason = published.profile_status;
    }
    up = network_up;
    xSemaphoreGive(lock);
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "application", "ESP32-P4 Modbus IP to BACnet IP Protocol Converter");
    cJSON_AddStringToObject(j, "project", esp_app_get_description()->project_name);
    cJSON_AddStringToObject(j, "git_revision", PROJECT_GIT_REVISION);
    cJSON_AddStringToObject(j, "board", "Waveshare ESP32-P4-POE-ETH");
    cJSON_AddNumberToObject(j, "uptime_seconds", now_ms() / 1000.0);
    cJSON_AddNumberToObject(j, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "free_internal_heap_bytes", esp_get_free_internal_heap_size());
    cJSON_AddNumberToObject(j, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());
    cJSON_AddBoolToObject(j, "ethernet_up", up);
    cJSON_AddBoolToObject(j, "profile_verified", profile);
    cJSON_AddStringToObject(j, "profile_status", reason ? reason : "Initializing");
    cJSON_AddStringToObject(j, "modbus_host", settings.modbus_host);
    cJSON_AddStringToObject(j, "profile", gateway_profile_id(settings.profile));
    cJSON_AddStringToObject(j, "config_error", config_error);
    cJSON_AddNumberToObject(j, "point_count", active_count);
    cJSON_AddNumberToObject(j, "modbus_unit", settings.modbus_unit);
    cJSON_AddNumberToObject(j, "modbus_requests", requests);
    cJSON_AddNumberToObject(j, "modbus_successful_responses", successes);
    cJSON_AddNumberToObject(j, "modbus_failures", failures);
    if (last) cJSON_AddNumberToObject(j, "last_modbus_response_age_seconds", (now_ms() - last) / 1000.0);
    else cJSON_AddNullToObject(j, "last_modbus_response_age_seconds");
    cJSON_AddNumberToObject(j, "bacnet_device_instance", settings.device_instance);
    cJSON_AddNumberToObject(j, "bacnet_received_packets", stats.received_packets);
    cJSON_AddNumberToObject(j, "good_points", stats.good_points);
    cJSON_AddNumberToObject(j, "fault_points", stats.fault_points);
    cJSON_AddBoolToObject(j, "read_only", true);
    cJSON_AddBoolToObject(j, "clock_synchronized", atomic_load(&clock_synchronized));
    int64_t timestamp = utc_ms();
    if (timestamp) cJSON_AddNumberToObject(j, "utc_ms", (double)timestamp);
    else cJSON_AddNullToObject(j, "utc_ms");
    return j;
}

static cJSON *points_json(void)
{
    ats_value_t *values = calloc(ATS_POINT_COUNT, sizeof(*values));
    if (!values) return NULL;
    snapshot_values(values);
    cJSON *j = cJSON_CreateArray();
    if (!j) { free(values); return NULL; }
    for (size_t i = 0; i < active_count; ++i) {
        cJSON *p = cJSON_CreateObject();
        if (!p) { free(values); cJSON_Delete(j); return NULL; }
        cJSON_AddStringToObject(p, "name", active_points[i].name);
        cJSON_AddNumberToObject(p, "object_type", active_points[i].object_type);
        cJSON_AddNumberToObject(p, "instance", active_points[i].instance);
        cJSON_AddNumberToObject(p, "modbus_offset", active_points[i].offset);
        if (is_custom()) {
            cJSON_AddNumberToObject(p, "modbus_function", custom_map.points[i].function);
            cJSON_AddNumberToObject(p, "poll_ms", custom_map.points[i].poll_ms);
            cJSON_AddNumberToObject(p, "stale_ms", custom_map_stale_ms(&custom_map, i));
        }
        if (values[i].last_good_ms)
            cJSON_AddNumberToObject(p, "sample_age_seconds", (now_ms() - values[i].last_good_ms) / 1000.0);
        else cJSON_AddNullToObject(p, "sample_age_seconds");
        cJSON_AddStringToObject(p, "quality", ats_quality_name(values[i].quality));
        cJSON_AddStringToObject(p, "quality_reason", values[i].quality_reason);
        if (active_points[i].object_type == 40) cJSON_AddStringToObject(p, "value", values[i].text);
        else if (isfinite(values[i].numeric)) cJSON_AddNumberToObject(p, "value", values[i].numeric);
        else cJSON_AddNullToObject(p, "value");
        cJSON_AddItemToArray(j, p);
    }
    free(values);
    return j;
}

void app_main(void)
{
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
#if CONFIG_GW_OTA_ENABLED
    ESP_ERROR_CHECK(gateway_ota_begin_validation(startup_healthy));
#endif
    gateway_poll_init(&published);
    gateway_config_defaults(&settings);
    snprintf(settings.modbus_host, sizeof(settings.modbus_host), "%s", CONFIG_GW_MODBUS_HOST);
    snprintf(settings.device_name, sizeof(settings.device_name), "%s", CONFIG_GW_DEVICE_NAME);
    settings.modbus_port = CONFIG_GW_MODBUS_PORT;
    settings.modbus_unit = CONFIG_GW_MODBUS_UNIT;
    settings.device_instance = CONFIG_GW_DEVICE_INSTANCE;
    settings.bacnet_port = CONFIG_GW_BACNET_PORT;
    settings.expected_firmware = CONFIG_GW_EXPECTED_FIRMWARE;
    settings.expected_mac_fragment = CONFIG_GW_EXPECTED_MAC_FRAGMENT;
    ESP_ERROR_CHECK(nvs_flash_init());
    gateway_web_load(&settings, &custom_map, config_error, sizeof(config_error));
    error_history_init();
    if (config_error[0]) ESP_LOGE(TAG, "%s; polling disabled until repaired", config_error);
    active_points = is_custom() ? custom_map.defs : ats_points;
    active_count = is_custom() ? custom_map.count :
        settings.profile == GW_PROFILE_ELECTRICAL ? GW_ELECTRICAL_POINT_COUNT : ATS_POINT_COUNT;
    custom_poll_init(&published_custom, &custom_map);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (CONFIG_GW_NTP_SERVER[0]) {
        esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_GW_NTP_SERVER);
        time_config.start = false;
        time_config.wait_for_sync = false;
        time_config.sync_cb = time_synchronized;
        esp_err_t err = esp_netif_sntp_init(&time_config);
        sntp_initialized = err == ESP_OK;
        if (err != ESP_OK) ESP_LOGW(TAG, "SNTP init: %s; history uses uptime", esp_err_to_name(err));
    }
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
    gateway_web_start(&settings, &custom_map, config_error, status_json, points_json, errors_json);
#if CONFIG_GW_OTA_ENABLED
    xSemaphoreTake(lock, portMAX_DELAY);
    web_started = true;
    xSemaphoreGive(lock);
#endif
    ESP_ERROR_CHECK(esp_eth_start(driver));
    ESP_LOGI(TAG, "Read-only Modbus gateway started; configure profiles and CSV maps in the web interface");
}
