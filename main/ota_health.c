#include "ota_health.h"

#include <stddef.h>

void ota_health_gate_reset(ota_health_gate_t *gate)
{
    if (gate != NULL) {
        gate->consecutive_healthy_samples = 0U;
    }
}

bool ota_health_gate_sample(
    ota_health_gate_t *gate,
    bool healthy,
    uint32_t required_consecutive_samples)
{
    if (gate == NULL || required_consecutive_samples == 0U) {
        return false;
    }
    if (!healthy) {
        gate->consecutive_healthy_samples = 0U;
        return false;
    }
    if (gate->consecutive_healthy_samples < required_consecutive_samples) {
        gate->consecutive_healthy_samples++;
    }
    return gate->consecutive_healthy_samples >= required_consecutive_samples;
}
