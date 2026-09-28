#!/usr/bin/env python3
"""Does the device advertisement publish IFAC credentials only when asked?

Resets a node running the advert test build (env
rtnode_heltec_v4_bench_advert: backbones off; IFAC and advertising forced on
in RAM with throwaway credentials), then listens on its local TCP server with
RNS 1.5.2's own discovery handler — what any node that hears the advert runs —
and checks whether the advert it decodes carries the IFAC network name and
passphrase.

    ../.venv/bin/python tests/bench_advert_ifac.py --serial /dev/cu.usbmodem101 \\
        --host 192.168.2.125 --expect absent

    # built with PLATFORMIO_BUILD_FLAGS=-DRTNODE_BENCH_PUBLISH_IFAC=1:
    ../.venv/bin/python tests/bench_advert_ifac.py ... --expect present

The first advert goes out about 60 s after boot. The handler requires stamp
value 16, as a default 1.5.2 node does. The test also fails if making the
stamp stalls the node's main loop ([STALL], 250 ms or more).
"""
import argparse
import os
import re
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_load import HERE, start_bench_node  # noqa: E402

NETNAME = "rtnode-bench"                 # RNode_Firmware.ino, RTNODE_BENCH_ADVERT
PASSPHRASE = "bench-only-not-secret"
DEFAULT_REQUIRED_VALUE = 16              # RNS 1.5.2 InterfaceAnnouncer.DEFAULT_STAMP_VALUE
ADVERT_WITHIN = 180                      # s after boot: 60 s delay, then the stamp


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
            "    ingress_control = No\n"
            f"    target_host = {host}\n"
            f"    target_port = {port}\n"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--expect", choices=["present", "absent"], required=True,
                        help="whether the advert should carry the IFAC credentials")
    parser.add_argument("--out", help="results directory (default tests/bench-results/<time>-advert-ifac)")
    args = parser.parse_args()

    out = args.out or os.path.join(HERE, "bench-results", time.strftime("%Y%m%d-%H%M%S") + "-advert-ifac")
    os.makedirs(out, exist_ok=True)
    boot = time.time()
    capture = start_bench_node(args.serial, args.host, args.port, out, isolated=True)
    if not capture.wait_for("[BENCH] overrides: IFAC and advert on", boot, 5):
        sys.exit("no advert-test mark: flash the rtnode_heltec_v4_bench_advert build")
    with capture.lock:
        mark = next(text for _, text in capture.lines if "[BENCH] overrides: IFAC and advert on" in text)
    publish = re.search(r"publish_ifac=(\d)", mark).group(1) == "1"
    print(f"node: IFAC and advert on, publish_ifac={int(publish)}", flush=True)

    import RNS
    from RNS.Discovery import InterfaceAnnounceHandler

    adverts = []
    heard = threading.Event()
    arrived = []  # any discovery announce, whatever its stamp

    def discovered(info):
        adverts.append(info)
        heard.set()

    class Arrivals:
        aspect_filter = "rnstransport.discovery.interface"

        def received_announce(self, destination_hash, announced_identity, app_data):
            arrived.append(time.time())

    work = tempfile.mkdtemp(prefix="rtnode-advert-ifac-")
    write_config(os.path.join(work, "listener"), args.host, args.port)
    RNS.Reticulum(os.path.join(work, "listener"))
    RNS.Transport.register_announce_handler(
        InterfaceAnnounceHandler(required_value=DEFAULT_REQUIRED_VALUE, callback=discovered))
    RNS.Transport.register_announce_handler(Arrivals())

    if not heard.wait(timeout=max(1, boot + ADVERT_WITHIN - time.time())):
        if arrived:
            print(f"FAIL: the advert arrived but a default 1.5.2 node ignores it "
                  f"(stamp value under {DEFAULT_REQUIRED_VALUE})", flush=True)
        else:
            print(f"FAIL: no advert within {ADVERT_WITHIN} s of boot", flush=True)
        os._exit(1)
    info = adverts[0]
    print(f"advert {time.time() - boot:.0f} s after boot: name {info.get('name')!r}, "
          f"stamp value {info.get('value')}, accepted by a default 1.5.2 node", flush=True)

    # What making the stamp cost the node, from its own marks.
    time.sleep(1)
    with capture.lock:
        lines = list(capture.lines)
    start = next((t for t, text in lines if "[Advertise] Generating workblock" in text), None)
    sent = next((t for t, text in lines if "[Advertise] Sending interface discovery announce" in text), None)
    stall_failure = None
    if start:
        found = next((m for _, text in lines if (m := re.search(r"Stamp found after (\d+) attempts in (\d+) ms", text))), None)
        stalls = [int(m.group(1)) for t, text in lines
                  if start <= t <= (sent or time.time()) and (m := re.search(r"\[STALL\] t=\d+ (\d+)ms", text))]
        if found:
            print(f"stamp made on the node: {found.group(1)} attempts, {found.group(2)} ms", flush=True)
        if stalls:
            stall_failure = f"main loop stalled {max(stalls)} ms while the stamp was made"
        else:
            print("no main-loop stall while the stamp was made", flush=True)
    else:
        print("stamp reused from the node's cache", flush=True)

    netname, netkey = info.get("ifac_netname"), info.get("ifac_netkey")
    if args.expect == "absent":
        ok = netname is None and netkey is None
        verdict = "no IFAC credentials in the advert" if ok else f"advert carries IFAC name {netname!r} and key"
    else:
        ok = netname == NETNAME and netkey == PASSPHRASE
        verdict = ("advert carries the IFAC name and passphrase, as asked" if ok
                   else f"expected the test credentials, got name {netname!r}, key {'set' if netkey else 'absent'}")
    print(("PASS: " if ok else "FAIL: ") + verdict, flush=True)
    if stall_failure:
        ok = False
        print("FAIL: " + stall_failure, flush=True)
    capture.stop.set()
    capture.join(timeout=5)
    print(f"results: {out}", flush=True)
    os._exit(0 if ok else 1)


if __name__ == "__main__":
    main()
