/* SPDX-License-Identifier: 0BSD */
#include "bip_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include "bacnet/bacdcode.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/datalink/bvlc.h"

static int s_socket = -1;
static uint16_t s_port = 47808;
static uint16_t s_broadcast_port;
static BACNET_IP_ADDRESS s_address;
static BACNET_IP_ADDRESS s_broadcast;
static BACNET_IP_ADDRESS s_gateway;
static uint8_t s_prefix;
static bool s_debug;
/* The BACnet task consumes each NPDU synchronously, so this records the BVLC
   origin type for the service handler invoked by that same receive call. */
static bool s_last_receive_was_broadcast;
static char s_interface[16] = "eth0";
#ifndef ESP_PLATFORM
static bip_port_send_fn s_send_hook;
static void *s_send_context;
void bip_port_set_send_hook(bip_port_send_fn callback, void *context)
{
    s_send_hook = callback;
    s_send_context = context;
}
#endif

static uint8_t contiguous_prefix(uint32_t network_mask)
{
    uint32_t mask = ntohl(network_mask);
    uint8_t prefix = 0;
    while (mask & 0x80000000U) {
        ++prefix;
        mask <<= 1;
    }
    return mask == 0 ? prefix : 0;
}

void bip_port_configure(uint32_t ip_network_order, uint32_t netmask_network_order,
    uint32_t gateway_network_order, uint16_t port)
{
    memcpy(s_address.address, &ip_network_order, 4);
    s_address.port = port;
    s_port = port;
    uint32_t broadcast = (ip_network_order & netmask_network_order) | ~netmask_network_order;
    memcpy(s_broadcast.address, &broadcast, 4);
    s_broadcast.port = port;
    memcpy(s_gateway.address, &gateway_network_order, 4);
    s_gateway.port = 0;
    s_prefix = contiguous_prefix(netmask_network_order);
}

