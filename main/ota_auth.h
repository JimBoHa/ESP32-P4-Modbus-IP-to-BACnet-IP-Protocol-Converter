#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_BEARER_PREFIX "Bearer "
#define OTA_TOKEN_MIN_LENGTH 32U
#define OTA_TOKEN_MAX_LENGTH 128U
#define OTA_AUTHORIZATION_MAX_LENGTH \
    ((sizeof(OTA_BEARER_PREFIX) - 1U) + OTA_TOKEN_MAX_LENGTH)

bool ota_token_configuration_valid(const char *token);

typedef enum {
    OTA_ROLE_NONE = 0,
    OTA_ROLE_VIEWER,
    OTA_ROLE_ADMIN,
    OTA_ROLE_ANONYMOUS,
} ota_role_t;

bool ota_role_tokens_valid(const char *admin_token, const char *viewer_token);
ota_role_t ota_authorization_role(
    const char *authorization, size_t authorization_length,
    const char *admin_token, const char *viewer_token);
bool ota_role_allows(ota_role_t role, bool read_only);

bool ota_copy_embedded_token(
    const uint8_t *embedded,
    size_t embedded_length,
    char *token,
    size_t token_capacity);

bool ota_authorization_valid(
    const char *authorization,
    size_t authorization_length,
    const char *token);
