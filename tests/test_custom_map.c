#include "custom_map.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static custom_map_t map, before;
static custom_poll_t poll_state;
static ats_value_t values[CUSTOM_MAX_POINTS];
static char error[200];
static unsigned calls;
static mb_error_t fake_error;
static uint16_t fake_words[50];
static uint8_t expected_fc;
static uint16_t expected_address, expected_count;

mb_error_t mb_read_points(const char *host, uint16_t port, uint8_t unit,
                         uint8_t function, uint16_t offset, uint16_t qty,
                         uint16_t tid, uint32_t timeout_ms,
                         uint16_t output[50], mb_result_t *result)
{
    assert(!strcmp(host, "192.0.2.1") && port == 502 && unit == 17);
    assert(function == expected_fc && offset == expected_address && qty == expected_count);
    assert(tid && timeout_ms == 1200);
    assert(poll_state.last_function == function && poll_state.last_offset == offset);
    assert(poll_state.last_quantity == qty && poll_state.transaction_id == tid);
    ++calls;
    memset(result, 0, sizeof(*result));
    result->elapsed_ms = 8;
    result->error = fake_error;
    if (fake_error == MB_ERR_EXCEPTION) result->exception_code = 2;
    if (fake_error == MB_OK) memcpy(output, fake_words, qty * sizeof(*output));
    return fake_error;
}

static bool parse_rows(const char *rows)
{
    size_t size = strlen(CUSTOM_CSV_HEADER) + strlen(rows) + 2;
    char *csv = malloc(size);
    assert(csv);
    snprintf(csv, size, "%s\n%s", CUSTOM_CSV_HEADER, rows);
    bool result = custom_map_parse(&map, csv, strlen(csv), error, sizeof(error));
    free(csv);
    return result;
}

static void rejects(const char *row, const char *message)
{
    memcpy(&before, &map, sizeof(map));
    assert(!parse_rows(row));
    assert(strstr(error, "Line "));
    if (message) assert(strstr(error, message));
    assert(!memcmp(&before, &map, sizeof(map)));
}

