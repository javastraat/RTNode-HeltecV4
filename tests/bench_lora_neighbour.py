#!/usr/bin/env python3
"""Does RTNode repeat traffic between two LoRa devices that hear each other?

Reticulum addresses every hop but the last: a packet in transport carries the
next hop's transport id, and only that node relays it. On the last hop the
destination is in range, so the packet goes out with no next-hop field
(HEADER_1) and nobody should relay it. This plays both LoRa neighbours with
one RNode:

1. The RNode joins as a Reticulum endpoint (contract_agent.py) and
   announces, so RTNode learns that destination one LoRa hop away.
2. The same RNode, driven raw over KISS, then sends what a neighbour next to
   it would: a data packet for that destination with no next-hop field, a
   link request, and a packet on that link — and listens for RTNode sending
   any of them again.

    ../.venv/bin/python tests/bench_lora_neighbour.py --node-log <serial log>

PASS: RTNode repeats none of them (the destination heard them directly).
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from kiss_serial import (KissSerial, CMD_RADIO_STATE, CMD_FREQUENCY, CMD_BANDWIDTH, CMD_TXPOWER,  # noqa: E402
                         CMD_SF, CMD_CR, CMD_IMPLICIT, RADIO_STATE_ON, RADIO_STATE_OFF)

PYTHON = os.path.abspath(os.path.join(HERE, "..", "..", ".venv", "bin", "python"))
HEADER_1, DATA, LINKREQUEST = 0, 0, 2
SINGLE, LINK = 0, 3


def flags(packet_type, destination_type):
    return (HEADER_1 << 6) | (destination_type << 2) | packet_type


def learn_destination(args):
    """The RNode as a Reticulum endpoint: announce, so RTNode learns it."""
    agent = subprocess.Popen([PYTHON, os.path.join(HERE, "contract_agent.py"), "--name", f"nb{int(time.time()) % 10000}",
                              "--medium", "lora", "--rnode-port", args.rnode_port, "--txpower", str(args.txpower)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    events = []
    threading.Thread(target=lambda: [events.append(json.loads(l)) for l in agent.stdout if l.startswith("{")],
                     daemon=True).start()
    deadline = time.time() + 60
    while time.time() < deadline and not any(e["event"] == "ready" for e in events):
        time.sleep(0.1)
    ready = next((e for e in events if e["event"] == "ready"), None)
    if not ready:
        agent.kill()
        sys.exit("the LoRa agent did not start")
    agent.stdin.write(json.dumps({"cmd": "announce"}) + "\n")
    agent.stdin.flush()
    time.sleep(args.learn_s)  # the announce's airtime, and RTNode's rebroadcast
    agent.stdin.write(json.dumps({"cmd": "quit"}) + "\n")
    agent.stdin.flush()
    try:
        agent.wait(timeout=10)
    except subprocess.TimeoutExpired:
        agent.kill()
    time.sleep(2)  # the serial port closing
    return bytes.fromhex(ready["dest"])


def open_rnode(args):
    ks = KissSerial(port=args.rnode_port, baud=115200)
    ks.start()
    time.sleep(1.5)
    ks._send_frame(CMD_RADIO_STATE, bytes([RADIO_STATE_OFF]))
    time.sleep(0.2)
    for cmd, payload in ((CMD_FREQUENCY, struct.pack(">I", args.frequency)),
                         (CMD_BANDWIDTH, struct.pack(">I", args.bandwidth)),
                         (CMD_TXPOWER, bytes([args.txpower])), (CMD_SF, bytes([args.sf])),
                         (CMD_CR, bytes([args.cr])), (CMD_IMPLICIT, bytes([0]))):
        ks._send_frame(cmd, payload)
        time.sleep(0.05)
    ks._send_frame(CMD_RADIO_STATE, bytes([RADIO_STATE_ON]))
    time.sleep(1.0)
    return ks


def send_and_listen(ks, raw, listen_s):
    """Sends raw on air and returns any packet heard back whose destination
    and payload match it (RTNode's repeat carries its own flags and hops)."""
    since = time.time()
    ks.send_packet(raw)
    time.sleep(listen_s)
    return [data for data, _rssi, t in ks.received_packets if t >= since and data[2:] == raw[2:]]


def node_lines(path, since, needle):
    lines = []
    try:
        with open(path, errors="replace") as f:
            for line in f:
                stamp, _, text = line.partition(" ")
                try:
                    if float(stamp) >= since and needle in text:
                        lines.append(text.strip())
                except ValueError:
                    pass
    except OSError:
        pass
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rnode-port", default="/dev/cu.usbserial-0001")
    parser.add_argument("--frequency", type=int, default=914875000)
    parser.add_argument("--bandwidth", type=int, default=125000)
    parser.add_argument("--sf", type=int, default=10)
    parser.add_argument("--cr", type=int, default=5)
    parser.add_argument("--txpower", type=int, default=2)
    parser.add_argument("--learn-s", type=float, default=15)
    parser.add_argument("--listen-s", type=float, default=8)
    parser.add_argument("--node-log")
    args = parser.parse_args()

    started = time.time()
    destination = learn_destination(args)
    print(f"RTNode has heard destination {destination.hex()[:8]} one LoRa hop away", flush=True)

    ks = open_rnode(args)
    failures = []
    try:
        # 1. A data packet for the neighbour, no next-hop field.
        data = bytes([flags(DATA, SINGLE), 0]) + destination + b"\x00" + os.urandom(64)
        repeats = send_and_listen(ks, data, args.listen_s)
        print(f"1. data packet, no next hop: RTNode sent it again {len(repeats)} time(s)", flush=True)
        if repeats:
            failures.append("RTNode repeated a data packet between direct neighbours")

        # 2. A link request for the neighbour.
        request = bytes([flags(LINKREQUEST, SINGLE), 0]) + destination + b"\x00" + os.urandom(64)
        link_id = hashlib.sha256(bytes([request[0] & 0x0F]) + request[2:]).digest()[:16]
        repeats = send_and_listen(ks, request, args.listen_s)
        print(f"2. link request, no next hop: RTNode sent it again {len(repeats)} time(s)", flush=True)
        if repeats:
            failures.append("RTNode repeated a link request between direct neighbours")

        # 3. A packet on that link.
        on_link = bytes([flags(DATA, LINK), 0]) + link_id + b"\x00" + os.urandom(48)
        repeats = send_and_listen(ks, on_link, args.listen_s)
        print(f"3. packet on link {link_id.hex()[:8]}: RTNode sent it again {len(repeats)} time(s)", flush=True)
        if repeats:
            failures.append("RTNode repeated link traffic between direct neighbours")
    finally:
        ks.stop()

    if args.node_log:
        for needle in (destination.hex()[:8], link_id.hex()[:8]):
            for line in node_lines(args.node_log, started, needle)[:6]:
                print("   node: " + line[:160], flush=True)
    for failure in failures:
        print("FAIL: " + failure, flush=True)
    if not failures:
        print("PASS: RTNode left traffic between direct LoRa neighbours alone", flush=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
