/* Actual production BACnet services with an in-memory datalink: no sockets. */
#include "gateway_bacnet.h"
#include "bip_port.h"
#include <arpa/inet.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bacnet/bacapp.h"
#include "bacnet/bacdcode.h"
#include "bacnet/cov.h"
#include "bacnet/npdu.h"
#include "bacnet/rp.h"
#include "bacnet/rpm.h"
#include "bacnet/wp.h"
#include "bacnet/wpm.h"
#include "bacnet/whois.h"
#include "bacnet/whohas.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/ms-input.h"
#include "bacnet/basic/service/h_cov.h"

#define FRAMES 8192u
typedef struct { BACNET_IP_ADDRESS destination; uint16_t length; uint8_t bytes[MAX_PDU + 32]; } frame_t;
static frame_t frames[FRAMES];
static unsigned frame_count;
static uint64_t clock_ms = 1000;
static uint8_t invoke = 1;
static uint32_t client_ip;
static ats_value_t values[ATS_POINT_COUNT];

static int send_frame(const BACNET_IP_ADDRESS *destination, const uint8_t *bytes, uint16_t length, void *context)
{
    (void)context;
    assert(frame_count < FRAMES && length <= sizeof(frames[0].bytes));
    frames[frame_count].destination = *destination;
    frames[frame_count].length = length;
    memcpy(frames[frame_count].bytes, bytes, length);
    frame_count++;
    return length;
}

static int apdu_of(unsigned index, uint8_t **apdu)
{
    assert(index < frame_count);
    BACNET_ADDRESS destination = {0}, source = {0};
    BACNET_NPDU_DATA npdu = {0};
    int offset = bacnet_npdu_decode(frames[index].bytes + 4, frames[index].length - 4,
                                    &destination, &source, &npdu);
    assert(offset >= 2 && !npdu.network_layer_message);
    *apdu = frames[index].bytes + 4 + offset;
    return frames[index].length - 4 - offset;
}

static void inject(const uint8_t *apdu, unsigned length, bool broadcast)
{
    uint8_t frame[MAX_PDU + 32] = {0x81, broadcast ? 0x0b : 0x0a};
    encode_unsigned16(frame + 2, (uint16_t)(length + 6));
    frame[4] = 1; frame[5] = 0;
    assert(length + 6 <= sizeof(frame));
    memcpy(frame + 6, apdu, length);
    assert(gateway_bacnet_process_datagram(frame, length + 6, client_ip, 47809));
}

static BACNET_READ_PROPERTY_DATA read_raw(BACNET_OBJECT_TYPE type, uint32_t instance,
                                         BACNET_PROPERTY_ID property, uint32_t index)
{
    uint8_t apdu[MAX_APDU];
    BACNET_READ_PROPERTY_DATA request = {.object_type=type, .object_instance=instance,
        .object_property=property, .array_index=index};
    int length = rp_encode_apdu(apdu, invoke++, &request);
    unsigned before = frame_count;
    inject(apdu, (unsigned)length, false);
    assert(frame_count == before + 1);
    uint8_t *reply;
    int reply_length = apdu_of(before, &reply);
    if (reply[0] != PDU_TYPE_COMPLEX_ACK) {
        fprintf(stderr, "Read failed: type=%u instance=%u property=%u index=%u response=%02x\n",
                type, instance, property, index, reply[0]);
    }
    assert(reply[0] == PDU_TYPE_COMPLEX_ACK && reply[2] == SERVICE_CONFIRMED_READ_PROPERTY);
    BACNET_READ_PROPERTY_DATA result = {0};
    assert(rp_ack_decode_service_request(reply + 3, reply_length - 3, &result) > 0);
    return result;
}

