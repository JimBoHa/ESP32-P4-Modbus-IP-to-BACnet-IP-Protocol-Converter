/* SPDX-License-Identifier: 0BSD */
#include "gateway_bacnet.h"
#include "bip_port.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bacnet/bacapp.h"
#include "bacnet/bacdcode.h"
#include "bacnet/bacstr.h"
#include "bacnet/cov.h"
#include "bacnet/ihave.h"
#include "bacnet/npdu.h"
#include "bacnet/whohas.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/csv.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/ms-input.h"
#include "bacnet/basic/object/netport.h"
#include "bacnet/basic/npdu/h_npdu.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/service/h_wpm.h"
#include "bacnet/basic/tsm/tsm.h"

#define NETWORK_INSTANCE 1u
#define DIAGNOSTIC_BASE 9001u
#define RECOVERY_CAPACITY (GATEWAY_BACNET_MAX_POINTS + 8u)
#define STATE_TEXT_BYTES 1024u

static gateway_bacnet_config_t config;
static gateway_bacnet_stats_t stats;
static char device_name[96], firmware_version[MAX_DEV_VER_LEN + 1], location[MAX_DEV_LOC_LEN + 1];
static bool legacy_catalog;
static uint64_t now_ms, started_ms, last_timer_ms, last_second_ms, next_announce_ms;
static uint8_t receive_buffer[MAX_PDU], transmit_buffer[MAX_PDU];
static BACNET_RELIABILITY csv_reliability[GATEWAY_BACNET_MAX_POINTS];
static bool csv_quality_changed[GATEWAY_BACNET_MAX_POINTS];
typedef struct { bool used; BACNET_OBJECT_TYPE type; uint32_t instance; uint64_t due; } recovery_t;
static recovery_t recovery[RECOVERY_CAPACITY];
static uint8_t failed_pdu[MAX_PDU];

unsigned long mstimer_now(void) { return (unsigned long)now_ms; }

static int catalog_index(uint32_t instance)
{
    for (size_t i = 0; i < config.point_count; ++i) {
        if (config.points[i].object_type == OBJECT_CHARACTERSTRING_VALUE &&
            config.points[i].instance == instance) { return (int)i; }
    }
    return -1;
}

static bool read_only(BACNET_WRITE_PROPERTY_DATA *data)
{
    data->error_class = ERROR_CLASS_PROPERTY;
    data->error_code = ERROR_CODE_WRITE_ACCESS_DENIED;
    return false;
}

static void no_writable_properties(uint32_t instance, const int32_t **properties)
{
    (void)instance;
    static const int32_t empty[] = {-1};
    if (properties) { *properties = empty; }
}

static void csv_property_lists(const int32_t **required, const int32_t **optional,
                               const int32_t **proprietary)
{
    static const int32_t extra[] = {PROP_EVENT_STATE, PROP_OUT_OF_SERVICE,
                                    PROP_DESCRIPTION, PROP_RELIABILITY, -1};
    CharacterString_Value_Property_Lists(required, NULL, proprietary);
    if (optional) { *optional = extra; }
}

static int csv_read_property(BACNET_READ_PROPERTY_DATA *data)
{
    int index = catalog_index(data->object_instance);
    if (index < 0 || (data->object_property != PROP_RELIABILITY &&
                      data->object_property != PROP_STATUS_FLAGS)) {
        return CharacterString_Value_Read_Property(data);
    }
    if (data->array_index != BACNET_ARRAY_ALL) {
        data->error_class = ERROR_CLASS_PROPERTY;
        data->error_code = ERROR_CODE_PROPERTY_IS_NOT_AN_ARRAY;
        return BACNET_STATUS_ERROR;
    }
    uint8_t encoded[16];
    int length;
    if (data->object_property == PROP_RELIABILITY) {
        length = encode_application_enumerated(encoded, csv_reliability[index]);
    } else {
        BACNET_BIT_STRING flags;
        bitstring_init(&flags);
        bitstring_set_bit(&flags, STATUS_FLAG_IN_ALARM, false);
        bitstring_set_bit(&flags, STATUS_FLAG_FAULT, csv_reliability[index] != RELIABILITY_NO_FAULT_DETECTED);
        bitstring_set_bit(&flags, STATUS_FLAG_OVERRIDDEN, false);
        bitstring_set_bit(&flags, STATUS_FLAG_OUT_OF_SERVICE, false);
        length = encode_application_bitstring(encoded, &flags);
    }
    if (length > data->application_data_len) {
        data->error_code = ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
        return BACNET_STATUS_ABORT;
    }
    memcpy(data->application_data, encoded, (size_t)length);
    return length;
}

