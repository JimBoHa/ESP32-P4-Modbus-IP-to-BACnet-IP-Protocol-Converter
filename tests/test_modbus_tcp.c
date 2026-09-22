#include "modbus_tcp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENTINEL 0xA55AU

static const uint8_t good_response[] = {
    0x12, 0x34, 0x00, 0x00, 0x00, 0x09, 0x11,
    0x03, 0x06, 0x00, 0x01, 0x80, 0x00, 0xFF, 0xFF
};

static void fill_output(uint16_t output[MB_MAX_REGISTERS])
{
    size_t index;
    for (index = 0; index < MB_MAX_REGISTERS; ++index) {
        output[index] = SENTINEL;
    }
}

static void assert_unchanged(const uint16_t output[MB_MAX_REGISTERS])
{
    size_t index;
    for (index = 0; index < MB_MAX_REGISTERS; ++index) {
        assert(output[index] == SENTINEL);
    }
}

static void expect_error(const uint8_t *frame, size_t length, uint16_t qty,
                         mb_error_t expected)
{
    uint16_t output[MB_MAX_REGISTERS];
    mb_result_t result;
    fill_output(output);
    memset(&result, 0xFF, sizeof(result));
    assert(mb_decode_fc03_response(frame, length, 0x1234, 0x11, qty,
                                   output, &result) == expected);
    assert(result.error == expected);
    assert(result.elapsed_ms == 0);
    assert(result.system_error == 0);
    if (expected != MB_ERR_EXCEPTION) {
        assert(result.exception_code == 0);
    }
    assert_unchanged(output);
}

static void test_request(void)
{
    const uint8_t expected[] = {
        0x12, 0x34, 0x00, 0x00, 0x00, 0x06, 0x11, 0x03,
        0x00, 0x6B, 0x00, 0x03
    };
    uint8_t request[MB_FC03_REQUEST_SIZE];
    uint8_t before[MB_FC03_REQUEST_SIZE];
    memset(request, 0xA5, sizeof(request));
    memcpy(before, request, sizeof(request));
    assert(mb_build_fc03_request(request, 0x1234, 0x11, 107, 0) == 0);
    assert(memcmp(request, before, sizeof(request)) == 0);
    assert(mb_build_fc03_request(request, 0x1234, 0x11, 107, 51) == 0);
    assert(memcmp(request, before, sizeof(request)) == 0);
    assert(mb_build_fc03_request(request, 0x1234, 0x11, 65535, 2) == 0);
    assert(memcmp(request, before, sizeof(request)) == 0);
    assert(mb_build_fc03_request(NULL, 1, 1, 0, 1) == 0);
    assert(mb_build_fc03_request(request, 0x1234, 0x11, 107, 3) == sizeof(expected));
    assert(memcmp(request, expected, sizeof(expected)) == 0);
    assert(mb_build_fc03_request(request, 0xFFFF, 0xFF, 65535, 1) == sizeof(request));
    assert(request[7] == 0x03 && request[8] == 0xFF && request[9] == 0xFF);
    assert(mb_build_fc03_request(request, 1, 0, 65486, 50) == sizeof(request));
    assert(request[7] == 0x03 && request[11] == 50);
}

static void test_success(void)
{
    uint16_t output[MB_MAX_REGISTERS];
    mb_result_t result;
    uint8_t maximum[MB_MAX_RESPONSE_SIZE] = {0x12, 0x34, 0, 0, 0, 103, 0x11, 3, 100};
    size_t index;
    fill_output(output);
    assert(mb_decode_fc03_response(good_response, sizeof(good_response),
                                   0x1234, 0x11, 3, output, &result) == MB_OK);
    assert(result.error == MB_OK && result.exception_code == 0);
    assert(output[0] == 1 && output[1] == 32768 && output[2] == 65535);
    for (index = 3; index < MB_MAX_REGISTERS; ++index) {
        assert(output[index] == SENTINEL);
    }
    for (index = 0; index < MB_MAX_REGISTERS; ++index) {
        maximum[9 + 2 * index] = (uint8_t)index;
        maximum[10 + 2 * index] = (uint8_t)(255 - index);
    }
    assert(mb_decode_fc03_response(maximum, sizeof(maximum), 0x1234, 0x11,
                                   MB_MAX_REGISTERS, output, NULL) == MB_OK);
    for (index = 0; index < MB_MAX_REGISTERS; ++index) {
        assert(output[index] == (uint16_t)(index * 256 + 255 - index));
    }
}

