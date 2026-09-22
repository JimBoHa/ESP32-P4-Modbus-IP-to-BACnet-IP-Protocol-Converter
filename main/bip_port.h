/* SPDX-License-Identifier: 0BSD */
#ifndef GATEWAY_BIP_PORT_H
#define GATEWAY_BIP_PORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bacnet/bacdef.h"
#include "bacnet/datalink/bip.h"
void bip_port_configure(uint32_t ip, uint32_t mask, uint32_t gateway, uint16_t port);
bool bip_port_last_receive_was_broadcast(void);
uint16_t bip_port_decode_datagram(const uint8_t *frame, size_t length,
    uint32_t source_ip, uint16_t source_port, BACNET_ADDRESS *source,
    uint8_t *npdu, uint16_t capacity);
#ifndef ESP_PLATFORM
/* Native alternative transport: production packet bytes, no socket or LAN. */
typedef int (*bip_port_send_fn)(const BACNET_IP_ADDRESS *, const uint8_t *, uint16_t, void *);
void bip_port_set_send_hook(bip_port_send_fn callback, void *context);
#endif
#endif