static bool csv_value_list(uint32_t instance, BACNET_PROPERTY_VALUE *list)
{
    int index = catalog_index(instance);
    BACNET_CHARACTER_STRING value;
    if (index < 0 || !CharacterString_Value_Present_Value(instance, &value)) { return false; }
    return cov_value_list_encode_character_string(list, &value, false,
        csv_reliability[index] != RELIABILITY_NO_FAULT_DETECTED, false, false);
}

static bool retry_pending(BACNET_OBJECT_TYPE type, uint32_t instance)
{
    for (unsigned i = 0; i < RECOVERY_CAPACITY; ++i) {
        if (recovery[i].used && recovery[i].type == type && recovery[i].instance == instance &&
            now_ms >= recovery[i].due) { return true; }
    }
    return false;
}

static void retry_clear(BACNET_OBJECT_TYPE type, uint32_t instance)
{
    for (unsigned i = 0; i < RECOVERY_CAPACITY; ++i) {
        if (recovery[i].used && recovery[i].type == type && recovery[i].instance == instance) {
            if (now_ms >= recovery[i].due) { stats.cov_refreshes++; }
            recovery[i].used = false;
            stats.cov_pending--;
        }
    }
}

#define COV_WRAPPERS(name, type, prefix) \
    static bool name##_changed(uint32_t instance) { \
        return prefix##_Change_Of_Value(instance) || retry_pending(type, instance); } \
    static void name##_clear(uint32_t instance) { \
        prefix##_Change_Of_Value_Clear(instance); retry_clear(type, instance); }
COV_WRAPPERS(ai, OBJECT_ANALOG_INPUT, Analog_Input)
COV_WRAPPERS(bi, OBJECT_BINARY_INPUT, Binary_Input)
COV_WRAPPERS(msi, OBJECT_MULTI_STATE_INPUT, Multistate_Input)

static bool csv_changed(uint32_t instance)
{
    int index = catalog_index(instance);
    return CharacterString_Value_Change_Of_Value(instance) ||
        (index >= 0 && csv_quality_changed[index]) || retry_pending(OBJECT_CHARACTERSTRING_VALUE, instance);
}
static void csv_clear(uint32_t instance)
{
    int index = catalog_index(instance);
    CharacterString_Value_Change_Of_Value_Clear(instance);
    if (index >= 0) { csv_quality_changed[index] = false; }
    retry_clear(OBJECT_CHARACTERSTRING_VALUE, instance);
}

