#!/usr/bin/env python3
"""Bluetooth LE spike peer (PERFORMANCE_STRATEGY.md, order of work step 6).

Finds the node by the Prns/Columba service it advertises, checks the
advertisement and the GATT table, then writes numbered frames to the data
characteristic with acknowledgement (as a Prns connecting side does) and
times the node's echo, which comes back as a notification. Run it alongside
bench_load.py to see what Bluetooth traffic costs WiFi.

    ../.venv/bin/python tests/bench_ble.py --rate 5 --size 180 --duration 120

With --serve it stays up as the bench's peer instead: it reconnects whenever
the node resets, takes what to do from a control file, and logs a record
every 10 s, so load tests can run while it does:

    ../.venv/bin/python tests/bench_ble.py --serve

The control file (default tests/bench-results/ble-peer/control.json) holds
{"mode": "echo", "rate": 5, "size": 180}, {"mode": "idle"} (connected, no
traffic) or {"mode": "off"} (disconnected); it is read every second. Records
go to tests/bench-results/ble-peer/log.jsonl.

Needs the spike build (env rtnode_heltec_v4_bench_ble) and Bluetooth access
for the app running it. macOS asks once in Terminal; it refuses without
asking for tools run from the Claude app.
"""
import argparse
import asyncio
import json
import os
import statistics
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE = "37145b00-442d-4a94-917f-8f42c5da28e3"
DATA = "37145b00-442d-4a94-917f-8f42c5da28e8"
# BleSpike.h, after Prns commit d48e9fc and Columba.
EXPECTED = {
    "37145b00-442d-4a94-917f-8f42c5da28e4": {"notify"},                           # Columba TX
    "37145b00-442d-4a94-917f-8f42c5da28e5": {"write", "write-without-response"},  # Columba RX
    "37145b00-442d-4a94-917f-8f42c5da28e6": {"read"},                             # Columba identity
    "37145b00-442d-4a94-917f-8f42c5da28e7": {"write", "notify"},                  # Prns control
    "37145b00-442d-4a94-917f-8f42c5da28e8": {"write", "notify"},                  # Prns data
}


def ms(values, fraction):
    if not values:
        return None
    values = sorted(values)
    return round(1000 * values[min(len(values) - 1, int(fraction * len(values)))], 1)


async def run(args):
    problems = []
    t0 = time.time()
    seen = {}

    def match(device, adv):
        if SERVICE in [u.lower() for u in adv.service_uuids]:
            seen["adv"] = adv
            return True
        return False

    device = await BleakScanner.find_device_by_filter(match, timeout=args.scan_timeout)
    if device is None:
        return {"error": f"no device advertising {SERVICE} within {args.scan_timeout} s"}
    found_s = time.time() - t0
    adv = seen["adv"]
    manufacturer = {f"{k:04x}": v.hex() for k, v in adv.manufacturer_data.items()}
    if manufacturer.get("ffff", "")[:2] != "03":
        problems.append(f"manufacturer data {manufacturer}, expected ffff 03 <flags>")

    t1 = time.time()
    async with BleakClient(device) as client:
        connect_s = time.time() - t1
        service = client.services.get_service(SERVICE)
        properties = {c.uuid.lower(): set(c.properties) for c in service.characteristics}
        for uuid, wanted in EXPECTED.items():
            if uuid not in properties:
                problems.append(f"characteristic {uuid[-4:]} missing")
            elif not wanted <= properties[uuid]:
                problems.append(f"characteristic {uuid[-4:]} has {sorted(properties[uuid])}, wanted {sorted(wanted)}")
        mtu = client.mtu_size

        sent, rtts, acks = {}, [], []
        write_errors = 0

        def on_notify(_, data):
            if len(data) >= 4:
                sent_at = sent.pop(struct.unpack(">I", bytes(data[:4]))[0], None)
                if sent_at is not None:
                    rtts.append(time.time() - sent_at)

        await client.start_notify(DATA, on_notify)
        frames = int(args.rate * args.duration)
        start = time.time()
        for seq in range(frames):
            delay = start + seq / args.rate - time.time()
            if delay > 0:
                await asyncio.sleep(delay)
            frame = struct.pack(">I", seq) + os.urandom(max(0, args.size - 4))
            sent[seq] = time.time()
            try:
                write_start = time.time()
                await client.write_gatt_char(DATA, frame, response=True)
                acks.append(time.time() - write_start)
            except Exception as error:  # counted, and the first one reported
                if write_errors == 0:
                    problems.append(f"write failed: {error}")
                write_errors += 1
        elapsed = time.time() - start
        await asyncio.sleep(2)  # the last echoes
        await client.stop_notify(DATA)

    return {
        "found_s": round(found_s, 1), "connect_s": round(connect_s, 1), "mtu": mtu,
        "manufacturer_data": manufacturer, "frames": frames, "size": args.size,
        "achieved_rate": round(frames / elapsed, 2) if elapsed else None,
        "echoed": len(rtts), "lost": frames - len(rtts) - write_errors, "write_errors": write_errors,
        "rtt_ms": {"p50": ms(rtts, 0.5), "p95": ms(rtts, 0.95), "max": ms(rtts, 1.0)},
        "ack_ms": {"p50": ms(acks, 0.5), "p95": ms(acks, 0.95), "max": ms(acks, 1.0)},
        "problems": problems,
    }


