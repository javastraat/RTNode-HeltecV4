#!/usr/bin/env python3
"""Does RTNode answer a path request from its announce cache?

Client A connects to the node's local TCP server, announces one destination
and exits. Client B then connects and requests a path to it. A is gone, so
only the announce RTNode cached can answer. Guards the in-RAM announce cache
(PERFORMANCE_STRATEGY.md, #43): a path request must be answered within 5 s.

Needs RNS, so run it with the workspace venv:

    ../.venv/bin/python tests/bench_path_response.py <host> [port]

Run it against the bench build (local TCP server on, backbones off).
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def write_config(directory, host, port):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "config"), "w") as config:
        config.write(
            "[reticulum]\n"
            "  enable_transport = No\n"
            "  share_instance = No\n"
            "[logging]\n"
            "  loglevel = 2\n"
            "[interfaces]\n"
            "  [[RTNode local TCP]]\n"
            "    type = TCPClientInterface\n"
            "    enabled = yes\n"
            f"    target_host = {host}\n"
            f"    target_port = {port}\n"
        )


ANNOUNCER = """
import sys, time, RNS
RNS.Reticulum(sys.argv[1])
time.sleep(3)  # the TCP connection
destination = RNS.Destination(RNS.Identity(), RNS.Destination.IN, RNS.Destination.SINGLE, "rtnodebench", "pathresponse")
destination.announce()
print(destination.hash.hex(), flush=True)
time.sleep(3)  # let RTNode receive and cache it before this client leaves
"""


def main():
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 4242
    work = tempfile.mkdtemp(prefix="rtnode-path-response-")
    write_config(os.path.join(work, "a"), host, port)
    write_config(os.path.join(work, "b"), host, port)

    announcer = subprocess.run([sys.executable, "-c", ANNOUNCER, os.path.join(work, "a")],
                               capture_output=True, text=True, timeout=60)
    destination_hash = bytes.fromhex(announcer.stdout.strip().splitlines()[-1])
    print(f"A announced {destination_hash.hex()} and left", flush=True)

    # RTNode rebroadcasts an announce for several seconds; let that finish so
    # B can only learn the path by asking.
    time.sleep(15)
    import RNS
    RNS.Reticulum(os.path.join(work, "b"))
    time.sleep(3)  # the TCP connection
    if RNS.Transport.has_path(destination_hash):
        print("INVALID: B learned the path before asking (a rebroadcast reached it)", flush=True)
        os._exit(2)
    requested = time.time()
    RNS.Transport.request_path(destination_hash)
    while time.time() - requested < 5.0:
        if RNS.Transport.has_path(destination_hash):
            print(f"PASS: path answered in {time.time() - requested:.2f} s, "
                  f"{RNS.Transport.hops_to(destination_hash)} hops", flush=True)
            os._exit(0)
        time.sleep(0.05)
    print("FAIL: no path response within 5 s", flush=True)
    os._exit(1)


if __name__ == "__main__":
    main()
