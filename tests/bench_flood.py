#!/usr/bin/env python3
"""Announce a stream of fresh destinations into an RTNode's local TCP server,
the way a transport rnsd on the LAN side does in issue #43
(PERFORMANCE_STRATEGY.md, scenario B). Run by bench_load.py; needs RNS, so use
the workspace venv:

    ../.venv/bin/python tests/bench_flood.py <host> <port> <per_second> <seconds> <config_dir> [linger]

Every line it prints starts with a Unix timestamp: announces sent, and the
TCP interface going offline or online. With linger, it then stays connected
and silent for that many seconds (the node discards a client's held
announces when it disconnects).
"""
import os
import sys
import time

import RNS


def event(text):
    print(f"{time.time():.3f} {text}", flush=True)


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    rate, seconds, config_dir = float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
    linger = float(sys.argv[6]) if len(sys.argv) > 6 else 0.0

    os.makedirs(config_dir, exist_ok=True)
    with open(os.path.join(config_dir, "config"), "w") as config:
        config.write(
            "[reticulum]\n"
            "  enable_transport = Yes\n"
            "  share_instance = No\n"
            "  panic_on_interface_error = No\n"
            "[logging]\n"
            "  loglevel = 4\n"
            "[interfaces]\n"
            "  [[RTNode local TCP]]\n"
            "    type = TCPClientInterface\n"
            "    enabled = yes\n"
            f"    target_host = {host}\n"
            f"    target_port = {port}\n"
        )

    RNS.Reticulum(config_dir)
    interface = next(i for i in RNS.Transport.interfaces if "RTNode local TCP" in str(i))

    kept = []  # destinations stay registered for the whole run
    interval = 1.0 / rate
    next_announce = time.time()
    end = time.time() + seconds
    was_online = None
    count = 0
    while time.time() < end:
        online = bool(getattr(interface, "online", False))
        if online != was_online:
            event("iface " + ("online" if online else "offline"))
            was_online = online
        if time.time() >= next_announce:
            destination = RNS.Destination(
                RNS.Identity(), RNS.Destination.IN, RNS.Destination.SINGLE, "rtnodebench", f"d{count}"
            )
            destination.announce()
            kept.append(destination)
            event(f"announce {count} {destination.hash.hex()} online={int(online)}")
            count += 1
            next_announce += interval
        time.sleep(0.02)
    event(f"done announced={count}")
    if linger > 0:
        time.sleep(linger)
        event("linger over")
    os._exit(0)  # skip RNS teardown; the harness only needs the log


if __name__ == "__main__":
    main()
