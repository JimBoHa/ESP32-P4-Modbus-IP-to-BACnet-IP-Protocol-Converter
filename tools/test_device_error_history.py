#!/usr/bin/env python3
"""Opt-in physical error-history test; temporarily reconfigures/reboots a gateway.

Only ordinary reads reach the source. A local proxy changes one response's
transaction ID after receiving it. The original gateway settings are restored
in finally. Output contains site addresses/read data: keep it outside Git.
"""
import argparse
import asyncio
import json
import time
from pathlib import Path

import ota_client


async def run(args):
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    token = args.token_file.read_text().strip()
    report = {"passed": False, "injected": None, "forwarded_reads": 0}
    original = None
    changed = False
    armed = False
    trial_passed = False

    def request(path, payload=None):
        connection = ota_client._connection(args.host, 443, args.cert, 8)
        try:
            headers = {"Accept": "application/json"}
            body = None
            if payload is not None:
                headers.update({"Authorization": f"Bearer {token}",
                                "X-Gateway-Request": "1", "Content-Type": "application/json"})
                body = json.dumps(payload)
            connection.request("POST" if payload is not None else "GET", path, body, headers)
            response = connection.getresponse()
            content = json.loads(ota_client._read_response(response))
            if response.status != 200:
                raise RuntimeError(f"{path}: HTTP {response.status}: {content}")
            return content
        finally:
            connection.close()

    async def api(path, payload=None):
        return await asyncio.to_thread(request, path, payload)

    async def until(path, predicate, timeout=90):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                last = await api(path)
                if predicate(last):
                    return last
            except (OSError, ValueError, RuntimeError):
                pass
            await asyncio.sleep(1)
        raise TimeoutError(f"{path} condition not met: {last}")

    def settings(config):
        names = ("profile", "modbus_host", "modbus_port", "modbus_unit", "device_instance",
                 "device_name", "bacnet_port", "expected_firmware", "expected_mac_fragment", "revision")
        return {key: config[key] for key in names}

    async def proxy(reader, writer):
        nonlocal armed
        remote = None
        try:
            if writer.get_extra_info("peername")[0] != args.host:
                return
            wire = await asyncio.wait_for(reader.readexactly(12), 2)
            assert wire[2:6] == b"\x00\x00\x00\x06" and wire[7] == 3
            assert wire[6] == original["modbus_unit"]
            assert 1 <= int.from_bytes(wire[10:12], "big") <= 125
            rr, remote = await asyncio.wait_for(asyncio.open_connection(
                original["modbus_host"], original["modbus_port"]), 2)
            remote.write(wire)
            await remote.drain()
            header = await asyncio.wait_for(rr.readexactly(7), 2)
            length = int.from_bytes(header[4:6], "big")
            assert 2 <= length <= 254
            response = header + await asyncio.wait_for(rr.readexactly(length - 1), 2)
            report["forwarded_reads"] += 1
            if armed:
                armed = False
                corrupt = bytes([response[0] ^ 0x80]) + response[1:]
                report["injected"] = {"request_hex": wire.hex(), "original_response_hex": response.hex(),
                                      "sent_response_hex": corrupt.hex(), "utc_ms": int(time.time() * 1000),
                                      "offset": int.from_bytes(wire[8:10], "big"),
                                      "quantity": int.from_bytes(wire[10:12], "big"),
                                      "transaction_id": int.from_bytes(wire[:2], "big")}
                response = corrupt
            writer.write(response)
            await writer.drain()
        except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, AssertionError) as exc:
            report.setdefault("proxy_errors", []).append(type(exc).__name__ + ": " + str(exc))
        finally:
            writer.close()
            if remote:
                remote.close()

    server = await asyncio.start_server(proxy, args.proxy_bind, args.proxy_port)
    try:
        original = await api("/api/config")
        assert original["profile"] != "custom", "This test requires an existing ATS preset"
        assert not original["config_error"]
        report["original_config"] = original
        backup = args.output / "original-config.json"
        backup.write_text(json.dumps(original, indent=2) + "\n")
        backup.chmod(0o600)
        report["before"] = await api("/api/errors")
        assert not report["before"]["persistence_error"]
        trial = settings(original)
        trial.update(modbus_host=args.proxy_bind, modbus_port=args.proxy_port)
        changed = True  # Restore even if a successful POST loses its response.
        await api("/api/config", trial)
        print("Proxy target saved; waiting for gateway restart and healthy real reads.", flush=True)
        healthy = await until("/api/status", lambda s: s["modbus_host"] == args.proxy_bind
                              and s["profile_verified"] and s["good_points"] >= 24
                              and s.get("clock_synchronized"))
        baseline = await api("/api/errors")
        assert baseline["total_errors"] == report["before"]["total_errors"], "Unexpected startup failure"
        armed = True
        logged = await until("/api/errors", lambda h: h["total_errors"] > baseline["total_errors"])
        assert report["injected"], "Gateway failure was not the injected transaction"
        assert logged["total_errors"] == baseline["total_errors"] + 1
        event = logged["events"][0]
        expected = report["injected"]
        assert event["host"] == args.proxy_bind and event["port"] == args.proxy_port
        assert event["function"] == 3 and event["unit"] == original["modbus_unit"]
        for field in ("offset", "quantity", "transaction_id"):
            assert event[field] == expected[field], (field, event, expected)
        assert event["error_code"] == 6, event  # MB_ERR_TRANSACTION
        assert event["utc_ms"] and abs(event["utc_ms"] - expected["utc_ms"]) < 5000
        assert event["exception_code"] == 0
        report["event"] = event
        print("One bad transaction ID captured with correct target/register/time; waiting for recovery and flash save.", flush=True)
        recovered = await until("/api/status", lambda s: s["modbus_successful_responses"]
                                > healthy["modbus_successful_responses"] + 10 and s["good_points"] >= 24)
        persisted = await until("/api/errors", lambda h: not h["pending_persistence"]
                                and not h["persistence_error"])
        assert persisted["events"][0] == event
        assert persisted["total_errors"] == baseline["total_errors"] + 1
        report["recovered_status"] = recovered
        report["persisted"] = persisted
        trial_passed = True
    finally:
        armed = False
        try:
            if changed and original:
                print("Restoring original source; verifying restart and retained error.", flush=True)
                # A save may have committed even if its HTTP response was lost.
                # Retry across the pending-restart 409 window, always using the
                # current revision, and verify the post-reboot configuration.
                deadline = time.monotonic() + 120
                expected_revision = None
                final_config = None
                while time.monotonic() < deadline:
                    try:
                        current = await api("/api/config")
                        matches = all(current[k] == v for k, v in settings(original).items()
                                      if k != "revision")
                        if expected_revision and current["revision"] >= expected_revision and matches:
                            final_config = current
                            break
                        restored = settings(original)
                        restored["revision"] = current["revision"]
                        expected_revision = current["revision"] + 1
                        await api("/api/config", restored)
                    except (OSError, ValueError, RuntimeError):
                        pass
                    await asyncio.sleep(2)
                if final_config is None:
                    raise RuntimeError("Original gateway settings could not be restored; use original-config.json")
                for key, value in settings(original).items():
                    if key != "revision":
                        assert final_config[key] == value, key
                report["restored_config"] = final_config
                report["final_status"] = await until("/api/status", lambda s: s["profile_verified"]
                    and s["good_points"] >= 24 and s["modbus_host"] == original["modbus_host"])
                history = await api("/api/errors")
                report["after_reboot"] = history
                if trial_passed:
                    assert history["events"][0] == report["event"]
                    assert history["boot_id"] > report["event"]["boot_id"]
                    assert history["total_errors"] == report["persisted"]["total_errors"]
                    assert not report.get("proxy_errors"), report["proxy_errors"]
                    report["passed"] = True
        finally:
            server.close()
            await server.wait_closed()
            path = args.output / "result.json"
            path.write_text(json.dumps(report, indent=2) + "\n")
            path.chmod(0o600)
    assert report["passed"]
    print("PASS: exact error details, UTC, successful-read retention, flash persistence, reboot retention; original settings restored.")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", required=True, help="Gateway numeric IPv4 address")
    p.add_argument("--cert", type=Path, required=True)
    p.add_argument("--token-file", type=Path, required=True)
    p.add_argument("--proxy-bind", required=True, help="This test computer's reachable numeric IPv4")
    p.add_argument("--proxy-port", type=int, default=15020)
    p.add_argument("--output", type=Path, required=True)
    asyncio.run(run(p.parse_args()))