static object_functions_t object_table[] = {
    {.Object_Type=OBJECT_DEVICE, .Object_Count=Device_Count,
     .Object_Index_To_Instance=Device_Index_To_Instance, .Object_Valid_Instance=Device_Valid_Object_Instance_Number,
     .Object_Name=Device_Object_Name, .Object_Read_Property=Device_Read_Property_Local,
     .Object_Write_Property=read_only, .Object_RPM_List=Device_Property_Lists,
     .Object_Writable_Property_List=no_writable_properties},
    {.Object_Type=OBJECT_ANALOG_INPUT, .Object_Init=Analog_Input_Init, .Object_Count=Analog_Input_Count,
     .Object_Index_To_Instance=Analog_Input_Index_To_Instance, .Object_Valid_Instance=Analog_Input_Valid_Instance,
     .Object_Name=Analog_Input_Object_Name, .Object_Read_Property=Analog_Input_Read_Property,
     .Object_Write_Property=read_only, .Object_RPM_List=Analog_Input_Property_Lists,
     .Object_Value_List=Analog_Input_Encode_Value_List, .Object_COV=ai_changed, .Object_COV_Clear=ai_clear,
     .Object_Writable_Property_List=no_writable_properties},
    {.Object_Type=OBJECT_BINARY_INPUT, .Object_Init=Binary_Input_Init, .Object_Count=Binary_Input_Count,
     .Object_Index_To_Instance=Binary_Input_Index_To_Instance, .Object_Valid_Instance=Binary_Input_Valid_Instance,
     .Object_Name=Binary_Input_Object_Name, .Object_Read_Property=Binary_Input_Read_Property,
     .Object_Write_Property=read_only, .Object_RPM_List=Binary_Input_Property_Lists,
     .Object_Value_List=Binary_Input_Encode_Value_List, .Object_COV=bi_changed, .Object_COV_Clear=bi_clear,
     .Object_Writable_Property_List=no_writable_properties},
    {.Object_Type=OBJECT_MULTI_STATE_INPUT, .Object_Init=Multistate_Input_Init, .Object_Count=Multistate_Input_Count,
     .Object_Index_To_Instance=Multistate_Input_Index_To_Instance, .Object_Valid_Instance=Multistate_Input_Valid_Instance,
     .Object_Name=Multistate_Input_Object_Name, .Object_Read_Property=Multistate_Input_Read_Property,
     .Object_Write_Property=read_only, .Object_RPM_List=Multistate_Input_Property_Lists,
     .Object_Value_List=Multistate_Input_Encode_Value_List, .Object_COV=msi_changed, .Object_COV_Clear=msi_clear,
     .Object_Writable_Property_List=no_writable_properties},
    {.Object_Type=OBJECT_CHARACTERSTRING_VALUE, .Object_Init=CharacterString_Value_Init,
     .Object_Count=CharacterString_Value_Count, .Object_Index_To_Instance=CharacterString_Value_Index_To_Instance,
     .Object_Valid_Instance=CharacterString_Value_Valid_Instance, .Object_Name=CharacterString_Value_Object_Name,
     .Object_Read_Property=csv_read_property, .Object_Write_Property=read_only, .Object_RPM_List=csv_property_lists,
     .Object_Value_List=csv_value_list, .Object_COV=csv_changed, .Object_COV_Clear=csv_clear,
     .Object_Writable_Property_List=no_writable_properties},
    {.Object_Type=OBJECT_NETWORK_PORT, .Object_Init=Network_Port_Init, .Object_Count=Network_Port_Count,
     .Object_Index_To_Instance=Network_Port_Index_To_Instance, .Object_Valid_Instance=Network_Port_Valid_Instance,
     .Object_Name=Network_Port_Object_Name, .Object_Read_Property=Network_Port_Read_Property,
     .Object_Write_Property=read_only, .Object_RPM_List=Network_Port_Property_Lists,
     .Object_Writable_Property_List=no_writable_properties},
    {.Object_Type=MAX_BACNET_OBJECT_TYPE}
};

static void who_is(uint8_t *data, uint16_t length, BACNET_ADDRESS *source)
{
    if (!stats.link_up) { return; }
    if (bip_port_last_receive_was_broadcast()) { handler_who_is(data, length, source); }
    else { handler_who_is_unicast(data, length, source); }
}

static void who_has(uint8_t *request, uint16_t length, BACNET_ADDRESS *source)
{
    BACNET_WHO_HAS_DATA query;
    if (!stats.link_up || whohas_decode_service_request(request, length, &query) != length) { return; }
    uint32_t device = Device_Object_Instance_Number();
    if (query.low_limit >= 0 && query.high_limit >= 0 &&
        (device < (uint32_t)query.low_limit || device > (uint32_t)query.high_limit)) { return; }
    BACNET_I_HAVE_DATA response = {.device_id={OBJECT_DEVICE, device}};
    BACNET_OBJECT_TYPE type;
    uint32_t instance;
    if (query.is_object_name) {
        if (!Device_Valid_Object_Name(&query.object.name, &type, &instance)) { return; }
        response.object_id.type = type; response.object_id.instance = instance;
        characterstring_copy(&response.object_name, &query.object.name);
    } else {
        response.object_id = query.object.identifier;
        if (!Device_Object_Name_Copy(response.object_id.type, response.object_id.instance, &response.object_name)) { return; }
    }
    BACNET_ADDRESS destination = *source, local;
    BACNET_NPDU_DATA npdu;
    if (bip_port_last_receive_was_broadcast()) { bip_get_broadcast_address(&destination); }
    bip_get_my_address(&local);
    npdu_encode_npdu_data(&npdu, false, MESSAGE_PRIORITY_NORMAL);
    int encoded = npdu_encode_pdu(transmit_buffer, &destination, &local, &npdu);
    encoded += ihave_encode_apdu(transmit_buffer + encoded, &response);
    (void)bip_send_pdu(&destination, &npdu, transmit_buffer, (unsigned)encoded);
}