static BACNET_APPLICATION_DATA_VALUE read_value(BACNET_OBJECT_TYPE type, uint32_t instance,
                                                BACNET_PROPERTY_ID property, uint32_t index)
{
    BACNET_READ_PROPERTY_DATA raw = read_raw(type, instance, property, index);
    BACNET_APPLICATION_DATA_VALUE value = {0};
    assert(bacapp_decode_application_data(raw.application_data, raw.application_data_len, &value) > 0);
    return value;
}

static unsigned first_type(BACNET_OBJECT_TYPE type)
{
    for (unsigned i = 0; i < ATS_POINT_COUNT; ++i) { if (ats_points[i].object_type == type) { return i; } }
    assert(false); return 0;
}

static void tick(void) { gateway_bacnet_tick(++clock_ms); }

static void subscribe(unsigned catalog, uint32_t process, bool confirmed, uint32_t lifetime, bool cancel)
{
    BACNET_SUBSCRIBE_COV_DATA data = {.subscriberProcessIdentifier=process,
        .monitoredObjectIdentifier={ats_points[catalog].object_type, ats_points[catalog].instance},
        .issueConfirmedNotifications=confirmed, .lifetime=lifetime, .cancellationRequest=cancel};
    uint8_t apdu[MAX_APDU];
    int length = cov_subscribe_encode_apdu(apdu, sizeof(apdu), invoke++, &data);
    inject(apdu, (unsigned)length, false);
}

static bool is_cov(unsigned frame, bool confirmed)
{
    uint8_t *apdu;
    int length = apdu_of(frame, &apdu);
    return confirmed ? length >= 4 && apdu[0] == PDU_TYPE_CONFIRMED_SERVICE_REQUEST && apdu[3] == SERVICE_CONFIRMED_COV_NOTIFICATION :
        length >= 2 && apdu[0] == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST && apdu[1] == SERVICE_UNCONFIRMED_COV_NOTIFICATION;
}

static BACNET_COV_DATA decode_cov(unsigned frame, bool confirmed, BACNET_PROPERTY_VALUE properties[2])
{
    uint8_t *apdu;
    int length = apdu_of(frame, &apdu);
    unsigned header = confirmed ? 4 : 2;
    BACNET_COV_DATA data = {0};
    bacapp_property_value_list_init(properties, 2); data.listOfValues = properties;
    assert(cov_notify_decode_service_request(apdu + header, length - header, &data) > 0);
    return data;
}

static void ack(unsigned frame)
{
    uint8_t *apdu;
    (void)apdu_of(frame, &apdu);
    uint8_t response[] = {PDU_TYPE_SIMPLE_ACK, apdu[2], SERVICE_CONFIRMED_COV_NOTIFICATION};
    inject(response, sizeof(response), false);
}

static void test_discovery(void)
{
    uint8_t apdu[64], *reply;
    int length = whois_encode_apdu(apdu, -1, -1);
    frame_count = 0;
    inject(apdu, length, false);
    assert(frame_count == 1 && frames[0].destination.port == 47809);
    assert(memcmp(frames[0].destination.address, &client_ip, 4) == 0);
    assert(frames[0].bytes[1] == 0x0a);
    (void)apdu_of(0, &reply); assert(reply[1] == SERVICE_UNCONFIRMED_I_AM);
    inject(apdu, length, true);
    assert(frame_count == 2 && frames[1].bytes[1] == 0x0b && frames[1].destination.port == 47808);
    length = whois_encode_apdu(apdu, 700000, 700001);
    inject(apdu, length, false); assert(frame_count == 2);
    length = whois_encode_apdu(apdu, 75001, 75001);
    inject(apdu, length, false); assert(frame_count == 3);
    uint8_t global[] = {0x81, 0x0a, 0, 12, 1, 0x20, 0xff, 0xff, 0, 255, 0x10, 8};
    assert(gateway_bacnet_process_datagram(global, sizeof(global), client_ip, 47809));
    assert(frame_count == 4 && frames[3].bytes[1] == 0x0b);
    BACNET_WHO_HAS_DATA has = {.low_limit=-1, .high_limit=-1, .is_object_name=false,
        .object.identifier={ats_points[0].object_type, ats_points[0].instance}};
    length = whohas_encode_apdu(apdu, &has);
    inject(apdu, length, false);
    assert(frame_count == 5 && frames[4].bytes[1] == 0x0a && frames[4].destination.port == 47809);
    inject(apdu, length, true); assert(frame_count == 6 && frames[5].bytes[1] == 0x0b);
    uint8_t malformed[] = {0x81, 0x0a, 0, 9, 1, 0, 0x10, 8};
    assert(!gateway_bacnet_process_datagram(malformed, sizeof(malformed), client_ip, 47809));
    puts("discovery: directed/broadcast/global NPDU/range/source-port/WhoHas/malformed passed");
}