static void test_parser(void)
{
    assert(parse_rows(
        "100,Voltage,AI,3,0,u16,AB,0.1,0,62,,,,1000,Voltage\n"
        "101,Alarm,BI,4,65535,bit,AB,1,0,95,3,,,5000,Alarm bit\n"
        "102,Mode,MSI,3,100,u16,AB,1,1,95,,Off|Auto,,1000,Mode\n"
        "103,Name,CSV,4,200,ascii,BA,1,0,95,,,8,60000,\"Name, label with \"\"quotes\"\"\"\n"
        "104,Running,BI,2,0,bool,,1,0,95,,,,1000,Discrete input\n"));
    assert(map.count == 5 && map.defs[0].units == 62);
    assert(map.defs[3].word_count == 4);
    assert(!strcmp(map.defs[3].description, "Name, label with \"quotes\""));
    assert(map.defs[2].state_count == 2 && !strcmp(map.defs[2].state_text[1], "Auto"));
    /* All pointer fields refer to destination storage, never freed scratch. */
    assert(map.defs[0].name >= map.strings && map.defs[0].name < map.strings + sizeof(map.strings));
    assert(map.defs[2].state_text >= map.state_text);
    rejects("9001,X,AI,3,0,u16,AB,,,,,,,1000,\n", "instance");
    rejects("4194303,X,AI,3,0,u16,AB,,,,,,,1000,\n", "instance");
    rejects("9999999999999999999,X,AI,3,0,u16,AB,,,,,,,1000,\n", "instance");
    rejects("1,Gateway-Thing,AI,3,0,u16,AB,,,,,,,1000,\n", "Name");
    rejects("1,X,AI,5,0,u16,AB,,,,,,,1000,\n", "functions");
    rejects("1,X,AI,3,65535,u32,ABCD,,,,,,,1000,\n", "extends");
    rejects("1,X,AI,3,0,f32,ABCD,nan,,,,,,1000,\n", "finite");
    rejects("1,X,AI,3,0,f32,ABCD,1,inf,,,,,1000,\n", "finite");
    rejects("1,X,AI,3,0,f32,AB,,,,,,,1000,\n", "byte_order");
    rejects("1,X,AI,1,0,u16,AB,,,,,,,1000,\n", "FC1/2");
    rejects("1,X,BI,3,0,bool,,,,,,,,1000,\n", "bool requires");
    rejects("1,X,BI,3,0,bit,AB,1,0,95,16,,,1000,\n", "bit must");
    rejects("1,X,BI,1,0,bool,AB,,,,,,,1000,\n", "byte_order");
    rejects("1,X,MSI,3,0,u16,AB,,,95,,,,1000,\n", "states");
    rejects("1,X,MSI,3,0,u16,AB,,,95,,a||b,,1000,\n", "state label");
    rejects("1,X,MSI,3,0,u16,AB,,,95,,a|a,,1000,\n", "Duplicate MSI");
    rejects("1,X,CSV,4,0,ascii,AB,,,95,,,21,1000,\n", "ASCII length");
    rejects("1,X,AI,3,0,u16,AB,,,,,,,999,\n", "poll_ms");
    rejects("1,X,AI,3,0,u16,AB,,,,,,,1000,\n1,Y,AI,3,1,u16,AB,,,,,,,1000,\n", "Duplicate BACnet");
    rejects("1,X,AI,3,0,u16,AB,,,,,,,1000,\n2,X,AI,3,1,u16,AB,,,,,,,1000,\n", "Duplicate point");
    rejects("1,X,AI,3,0,u16,AB,,,,,,,1000,\xff\n", "non-ASCII");
    rejects("1,X,AI,3,0,u16,AB,,,,,,,1000,\"line\nbreak\"\n", "syntax");
    rejects("1,X,AI,3,0,u16,AB,,,,,,,1000,\"unterminated\n", "syntax");
    rejects("1,X,AI,3,0,u16,AB,,,,,,,1000,\n\n", "syntax");
    rejects("", "At least");
    char header[300];
    snprintf(header, sizeof(header), "%sEXTRA\n", CUSTOM_CSV_HEADER);
    assert(!custom_map_parse(&map, header, strlen(header), error, sizeof(error)));
    assert(strstr(error, "Header"));
    assert(!custom_map_parse(&map, header, CUSTOM_MAX_CSV_BYTES + 1, error, sizeof(error)));
    /* CRLF is supported; field whitespace is preserved, not silently trimmed. */
    assert(parse_rows("1,X,AI,3,65535,u16,AB,,,,,,,1000,\r\n"));
    assert(map.defs[0].offset == 65535);
    char many[20000];
    size_t used = 0;
    for (unsigned i = 0; i < 129; ++i) {
        used += (size_t)snprintf(many + used, sizeof(many) - used,
                                "%u,P%u,AI,3,0,u16,AB,,,,,,,1000,\n", i + 1, i);
    }
    rejects(many, "Maximum 128");
}

static void test_decoding(void)
{
    assert(parse_rows(
        "1,U16,AI,3,0,u16,BA,0.1,10,95,,,,1000,\n"
        "2,S16,AI,4,0,s16,AB,1,0,95,,,,1000,\n"
        "3,U32,AI,3,0,u32,CDAB,1,0,95,,,,1000,\n"
        "4,S32,AI,3,0,s32,DCBA,1,0,95,,,,1000,\n"
        "5,F32,AI,4,0,f32,BADC,1,0,95,,,,1000,\n"
        "6,Bit,BI,3,0,bit,BA,1,0,95,3,,,1000,\n"
        "7,Ascii,CSV,3,0,ascii,BA,1,0,95,,,4,1000,\n"
        "8,Bool,BI,1,0,bool,,1,0,95,,,,1000,\n"
        "9,Mode,MSI,3,0,u16,AB,1,1,95,,Off|On,,1000,\n"));
    custom_poll_init(&poll_state, &map);
    uint16_t words[][2] = {{0xE803,0}, {0xFFFE,0}, {0x5678,0x1234},
        {0xFFFF,0xFFFF}, {0xC043,0}, {0x0800,0}, {0x4241,0x4443}, {1,0}, {1,0}};
    double expected[] = {110, -2, 305419896, -1, 384, 1, 0, 1, 2};
    for (size_t i = 0; i < map.count; ++i) {
        assert(custom_map_apply(&map, i, words[i], map.defs[i].word_count, 100, &poll_state.values[i]));
        assert(poll_state.values[i].quality == ATS_QUALITY_GOOD);
        assert(poll_state.values[i].last_good_ms == 100);
        if (i != 6) assert(poll_state.values[i].numeric == expected[i]);
    }
    assert(!strcmp(poll_state.values[6].text, "ABCD"));
    uint16_t invalid_float[] = {0xC07F,0}; /* swapped bytes -> quiet NaN */
    assert(!custom_map_apply(&map, 4, invalid_float, 2, 200, &poll_state.values[4]));
    assert(poll_state.values[4].numeric == 384 && poll_state.values[4].last_good_ms == 100);
    assert(poll_state.values[4].quality == ATS_QUALITY_UNVERIFIED);
    uint16_t out_of_range[] = {2,0};
    assert(!custom_map_apply(&map, 8, out_of_range, 1, 200, &poll_state.values[8]));
    assert(poll_state.values[8].numeric == 2);
    assert(!custom_map_apply(&map, 7, out_of_range, 1, 200, &poll_state.values[7]));
    uint16_t invalid_ascii[] = {0x0041,0x4443};
    assert(!custom_map_apply(&map, 6, invalid_ascii, 2, 200, &poll_state.values[6]));
    assert(!strcmp(poll_state.values[6].text, "ABCD"));
    assert(!custom_map_apply(&map, 0, words[0], 2, 200, &poll_state.values[0]));
    assert(!custom_map_apply(&map, map.count, words[0], 1, 200, &poll_state.values[0]));
}