static void cov_timeout(uint8_t invoke)
{
    BACNET_ADDRESS dest = {0}, npdu_dest = {0}, source = {0};
    BACNET_NPDU_DATA npdu = {0};
    BACNET_PROPERTY_VALUE values[2] = {0};
    BACNET_COV_DATA cov = {0};
    uint16_t length = 0;
    if (!tsm_get_transaction_pdu(invoke, &dest, &npdu, failed_pdu, &length)) { return; }
    int offset = bacnet_npdu_decode(failed_pdu, length, &npdu_dest, &source, &npdu);
    if (offset <= 0 || offset + 4 > length || npdu.network_layer_message ||
        failed_pdu[offset] != PDU_TYPE_CONFIRMED_SERVICE_REQUEST ||
        failed_pdu[offset + 2] != invoke || failed_pdu[offset + 3] != SERVICE_CONFIRMED_COV_NOTIFICATION) { return; }
    bacapp_property_value_list_init(values, 2);
    cov.listOfValues = values;
    if (cov_notify_decode_service_request(failed_pdu + offset + 4, length - offset - 4, &cov) != length - offset - 4) { return; }
    stats.cov_timeouts++;
    for (unsigned i = 0; i < RECOVERY_CAPACITY; ++i) {
        if (recovery[i].used && recovery[i].type == cov.monitoredObjectIdentifier.type &&
            recovery[i].instance == cov.monitoredObjectIdentifier.instance) { return; }
    }
    for (unsigned i = 0; i < RECOVERY_CAPACITY; ++i) {
        if (!recovery[i].used) {
            recovery[i] = (recovery_t){true, cov.monitoredObjectIdentifier.type,
                cov.monitoredObjectIdentifier.instance, now_ms + 1000};
            stats.cov_pending++;
            return;
        }
    }
}

static bool create_point(const ats_point_def_t *point)
{
    uint32_t id = point->instance;
    switch (point->object_type) {
        case OBJECT_ANALOG_INPUT:
            if (Analog_Input_Create(id) != id) { return false; }
            Analog_Input_Name_Set(id, point->name);
            Analog_Input_Description_Set(id, point->description ? point->description : "");
            Analog_Input_Units_Set(id, point->units);
            Analog_Input_COV_Increment_Set(id, point->units == UNITS_HERTZ ? 0.01f : 0.1f);
            Analog_Input_Reliability_Set(id, RELIABILITY_COMMUNICATION_FAILURE);
            break;
        case OBJECT_BINARY_INPUT:
            if (Binary_Input_Create(id) != id) { return false; }
            Binary_Input_Name_Set(id, point->name);
            Binary_Input_Description_Set(id, point->description ? point->description : "");
            Binary_Input_Active_Text_Set(id, "Active");
            Binary_Input_Inactive_Text_Set(id, "Inactive");
            Binary_Input_Reliability_Set(id, RELIABILITY_COMMUNICATION_FAILURE);
            break;
        case OBJECT_MULTI_STATE_INPUT:
            if (Multistate_Input_Create(id) != id) { return false; }
            Multistate_Input_Name_Set(id, point->name);
            Multistate_Input_Description_Set(id, point->description ? point->description : "");
            {
                char states[STATE_TEXT_BYTES] = {0};
                size_t used = 0;
                if (!point->state_count || !point->state_text) { return false; }
                for (uint32_t n = 0; n < point->state_count; ++n) {
                    if (!point->state_text[n]) { return false; }
                    size_t length = strlen(point->state_text[n]) + 1;
                    if (used + length >= sizeof(states)) { return false; }
                    memcpy(states + used, point->state_text[n], length); used += length;
                }
                if (!Multistate_Input_State_Text_List_Set(id, states)) { return false; }
            }
            Multistate_Input_Reliability_Set(id, RELIABILITY_COMMUNICATION_FAILURE);
            break;
        case OBJECT_CHARACTERSTRING_VALUE:
            if (CharacterString_Value_Create(id) != id) { return false; }
            CharacterString_Value_Name_Set(id, point->name);
            CharacterString_Value_Description_Set(id, point->description ? point->description : "");
            break;
        default: return false;
    }
    return true;
}

static void network_properties(void)
{
    uint8_t ip[4], gateway[4], mac[6];
    memcpy(ip, &config.local_ip, 4); memcpy(gateway, &config.gateway, 4);
    memcpy(mac, ip, 4); encode_unsigned16(mac + 4, config.udp_port);
    Network_Port_IP_Address_Set(NETWORK_INSTANCE, ip[0], ip[1], ip[2], ip[3]);
    Network_Port_IP_Subnet_Prefix_Set(NETWORK_INSTANCE, bip_get_subnet_prefix());
    Network_Port_IP_Gateway_Set(NETWORK_INSTANCE, gateway[0], gateway[1], gateway[2], gateway[3]);
    Network_Port_MAC_Address_Set(NETWORK_INSTANCE, mac, 6);
    Network_Port_Reliability_Set(NETWORK_INSTANCE, stats.link_up ? RELIABILITY_NO_FAULT_DETECTED : RELIABILITY_COMMUNICATION_FAILURE);
    Network_Port_Changes_Pending_Set(NETWORK_INSTANCE, false);
}

