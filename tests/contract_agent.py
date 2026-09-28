#!/usr/bin/env python3
"""One Reticulum endpoint for the RTNode contract tests (bench_contracts.py).

Joins the network through exactly one medium and takes JSON commands on stdin,
one per line, answering with JSON events on stdout:

    tcp     RTNode's local TCP server (LAN)
    prnsd   a prnsd on this machine, which reaches RTNode over Bluetooth (LAN)
    lora    an RNode on a serial port, which reaches RTNode over LoRa (LAN)
    wan     a private backbone rnsd that RTNode's backbone slot connects to

It owns two destinations, rtnodebench.contract.<name> (announced on command)
and rtnodebench.contract.<name>.silent (never announced, so the only way to
find it is a path request answered by this agent). Both accept links, echo
every packet sent over a link, and accept resources, reporting their size and
SHA-256.

Commands: announce; path {dest, timeout}; link {dest, aspects, timeout};
echo {dest, n, size, timeout}; resource {dest, size, timeout}; send {dest,
aspects, n} (single packets, no link); close {dest}; quit.
Events: ready, heard (every announce heard), path, link, link_in, echo,
resource, resource_in, packet_in, error.
"""
import argparse
import hashlib
import json
import os
import statistics
import sys
import tempfile
import threading
import time

import RNS

APP = "rtnodebench"


def emit(**event):
    event.setdefault("t", round(time.time(), 3))
    sys.stdout.write(json.dumps(event) + "\n")
    sys.stdout.flush()


