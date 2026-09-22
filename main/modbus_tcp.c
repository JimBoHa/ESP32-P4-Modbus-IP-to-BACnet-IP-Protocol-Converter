#if !defined(ESP_PLATFORM) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "modbus_tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#endif

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static int valid_quantity(uint16_t qty)
{
    return qty >= 1U && qty <= MB_MAX_REGISTERS;
}

static void reset_result(mb_result_t *result)
{
    memset(result, 0, sizeof(*result));
}

uint64_t mb_monotonic_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time() / 1000U;
#else
    struct timespec now = {0, 0};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
#endif
}

const char *mb_error_string(mb_error_t error)
{
    switch (error) {
    case MB_OK: return "ok";
    case MB_ERR_ARGUMENT: return "argument";
    case MB_ERR_CONNECT: return "connect";
    case MB_ERR_IO: return "io";
    case MB_ERR_TIMEOUT: return "timeout";
    case MB_ERR_TRUNCATED: return "truncated";
    case MB_ERR_TRANSACTION: return "transaction";
    case MB_ERR_PROTOCOL: return "protocol";
    case MB_ERR_UNIT: return "unit";
    case MB_ERR_LENGTH: return "length";
    case MB_ERR_FUNCTION: return "function";
    case MB_ERR_BYTE_COUNT: return "byte_count";
    case MB_ERR_EXCEPTION: return "exception";
    default: return "unknown";
    }
}

size_t mb_build_fc03_request(uint8_t request[MB_FC03_REQUEST_SIZE],
                            uint16_t tid, uint8_t unit, uint16_t offset,
                            uint16_t qty)
{
    uint8_t encoded[MB_FC03_REQUEST_SIZE] = {0};
    if (request == NULL || !valid_quantity(qty) ||
        (uint32_t)offset + qty > 65536U) {
        return 0;
    }
    write_be16(encoded, tid);
    write_be16(encoded + 4, 6U); /* Unit + FC03 + offset + quantity. */
    encoded[6] = unit;
    encoded[7] = 0x03U;
    write_be16(encoded + 8, offset);
    write_be16(encoded + 10, qty);
    memcpy(request, encoded, sizeof(encoded));
    return sizeof(encoded);
}

static mb_error_t check_header(const uint8_t *frame, uint16_t expected_tid,
                              uint8_t expected_unit)
{
    uint16_t remaining = read_be16(frame + 4);
    if (read_be16(frame) != expected_tid) {
        return MB_ERR_TRANSACTION;
    }
    if (read_be16(frame + 2) != 0U) {
        return MB_ERR_PROTOCOL;
    }
    if (frame[6] != expected_unit) {
        return MB_ERR_UNIT;
    }
    if (remaining < 3U || remaining > 3U + 2U * MB_MAX_REGISTERS) {
        return MB_ERR_LENGTH;
    }
    return MB_OK;
}

mb_error_t mb_decode_fc03_response(const uint8_t *frame, size_t length,
                                   uint16_t expected_tid, uint8_t expected_unit,
                                   uint16_t qty, uint16_t out[MB_MAX_REGISTERS],
                                   mb_result_t *result)
{
    mb_result_t local_result;
    uint16_t decoded[MB_MAX_REGISTERS];
    uint16_t remaining;
    mb_error_t error = MB_OK;
    size_t index;
    if (result == NULL) {
        result = &local_result;
    }
    reset_result(result);
    if (frame == NULL || out == NULL || !valid_quantity(qty)) {
        error = MB_ERR_ARGUMENT;
        goto done;
    }
    if (length < 7U || length > MB_MAX_RESPONSE_SIZE) {
        error = MB_ERR_LENGTH;
        goto done;
    }
    error = check_header(frame, expected_tid, expected_unit);
    if (error != MB_OK) {
        goto done;
    }
    remaining = read_be16(frame + 4);
    if (length != 6U + remaining) {
        error = MB_ERR_LENGTH;
        goto done;
    }
    if (frame[7] == 0x83U) {
        if (remaining != 3U || frame[8] == 0U) {
            error = MB_ERR_LENGTH;
        } else {
            result->exception_code = frame[8];
            error = MB_ERR_EXCEPTION;
        }
        goto done;
    }
    if (frame[7] != 0x03U) {
        error = MB_ERR_FUNCTION;
        goto done;
    }
    if (frame[8] != 2U * qty) {
        error = MB_ERR_BYTE_COUNT;
        goto done;
    }
    if (remaining != 3U + 2U * qty) {
        error = MB_ERR_LENGTH;
        goto done;
    }
    for (index = 0; index < qty; ++index) {
        decoded[index] = read_be16(frame + 9U + 2U * index);
    }
    memcpy(out, decoded, (size_t)qty * sizeof(*out));
done:
    result->error = error;
    return error;
}

static int would_block(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK;
}

/* Recalculate remaining time after every EINTR, readiness wakeup, or partial
 * transfer. A slow peer cannot extend the original request's deadline. */