static void announce(void)
{
    if (!stats.link_up || !bip_valid()) { return; }
    Send_I_Am_Broadcast(transmit_buffer);
    for (size_t i = 0; i < config.peer_count; ++i) {
        BACNET_ADDRESS peer = {0};
        peer.mac_len = 6;
        memcpy(peer.mac, &config.peers[i].ip, 4);
        encode_unsigned16(peer.mac + 4, config.peers[i].port ? config.peers[i].port : config.udp_port);
        Send_I_Am_Unicast(transmit_buffer, &peer);
    }
    next_announce_ms = now_ms + 60000;
}

/* Bounded validation runs before changing any live stack state. Names remain
 * unique across Device, Network Port, diagnostics and all configured types. */
static bool valid_string(const char *value, size_t maximum, bool nonempty)
{
    return value && (!nonempty || value[0]) && strnlen(value, maximum + 1) <= maximum;
}

static bool valid_catalog(const ats_point_def_t *points, size_t count, const char *name)
{
    if (!points || !count || count > GATEWAY_BACNET_MAX_POINTS) { return false; }
    for (size_t i = 0; i < count; ++i) {
        const ats_point_def_t *point = &points[i];
        if ((point->object_type != OBJECT_ANALOG_INPUT && point->object_type != OBJECT_BINARY_INPUT &&
             point->object_type != OBJECT_MULTI_STATE_INPUT && point->object_type != OBJECT_CHARACTERSTRING_VALUE) ||
            point->instance >= BACNET_MAX_INSTANCE ||
            (point->instance >= DIAGNOSTIC_BASE && point->instance < DIAGNOSTIC_BASE + 4) ||
            !valid_string(point->name, GATEWAY_BACNET_POINT_NAME_MAX, true) ||
            (point->description && !valid_string(point->description, GATEWAY_BACNET_POINT_DESCRIPTION_MAX, false)) ||
            !strncmp(point->name, "Gateway-", 8) || !strcmp(point->name, name)) { return false; }
        for (size_t n = 0; n < i; ++n) {
            if (!strcmp(point->name, points[n].name) ||
                (point->object_type == points[n].object_type && point->instance == points[n].instance)) { return false; }
        }
        if (point->object_type == OBJECT_MULTI_STATE_INPUT) {
            if (!point->state_count || point->state_count > GATEWAY_BACNET_MAX_STATES || !point->state_text) { return false; }
            size_t bytes = 1; /* Final NUL terminates the state-name list. */
            for (size_t n = 0; n < point->state_count; ++n) {
                if (!valid_string(point->state_text[n], GATEWAY_BACNET_STATE_NAME_MAX, true)) { return false; }
                bytes += strlen(point->state_text[n]) + 1;
                if (bytes > STATE_TEXT_BYTES) { return false; }
                for (size_t previous = 0; previous < n; ++previous) {
                    if (!strcmp(point->state_text[n], point->state_text[previous])) { return false; }
                }
            }
        } else if (point->state_count || point->state_text) { return false; }
    }
    return true;
}

