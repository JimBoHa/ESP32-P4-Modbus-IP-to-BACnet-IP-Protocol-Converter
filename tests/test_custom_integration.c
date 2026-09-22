/* Imported CSV -> real loopback TCP Modbus -> production BACnet packet handlers.
 * BACnet uses an in-memory datalink; all TCP sockets bind/connect 127.0.0.1 only. */
#include "custom_map.h"
#include "gateway_bacnet.h"
#include "bip_port.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "bacnet/bacapp.h"
#include "bacnet/bacdcode.h"
#include "bacnet/npdu.h"
#include "bacnet/rp.h"
#include "bacnet/whois.h"

static custom_map_t map;
static custom_poll_t state;
static ats_value_t samples[CUSTOM_MAX_POINTS];
static uint8_t last_frame[MAX_PDU + 32];
static uint16_t last_length;
static unsigned frames;
static uint8_t invoke = 1;

static int send_frame(const BACNET_IP_ADDRESS *destination, const uint8_t *bytes,
                       uint16_t length, void *context)
{
    (void)destination; (void)context;
    assert(length <= sizeof(last_frame));
    memcpy(last_frame, bytes, length); last_length = length; ++frames;
    return length;
}

static void inject(const uint8_t *apdu, unsigned length, bool broadcast)
{
    uint8_t frame[MAX_PDU + 32] = {0x81, broadcast ? 0x0b : 0x0a};
    encode_unsigned16(frame + 2, (uint16_t)(length + 6));
    frame[4] = 1; frame[5] = 0;
    assert(length + 6 <= sizeof(frame));
    memcpy(frame + 6, apdu, length);
    assert(gateway_bacnet_process_datagram(frame, length + 6, inet_addr("127.0.0.2"), 47809));
}

static int last_apdu(uint8_t **apdu)
{
    BACNET_ADDRESS destination = {0}, source = {0};
    BACNET_NPDU_DATA npdu = {0};
    int offset = bacnet_npdu_decode(last_frame + 4, last_length - 4, &destination, &source, &npdu);
    assert(offset >= 2 && !npdu.network_layer_message);
    *apdu = last_frame + 4 + offset;
    return last_length - 4 - offset;
}

static BACNET_APPLICATION_DATA_VALUE read_property(BACNET_OBJECT_TYPE type, uint32_t instance,
                                                   BACNET_PROPERTY_ID property, uint32_t index)
{
    uint8_t apdu[MAX_APDU], *reply;
    BACNET_READ_PROPERTY_DATA request = {.object_type=type, .object_instance=instance,
        .object_property=property, .array_index=index};
    int length = rp_encode_apdu(apdu, invoke++, &request);
    unsigned before = frames;
    inject(apdu, (unsigned)length, false);
    assert(frames == before + 1);
    length = last_apdu(&reply);
    assert(reply[0] == PDU_TYPE_COMPLEX_ACK && reply[2] == SERVICE_CONFIRMED_READ_PROPERTY);
    BACNET_READ_PROPERTY_DATA raw = {0};
    assert(rp_ack_decode_service_request(reply + 3, length - 3, &raw) > 0);
    BACNET_APPLICATION_DATA_VALUE value = {0};
    assert(bacapp_decode_application_data(raw.application_data, raw.application_data_len, &value) > 0);
    return value;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t n) { p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n; }

