"""Compile the C client and test TCP behavior against 127.0.0.1 only.

Run: python3 -m unittest discover -s tests -p 'test_modbus_loopback.py' -v
Requires a C99 compiler; never contacts a LAN address or Modbus device.
"""

import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
REQUEST = bytes.fromhex("1234000000061103006b0003")
RESPONSE = bytes.fromhex("12340000000911030600018000ffff")
UNCHANGED = [0xA55A] * 50


class ModbusLoopbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executable = Path(cls.temp.name) / "test_modbus_tcp"
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c99",
                   "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                   "-I", str(ROOT / "main"), str(ROOT / "main/modbus_tcp.c"),
                   str(ROOT / "tests/test_modbus_tcp.c"), "-o", str(cls.executable)]
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)

    def invoke(self, port, timeout_ms=1200, function=3):
        completed = subprocess.run([str(self.executable), "--probe", str(port), str(timeout_ms), str(function)],
                                   check=True, capture_output=True, text=True, timeout=4)
        return json.loads(completed.stdout)

    def exchange(self, handler, timeout_ms=1200, function=3):
        errors = []
        requests = []
        stop = threading.Event()
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(3)
        port = listener.getsockname()[1]

        def serve():
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(2)
                    request = bytearray()
                    while len(request) < 12:
                        chunk = connection.recv(12 - len(request))
                        if not chunk:
                            raise AssertionError("Client closed before sending its full request")
                        request.extend(chunk)
                    requests.append(bytes(request))
                    handler(connection, stop)
            except (BrokenPipeError, ConnectionResetError):
                # Expected when a malformed frame/deadline makes the client close.
                pass
            except Exception as error:
                errors.append(error)

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            result = self.invoke(port, timeout_ms, function)
        finally:
            stop.set()
            listener.close()
            thread.join(4)
        self.assertFalse(thread.is_alive(), "Loopback server did not finish")
        if errors:
            raise errors[0]
        expected_request = bytearray(REQUEST)
        expected_request[7] = function
        self.assertEqual(requests, [bytes(expected_request)])
        return result

    def assert_failure(self, result, error, exception=0):
        self.assertEqual(result["error"], error)
        self.assertEqual(result["values"], UNCHANGED)
        self.assertEqual(result["exception"], exception)

    def test_pure_packet_suite(self):
        completed = subprocess.run([str(self.executable)], check=True,
                                   capture_output=True, text=True, timeout=3)
        self.assertIn("packet tests passed", completed.stdout)

    def test_valid_response_fragmented_into_single_bytes(self):
        def fragmented(connection, stop):
            for byte in RESPONSE:
                connection.sendall(bytes([byte]))
                if stop.wait(0.004):
                    break
        result = self.exchange(fragmented)
        self.assertEqual(result["error"], "ok")
        self.assertEqual(result["values"][:3], [1, 32768, 65535])
        self.assertEqual(result["values"][3:], UNCHANGED[3:])

    def test_new_read_functions_and_matching_exceptions(self):
        for function in (1, 2, 4):
            with self.subTest(function=function):
                frame = bytearray(RESPONSE)
                frame[7] = function
                if function <= 2:
                    frame = bytearray.fromhex("12340000000411010105")
                    frame[7] = function
                result = self.exchange(lambda connection, stop: connection.sendall(frame),
                                       function=function)
                self.assertEqual(result["error"], "ok")
                self.assertEqual(result["values"][:3], [1, 0, 1] if function <= 2 else [1, 32768, 65535])
                self.assertEqual(result["values"][3:], UNCHANGED[3:])
                exception = bytearray.fromhex("123400000003118302")
                exception[7] = function | 0x80
                result = self.exchange(lambda connection, stop: connection.sendall(exception),
                                       function=function)
                self.assert_failure(result, "exception", exception=2)

    def test_bit_response_wrong_byte_count_is_rejected(self):
        frame = bytes.fromhex("1234000000051101020500")
        result = self.exchange(lambda connection, stop: connection.sendall(frame), function=1)
        self.assert_failure(result, "byte_count")

    def test_invalid_headers_and_pdus_do_not_update_registers(self):
        mutations = ((0, 0x13, "transaction"), (2, 1, "protocol"),
                     (6, 0x12, "unit"), (5, 2, "length"),
                     (5, 104, "length"), (7, 4, "function"),
                     (8, 4, "byte_count"), (8, 7, "byte_count"))
        for index, value, error in mutations:
            with self.subTest(error=error, value=value):
                frame = bytearray(RESPONSE)
                frame[index] = value
                result = self.exchange(lambda connection, stop: connection.sendall(frame))
                self.assert_failure(result, error)

    def test_header_rejected_without_waiting_for_claimed_payload(self):
        header = bytes.fromhex("12340000ffff11")
        def invalid_length(connection, stop):
            connection.sendall(header)
            stop.wait(2)
        result = self.exchange(invalid_length)
        self.assert_failure(result, "length")
        self.assertLess(result["elapsed_ms"], 800)

    def test_exception_response(self):
        frame = bytes.fromhex("123400000003118302")
        result = self.exchange(lambda connection, stop: connection.sendall(frame))
        self.assert_failure(result, "exception", exception=2)

    def test_connection_closes_mid_header_or_payload(self):
        for length in (0, 3, 7, len(RESPONSE) - 1):
            with self.subTest(length=length):
                result = self.exchange(lambda connection, stop: connection.sendall(RESPONSE[:length]))
                self.assert_failure(result, "truncated")

    def test_silent_peer_times_out(self):
        result = self.exchange(lambda connection, stop: stop.wait(2), timeout_ms=250)
        self.assert_failure(result, "timeout")
        self.assertGreaterEqual(result["elapsed_ms"], 230)
        self.assertLess(result["elapsed_ms"], 800)

    def test_drip_fed_fragments_cannot_reset_absolute_deadline(self):
        def drip(connection, stop):
            for byte in RESPONSE:
                connection.sendall(bytes([byte]))
                if stop.wait(0.08):
                    break
        before = time.monotonic()
        result = self.exchange(drip, timeout_ms=250)
        elapsed = time.monotonic() - before
        self.assert_failure(result, "timeout")
        self.assertGreaterEqual(result["elapsed_ms"], 230)
        self.assertLess(result["elapsed_ms"], 800)
        self.assertLess(elapsed, 1.0)

    def test_header_and_payload_share_one_deadline(self):
        def delayed_parts(connection, stop):
            if stop.wait(0.16):
                return
            connection.sendall(RESPONSE[:7])
            if stop.wait(0.16):
                return
            connection.sendall(RESPONSE[7:])
        result = self.exchange(delayed_parts, timeout_ms=250)
        self.assert_failure(result, "timeout")
        self.assertLess(result["elapsed_ms"], 800)

    def test_connection_refused_reports_connect_error(self):
        # A bound-but-not-listening socket drops SYNs on some hosts; close the
        # temporary reservation so the local TCP stack sends connection refused.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]
        result = self.invoke(port)
        self.assert_failure(result, "connect")
        self.assertNotEqual(result["system_error"], 0)


if __name__ == "__main__":
    unittest.main()