HERE = os.path.dirname(os.path.abspath(__file__))
PEER_DIR = os.path.join(HERE, "bench-results", "ble-peer")
DEFAULT_CONTROL = {"mode": "echo", "rate": 5.0, "size": 180}
REPORT_EVERY = 10.0
LOST_AFTER = 5.0  # an echo not back after this long counts as lost


def read_control(path):
    try:
        with open(path) as f:
            control = dict(DEFAULT_CONTROL, **json.load(f))
    except (OSError, ValueError):
        control = dict(DEFAULT_CONTROL)
    return control


async def serve(args):
    os.makedirs(os.path.dirname(args.log), exist_ok=True)
    log = open(args.log, "a")

    def emit(**record):
        record = {"t": round(time.time(), 3), **record}
        log.write(json.dumps(record) + "\n")
        log.flush()
        print(json.dumps(record), flush=True)

    emit(event="start", control=args.control)
    while True:
        control = read_control(args.control)
        if control["mode"] == "off":
            await asyncio.sleep(1)
            continue
        device = await BleakScanner.find_device_by_filter(
            lambda d, adv: SERVICE in [u.lower() for u in adv.service_uuids], timeout=10)
        if device is None:
            continue  # the node may be rebooting; look again
        sent, window = {}, {"sent": 0, "rtts": [], "acks": [], "lost": 0, "write_errors": 0}

        def on_notify(_, data):
            if len(data) >= 4:
                sent_at = sent.pop(struct.unpack(">I", bytes(data[:4]))[0], None)
                if sent_at is not None:
                    window["rtts"].append(time.time() - sent_at)

        try:
            async with BleakClient(device) as client:
                emit(event="connected", mtu=client.mtu_size)
                await client.start_notify(DATA, on_notify)
                seq, next_send, next_report = 0, time.time(), time.time() + REPORT_EVERY
                control_read_at = time.time()
                while client.is_connected:
                    if time.time() - control_read_at >= 1.0:
                        control, control_read_at = read_control(args.control), time.time()
                    if control["mode"] == "off":
                        break
                    now = time.time()
                    if control["mode"] == "echo" and now >= next_send:
                        frame = struct.pack(">I", seq) + os.urandom(max(0, int(control["size"]) - 4))
                        sent[seq] = now
                        seq += 1
                        try:
                            await client.write_gatt_char(DATA, frame, response=True)
                            window["acks"].append(time.time() - now)
                        except Exception as error:
                            if window["write_errors"] == 0:
                                emit(event="write_error", error=str(error))
                            window["write_errors"] += 1
                        window["sent"] += 1
                        next_send = max(next_send + 1.0 / float(control["rate"]), time.time() - 1.0)
                    else:
                        await asyncio.sleep(0.01)
                    if time.time() >= next_report:
                        expired = [n for n, at in sent.items() if time.time() - at > LOST_AFTER]
                        for n in expired:
                            del sent[n]
                        window["lost"] += len(expired)
                        emit(event="window", mode=control["mode"], rate=control.get("rate"),
                             size=control.get("size"), sent=window["sent"], echoed=len(window["rtts"]),
                             lost=window["lost"], write_errors=window["write_errors"],
                             rtt_ms={"p50": ms(window["rtts"], 0.5), "p95": ms(window["rtts"], 0.95),
                                     "max": ms(window["rtts"], 1.0)},
                             ack_ms={"p50": ms(window["acks"], 0.5), "p95": ms(window["acks"], 0.95)})
                        window = {"sent": 0, "rtts": [], "acks": [], "lost": 0, "write_errors": 0}
                        next_report += REPORT_EVERY
        except Exception as error:
            emit(event="error", error=str(error))
        emit(event="disconnected", unanswered=len(sent))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rate", type=float, default=5.0, help="frames per second")
    parser.add_argument("--size", type=int, default=180, help="bytes per frame (Prns sends 180)")
    parser.add_argument("--duration", type=float, default=120.0)
    parser.add_argument("--scan-timeout", type=float, default=30.0)
    parser.add_argument("--out", help="write the summary JSON here")
    parser.add_argument("--serve", action="store_true", help="stay up as the bench's peer (see above)")
    parser.add_argument("--control", default=os.path.join(PEER_DIR, "control.json"))
    parser.add_argument("--log", default=os.path.join(PEER_DIR, "log.jsonl"))
    args = parser.parse_args()

    if args.serve:
        try:
            asyncio.run(serve(args))
        except KeyboardInterrupt:
            pass
        return

    summary = asyncio.run(run(args))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(summary, f, indent=2)
    if "error" in summary:
        print("FAIL: " + summary["error"], flush=True)
        sys.exit(1)
    print(f"found in {summary['found_s']} s, connected in {summary['connect_s']} s, MTU {summary['mtu']}, "
          f"manufacturer data {summary['manufacturer_data']}")
    print(f"{summary['frames']} frames of {summary['size']} B at {summary['achieved_rate']}/s: "
          f"{summary['echoed']} echoed, {summary['lost']} lost, {summary['write_errors']} write errors")
    print(f"echo RTT p50 {summary['rtt_ms']['p50']} p95 {summary['rtt_ms']['p95']} max {summary['rtt_ms']['max']} ms; "
          f"write ack p50 {summary['ack_ms']['p50']} p95 {summary['ack_ms']['p95']} ms")
    for problem in summary["problems"]:
        print("PROBLEM: " + problem)
    sys.exit(1 if summary["problems"] or summary["lost"] or summary["write_errors"] else 0)


if __name__ == "__main__":
    main()