bool gateway_bacnet_init(const gateway_bacnet_config_t *input, uint64_t timestamp)
{
    if (!input || stats.initialized || input->device_instance >= BACNET_MAX_INSTANCE ||
        !valid_string(input->device_name, sizeof(device_name) - 1, true) ||
        !strncmp(input->device_name, "Gateway-", 8) || !input->udp_port || !input->local_ip || input->peer_count > GATEWAY_BACNET_MAX_PEERS ||
        (input->firmware_version && !valid_string(input->firmware_version, MAX_DEV_VER_LEN, false)) ||
        (input->location && !valid_string(input->location, MAX_DEV_LOC_LEN, false)) ||
        (input->model_name && !valid_string(input->model_name, MAX_DEV_MOD_LEN, true)) ||
        (input->description && !valid_string(input->description, MAX_DEV_DESC_LEN, false)) ||
        ((input->points == NULL) != (input->point_count == 0))) { return false; }
    const ats_point_def_t *points = input->points ? input->points : ats_points;
    size_t point_count = input->points ? input->point_count : ATS_POINT_COUNT;
    if (!valid_catalog(points, point_count, input->device_name)) { return false; }
    config = *input;
    config.points = points; config.point_count = point_count;
    legacy_catalog = points == ats_points;
    snprintf(device_name, sizeof(device_name), "%s", input->device_name);
    snprintf(firmware_version, sizeof(firmware_version), "%s", input->firmware_version ? input->firmware_version : "development");
    snprintf(location, sizeof(location), "%s", input->location ? input->location : "");
    config.device_name = device_name; config.firmware_version = firmware_version; config.location = location;
    now_ms = started_ms = last_timer_ms = last_second_ms = timestamp;
    stats = (gateway_bacnet_stats_t){.link_up = true, .fault_points = (uint32_t)config.point_count};
    memset(recovery, 0, sizeof(recovery));
    for (size_t i = 0; i < config.point_count; ++i) {
        csv_reliability[i] = RELIABILITY_COMMUNICATION_FAILURE;
        csv_quality_changed[i] = true;
    }
    bip_port_configure(config.local_ip, config.netmask, config.gateway, config.udp_port);
    Device_Init(object_table);
    Device_Set_Object_Instance_Number(config.device_instance);
    Device_Object_Name_ANSI_Init(device_name);
    Device_Set_Vendor_Name("Site Modbus Gateway", 19);
    Device_Set_Vendor_Identifier(config.vendor_id);
    const char *model_name = input->model_name ? input->model_name : "ESP32 Modbus-to-BACnet";
    const char *description = input->description ? input->description : "Read-only Modbus TCP to BACnet/IP converter";
    Device_Set_Model_Name(model_name, strlen(model_name));
    Device_Set_Description(description, strlen(description));
    Device_Set_Location(location, strlen(location));
    Device_Set_Firmware_Revision(firmware_version, strlen(firmware_version));
    Device_Set_Application_Software_Version(firmware_version, strlen(firmware_version));
    Device_Set_System_Status(STATUS_OPERATIONAL, true);
    for (size_t i = 0; i < config.point_count; ++i) {
        if (!create_point(&config.points[i])) { gateway_bacnet_shutdown(); return false; }
    }
    const char *names[] = {"Gateway-Uptime", "Gateway-Packets", "Gateway-Good-Points", "Gateway-Fault-Points"};
    for (unsigned i = 0; i < 4; ++i) {
        if (Analog_Input_Create(DIAGNOSTIC_BASE + i) != DIAGNOSTIC_BASE + i) { gateway_bacnet_shutdown(); return false; }
        Analog_Input_Name_Set(DIAGNOSTIC_BASE + i, names[i]);
        Analog_Input_Units_Set(DIAGNOSTIC_BASE + i, i == 0 ? UNITS_SECONDS : UNITS_NO_UNITS);
        Analog_Input_COV_Increment_Set(DIAGNOSTIC_BASE + i, 1.0f);
    }
    Analog_Input_Present_Value_Set(DIAGNOSTIC_BASE + 3, (float)config.point_count);
    if (Binary_Input_Create(DIAGNOSTIC_BASE) != DIAGNOSTIC_BASE ||
        Binary_Input_Create(DIAGNOSTIC_BASE + 1) != DIAGNOSTIC_BASE + 1) {
        gateway_bacnet_shutdown(); return false;
    }
    Binary_Input_Name_Set(DIAGNOSTIC_BASE, "Gateway-Modbus-Healthy");
    Binary_Input_Description_Set(DIAGNOSTIC_BASE, legacy_catalog ?
        "Active when the core system-overview point is freshly acquired and qualified; optional points may be unavailable" :
        "Active when every configured Modbus point is freshly acquired and qualified");
    Binary_Input_Name_Set(DIAGNOSTIC_BASE + 1, "Gateway-Ethernet-Link");
    Network_Port_Object_Instance_Number_Set(0, NETWORK_INSTANCE);
    Network_Port_Name_Set(NETWORK_INSTANCE, "Gateway-Ethernet");
    Network_Port_Description_Set(NETWORK_INSTANCE, "BACnet/IP Ethernet port; configuration is read-only over BACnet");
    Network_Port_Type_Set(NETWORK_INSTANCE, PORT_TYPE_BIP);
    Network_Port_Network_Number_Set(NETWORK_INSTANCE, 0);
    Network_Port_BIP_Port_Set(NETWORK_INSTANCE, config.udp_port);
    Network_Port_BIP_Mode_Set(NETWORK_INSTANCE, BACNET_IP_MODE_NORMAL);
    Network_Port_APDU_Length_Set(NETWORK_INSTANCE, MAX_APDU);
    Network_Port_Link_Speed_Set(NETWORK_INSTANCE, 100000000.0f);
    Network_Port_IP_DHCP_Enable_Set(NETWORK_INSTANCE, config.dhcp_enabled);
    Network_Port_Quality_Set(NETWORK_INSTANCE, PORT_QUALITY_UNKNOWN);
    network_properties();
    address_init();
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, who_has);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROP_MULTIPLE, handler_write_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    apdu_timeout_set(3000); apdu_retries_set(3);
    handler_cov_init(); tsm_set_timeout_handler(cov_timeout);
    if (!bip_init(NULL)) { gateway_bacnet_shutdown(); return false; }
    Device_Set_Database_Revision(input->database_revision ? input->database_revision : 1);
    stats.initialized = true;
    Binary_Input_Present_Value_Set(DIAGNOSTIC_BASE + 1, BINARY_ACTIVE);
    announce();
    return true;
}