def interface_config(args):
    if args.medium in ("tcp", "prnsd", "wan"):
        return ("  [[{m}]]\n    type = TCPClientInterface\n    enabled = yes\n    ingress_control = No\n"
                "    target_host = {h}\n    target_port = {p}\n").format(m=args.medium, h=args.host, p=args.port)
    if args.medium == "lora":
        return ("  [[lora]]\n    type = RNodeInterface\n    enabled = yes\n    ingress_control = No\n"
                f"    port = {args.rnode_port}\n    frequency = {args.frequency}\n"
                f"    bandwidth = {args.bandwidth}\n    txpower = {args.txpower}\n"
                f"    spreadingfactor = {args.spreadingfactor}\n    codingrate = {args.codingrate}\n")
    raise SystemExit(f"unknown medium {args.medium}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", required=True)
    parser.add_argument("--medium", required=True, choices=["tcp", "prnsd", "lora", "wan"])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--rnode-port")
    parser.add_argument("--frequency", type=int, default=914875000)
    parser.add_argument("--bandwidth", type=int, default=125000)
    parser.add_argument("--txpower", type=int, default=2)
    parser.add_argument("--spreadingfactor", type=int, default=10)
    parser.add_argument("--codingrate", type=int, default=5)
    parser.add_argument("--transport", action="store_true", help="enable_transport (a WAN backbone stand-in)")
    args = parser.parse_args()

    work = tempfile.mkdtemp(prefix=f"contract-{args.name}-")
    with open(os.path.join(work, "config"), "w") as f:
        f.write("[reticulum]\n  enable_transport = {}\n  share_instance = No\n  panic_on_interface_error = No\n"
                "[logging]\n  loglevel = 2\n[interfaces]\n".format("Yes" if args.transport else "No")
                + interface_config(args))
    RNS.Reticulum(work)
    identity = RNS.Identity()

    links_out = {}     # dest hex -> Link we opened
    echoes = {}        # dest hex -> list of (time, data) echoed back to us

    def on_link(link, which):
        emit(event="link_in", dest=which)
        link.set_packet_callback(lambda data, packet: RNS.Packet(link, data).send())
        link.set_resource_strategy(RNS.Link.ACCEPT_ALL)

        def concluded(resource):
            if resource.status == RNS.Resource.COMPLETE:
                data = resource.data.read()
                emit(event="resource_in", dest=which, size=len(data), sha=hashlib.sha256(data).hexdigest())
            else:
                emit(event="resource_in", dest=which, failed=True, status=resource.status)

        link.set_resource_concluded_callback(concluded)

    destinations = {}
    for suffix in ("", "silent"):
        aspects = ["contract", args.name] + ([suffix] if suffix else [])
        d = RNS.Destination(identity, RNS.Destination.IN, RNS.Destination.SINGLE, APP, *aspects)
        which = d.hash.hex()
        d.set_link_established_callback(lambda link, which=which: on_link(link, which))
        d.set_packet_callback(lambda data, packet, which=which: emit(event="packet_in", dest=which, size=len(data)))
        d.set_proof_strategy(RNS.Destination.PROVE_ALL)
        destinations[suffix or "main"] = d

    class Heard:
        aspect_filter = None
        receive_path_responses = True

        def received_announce(self, destination_hash, announced_identity, app_data):
            emit(event="heard", dest=destination_hash.hex(), hops=RNS.Transport.hops_to(destination_hash))

    RNS.Transport.register_announce_handler(Heard())
    time.sleep(2)  # the interface coming up
    emit(event="ready", name=args.name, medium=args.medium, dest=destinations["main"].hash.hex(),
         silent=destinations["silent"].hash.hex())

    def destination_for(dest_hex, aspects):
        dest = bytes.fromhex(dest_hex)
        known = RNS.Identity.recall(dest)
        if not known:
            return None
        return RNS.Destination(known, RNS.Destination.OUT, RNS.Destination.SINGLE, APP, *aspects.split("."))

    def run(command):
        cmd = command.get("cmd")
        if cmd == "announce":
            destinations["main"].announce()
            emit(event="announced", dest=destinations["main"].hash.hex())
        elif cmd == "path":
            dest = bytes.fromhex(command["dest"])
            start = time.time()
            RNS.Transport.request_path(dest)
            deadline = start + command.get("timeout", 15)
            while time.time() < deadline and not RNS.Transport.has_path(dest):
                time.sleep(0.05)
            ok = RNS.Transport.has_path(dest)
            emit(event="path", dest=command["dest"], ok=ok, s=round(time.time() - start, 2),
                 hops=RNS.Transport.hops_to(dest) if ok else None)
        elif cmd == "link":
            out = destination_for(command["dest"], command["aspects"])
            if not out:
                emit(event="link", dest=command["dest"], ok=False, reason="identity unknown")
                return
            start = time.time()
            link = RNS.Link(out)
            deadline = start + command.get("timeout", 20)
            while time.time() < deadline and link.status not in (RNS.Link.ACTIVE, RNS.Link.CLOSED):
                time.sleep(0.05)
            ok = link.status == RNS.Link.ACTIVE
            if ok:
                echoes[command["dest"]] = []
                link.set_packet_callback(lambda data, packet, d=command["dest"]: echoes[d].append((time.time(), data)))
                links_out[command["dest"]] = link
                # The far end completes the link on the RTT packet sent at
                # activation; give it that moment before any data goes.
                time.sleep(0.5)
            emit(event="link", dest=command["dest"], ok=ok, s=round(time.time() - start, 2),
                 mtu=getattr(link, "mtu", None), mdu=getattr(link, "mdu", None))
        elif cmd == "echo":
            link = links_out.get(command["dest"])
            if not link:
                emit(event="echo", dest=command["dest"], ok=0, n=0, reason="no link")
                return
            size = min(command.get("size", 200), getattr(link, "mdu", RNS.Link.MDU) or RNS.Link.MDU)
            rtts = []
            for n in range(command.get("n", 5)):
                payload = n.to_bytes(2, "big") + os.urandom(size - 2)
                sent = time.time()
                RNS.Packet(link, payload).send()
                deadline = sent + command.get("timeout", 10)
                got = None
                while time.time() < deadline and got is None:
                    got = next((t for t, d in echoes[command["dest"]] if d == payload), None)
                    time.sleep(0.02)
                if got:
                    rtts.append(got - sent)
            emit(event="echo", dest=command["dest"], ok=len(rtts), n=command.get("n", 5), size=size,
                 rtt_p50_ms=round(1000 * statistics.median(rtts)) if rtts else None,
                 rtt_max_ms=round(1000 * max(rtts)) if rtts else None)
        elif cmd == "resource":
            link = links_out.get(command["dest"])
            if not link:
                emit(event="resource", dest=command["dest"], ok=False, reason="no link")
                return
            data = os.urandom(command.get("size", 10000))
            done = threading.Event()
            outcome = {}

            def concluded(resource):
                outcome["status"] = resource.status
                done.set()

            start = time.time()
            RNS.Resource(data, link, callback=concluded)
            done.wait(command.get("timeout", 120))
            emit(event="resource", dest=command["dest"], ok=outcome.get("status") == RNS.Resource.COMPLETE,
                 size=len(data), sha=hashlib.sha256(data).hexdigest(), s=round(time.time() - start, 2),
                 status=outcome.get("status"))
        elif cmd == "send":
            out = destination_for(command["dest"], command["aspects"])
            if not out:
                emit(event="send", dest=command["dest"], ok=0, reason="identity unknown")
                return
            proved = []
            for n in range(command.get("n", 1)):
                receipt = RNS.Packet(out, os.urandom(command.get("size", 64))).send()
                if receipt:
                    receipt.set_delivery_callback(lambda r: proved.append(r))
            time.sleep(command.get("wait", 10))
            emit(event="send", dest=command["dest"], n=command.get("n", 1), proved=len(proved))
        elif cmd == "close":
            link = links_out.pop(command["dest"], None)
            if link:
                link.teardown()
            emit(event="closed", dest=command["dest"])
        elif cmd == "quit":
            emit(event="bye")
            os._exit(0)
        else:
            emit(event="error", reason=f"unknown command {cmd}")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            command = json.loads(line)
        except ValueError:
            emit(event="error", reason="not JSON")
            continue
        # Commands run on their own thread so events keep flowing while one waits.
        threading.Thread(target=run, args=(command,), daemon=True).start()
    os._exit(0)


if __name__ == "__main__":
    main()