static void test_polling(void)
{
    assert(parse_rows("1,Voltage,AI,4,10,u16,AB,0.1,0,62,,,,1000,\n"
                      "2,Running,BI,2,3,bool,,1,0,95,,,,1000,\n"));
    custom_poll_init(&poll_state, &map);
    custom_poll_snapshot(&poll_state, &map, 0, values);
    assert(isnan(values[0].numeric) && values[0].quality == ATS_QUALITY_COMM);
    fake_error = MB_OK;
    expected_fc = 4; expected_address = 10; expected_count = 1; fake_words[0] = 4900;
    assert(custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, 1000));
    assert(poll_state.requests == 1 && poll_state.successes == 1 && calls == 1);
    assert(poll_state.failures == 0 && poll_state.last_function == 4 && poll_state.last_offset == 10);
    assert(poll_state.next_request_ms == 1258);
    assert(!custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, 1257));
    expected_fc = 2; expected_address = 3; fake_words[0] = 1;
    assert(custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, 1258));
    assert(poll_state.failures == 0 && poll_state.last_function == 2 && poll_state.last_offset == 3);
    custom_poll_snapshot(&poll_state, &map, 1300, values);
    assert(values[0].numeric == 490 && values[1].numeric == 1);
    assert(values[0].quality == ATS_QUALITY_GOOD && values[1].quality == ATS_QUALITY_GOOD);
    assert(!custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, 1600));
    expected_fc = 4; expected_address = 10; fake_error = MB_ERR_EXCEPTION;
    assert(custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, 2100));
    assert(poll_state.failures == 1 && poll_state.last_function == 4 && poll_state.last_offset == 10);
    assert(poll_state.last_quantity == 1 && poll_state.last_result.exception_code == 2);
    custom_poll_snapshot(&poll_state, &map, 2110, values);
    assert(values[0].quality == ATS_QUALITY_COMM && values[0].numeric == 490);
    assert(values[1].quality == ATS_QUALITY_GOOD);
    assert(poll_state.next_request_ms == 7108);
    expected_fc = 2; expected_address = 3; fake_error = MB_ERR_TIMEOUT;
    assert(custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, 7108));
    assert(poll_state.next_request_ms == 17116 && poll_state.failures == 2);
    assert(poll_state.last_function == 2 && poll_state.last_offset == 3 && poll_state.last_quantity == 1);
    assert(poll_state.last_result.error == MB_ERR_TIMEOUT);
    assert(poll_state.values[0].quality == ATS_QUALITY_COMM && poll_state.values[1].quality == ATS_QUALITY_COMM);
    expected_fc = 4; expected_address = 10; fake_error = MB_OK; fake_words[0] = 4910;
    assert(custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, 17116));
    assert(poll_state.consecutive_failures == 0 && poll_state.values[0].numeric == 491);
    assert(poll_state.failures == 2 && poll_state.last_function == 4 && poll_state.last_offset == 10);
    custom_poll_snapshot(&poll_state, &map, 24000, values);
    assert(values[0].quality == ATS_QUALITY_COMM && values[0].numeric == 491);
    custom_poll_offline(&poll_state, &map, 24000);
    assert(poll_state.values[0].quality == ATS_QUALITY_COMM && poll_state.values[0].numeric == 491);
}

