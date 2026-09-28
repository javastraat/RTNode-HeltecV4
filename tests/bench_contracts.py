#!/usr/bin/env python3
"""RTNode's transport contracts, end to end (CORE_PRINCIPLES.md).

Starts one contract_agent.py per medium and checks, for every ordered pair:

  announce  A's announce reaches B (LAN announces cross every interface)
  path      A finds B's never-announced destination: the path request goes
            out through RTNode and B's answer comes back
  link      A opens a Link to B, echoes packets over it, and sends B a
            resource that B receives intact (SHA-256 checked)

LAN media: tcp (RTNode's local TCP server), prnsd (Prns Bluetooth, through a
prnsd on this Mac: tests/prnsd/config) and lora (an RNode on a serial port).

    ../.venv/bin/python tests/bench_contracts.py lan --host 192.168.2.125 \\
        --media tcp,prnsd,lora --rnode-port /dev/cu.usbserial-0001

LoRa pairs use smaller payloads (SF10/125 kHz is about 1 kbps). Results go to
tests/bench-results/<time>-contracts-<suite>/results.json.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PYTHON = os.path.abspath(os.path.join(HERE, "..", "..", ".venv", "bin", "python"))


class Agent:
    def __init__(self, name, medium, args, extra=()):
        self.name, self.medium = name, medium
        command = [PYTHON, os.path.join(HERE, "contract_agent.py"), "--name", name, "--medium", medium]
        if medium == "tcp":
            command += ["--host", args.host, "--port", str(args.port)]
        elif medium == "prnsd":
            command += ["--host", "127.0.0.1", "--port", str(args.prnsd_port)]
        elif medium == "lora":
            command += ["--rnode-port", args.rnode_port, "--txpower", str(args.lora_txpower)]
        elif medium == "wan":
            command += ["--host", args.wan_host, "--port", str(args.wan_port)]
        command += list(extra)
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.events, self.lock = [], threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()
        ready = self.wait(lambda e: e["event"] == "ready", 60)
        if not ready:
            raise SystemExit(f"agent {name} ({medium}) did not start")
        self.dest, self.silent = ready["dest"], ready["silent"]

    def _read(self):
        for line in self.process.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                continue  # RNS log lines
            with self.lock:
                self.events.append(event)

    def mark(self):
        with self.lock:
            return len(self.events)

    def wait(self, predicate, timeout, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for event in self.events[since:]:
                    if predicate(event):
                        return event
            time.sleep(0.05)
        return None

    def call(self, reply, wait_s, **command):
        since = self.mark()
        self.process.stdin.write(json.dumps(command) + "\n")
        self.process.stdin.flush()
        return self.wait(lambda e: e["event"] == reply and e.get("dest", command.get("dest")) == command.get("dest"),
                         wait_s, since)

    def quit(self):
        try:
            self.process.stdin.write(json.dumps({"cmd": "quit"}) + "\n")
            self.process.stdin.flush()
        except OSError:
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()


def slow(*agents):
    return any(a.medium == "lora" for a in agents)


def lan(args):
    media = args.media.split(",")
    agents = [Agent(f"{m}{int(time.time()) % 10000}", m, args) for m in media]
    print("agents: " + ", ".join(f"{a.medium} {a.dest[:8]} (silent {a.silent[:8]})" for a in agents), flush=True)
    results, failures = [], []

    def record(check, a, b, ok, **detail):
        row = {"check": check, "from": a.medium, "to": b.medium, "ok": bool(ok), **detail}
        results.append(row)
        print(f"  {'PASS' if ok else 'FAIL'} {check:9} {a.medium:>5} -> {b.medium:<5} " +
              " ".join(f"{k}={v}" for k, v in detail.items()), flush=True)
        if not ok:
            failures.append(row)

    try:
        print("announces:", flush=True)
        for a in agents:
            marks = {b.name: b.mark() for b in agents if b is not a}
            sent = time.time()
            a.call("announced", 5, cmd="announce")
            for b in agents:
                if b is a:
                    continue
                heard = b.wait(lambda e: e["event"] == "heard" and e["dest"] == a.dest,
                               45 if slow(a, b) else 15, marks[b.name])
                record("announce", a, b, heard, s=round(heard["t"] - sent, 2) if heard else None,
                       hops=heard.get("hops") if heard else None)

        print("paths to never-announced destinations:", flush=True)
        for a in agents:
            for b in agents:
                if b is a:
                    continue
                if "prnsd" in (a.medium, b.medium) and not args.prnsd_gateway:
                    # prnsd is a transport node; it passes requests for unknown
                    # paths on only from gateway-mode interfaces.
                    print(f"  SKIP path      {a.medium:>5} -> {b.medium:<5} prnsd not in gateway mode "
                          "(tests/prnsd/config; then --prnsd-gateway)", flush=True)
                    continue
                path = a.call("path", 60, cmd="path", dest=b.silent, timeout=45 if slow(a, b) else 15)
                record("path", a, b, path and path["ok"], s=path.get("s") if path else None,
                       hops=path.get("hops") if path else None)

        print("links, echoes and resources:", flush=True)
        for a in agents:
            for b in agents:
                if b is a:
                    continue
                lora = slow(a, b)
                link = a.call("link", 90, cmd="link", dest=b.dest, aspects=f"contract.{b.name}",
                              timeout=60 if lora else 20)
                if not (link and link["ok"]):
                    record("link", a, b, False, reason=link.get("reason") if link else "no reply")
                    continue
                echo = a.call("echo", 200, cmd="echo", dest=b.dest, n=3, size=200 if lora else 400,
                              timeout=40 if lora else 10)
                size = args.lora_resource if lora else args.resource
                since = b.mark()
                resource = a.call("resource", 600, cmd="resource", dest=b.dest, size=size,
                                  timeout=540 if lora else 120)
                received = b.wait(lambda e: e["event"] == "resource_in" and e["dest"] == b.dest, 10, since)
                intact = bool(resource and resource["ok"] and received and received.get("sha") == resource["sha"])
                record("link", a, b, echo and echo["ok"] == echo["n"] and intact,
                       link_s=link["s"], echo=f"{echo['ok']}/{echo['n']}" if echo else None,
                       echo_p50_ms=echo.get("rtt_p50_ms") if echo else None,
                       resource=f"{size}B in {resource['s']}s" if resource else None,
                       intact=intact)
                a.call("closed", 10, cmd="close", dest=b.dest)
    finally:
        for agent in agents:
            agent.quit()
    return results, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("suite", choices=["lan"])
    parser.add_argument("--host", default="192.168.2.125")
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--prnsd-port", type=int, default=4270)
    parser.add_argument("--rnode-port", default="/dev/cu.usbserial-0001")
    parser.add_argument("--lora-txpower", type=int, default=2)
    parser.add_argument("--wan-host", default="127.0.0.1")
    parser.add_argument("--wan-port", type=int, default=4290)
    parser.add_argument("--media", default="tcp,prnsd,lora")
    parser.add_argument("--prnsd-gateway", action="store_true",
                        help="prnsd runs with mode = gateway, so path discovery through it can be checked")
    parser.add_argument("--resource", type=int, default=20000, help="resource bytes between fast media")
    parser.add_argument("--lora-resource", type=int, default=1500, help="resource bytes when LoRa is involved")
    parser.add_argument("--out")
    args = parser.parse_args()

    out = args.out or os.path.join(HERE, "bench-results", time.strftime("%Y%m%d-%H%M%S") + f"-contracts-{args.suite}")
    os.makedirs(out, exist_ok=True)
    started = time.time()
    results, failures = lan(args)
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump({"suite": args.suite, "media": args.media, "started": started, "results": results}, f, indent=2)
    print(f"\n{len(results) - len(failures)}/{len(results)} passed in {time.time() - started:.0f} s; results: {out}",
          flush=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
