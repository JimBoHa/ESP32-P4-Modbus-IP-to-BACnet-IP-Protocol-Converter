#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t consecutive_healthy_samples;
} ota_health_gate_t;

void ota_health_gate_reset(ota_health_gate_t *gate);

bool ota_health_gate_sample(
    ota_health_gate_t *gate,
    bool healthy,
    uint32_t required_consecutive_samples);