static void test_invalid_frames(void)
{
    uint8_t frame[sizeof(good_response) + 1];
    size_t length;
    memcpy(frame, good_response, sizeof(good_response));
    frame[sizeof(good_response)] = 0;
    for (length = 0; length < sizeof(good_response); ++length) {
        expect_error(frame, length, 3, MB_ERR_LENGTH);
    }
    expect_error(frame, sizeof(frame), 3, MB_ERR_LENGTH);
    expect_error(NULL, sizeof(good_response), 3, MB_ERR_ARGUMENT);
    expect_error(frame, sizeof(good_response), 0, MB_ERR_ARGUMENT);
    expect_error(frame, sizeof(good_response), 51, MB_ERR_ARGUMENT);

#define MUTATE_AND_CHECK(POSITION, VALUE, ERROR) do { \
        memcpy(frame, good_response, sizeof(good_response)); \
        frame[(POSITION)] = (VALUE); \
        expect_error(frame, sizeof(good_response), 3, (ERROR)); \
    } while (0)
    MUTATE_AND_CHECK(0, 0x13, MB_ERR_TRANSACTION);
    MUTATE_AND_CHECK(2, 1, MB_ERR_PROTOCOL);
    MUTATE_AND_CHECK(6, 0x12, MB_ERR_UNIT);
    MUTATE_AND_CHECK(5, 2, MB_ERR_LENGTH);
    MUTATE_AND_CHECK(5, 104, MB_ERR_LENGTH);
    MUTATE_AND_CHECK(5, 8, MB_ERR_LENGTH);
    MUTATE_AND_CHECK(7, 4, MB_ERR_FUNCTION);
    MUTATE_AND_CHECK(7, 0x84, MB_ERR_FUNCTION);
    MUTATE_AND_CHECK(7, 0x83, MB_ERR_LENGTH);
    MUTATE_AND_CHECK(8, 4, MB_ERR_BYTE_COUNT);
    MUTATE_AND_CHECK(8, 7, MB_ERR_BYTE_COUNT);
#undef MUTATE_AND_CHECK

    /* Internally consistent byte count, but too few bytes for the request. */
    memcpy(frame, good_response, sizeof(good_response));
    frame[5] = 7;
    expect_error(frame, 13, 3, MB_ERR_LENGTH);
}

static void test_exception(void)
{
    uint8_t frame[] = {0x12, 0x34, 0, 0, 0, 3, 0x11, 0x83, 2};
    uint16_t output[MB_MAX_REGISTERS];
    mb_result_t result;
    fill_output(output);
    assert(mb_decode_fc03_response(frame, sizeof(frame), 0x1234, 0x11,
                                   3, output, &result) == MB_ERR_EXCEPTION);
    assert(result.error == MB_ERR_EXCEPTION && result.exception_code == 2);
    assert_unchanged(output);
    frame[8] = 0;
    expect_error(frame, sizeof(frame), 3, MB_ERR_LENGTH);
}

static void test_invalid_network_arguments(void)
{
    uint16_t output[MB_MAX_REGISTERS];
    mb_result_t result;
    fill_output(output);
    assert(mb_read_holding("localhost", 502, 1, 0, 1, 1, 100,
                           output, &result) == MB_ERR_ARGUMENT);
    assert(mb_read_holding("127.0.0.1", 0, 1, 0, 1, 1, 100,
                           output, &result) == MB_ERR_ARGUMENT);
    assert(mb_read_holding("127.0.0.1", 502, 1, 0, 1, 1, 0,
                           output, &result) == MB_ERR_ARGUMENT);
    assert(mb_read_holding("127.0.0.1", 502, 1, 65535, 2, 1, 100,
                           output, &result) == MB_ERR_ARGUMENT);
    assert(mb_read_holding(NULL, 502, 1, 0, 1, 1, 100,
                           output, &result) == MB_ERR_ARGUMENT);
    assert(mb_read_holding("127.0.0.1", 502, 1, 0, 1, 1, 100,
                           NULL, &result) == MB_ERR_ARGUMENT);
    assert_unchanged(output);
}

/* The Python integration driver may contact only a loopback ephemeral port. */
static int probe(int argc, char **argv)
{
    uint16_t output[MB_MAX_REGISTERS];
    mb_result_t result;
    unsigned long port, timeout;
    size_t index;
    if (argc != 4) {
        return 2;
    }
    port = strtoul(argv[2], NULL, 10);
    timeout = strtoul(argv[3], NULL, 10);
    if (port == 0 || port > 65535 || timeout == 0 || timeout > 10000) {
        return 2;
    }
    fill_output(output);
    (void)mb_read_holding("127.0.0.1", (uint16_t)port, 0x11, 107, 3,
                         0x1234, (uint32_t)timeout, output, &result);
    printf("{\"error\":\"%s\",\"elapsed_ms\":%lu,\"exception\":%u,"
           "\"system_error\":%d,\"values\":[", mb_error_string(result.error),
           (unsigned long)result.elapsed_ms, (unsigned)result.exception_code,
           result.system_error);
    for (index = 0; index < MB_MAX_REGISTERS; ++index) {
        printf("%s%u", index ? "," : "", (unsigned)output[index]);
    }
    puts("]}");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--probe") == 0) {
        return probe(argc, argv);
    }
    assert(argc == 1);
    test_request();
    test_success();
    test_invalid_frames();
    test_exception();
    test_invalid_network_arguments();
    puts("Modbus TCP packet tests passed");
    return 0;
}