static void *server(void *argument)
{
    int listener = *(int *)argument;
    const uint8_t functions[] = {3, 4, 1, 2, 3, 3};
    const uint16_t addresses[] = {10, 20, 30, 40, 50, 60};
    const uint16_t quantities[] = {1, 2, 1, 1, 1, 2};
    for (unsigned exchange = 0; exchange < 19; ++exchange) {
        int fd = accept(listener, NULL, NULL);
        assert(fd >= 0);
        struct timeval timeout = {.tv_sec=2, .tv_usec=0};
        assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
        uint8_t request[12]; size_t got = 0;
        while (got < sizeof(request)) {
            ssize_t n = recv(fd, request + got, sizeof(request) - got, 0);
            assert(n > 0); got += (size_t)n;
        }
        unsigned point = exchange % 6;
        assert(be16(request + 2) == 0 && be16(request + 4) == 6 && request[6] == 17);
        assert(request[7] == functions[point] && be16(request + 8) == addresses[point]);
        assert(be16(request + 10) == quantities[point]);
        uint8_t response[13] = {0};
        memcpy(response, request, 4); response[6] = 17; response[7] = request[7];
        uint16_t words[2] = {0,0};
        switch (point) {
        case 0: words[0] = (uint16_t)(4900 + exchange / 6); break;
        case 1: words[0] = 0x43c0; break; /* 384.0 IEEE32 */
        case 2: words[0] = 1; break;
        case 3: words[0] = 0; break;
        case 4: words[0] = 1; break;
        case 5: words[0] = 0x534e; words[1] = 0x3031; break;
        }
        response[8] = request[7] <= 2 ? 1 : (uint8_t)(2 * quantities[point]);
        put16(response + 4, (uint16_t)(response[8] + 3));
        if (request[7] <= 2) response[9] = (uint8_t)words[0];
        else for (unsigned i = 0; i < quantities[point]; ++i) put16(response + 9 + 2*i, words[i]);
        if (exchange == 12) response[1] ^= 0x80; /* Real wrong-transaction failure. */
        size_t total = 9U + response[8], sent = 0;
        while (sent < total) {
            ssize_t n = send(fd, response + sent, total - sent, 0);
            assert(n > 0); sent += (size_t)n;
        }
        close(fd);
    }
    return NULL;
}

