/* SPDX-License-Identifier: 0BSD */
#ifndef GATEWAY_BACNET_H
#define GATEWAY_BACNET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ats_model.h"

#define GATEWAY_BACNET_MAX_PEERS 4u
#define GATEWAY_BACNET_MAX_POINTS 256u
#define GATEWAY_BACNET_POINT_NAME_MAX 95u
#define GATEWAY_BACNET_POINT_DESCRIPTION_MAX 511u
#define GATEWAY_BACNET_STATE_NAME_MAX 63u
#define GATEWAY_BACNET_MAX_STATES 64u
typedef struct {
    uint32_t ip; /* IPv4 in network byte order. */
    uint16_t port; /* host byte order; zero uses BACnet listening port. */
} gateway_bacnet_peer_t;
typedef struct {
    uint32_t device_instance;
    const char *device_name;
    const char *firmware_version;
    const char *location;
    uint16_t vendor_id; /* Site/application vendor ID; do not claim Kohler ID. */
    uint32_t local_ip, netmask, gateway; /* network byte order */
    uint16_t udp_port; /* host byte order, normally 47808 */
    bool dhcp_enabled;
    gateway_bacnet_peer_t peers[GATEWAY_BACNET_MAX_PEERS];
    size_t peer_count;
    /* NULL/0 selects the built-in ATS map. Otherwise both fields are required.
     * Catalog and all referenced strings remain valid until shutdown. */
    const ats_point_def_t *points;
    size_t point_count;
    const char *model_name; /* Optional, at most 32 bytes. */
    const char *description; /* Optional, at most 64 bytes. */
    uint32_t database_revision; /* 0 uses 1; increment for each changed map. */
} gateway_bacnet_config_t;
typedef struct {
    bool initialized, link_up;
    uint32_t received_packets, good_points, fault_points;
    uint32_t cov_timeouts, cov_refreshes, cov_pending;
} gateway_bacnet_stats_t;

/* All functions belong to one BACnet task. Caller snapshots Modbus state
 * under its own lock, then calls update without holding that lock. */
bool gateway_bacnet_init(const gateway_bacnet_config_t *config, uint64_t now_ms);
/* values contains one entry per active catalog point, in catalog order. */
void gateway_bacnet_update(const ats_value_t *values);
void gateway_bacnet_tick(uint64_t now_ms);
unsigned gateway_bacnet_poll(unsigned timeout_ms);
bool gateway_bacnet_network_update(uint32_t ip, uint32_t mask, uint32_t gateway,
                                   bool link_up, uint64_t now_ms);
void gateway_bacnet_stats(gateway_bacnet_stats_t *out);
void gateway_bacnet_shutdown(void);
/* Production parser entry, also usable by an in-memory native test transport. */
bool gateway_bacnet_process_datagram(const uint8_t *data, size_t size,
                                    uint32_t source_ip, uint16_t source_port);
#endif
