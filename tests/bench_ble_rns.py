#!/usr/bin/env python3
"""Reticulum through RTNode's Bluetooth interface, end to end.

A: a Reticulum instance here whose UDPInterface talks to the Bluetooth peer
   in relay mode (bench_ble.py --serve, running in Terminal), which speaks
   ble-reticulum v2.2 to the node as Columba does.
B: a Reticulum instance in a subprocess, on the node's local TCP server.

Checks, each within a time limit:
  1. B's announce reaches A (TCP -> node -> Bluetooth notifications).
  2. A's announce reaches B (Bluetooth writes -> node -> TCP).
  3. A opens a Link to B across the node, and echoes of near-MTU packets
     come back.

    ../.venv/bin/python tests/bench_ble_rns.py --host 192.168.2.125 [--chunk 100]

--chunk caps the relay's fragment payload, so A's packets reach the node in
several fragments. Needs a V4 bench build with Bluetooth on (the default)
and the peer running with --serve.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PEER_DIR = os.path.join(HERE, "bench-results", "ble-peer")
UDP_IN, UDP_OUT = 4250, 4251   # relay listens on UDP_IN, sends to UDP_OUT
ECHOES = 10

B_SCRIPT = r"""
import sys, time, RNS
RNS.Reticulum(sys.argv[1])
identity = RNS.Identity()
destination = RNS.Destination(identity, RNS.Destination.IN, RNS.Destination.SINGLE, "rtnodebench", "ble", "b")

def established(link):
    link.set_packet_callback(lambda data, packet: RNS.Packet(link, data).send())
    print("link", flush=True)

destination.set_link_established_callback(established)

class Heard:
    aspect_filter = "rtnodebench.ble.a"
    def received_announce(self, destination_hash, announced_identity, app_data):
        print("heard", destination_hash.hex(), RNS.Transport.hops_to(destination_hash), flush=True)

RNS.Transport.register_announce_handler(Heard())
time.sleep(3)  # the TCP connection
print("b", destination.hash.hex(), flush=True)
destination.announce()
while True:
    time.sleep(1)