static void test_unqualified_information(void)
{
    unsigned outage = 1132 - 1001, csv = first_type(OBJECT_CHARACTERSTRING_VALUE);
    values[outage].numeric = 69; values[outage].quality = ATS_QUALITY_UNVERIFIED;
    values[outage].raw_count = 2;
    snprintf(values[csv].text, sizeof(values[csv].text), "2001-01-01; unverified calendar");
    values[csv].quality = ATS_QUALITY_UNVERIFIED; values[csv].raw_count = 2;
    gateway_bacnet_update(values);
    BACNET_APPLICATION_DATA_VALUE raw = read_value(OBJECT_ANALOG_INPUT, 1132, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    assert(raw.type.Real == 69);
    raw = read_value(OBJECT_ANALOG_INPUT, 1132, PROP_RELIABILITY, BACNET_ARRAY_ALL);
    assert(raw.type.Enumerated == RELIABILITY_UNRELIABLE_OTHER);
    raw = read_value(OBJECT_CHARACTERSTRING_VALUE, ats_points[csv].instance, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    BACNET_CHARACTER_STRING expected;
    characterstring_init_ansi(&expected, "2001-01-01; unverified calendar");
    assert(characterstring_same(&raw.type.Character_String, &expected));
    raw = read_value(OBJECT_CHARACTERSTRING_VALUE, ats_points[csv].instance, PROP_STATUS_FLAGS, BACNET_ARRAY_ALL);
    assert(bitstring_bit(&raw.type.Bit_String, STATUS_FLAG_FAULT));
    values[outage].numeric = 999; values[outage].quality = ATS_QUALITY_COMM;
    gateway_bacnet_update(values);
    raw = read_value(OBJECT_ANALOG_INPUT, 1132, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL); assert(raw.type.Real == 69);
    puts("unqualified: acquired raw AI/CSV visible with fault; COMM failure retains previous raw value passed");
}

static void test_object_inventory(void)
{
    frame_count = 0;
    BACNET_APPLICATION_DATA_VALUE count = read_value(OBJECT_DEVICE, 75001, PROP_OBJECT_LIST, 0);
    assert(count.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT && count.type.Unsigned_Int == 172);
    for (unsigned n = 1; n <= count.type.Unsigned_Int; ++n) {
        BACNET_APPLICATION_DATA_VALUE id = read_value(OBJECT_DEVICE, 75001, PROP_OBJECT_LIST, n);
        assert(id.tag == BACNET_APPLICATION_TAG_OBJECT_ID);
        (void)read_value(id.type.Object_Id.type, id.type.Object_Id.instance, PROP_OBJECT_NAME, BACNET_ARRAY_ALL);
        BACNET_READ_PROPERTY_DATA list = read_raw(id.type.Object_Id.type, id.type.Object_Id.instance, PROP_PROPERTY_LIST, BACNET_ARRAY_ALL);
        assert(list.application_data_len > 0);
    }
    BACNET_READ_PROPERTY_DATA all = read_raw(OBJECT_DEVICE, 75001, PROP_OBJECT_LIST, BACNET_ARRAY_ALL);
    unsigned decoded_count = 0;
    for (int offset = 0; offset < all.application_data_len; ++decoded_count) {
        BACNET_APPLICATION_DATA_VALUE id = {0};
        int length = bacapp_decode_application_data(all.application_data + offset, all.application_data_len - offset, &id);
        assert(length > 0 && id.tag == BACNET_APPLICATION_TAG_OBJECT_ID); offset += length;
    }
    assert(decoded_count == 172);
    for (unsigned n = 0; n < ATS_POINT_COUNT; ++n) {
        const ats_point_def_t *def = &ats_points[n];
        BACNET_APPLICATION_DATA_VALUE reliability = read_value(def->object_type, def->instance, PROP_RELIABILITY, BACNET_ARRAY_ALL);
        assert(reliability.type.Enumerated == RELIABILITY_COMMUNICATION_FAILURE);
        BACNET_APPLICATION_DATA_VALUE flags = read_value(def->object_type, def->instance, PROP_STATUS_FLAGS, BACNET_ARRAY_ALL);
        assert(bitstring_bit(&flags.type.Bit_String, STATUS_FLAG_FAULT));
        if (def->object_type == OBJECT_MULTI_STATE_INPUT) {
            BACNET_APPLICATION_DATA_VALUE states = read_value(def->object_type, def->instance, PROP_NUMBER_OF_STATES, BACNET_ARRAY_ALL);
            assert(states.type.Unsigned_Int == def->state_count);
            (void)read_value(def->object_type, def->instance, PROP_STATE_TEXT, def->state_count);
        }
    }
    puts("objects: 172 indexed/all object list, names/property lists,164 initial fault flags/reliabilities, MSI state texts passed");
}

static void test_values_and_quality(void)
{
    frame_count = 0;
    for (unsigned i = 0; i < ATS_POINT_COUNT; ++i) {
        values[i].numeric = 1; values[i].quality = ATS_QUALITY_GOOD;
        snprintf(values[i].text, sizeof(values[i].text), "Acquired-%u", i);
    }
    unsigned ai = first_type(OBJECT_ANALOG_INPUT), bi = first_type(OBJECT_BINARY_INPUT),
             msi = first_type(OBJECT_MULTI_STATE_INPUT), csv = first_type(OBJECT_CHARACTERSTRING_VALUE);
    values[ai].numeric = 123.5; gateway_bacnet_update(values);
    const unsigned tests[] = {ai, bi, msi, csv};
    for (unsigned n = 0; n < 4; ++n) {
        unsigned i = tests[n];
        BACNET_APPLICATION_DATA_VALUE reliability = read_value(ats_points[i].object_type, ats_points[i].instance, PROP_RELIABILITY, BACNET_ARRAY_ALL);
        assert(reliability.type.Enumerated == RELIABILITY_NO_FAULT_DETECTED);
        subscribe(i, 10 + n, n != 3, 60, false);
    }
    unsigned before = frame_count; tick();
    unsigned notifications = 0;
    for (unsigned n = before; n < frame_count; ++n) {
        bool confirmed = is_cov(n, true);
        if (!confirmed && !is_cov(n, false)) { continue; }
        BACNET_PROPERTY_VALUE props[2]; (void)decode_cov(n, confirmed, props);
        assert(!bitstring_bit(&props[1].value.type.Bit_String, STATUS_FLAG_FAULT));
        notifications++; if (confirmed) { ack(n); }
    }
    assert(notifications == 4);
    for (unsigned n = 0; n < 4; ++n) { values[tests[n]].quality = ATS_QUALITY_COMM; }
    values[ai].numeric = NAN; snprintf(values[csv].text, sizeof(values[csv].text), "Do not publish invalid");
    gateway_bacnet_update(values); before = frame_count; tick();
    notifications = 0;
    for (unsigned n = before; n < frame_count; ++n) {
        bool confirmed = is_cov(n, true);
        if (!confirmed && !is_cov(n, false)) { continue; }
        BACNET_PROPERTY_VALUE props[2]; BACNET_COV_DATA cov = decode_cov(n, confirmed, props);
        assert(bitstring_bit(&props[1].value.type.Bit_String, STATUS_FLAG_FAULT));
        if (cov.monitoredObjectIdentifier.type == OBJECT_ANALOG_INPUT) { assert(props[0].value.type.Real == 123.5f); }
        notifications++; if (confirmed) { ack(n); }
    }
    assert(notifications == 4);
    for (unsigned n = 0; n < 4; ++n) { values[tests[n]].quality = ATS_QUALITY_GOOD; }
    values[ai].numeric = 125; gateway_bacnet_update(values); before = frame_count; tick();
    notifications = 0;
    for (unsigned n = before; n < frame_count; ++n) {
        bool confirmed = is_cov(n, true);
        if (!confirmed && !is_cov(n, false)) { continue; }
        BACNET_PROPERTY_VALUE props[2]; (void)decode_cov(n, confirmed, props);
        assert(!bitstring_bit(&props[1].value.type.Bit_String, STATUS_FLAG_FAULT));
        notifications++; if (confirmed) { ack(n); }
    }
    assert(notifications == 4);
    for (unsigned n = 0; n < 4; ++n) { subscribe(tests[n], 10 + n, n != 3, 0, true); }
    tick();
    puts("quality: AI/BI/MSI/CSV actual COV initial/fault/recovery, invalid last-value retention and unconfirmed CSV passed");
}

static void test_writes_rejected_and_rpm(void)
{
    frame_count = 0;
    unsigned ai = first_type(OBJECT_ANALOG_INPUT);
    BACNET_WRITE_PROPERTY_DATA data = {.object_type=OBJECT_ANALOG_INPUT,
        .object_instance=ats_points[ai].instance, .object_property=PROP_PRESENT_VALUE,
        .array_index=BACNET_ARRAY_ALL, .priority=BACNET_NO_PRIORITY};
    data.application_data_len = encode_application_real(data.application_data, 777);
    uint8_t apdu[MAX_APDU], *response;
    int length = wp_encode_apdu(apdu, invoke++, &data);
    inject(apdu, length, false);
    (void)apdu_of(frame_count - 1, &response); assert(response[0] == PDU_TYPE_ERROR);
    length = wpm_encode_apdu_init(apdu, invoke++);
    length += wpm_encode_apdu_object_begin(apdu + length, data.object_type, data.object_instance);
    length += wpm_encode_apdu_object_property(apdu + length, &data);
    length += wpm_encode_apdu_object_end(apdu + length);
    inject(apdu, length, false);
    (void)apdu_of(frame_count - 1, &response); assert(response[0] == PDU_TYPE_ERROR);
    assert(Analog_Input_Present_Value(data.object_instance) == 125);
    length = rpm_encode_apdu_init(apdu, invoke++);
    length += rpm_encode_apdu_object_begin(apdu + length, data.object_type, data.object_instance);
    length += rpm_encode_apdu_object_property(apdu + length, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL);
    length += rpm_encode_apdu_object_property(apdu + length, PROP_STATUS_FLAGS, BACNET_ARRAY_ALL);
    length += rpm_encode_apdu_object_end(apdu + length);
    inject(apdu, length, false);
    (void)apdu_of(frame_count - 1, &response); assert(response[0] == PDU_TYPE_COMPLEX_ACK && response[2] == SERVICE_CONFIRMED_READ_PROP_MULTIPLE);
    puts("access: WP/WPM rejected without mutation; actual RPM returns complex ACK passed");
}

static void test_cov_capacity_queue_expiration(void)
{
    frame_count = 0; handler_cov_init(); tick();
    unsigned ai = first_type(OBJECT_ANALOG_INPUT);
    for (unsigned n = 0; n < 256; ++n) {
        subscribe(ai, 1000 + n, true, 30, false);
        uint8_t *reply; (void)apdu_of(frame_count - 1, &reply); assert(reply[0] == PDU_TYPE_SIMPLE_ACK);
    }
    subscribe(ai, 9999, true, 30, false);
    uint8_t *reply; (void)apdu_of(frame_count - 1, &reply); assert(reply[0] == PDU_TYPE_ERROR);
    bool seen[256] = {false}; unsigned total = 0;
    for (unsigned pass = 0; pass < 12 && total < 256; ++pass) {
        unsigned before = frame_count; tick(); unsigned batch = 0;
        for (unsigned n = before; n < frame_count; ++n) {
            if (!is_cov(n, true)) { continue; }
            BACNET_PROPERTY_VALUE props[2]; BACNET_COV_DATA data = decode_cov(n, true, props);
            assert(data.subscriberProcessIdentifier >= 1000 && data.subscriberProcessIdentifier < 1256);
            unsigned i = data.subscriberProcessIdentifier - 1000;
            assert(!seen[i]); seen[i] = true; total++; batch++; ack(n);
        }
        assert(batch <= 32);
    }
    assert(total == 256);
    clock_ms += 31000; gateway_bacnet_tick(clock_ms);
    unsigned before = frame_count; values[ai].numeric = 150; gateway_bacnet_update(values); tick();
    for (unsigned n = before; n < frame_count; ++n) { assert(!is_cov(n, true)); }
    puts("capacity: 256 subscriptions,257th explicit denial,32-TSM bounded queue delivers all,expiration passed");
}

static void test_timeout_recovery_and_link(void)
{
    frame_count = 0; handler_cov_init(); tick();
    unsigned ai = first_type(OBJECT_ANALOG_INPUT);
    subscribe(ai, 900, true, 60, false); tick();
    for (unsigned i = 0; i < 4; ++i) { clock_ms += 3001; gateway_bacnet_tick(clock_ms); }
    gateway_bacnet_stats_t state; gateway_bacnet_stats(&state); assert(state.cov_timeouts >= 1);
    values[ai].numeric = 200; gateway_bacnet_update(values);
    clock_ms += 1001; unsigned before = frame_count; gateway_bacnet_tick(clock_ms);
    bool delivered = false;
    for (unsigned n = before; n < frame_count; ++n) {
        if (is_cov(n, true)) { BACNET_PROPERTY_VALUE props[2]; (void)decode_cov(n, true, props);
            assert(props[0].value.type.Real == 200); delivered = true; ack(n); }
    }
    assert(delivered);
    assert(gateway_bacnet_network_update(0, 0, 0, false, ++clock_ms));
    before = frame_count;
    assert(gateway_bacnet_network_update(inet_addr("127.0.0.1"), inet_addr("255.0.0.0"), 0, true, ++clock_ms));
    assert(frame_count > before); /* fresh I-Am after link/IP recovery */
    puts("recovery: confirmed timeout schedules fresh current value; link recovery announces passed");
}

int main(void)
{
    client_ip = inet_addr("127.0.0.2");
    bip_port_set_send_hook(send_frame, NULL);
    gateway_bacnet_config_t settings = {.device_instance=75001, .device_name="Native-ATS-Test",
        .firmware_version="test", .vendor_id=999, .local_ip=inet_addr("127.0.0.1"),
        .netmask=inet_addr("255.0.0.0"), .udp_port=47808};
    assert(gateway_bacnet_init(&settings, clock_ms));
    test_discovery(); test_object_inventory(); test_values_and_quality();
    test_writes_rejected_and_rpm(); test_unqualified_information(); test_cov_capacity_queue_expiration();
    test_timeout_recovery_and_link();
    gateway_bacnet_shutdown();
    puts("BACnet native production-service tests passed; no network IO");
    return 0;
}
