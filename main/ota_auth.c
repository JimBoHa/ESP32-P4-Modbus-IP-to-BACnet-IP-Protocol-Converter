#include "ota_auth.h"

#include <stdint.h>
#include <string.h>

static size_t bounded_length(const char *value, size_t maximum)
{
    if (value == NULL) {
        return 0;
    }
    size_t length = 0;
    while (length < maximum && value[length] != '\0') {
        length++;
    }
    return length;
}

bool ota_token_configuration_valid(const char *token)
{
    const size_t length = bounded_length(token, OTA_TOKEN_MAX_LENGTH + 1U);
    if (length < OTA_TOKEN_MIN_LENGTH || length > OTA_TOKEN_MAX_LENGTH) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        if ((unsigned char)token[index] < 0x21U ||
            (unsigned char)token[index] > 0x7EU) {
            return false;
        }
    }
    return true;
}

bool ota_role_tokens_valid(const char *admin_token, const char *viewer_token)
{
    return ota_token_configuration_valid(admin_token) &&
        ota_token_configuration_valid(viewer_token) &&
        strcmp(admin_token, viewer_token) != 0;
}

ota_role_t ota_authorization_role(
    const char *authorization, size_t authorization_length,
    const char *admin_token, const char *viewer_token)
{
    if (!ota_role_tokens_valid(admin_token, viewer_token)) {
        return OTA_ROLE_NONE;
    }
    /* Missing credentials grant read-only access, never administrative access.
     * An explicitly supplied invalid credential still fails authentication. */
    if (authorization == NULL && authorization_length == 0U) {
        return OTA_ROLE_ANONYMOUS;
    }
    const bool admin = ota_authorization_valid(
        authorization, authorization_length, admin_token);
    const bool viewer = ota_authorization_valid(
        authorization, authorization_length, viewer_token);
    return admin ? OTA_ROLE_ADMIN : viewer ? OTA_ROLE_VIEWER : OTA_ROLE_NONE;
}

bool ota_role_allows(ota_role_t role, bool read_only)
{
    return role == OTA_ROLE_ADMIN ||
        ((role == OTA_ROLE_VIEWER || role == OTA_ROLE_ANONYMOUS) && read_only);
}

bool ota_copy_embedded_token(
    const uint8_t *embedded,
    size_t embedded_length,
    char *token,
    size_t token_capacity)
{
    if (embedded == NULL || token == NULL) {
        return false;
    }
    while (embedded_length > 0U &&
           (embedded[embedded_length - 1U] == '\0' ||
            embedded[embedded_length - 1U] == '\n' ||
            embedded[embedded_length - 1U] == '\r' ||
            embedded[embedded_length - 1U] == ' ' ||
            embedded[embedded_length - 1U] == '\t')) {
        embedded_length--;
    }
    if (embedded_length < OTA_TOKEN_MIN_LENGTH ||
        embedded_length > OTA_TOKEN_MAX_LENGTH ||
        token_capacity <= embedded_length) {
        return false;
    }
    for (size_t index = 0; index < embedded_length; ++index) {
        if (embedded[index] < 0x21U || embedded[index] > 0x7EU) {
            return false;
        }
    }
    memcpy(token, embedded, embedded_length);
    token[embedded_length] = '\0';
    return ota_token_configuration_valid(token);
}

bool ota_authorization_valid(
    const char *authorization,
    size_t authorization_length,
    const char *token)
{
    static const char PREFIX[] = OTA_BEARER_PREFIX;

    if (authorization == NULL || !ota_token_configuration_valid(token)) {
        return false;
    }
    const size_t prefix_length = sizeof(PREFIX) - 1U;
    const size_t token_length = strlen(token);
    if (authorization_length != prefix_length + token_length) {
        return false;
    }

    uint8_t difference = 0;
    for (size_t index = 0; index < prefix_length; ++index) {
        difference |= (uint8_t)authorization[index] ^ (uint8_t)PREFIX[index];
    }
    for (size_t index = 0; index < token_length; ++index) {
        difference |=
            (uint8_t)authorization[prefix_length + index] ^
            (uint8_t)token[index];
    }
    return difference == 0U;
}