void gateway_bacnet_update(const ats_value_t *values)
{
    if (!stats.initialized || !values) { return; }
    stats.good_points = 0;
    for (size_t i = 0; i < config.point_count; ++i) {
        const ats_point_def_t *def = &config.points[i];
        const ats_value_t *value = &values[i];
        bool representable = true;
        if (def->object_type != OBJECT_CHARACTERSTRING_VALUE) {
            representable = isfinite(value->numeric) && fabs(value->numeric) <= FLT_MAX;
        }
        if (def->object_type == OBJECT_BINARY_INPUT) { representable = representable && (value->numeric == 0 || value->numeric == 1); }
        if (def->object_type == OBJECT_MULTI_STATE_INPUT) {
            representable = representable && value->numeric >= 1 && value->numeric <= def->state_count && floor(value->numeric) == value->numeric;
        }
        if (def->object_type == OBJECT_CHARACTERSTRING_VALUE) {
            representable = strnlen(value->text, sizeof(value->text)) < sizeof(value->text);
        }
        bool good = value->quality == ATS_QUALITY_GOOD && representable;
        /* Acquired but unqualified information is useful as a faulted value.
         * Never replace a point from a failed read or absent feature. */
        bool publish = representable && (good ||
            (value->quality == ATS_QUALITY_UNVERIFIED && value->raw_count > 0));
        BACNET_RELIABILITY quality = good ? RELIABILITY_NO_FAULT_DETECTED :
            value->quality == ATS_QUALITY_COMM ? RELIABILITY_COMMUNICATION_FAILURE : RELIABILITY_UNRELIABLE_OTHER;
        stats.good_points += good ? 1 : 0;
        switch (def->object_type) {
            case OBJECT_ANALOG_INPUT:
                if (publish) { Analog_Input_Present_Value_Set(def->instance, (float)value->numeric); }
                Analog_Input_Reliability_Set(def->instance, quality); break;
            case OBJECT_BINARY_INPUT:
                if (publish) { Binary_Input_Present_Value_Set(def->instance, value->numeric ? BINARY_ACTIVE : BINARY_INACTIVE); }
                Binary_Input_Reliability_Set(def->instance, quality); break;
            case OBJECT_MULTI_STATE_INPUT:
                if (publish) { Multistate_Input_Present_Value_Set(def->instance, (uint32_t)value->numeric); }
                Multistate_Input_Reliability_Set(def->instance, quality); break;
            case OBJECT_CHARACTERSTRING_VALUE:
                if (publish) {
                    BACNET_CHARACTER_STRING string;
                    size_t length = strnlen(value->text, sizeof(value->text));
                    characterstring_init(&string, CHARACTER_UTF8, value->text, length);
                    CharacterString_Value_Present_Value_Set(def->instance, &string);
                }
                if (csv_reliability[i] != quality) { csv_quality_changed[i] = true; }
                csv_reliability[i] = quality; break;
            default: break;
        }
    }
    stats.fault_points = (uint32_t)config.point_count - stats.good_points;
    Analog_Input_Present_Value_Set(DIAGNOSTIC_BASE + 2, (float)stats.good_points);
    Analog_Input_Present_Value_Set(DIAGNOSTIC_BASE + 3, (float)stats.fault_points);
    Binary_Input_Present_Value_Set(DIAGNOSTIC_BASE,
        (legacy_catalog ? values[0].quality == ATS_QUALITY_GOOD && isfinite(values[0].numeric) :
            stats.fault_points == 0) ? BINARY_ACTIVE : BINARY_INACTIVE);
}