static void test_failure_metadata(void)
{
    const char *rows[] = {
        "1,Coil,BI,1,3,bool,,1,0,95,,,,1000,\n",
        "1,Input,BI,2,7,bool,,1,0,95,,,,1000,\n",
        "1,Holding,AI,3,11,u32,ABCD,1,0,95,,,,1000,\n",
        "1,Name,CSV,4,15,ascii,AB,1,0,95,,,8,1000,\n",
    };
    const uint16_t addresses[] = {3,7,11,15};
    const uint16_t quantities[] = {1,1,2,4};
    const mb_error_t failures[] = {MB_ERR_CONNECT,MB_ERR_TRANSACTION,MB_ERR_EXCEPTION};
    for (size_t f=0;f<sizeof(rows)/sizeof(rows[0]);++f) {
        assert(parse_rows(rows[f]));
        expected_fc = (uint8_t)(f+1);
        expected_address = addresses[f];
        expected_count = quantities[f];
        for (size_t e=0;e<sizeof(failures)/sizeof(failures[0]);++e) {
            custom_poll_init(&poll_state, &map);
            fake_error = failures[e];
            assert(custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, 1000));
            assert(poll_state.requests == 1 && poll_state.successes == 0 && poll_state.failures == 1);
            assert(poll_state.last_function == expected_fc && poll_state.last_offset == expected_address);
            assert(poll_state.last_quantity == expected_count && poll_state.transaction_id == 1);
            assert(poll_state.last_result.error == failures[e] && poll_state.last_result.elapsed_ms == 8);
            /* Backoff calls must preserve the failed request for diagnostics. */
            assert(!custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17,
                                     poll_state.next_request_ms-1));
            assert(poll_state.requests == 1 && poll_state.last_result.error == failures[e]);
            assert(poll_state.last_function == expected_fc && poll_state.last_quantity == expected_count);
            fake_error = MB_OK;
            memset(fake_words, 0, sizeof(fake_words));
            if (f==3) {
                fake_words[0]=0x4142; fake_words[1]=0x4344;
                fake_words[2]=0x4546; fake_words[3]=0x4748;
            }
            assert(custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17,
                                    poll_state.next_request_ms));
            assert(poll_state.requests == 2 && poll_state.successes == 1 && poll_state.failures == 1);
            assert(poll_state.last_result.error == MB_OK && poll_state.transaction_id == 2);
            assert(poll_state.last_function == expected_fc && poll_state.last_offset == expected_address);
            assert(poll_state.last_quantity == expected_count);
        }
    }
}

static void test_full_map_cadence(void)
{
    char many[20000];
    size_t used = 0;
    for (unsigned i = 0; i < CUSTOM_MAX_POINTS; ++i) {
        used += (size_t)snprintf(many + used, sizeof(many) - used,
                                "%u,P%u,AI,3,0,u16,AB,,,,,,,1000,\n", i + 1, i);
    }
    assert(parse_rows(many));
    assert(map.count == 128 && custom_map_stale_ms(&map, 0) == 97200);
    assert(custom_map_stale_ms(&map, 128) == 0);
    custom_poll_init(&poll_state, &map);
    fake_error = MB_OK; fake_words[0] = 123;
    expected_fc = 3; expected_address = 0; expected_count = 1;
    uint64_t now = 1000;
    for (size_t i = 0; i < map.count; ++i) {
        assert(custom_poll_step(&poll_state, &map, "192.0.2.1", 502, 17, now));
        now = poll_state.next_request_ms;
    }
    custom_poll_snapshot(&poll_state, &map, now, values);
    for (size_t i = 0; i < map.count; ++i) {
        assert(values[i].quality == ATS_QUALITY_GOOD && values[i].numeric == 123);
    }
    custom_poll_snapshot(&poll_state, &map, values[0].last_good_ms + 97200, values);
    assert(values[0].quality == ATS_QUALITY_GOOD);
    custom_poll_snapshot(&poll_state, &map, values[0].last_good_ms + 97201, values);
    assert(values[0].quality == ATS_QUALITY_COMM && values[0].numeric == 123);
}

int main(void)
{
    test_parser(); test_decoding(); test_polling(); test_failure_metadata(); test_full_map_cadence();
    puts("Custom map parser, decoder, polling and failure metadata tests passed");
    return 0;
}
