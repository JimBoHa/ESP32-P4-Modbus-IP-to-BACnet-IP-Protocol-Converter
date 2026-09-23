#ifndef TEST_OTA_SECURE_H
#define TEST_OTA_SECURE_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { unsigned num_digests; uint8_t key_digests[3][32]; } esp_image_sig_public_key_digests_t;
esp_err_t esp_secure_boot_get_signature_blocks_for_running_app(bool, esp_image_sig_public_key_digests_t *);
#endif