bool bip_init(const char *ifname)
{
    if (ifname && ifname[0]) {
        bip_set_interface(ifname);
    }
    bip_cleanup();
#ifndef ESP_PLATFORM
    if (s_send_hook) {
        return true;
    }
#endif
    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0) {
        return false;
    }
    int enabled = 1;
    (void)setsockopt(s_socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (setsockopt(s_socket, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) < 0) {
        bip_cleanup();
        return false;
    }
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(s_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_socket, (struct sockaddr *)&local, sizeof(local)) < 0) {
        bip_cleanup();
        return false;
    }
    int flags = fcntl(s_socket, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(s_socket, F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}

void bip_set_interface(const char *ifname)
{
    if (ifname) {
        snprintf(s_interface, sizeof(s_interface), "%s", ifname);
    }
}

const char *bip_get_interface(void)
{
    return s_interface;
}

void bip_cleanup(void)
{
    if (s_socket >= 0) {
        close(s_socket);
        s_socket = -1;
    }
}

bool bip_valid(void)
{
#ifndef ESP_PLATFORM
    if (s_send_hook) {
        return true;
    }
#endif
    return s_socket >= 0;
}

void bip_get_broadcast_address(BACNET_ADDRESS *dest)
{
    if (!dest) {
        return;
    }
    memset(dest, 0, sizeof(*dest));
    dest->mac_len = 6;
    memcpy(dest->mac, s_broadcast.address, 4);
    encode_unsigned16(&dest->mac[4], bip_get_broadcast_port());
    dest->net = BACNET_BROADCAST_NETWORK;
}

void bip_get_my_address(BACNET_ADDRESS *address)
{
    if (!address) {
        return;
    }
    memset(address, 0, sizeof(*address));
    address->mac_len = 6;
    memcpy(address->mac, s_address.address, 4);
    encode_unsigned16(&address->mac[4], s_port);
}

int bip_send_mpdu(const BACNET_IP_ADDRESS *dest, const uint8_t *mtu, uint16_t mtu_len)
{
#ifndef ESP_PLATFORM
    if (s_send_hook) {
        return s_send_hook(dest, mtu, mtu_len, s_send_context);
    }
#endif
    if (s_socket < 0 || !dest || !mtu || mtu_len == 0) {
        return -1;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(dest->port),
    };
    memcpy(&address.sin_addr.s_addr, dest->address, 4);
    int result = sendto(s_socket, mtu, mtu_len, 0,
        (struct sockaddr *)&address, sizeof(address));
    if (s_debug && result < 0) {
        printf("BIP send error %d\n", errno);
    }
    return result;
}

int bip_send_pdu(BACNET_ADDRESS *dest, BACNET_NPDU_DATA *npdu_data,
    uint8_t *pdu, unsigned pdu_len)
{
    (void)npdu_data;
    if (!dest || !pdu || pdu_len > MAX_PDU) {
        return -1;
    }
    uint8_t frame[BIP_MPDU_MAX];
    BACNET_IP_ADDRESS target = {0};
    int frame_len;

    if (dest->net == BACNET_BROADCAST_NETWORK || dest->mac_len == 0 ||
        (dest->net > 0 && dest->len == 0 && dest->mac_len != 6)) {
        target = s_broadcast;
        target.port = bip_get_broadcast_port();
        frame_len = bvlc_encode_original_broadcast(frame, sizeof(frame), pdu, (uint16_t)pdu_len);
    } else if (dest->mac_len == 6) {
        memcpy(target.address, dest->mac, 4);
        decode_unsigned16(&dest->mac[4], &target.port);
        frame_len = bvlc_encode_original_unicast(frame, sizeof(frame), pdu, (uint16_t)pdu_len);
    } else {
        return -1;
    }
    int result = frame_len > 0 ? bip_send_mpdu(&target, frame, (uint16_t)frame_len) : -1;
    return result;
}

uint16_t bip_receive(BACNET_ADDRESS *src, uint8_t *pdu, uint16_t max_pdu, unsigned timeout)
{
    if (s_socket < 0 || !src || !pdu || max_pdu < 4) {
        return 0;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(s_socket, &read_set);
    struct timeval wait = {
        .tv_sec = (time_t)(timeout / 1000U),
        .tv_usec = (suseconds_t)((timeout % 1000U) * 1000U),
    };
    int ready = select(s_socket + 1, &read_set, NULL, NULL, &wait);
    if (ready <= 0) {
        return 0;
    }

    struct sockaddr_in remote = {0};
    socklen_t remote_length = sizeof(remote);
    uint8_t frame[BIP_MPDU_MAX];
    int received = recvfrom(s_socket, frame, sizeof(frame), 0,
        (struct sockaddr *)&remote, &remote_length);
    if (received <= 0) {
        return 0;
    }
    return bip_port_decode_datagram(frame, (size_t)received,
        remote.sin_addr.s_addr, ntohs(remote.sin_port), src, pdu, max_pdu);
}

uint16_t bip_port_decode_datagram(const uint8_t *frame, size_t received,
    uint32_t source_ip, uint16_t source_port, BACNET_ADDRESS *src,
    uint8_t *pdu, uint16_t max_pdu)
{
    if (!frame || !src || !pdu || received < 6 || received > UINT16_MAX ||
        frame[0] != BVLL_TYPE_BACNET_IP) {
        return 0;
    }
    uint8_t function = 0;
    uint16_t declared_length = 0;
    if (bvlc_decode_header(frame, (uint16_t)received, &function, &declared_length) != 4 ||
        declared_length != (uint16_t)received) {
        return 0;
    }

    BACNET_IP_ADDRESS source = {.port = source_port};
    memcpy(source.address, &source_ip, 4);
    uint16_t offset;
    if (function == BVLC_ORIGINAL_UNICAST_NPDU) {
        s_last_receive_was_broadcast = false;
        offset = 4;
    } else if (function == BVLC_ORIGINAL_BROADCAST_NPDU) {
        s_last_receive_was_broadcast = true;
        offset = 4;
    } else if (function == BVLC_FORWARDED_NPDU && received >= 10) {
        s_last_receive_was_broadcast = true;
        memcpy(source.address, &frame[4], 4);
        decode_unsigned16(&frame[8], &source.port);
        offset = 10;
    } else {
        return 0;
    }

    if (memcmp(source.address, s_address.address, 4) == 0 && source.port == s_port) {
        return 0;
    }
    uint16_t npdu_len = (uint16_t)received - offset;
    if (npdu_len > max_pdu) {
        return 0;
    }
    memset(src, 0, sizeof(*src));
    src->mac_len = 6;
    memcpy(src->mac, source.address, 4);
    encode_unsigned16(&src->mac[4], source.port);
    if (npdu_len < 2) {
        return 0;
    }
    /* Metasys can carry a global-broadcast NPDU in a unicast BVLC frame.
     * Its discovery response still needs broadcast routing semantics. */
    if (npdu_len >= 6 && (frame[offset + 1] & 0x20u) &&
        frame[offset + 2] == 0xffu && frame[offset + 3] == 0xffu &&
        frame[offset + 4] == 0) {
        s_last_receive_was_broadcast = true;
    }
    memcpy(pdu, &frame[offset], npdu_len);
    return npdu_len;
}

bool bip_port_last_receive_was_broadcast(void)
{
    return s_last_receive_was_broadcast;
}

void bip_set_port(uint16_t port)
{
    s_port = port;
    s_address.port = port;
}

void bip_set_broadcast_port(uint16_t port)
{
    s_broadcast_port = port;
    s_broadcast.port = port;
}

bool bip_port_changed(void)
{
    return false;
}

uint16_t bip_get_port(void)
{
    return s_port;
}

uint16_t bip_get_broadcast_port(void)
{
    return s_broadcast_port ? s_broadcast_port : s_port;
}

bool bip_set_addr(const BACNET_IP_ADDRESS *address)
{
    if (!address) {
        return false;
    }
    s_address = *address;
    s_port = address->port;
    return true;
}

bool bip_get_addr(BACNET_IP_ADDRESS *address)
{
    if (!address) {
        return false;
    }
    *address = s_address;
    return true;
}

bool bip_get_addr_by_name(const char *host_name, BACNET_IP_ADDRESS *address)
{
    if (!host_name || !address) {
        return false;
    }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *result = NULL;
    if (getaddrinfo(host_name, NULL, &hints, &result) != 0 || !result) {
        return false;
    }
    struct sockaddr_in *resolved = (struct sockaddr_in *)result->ai_addr;
    memcpy(address->address, &resolved->sin_addr.s_addr, 4);
    freeaddrinfo(result);
    return true;
}

bool bip_set_broadcast_addr(const BACNET_IP_ADDRESS *address)
{
    if (!address) {
        return false;
    }
    s_broadcast = *address;
    return true;
}

bool bip_get_broadcast_addr(BACNET_IP_ADDRESS *address)
{
    if (!address) {
        return false;
    }
    *address = s_broadcast;
    address->port = bip_get_broadcast_port();
    return true;
}

bool bip_get_gateway_addr(BACNET_IP_ADDRESS *address)
{
    if (!address) {
        return false;
    }
    *address = s_gateway;
    return true;
}

bool bip_set_subnet_prefix(uint8_t prefix)
{
    if (prefix == 0 || prefix > 32) {
        return false;
    }
    s_prefix = prefix;
    uint32_t mask = htonl(prefix == 32 ? UINT32_MAX : UINT32_MAX << (32 - prefix));
    uint32_t address;
    memcpy(&address, s_address.address, 4);
    uint32_t broadcast = (address & mask) | ~mask;
    memcpy(s_broadcast.address, &broadcast, 4);
    return true;
}

uint8_t bip_get_subnet_prefix(void)
{
    return s_prefix;
}

void bip_debug_enable(void)
{
    s_debug = true;
}

void bip_debug_disable(void)
{
    s_debug = false;
}

int bip_get_socket(void)
{
    return s_socket;
}

int bip_get_broadcast_socket(void)
{
    return s_socket;
}

int bip_set_broadcast_binding(const char *ip4_broadcast)
{
    struct in_addr address;
    if (!ip4_broadcast || inet_aton(ip4_broadcast, &address) == 0) {
        return -1;
    }
    memcpy(s_broadcast.address, &address.s_addr, 4);
    return 0;
}

void bvlc_maintenance_timer(uint16_t seconds)
{
    (void)seconds;
}