"""


def config(directory, interface):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "config"), "w") as f:
        f.write("[reticulum]\n  enable_transport = No\n  share_instance = No\n"
                "[logging]\n  loglevel = 2\n[interfaces]\n" + interface)


def wait_for(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    return None


def peer_events(since):
    try:
        with open(os.path.join(PEER_DIR, "log.jsonl")) as f:
            return [json.loads(line) for line in f if line.strip() and json.loads(line)["t"] >= since]
    except OSError:
        return []


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--chunk", type=int, default=512, help="relay fragment cap (with header)")
    args = parser.parse_args()
    results = {}

    # The relay first: A's interface must be up before B announces. End any
    # session still running, so this one starts with this run's settings.
    stopping = time.time()
    with open(os.path.join(PEER_DIR, "control.json"), "w") as f:
        json.dump({"mode": "off"}, f)
    wait_for(lambda: any(e.get("event") in ("relay_down", "disconnected") for e in peer_events(stopping)), 10)
    started = time.time()
    with open(os.path.join(PEER_DIR, "control.json"), "w") as f:
        json.dump({"mode": "relay", "udp_in": UDP_IN, "udp_out": UDP_OUT, "chunk": args.chunk}, f)
    relay = wait_for(lambda: next((e for e in peer_events(started) if e.get("event") == "relay_up"), None), 60)
    if not relay:
        sys.exit("FAIL: the Bluetooth peer did not come up in relay mode (is bench_ble.py --serve running?)")
    print(f"relay up: node identity {relay['node_identity'][:8]}, MTU {relay['mtu']}, fragment payload {relay['chunk']}",
          flush=True)

    import RNS
    work = tempfile.mkdtemp(prefix="rtnode-ble-rns-")
    config(os.path.join(work, "a"), "  [[BLE relay]]\n    type = UDPInterface\n    enabled = yes\n"
           "    ingress_control = No\n    listen_ip = 127.0.0.1\n"
           f"    listen_port = {UDP_OUT}\n    forward_ip = 127.0.0.1\n    forward_port = {UDP_IN}\n")
    config(os.path.join(work, "b"), "  [[RTNode TCP]]\n    type = TCPClientInterface\n    enabled = yes\n"
           f"    ingress_control = No\n    target_host = {args.host}\n    target_port = {args.port}\n")
    RNS.Reticulum(os.path.join(work, "a"))
    a = RNS.Destination(RNS.Identity(), RNS.Destination.IN, RNS.Destination.SINGLE, "rtnodebench", "ble", "a")

    heard_b = {}

    class HeardB:
        aspect_filter = "rtnodebench.ble.b"

        def received_announce(self, destination_hash, announced_identity, app_data):
            heard_b.setdefault("at", time.time())
            heard_b["hash"] = destination_hash

    RNS.Transport.register_announce_handler(HeardB())

    b_lines = []
    b = subprocess.Popen([sys.executable, "-c", B_SCRIPT, os.path.join(work, "b")],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    threading.Thread(target=lambda: [b_lines.append((time.time(), line.split())) for line in b.stdout],
                     daemon=True).start()
    failures = []
    try:
        b_hash = wait_for(lambda: next((w[1] for _, w in b_lines if w and w[0] == "b"), None), 30)
        if not b_hash:
            sys.exit("FAIL: B did not start")
        b_announced = next(t for t, w in b_lines if w and w[0] == "b")

        # 1. B's announce over Bluetooth to A.
        if wait_for(lambda: heard_b.get("at"), 20):
            results["b_to_a_s"] = round(heard_b["at"] - b_announced, 2)
            print(f"1. B's announce reached A over Bluetooth in {results['b_to_a_s']} s, "
                  f"{RNS.Transport.hops_to(heard_b['hash'])} hops", flush=True)
        else:
            failures.append("B's announce never reached A over Bluetooth")

        # 2. A's announce over Bluetooth to B.
        a_announced = time.time()
        a.announce()
        heard_a = wait_for(lambda: next(((t, w) for t, w in b_lines if w and w[0] == "heard" and w[1] == a.hash.hex()),
                                        None), 20)
        if heard_a:
            results["a_to_b_s"] = round(heard_a[0] - a_announced, 2)
            print(f"2. A's announce reached B in {results['a_to_b_s']} s, {heard_a[1][2]} hops", flush=True)
        else:
            failures.append("A's announce never reached B")

        # 3. A Link across the node, and echoes of near-MTU packets.
        if "hash" in heard_b:
            remote = RNS.Destination(RNS.Identity.recall(heard_b["hash"]), RNS.Destination.OUT,
                                     RNS.Destination.SINGLE, "rtnodebench", "ble", "b")
            link_started = time.time()
            link = RNS.Link(remote)
            # The initiator marks a link active before it sends the RTT packet
            # that completes it on B's side; data sent in between is dropped
            # by B, so wait for B to report the link.
            if wait_for(lambda: link.status == RNS.Link.ACTIVE and
                        any(w and w[0] == "link" for _, w in b_lines), 20):
                results["link_s"] = round(time.time() - link_started, 2)
                echoed = []
                link.set_packet_callback(lambda data, packet: echoed.append((time.time(), data)))
                rtts = []
                for n in range(ECHOES):
                    payload = n.to_bytes(2, "big") + os.urandom(RNS.Link.MDU - 2)
                    sent = time.time()
                    RNS.Packet(link, payload).send()
                    got = wait_for(lambda: next((t for t, d in echoed if d == payload), None), 10)
                    if got:
                        rtts.append(got - sent)
                results["echoes"] = f"{len(rtts)}/{ECHOES}"
                results["echo_bytes"] = RNS.Link.MDU
                if rtts:
                    results["echo_rtt_ms"] = {"p50": round(1000 * statistics.median(rtts)),
                                              "max": round(1000 * max(rtts))}
                print(f"3. Link up in {results['link_s']} s; {results['echoes']} echoes of {RNS.Link.MDU} bytes, "
                      f"RTT {results.get('echo_rtt_ms')}", flush=True)
                if len(rtts) < ECHOES:
                    failures.append(f"only {len(rtts)} of {ECHOES} echoes came back")
                link.teardown()
            else:
                failures.append("the Link to B never became active")
    finally:
        b.kill()

    relay_down = peer_events(started)
    stats = [e for e in relay_down if e.get("event") == "relay"]
    if stats:
        print(f"relay: {stats[-1]}", flush=True)
    for failure in failures:
        print("FAIL: " + failure, flush=True)
    if not failures:
        print("PASS: announces both ways and a Link across RTNode's Bluetooth interface", flush=True)
    os._exit(1 if failures else 0)


if __name__ == "__main__":
    main()