static mb_error_t wait_ready(int fd, int writing, uint64_t deadline,
                             mb_result_t *result)
{
    for (;;) {
        uint64_t now = mb_monotonic_ms();
        uint64_t remaining;
        struct timeval timeout;
        fd_set active, errors;
        int count;
        if (now >= deadline) {
            return MB_ERR_TIMEOUT;
        }
        remaining = deadline - now;
        timeout.tv_sec = (long)(remaining / 1000U);
        timeout.tv_usec = (long)((remaining % 1000U) * 1000U);
        FD_ZERO(&active);
        FD_ZERO(&errors);
        FD_SET(fd, &active);
        FD_SET(fd, &errors);
        count = select(fd + 1, writing ? NULL : &active,
                       writing ? &active : NULL, &errors, &timeout);
        if (count > 0) {
            return mb_monotonic_ms() < deadline ? MB_OK : MB_ERR_TIMEOUT;
        }
        if (count == 0) {
            return MB_ERR_TIMEOUT;
        }
        if (errno != EINTR) {
            result->system_error = errno;
            return MB_ERR_IO;
        }
    }
}

static mb_error_t send_exact(int fd, const uint8_t *bytes, size_t length,
                            uint64_t deadline, mb_result_t *result)
{
    size_t position = 0;
    while (position < length) {
        ssize_t sent;
        int flags = 0;
        mb_error_t error = wait_ready(fd, 1, deadline, result);
        if (error != MB_OK) {
            return error;
        }
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        sent = send(fd, bytes + position, length - position, flags);
        if (sent > 0) {
            position += (size_t)sent;
        } else if (sent == 0) {
            return MB_ERR_IO;
        } else if (errno != EINTR && !would_block(errno)) {
            result->system_error = errno;
            return MB_ERR_IO;
        }
    }
    return mb_monotonic_ms() < deadline ? MB_OK : MB_ERR_TIMEOUT;
}

static mb_error_t receive_exact(int fd, uint8_t *bytes, size_t length,
                               uint64_t deadline, mb_result_t *result)
{
    size_t position = 0;
    while (position < length) {
        ssize_t received;
        mb_error_t error = wait_ready(fd, 0, deadline, result);
        if (error != MB_OK) {
            return error;
        }
        received = recv(fd, bytes + position, length - position, 0);
        if (received > 0) {
            position += (size_t)received;
        } else if (received == 0) {
            return MB_ERR_TRUNCATED;
        } else if (errno != EINTR && !would_block(errno)) {
            result->system_error = errno;
            return MB_ERR_IO;
        }
    }
    return mb_monotonic_ms() < deadline ? MB_OK : MB_ERR_TIMEOUT;
}

mb_error_t mb_read_holding(const char *host_ipv4, uint16_t port, uint8_t unit,
                           uint16_t offset, uint16_t qty, uint16_t tid,
                           uint32_t timeout_ms,
                           uint16_t out[MB_MAX_REGISTERS], mb_result_t *result)
{
    mb_result_t local_result;
    uint8_t request[MB_FC03_REQUEST_SIZE];
    uint8_t response[MB_MAX_RESPONSE_SIZE];
    struct sockaddr_in address;
    uint64_t started = mb_monotonic_ms();
    uint64_t deadline = started + timeout_ms;
    uint64_t elapsed;
    size_t length;
    int fd = -1;
    int flags;
    int connected;
    mb_error_t error = MB_ERR_ARGUMENT;
    if (result == NULL) {
        result = &local_result;
    }
    reset_result(result);
    memset(&address, 0, sizeof(address));
    if (host_ipv4 == NULL || out == NULL || port == 0U || timeout_ms == 0U ||
        mb_build_fc03_request(request, tid, unit, offset, qty) == 0U ||
        inet_pton(AF_INET, host_ipv4, &address.sin_addr) != 1) {
        goto done;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        result->system_error = errno;
        error = MB_ERR_CONNECT;
        goto done;
    }
#ifndef ESP_PLATFORM
    /* lwIP has its own socket-offset-aware FD_SET implementation. */
    if (fd >= FD_SETSIZE) {
        result->system_error = EMFILE;
        error = MB_ERR_IO;
        goto done;
    }
#endif
#ifdef SO_NOSIGPIPE
    {
        int enabled = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
            result->system_error = errno;
            error = MB_ERR_IO;
            goto done;
        }
    }
#endif
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        result->system_error = errno;
        error = MB_ERR_IO;
        goto done;
    }
    if (mb_monotonic_ms() >= deadline) {
        error = MB_ERR_TIMEOUT;
        goto done;
    }
    connected = connect(fd, (const struct sockaddr *)&address, sizeof(address));
    if (connected < 0) {
        int pending = errno;
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (pending != EINPROGRESS && pending != EALREADY &&
            pending != EINTR && !would_block(pending)) {
            result->system_error = pending;
            error = MB_ERR_CONNECT;
            goto done;
        }
        error = wait_ready(fd, 1, deadline, result);
        if (error != MB_OK) {
            goto done;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) < 0) {
            result->system_error = errno;
            error = MB_ERR_CONNECT;
            goto done;
        }
        if (socket_error != 0) {
            result->system_error = socket_error;
            error = MB_ERR_CONNECT;
            goto done;
        }
    }
    error = send_exact(fd, request, sizeof(request), deadline, result);
    if (error != MB_OK) {
        goto done;
    }
    error = receive_exact(fd, response, 7U, deadline, result);
    if (error != MB_OK) {
        goto done;
    }
    error = check_header(response, tid, unit);
    if (error != MB_OK) {
        goto done;
    }
    length = 6U + read_be16(response + 4);
    error = receive_exact(fd, response + 7, length - 7U, deadline, result);
    if (error != MB_OK) {
        goto done;
    }
    error = mb_decode_fc03_response(response, length, tid, unit, qty, out, result);
done:
    if (fd >= 0) {
        (void)close(fd);
    }
    elapsed = mb_monotonic_ms() - started;
    result->elapsed_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    result->error = error;
    return error;
}
