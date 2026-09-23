/* Adapted from JimBoHa/esp32-p4-bacnet-switches dashboard_redirect.c;
 * redirects the converter console to its HTTPS root. */
#include "dashboard_redirect.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "lwip/inet.h"
#include "lwip/sockets.h"

static httpd_handle_t redirect_server;
static uint16_t redirect_https_port;

static esp_err_t reject_and_close(httpd_req_t *request, const char *status)
{
    esp_err_t result = httpd_resp_set_status(request, status);
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(request, "Connection", "close");
    }
    if (result == ESP_OK) {
        (void)httpd_resp_send(request, NULL, 0);
    }
    /* ESP_FAIL closes immediately, skipping ESP-IDF's unread-body drain. */
    return ESP_FAIL;
}

static esp_err_t send_redirect(httpd_req_t *request, const char *location)
{
    esp_err_t result = httpd_resp_set_status(request, "302 Found");
    if (result == ESP_OK) {
        result = httpd_resp_set_type(request, "text/plain; charset=utf-8");
    }
    const struct { const char *name; const char *value; } headers[] = {
        {"Location", location},
        {"Cache-Control", "no-store"},
        {"Referrer-Policy", "no-referrer"},
        {"X-Content-Type-Options", "nosniff"},
    };
    for (size_t index = 0; result == ESP_OK && index < sizeof(headers) / sizeof(headers[0]); ++index) {
        result = httpd_resp_set_hdr(request, headers[index].name, headers[index].value);
    }
    /* Empty response also gives HEAD correct semantics; no reflected input. */
    return result == ESP_OK ? httpd_resp_send(request, NULL, 0) : result;
}

esp_err_t dashboard_root_redirect_handler(httpd_req_t *request)
{
    if (request->method != HTTP_GET && request->method != HTTP_HEAD) {
        return reject_and_close(request, "405 Method Not Allowed");
    }
    if (request->content_len != 0U) {
        return reject_and_close(request, "400 Bad Request");
    }
    return send_redirect(request, "/");
}

static bool local_https_location(httpd_req_t *request, char *location, size_t capacity)
{
    struct sockaddr_storage local = {0};
    socklen_t length = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(request), (struct sockaddr *)&local, &length) != 0) {
        return false;
    }
    char address[46]; /* Longest numeric IPv6 address, including terminator. */
    const void *binary_address = NULL;
    int family = local.ss_family;
    bool brackets = false;
    if (family == AF_INET && length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)&local;
        if (ipv4->sin_addr.s_addr == htonl(INADDR_ANY)) {
            return false;
        }
        binary_address = &ipv4->sin_addr;
    }
#if LWIP_IPV6
    else if (family == AF_INET6 && length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)&local;
        if (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr)) {
            family = AF_INET;
            binary_address = &ipv6->sin6_addr.s6_addr[12];
            const uint8_t *mapped = binary_address;
            if ((mapped[0] | mapped[1] | mapped[2] | mapped[3]) == 0U) {
                return false;
            }
        } else {
            /* The client's link-local zone is not knowable from this socket. */
            if (IN6_IS_ADDR_UNSPECIFIED(&ipv6->sin6_addr) || IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr)) {
                return false;
            }
            binary_address = &ipv6->sin6_addr;
            brackets = true;
        }
    }
#endif
    else {
        return false;
    }
    if (inet_ntop(family, binary_address, address, sizeof(address)) == NULL) {
        return false;
    }
    char port[8] = "";
    if (redirect_https_port != 443U) {
        (void)snprintf(port, sizeof(port), ":%u", (unsigned)redirect_https_port);
    }
    /* Never use Host, request URI/query, forwarded headers, or credentials. */
    const int written = snprintf(location, capacity, "https://%s%s%s%s/",
                                 brackets ? "[" : "", address, brackets ? "]" : "", port);
    return written > 0 && (size_t)written < capacity;
}

static esp_err_t plaintext_redirect_handler(httpd_req_t *request)
{
    if (request->method != HTTP_GET && request->method != HTTP_HEAD) {
        return reject_and_close(request, "405 Method Not Allowed");
    }
    if (request->content_len != 0U) {
        return reject_and_close(request, "400 Bad Request");
    }
    char location[96];
    esp_err_t result = httpd_resp_set_hdr(request, "Connection", "close");
    if (result == ESP_OK) {
        if (!local_https_location(request, location, sizeof(location))) {
            result = httpd_resp_set_status(request, "503 Service Unavailable");
            if (result == ESP_OK) {
                result = httpd_resp_send(request, NULL, 0);
            }
        } else {
            result = send_redirect(request, location);
        }
    }
    const esp_err_t close_result = httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    return result == ESP_OK ? close_result : result;
}

esp_err_t dashboard_redirect_start(uint16_t https_port)
{
    if (https_port == 0U || https_port == 80U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (redirect_server != NULL) {
        return redirect_https_port == https_port ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32769; /* HTTPS keeps its distinct default control port. */
    config.max_uri_handlers = 2;
    config.max_resp_headers = 5;
    config.max_open_sockets = 1;
    config.backlog_conn = 2;
    config.lru_purge_enable = true;
    config.stack_size = 4096;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    config.keep_alive_enable = false;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t created = NULL;
    esp_err_t result = httpd_start(&created, &config);
    if (result != ESP_OK) {
        return result;
    }
    const httpd_uri_t get_uri = {
        .uri = "/*", .method = HTTP_GET, .handler = plaintext_redirect_handler,
    };
    const httpd_uri_t head_uri = {
        .uri = "/*", .method = HTTP_HEAD, .handler = plaintext_redirect_handler,
    };
    redirect_https_port = https_port;
    result = httpd_register_uri_handler(created, &get_uri);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(created, &head_uri);
    }
    if (result != ESP_OK) {
        (void)httpd_stop(created);
        redirect_https_port = 0;
        return result;
    }
    redirect_server = created;
    return ESP_OK;
}

void dashboard_redirect_stop(void)
{
    if (redirect_server != NULL) {
        (void)httpd_stop(redirect_server);
        redirect_server = NULL;
        redirect_https_port = 0;
    }
}