static void verify_values(float voltage)
{
    BACNET_APPLICATION_DATA_VALUE value = read_property(OBJECT_ANALOG_INPUT, 101, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    assert(value.tag == BACNET_APPLICATION_TAG_REAL && value.type.Real == voltage);
    value = read_property(OBJECT_ANALOG_INPUT, 102, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    assert(value.tag == BACNET_APPLICATION_TAG_REAL && value.type.Real == 384);
    value = read_property(OBJECT_BINARY_INPUT, 103, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    assert(value.tag == BACNET_APPLICATION_TAG_ENUMERATED && value.type.Enumerated == 1);
    value = read_property(OBJECT_BINARY_INPUT, 104, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    assert(value.tag == BACNET_APPLICATION_TAG_ENUMERATED && value.type.Enumerated == 0);
    value = read_property(OBJECT_MULTI_STATE_INPUT, 105, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    assert(value.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT && value.type.Unsigned_Int == 2);
    value = read_property(OBJECT_CHARACTERSTRING_VALUE, 106, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    BACNET_CHARACTER_STRING expected;
    characterstring_init_ansi(&expected, "SN01");
    assert(value.tag == BACNET_APPLICATION_TAG_CHARACTER_STRING && characterstring_same(&value.type.Character_String, &expected));
    value = read_property(OBJECT_MULTI_STATE_INPUT, 105, PROP_STATE_TEXT, 2);
    characterstring_init_ansi(&expected, "Automatic");
    assert(characterstring_same(&value.type.Character_String, &expected));
}

int main(void)
{
    const char *csv = CUSTOM_CSV_HEADER "\n"
        "101,Generator-Voltage,AI,3,10,u16,AB,0.1,0,62,,,,1000,Fixture voltage\n"
        "102,Generator-Power,AI,4,20,f32,ABCD,1,0,48,,,,1000,Fixture power\n"
        "103,Generator-Running,BI,1,30,bool,,1,0,95,,,,1000,Fixture coil\n"
        "104,Generator-Alarm,BI,2,40,bool,,1,0,95,,,,1000,Fixture input\n"
        "105,Generator-Mode,MSI,3,50,u16,AB,1,1,95,,Off|Automatic,,1000,Fixture mode\n"
        "106,Generator-Serial,CSV,3,60,ascii,AB,1,0,95,,,4,1000,Fixture label\n";
    char error[200];
    assert(custom_map_parse(&map, csv, strlen(csv), error, sizeof(error)));
    custom_poll_init(&state, &map);
    bip_port_set_send_hook(send_frame, NULL);
    gateway_bacnet_config_t config = {.device_instance=75002, .device_name="Imported-Map-Test",
        .firmware_version="test", .vendor_id=999, .local_ip=inet_addr("127.0.0.1"),
        .netmask=inet_addr("255.0.0.0"), .udp_port=47808, .points=map.defs, .point_count=map.count};
    assert(gateway_bacnet_init(&config, 1000));
    uint8_t whois[64], *reply;
    int length = whois_encode_apdu(whois, -1, -1);
    unsigned before = frames;
    inject(whois, (unsigned)length, true);
    assert(frames == before + 1);
    (void)last_apdu(&reply);
    assert(reply[1] == SERVICE_UNCONFIRMED_I_AM);
    BACNET_APPLICATION_DATA_VALUE value = read_property(OBJECT_DEVICE, 75002, PROP_OBJECT_LIST, 0);
    assert(value.type.Unsigned_Int == 14); /* Six custom + six diagnostic + Device + Network Port. */
    bool found[6] = {0};
    for (unsigned i = 1; i <= 14; ++i) {
        value = read_property(OBJECT_DEVICE, 75002, PROP_OBJECT_LIST, i);
        for (size_t j = 0; j < map.count; ++j)
            if (value.type.Object_Id.type == map.defs[j].object_type && value.type.Object_Id.instance == map.defs[j].instance) found[j] = true;
    }
    for (unsigned i = 0; i < 6; ++i) assert(found[i]);
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_port=0};
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(!bind(listener, (const struct sockaddr *)&address, sizeof(address)));
    socklen_t address_length = sizeof(address);
    assert(!getsockname(listener, (struct sockaddr *)&address, &address_length));
    assert(!listen(listener, 1));
    struct timeval timeout = {.tv_sec=3, .tv_usec=0};
    assert(!setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, server, &listener));
    uint64_t now = 1000;
    for (unsigned exchange = 0; exchange < 19; ++exchange) {
        if (now < state.next_request_ms) now = state.next_request_ms;
        assert(custom_poll_step(&state, &map, "127.0.0.1", ntohs(address.sin_port), 17, now));
        custom_poll_snapshot(&state, &map, now + state.last_result.elapsed_ms, samples);
        gateway_bacnet_update(samples);
        if (exchange == 5) verify_values(490.0f);
        if (exchange == 6) verify_values(490.1f);
        if (exchange == 12) {
            assert(state.last_result.error == MB_ERR_TRANSACTION);
            value = read_property(OBJECT_ANALOG_INPUT, 101, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
            assert(value.type.Real == 490.1f);
            for (size_t i = 0; i < map.count; ++i) {
                value = read_property(map.defs[i].object_type, map.defs[i].instance, PROP_RELIABILITY, BACNET_ARRAY_ALL);
                assert(value.type.Enumerated == RELIABILITY_COMMUNICATION_FAILURE);
            }
        }
        now += 1000;
    }
    assert(!pthread_join(thread, NULL));
    close(listener);
    verify_values(490.3f);
    for (size_t i = 0; i < map.count; ++i) {
        value = read_property(map.defs[i].object_type, map.defs[i].instance, PROP_RELIABILITY, BACNET_ARRAY_ALL);
        assert(value.type.Enumerated == RELIABILITY_NO_FAULT_DETECTED);
    }
    gateway_bacnet_stats_t status;
    gateway_bacnet_stats(&status);
    assert(status.good_points == 6 && status.fault_points == 0);
    assert(state.requests == 19 && state.successes == 18 && state.failures == 1);
    gateway_bacnet_shutdown();
    puts("Custom integration: CSV -> 19 real loopback FC1/2/3/4 reads -> BACnet discovery/values, changing telemetry, fault retention/recovery passed");
    return 0;
}