void gateway_bacnet_tick(uint64_t timestamp)
{
    if (!stats.initialized) { return; }
    if (timestamp < now_ms) { return; } /* Caller supplies monotonic milliseconds. */
    now_ms = timestamp;
    uint64_t delta = now_ms - last_timer_ms;
    if (delta) {
        /* TSM accepts 16-bit elapsed time. A delayed task needs only enough
         * elapsed time to expire outstanding 3-second transactions; do not
         * wrap the timer or run an unbounded catch-up loop after a long pause. */
        tsm_timer_milliseconds(delta > UINT16_MAX ? UINT16_MAX : (uint16_t)delta);
        last_timer_ms = now_ms;
    }
    delta = (now_ms - last_second_ms) / 1000;
    if (delta) { handler_cov_timer_seconds(delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta); last_second_ms += delta * 1000; }
    Analog_Input_Present_Value_Set(DIAGNOSTIC_BASE, (float)(now_ms - started_ms) / 1000.0f);
    Analog_Input_Present_Value_Set(DIAGNOSTIC_BASE + 1, (float)stats.received_packets);
    if (stats.link_up) {
        /* One full bounded pass through at most 256 subscriptions; no wait for ACK. */
        for (unsigned step = 0; step < 4u * MAX_COV_SUBSCRIPTIONS + 8u; ++step) {
            if (handler_cov_fsm()) { break; }
        }
        if (now_ms >= next_announce_ms) { announce(); }
    }
}

bool gateway_bacnet_process_datagram(const uint8_t *data, size_t size,
                                    uint32_t source_ip, uint16_t source_port)
{
    if (!stats.initialized || !stats.link_up) { return false; }
    BACNET_ADDRESS source;
    uint16_t length = bip_port_decode_datagram(data, size, source_ip, source_port,
        &source, receive_buffer, sizeof(receive_buffer));
    if (!length) { return false; }
    stats.received_packets++;
    npdu_handler(&source, receive_buffer, length);
    return true;
}

unsigned gateway_bacnet_poll(unsigned timeout_ms)
{
    if (!stats.initialized || !stats.link_up) { return 0; }
    unsigned count = 0;
    for (; count < 16; ++count) {
        BACNET_ADDRESS source;
        uint16_t length = bip_receive(&source, receive_buffer, sizeof(receive_buffer), count == 0 ? timeout_ms : 0);
        if (!length) { break; }
        stats.received_packets++;
        npdu_handler(&source, receive_buffer, length);
    }
    return count;
}

bool gateway_bacnet_network_update(uint32_t ip, uint32_t mask, uint32_t gateway,
                                   bool link_up, uint64_t timestamp)
{
    if (!stats.initialized) { return false; }
    now_ms = timestamp >= now_ms ? timestamp : now_ms;
    bool changed = config.local_ip != ip || config.netmask != mask || config.gateway != gateway;
    bool was_up = stats.link_up;
    stats.link_up = link_up && ip != 0;
    if (stats.link_up) {
        config.local_ip = ip; config.netmask = mask; config.gateway = gateway;
        bip_port_configure(ip, mask, gateway, config.udp_port);
        if ((!was_up || changed || !bip_valid()) && !bip_init(NULL)) { stats.link_up = false; }
    } else { bip_cleanup(); }
    network_properties();
    Binary_Input_Present_Value_Set(DIAGNOSTIC_BASE + 1, stats.link_up ? BINARY_ACTIVE : BINARY_INACTIVE);
    if (stats.link_up && (!was_up || changed)) { announce(); }
    return stats.link_up == link_up;
}

void gateway_bacnet_stats(gateway_bacnet_stats_t *out) { if (out) { *out = stats; } }
void gateway_bacnet_shutdown(void)
{
    bip_cleanup();
    handler_cov_init();
    /* The stack keeps its COV FSM position across handler_cov_init(). Empty
     * the state machine before loading another catalog in the same process. */
    for (unsigned step = 0; step < 8; ++step) { if (handler_cov_fsm()) { break; } }
    for (unsigned invoke = 1; invoke <= UINT8_MAX; ++invoke) { tsm_free_invoke_id((uint8_t)invoke); }
    Analog_Input_Cleanup(); Binary_Input_Cleanup(); Multistate_Input_Cleanup();
    CharacterString_Value_Cleanup(); Network_Port_Cleanup();
    memset(recovery, 0, sizeof(recovery));
    memset(csv_reliability, 0, sizeof(csv_reliability));
    memset(csv_quality_changed, 0, sizeof(csv_quality_changed));
    config = (gateway_bacnet_config_t){0};
    stats = (gateway_bacnet_stats_t){0};
    legacy_catalog = false;
    now_ms = started_ms = last_timer_ms = last_second_ms = next_announce_ms = 0;
}
