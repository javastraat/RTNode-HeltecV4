#!/usr/bin/env python3
"""A real Columba phone, reached only through RTNode's Bluetooth interface.

A Reticulum + LXMF client here, on the node's local TCP server, asks for a
path to the phone's LXMF delivery destination — which only the phone can
answer, over Bluetooth — then sends it a direct LXMF message and waits for
the delivery proof to come back the same way.

    ../.venv/bin/python tests/bench_ble_columba.py --host 192.168.2.125 \\
        --destination <Columba's lxmf.delivery hash>

Columba logs its delivery destination at start ("Ratchets enabled on
<lxmf.delivery.<identity>:<destination>>"). The phone shows the message.
"""
import argparse
import os
import sys
import tempfile
import time


def wait_for(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.1)
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--destination", required=True, help="Columba's lxmf.delivery hash (hex)")
    args = parser.parse_args()

    import RNS
    import LXMF

    work = tempfile.mkdtemp(prefix="rtnode-ble-columba-")
    os.makedirs(os.path.join(work, "rns"))
    with open(os.path.join(work, "rns", "config"), "w") as f:
        f.write("[reticulum]\n  enable_transport = No\n  share_instance = No\n[logging]\n  loglevel = 2\n"
                "[interfaces]\n  [[RTNode TCP]]\n    type = TCPClientInterface\n    enabled = yes\n"
                f"    ingress_control = No\n    target_host = {args.host}\n    target_port = {args.port}\n")
    RNS.Reticulum(os.path.join(work, "rns"))
    identity = RNS.Identity()
    router = LXMF.LXMRouter(identity=identity, storagepath=os.path.join(work, "lxmf"))
    source = router.register_delivery_identity(identity, display_name="RTNode bench")
    time.sleep(3)  # the TCP connection

    phone = bytes.fromhex(args.destination)
    failures = []
    asked = time.time()
    RNS.Transport.request_path(phone)
    if not wait_for(lambda: RNS.Transport.has_path(phone), 20):
        print("FAIL: no path to the phone within 20 s", flush=True)
        os._exit(1)
    print(f"1. path to the phone in {time.time() - asked:.2f} s, {RNS.Transport.hops_to(phone)} hops", flush=True)

    recipient = RNS.Destination(RNS.Identity.recall(phone), RNS.Destination.OUT, RNS.Destination.SINGLE,
                                "lxmf", "delivery")
    # The phone's announced stamp cost: LXMF works a stamp before sending,
    # which is time spent here, not on the way.
    print(f"   the phone asks for stamp cost {router.get_outbound_stamp_cost(phone)}", flush=True)
    outcome = {}
    message = LXMF.LXMessage(recipient, source,
                             f"RTNode Bluetooth test, {time.strftime('%H:%M:%S')}: sent over TCP to RTNode, "
                             "then Bluetooth to this phone.",
                             title="RTNode BLE test", desired_method=LXMF.LXMessage.DIRECT)
    message.register_delivery_callback(lambda m: outcome.setdefault("delivered", time.time()))
    message.register_failed_callback(lambda m: outcome.setdefault("failed", time.time()))
    sent = time.time()
    router.handle_outbound(message)
    wait_for(lambda: outcome, 90)
    if "delivered" in outcome:
        print(f"2. LXMF message delivered, proof back in {outcome['delivered'] - sent:.2f} s", flush=True)
    else:
        failures.append("the message was " + ("refused or failed" if "failed" in outcome else "not delivered in 90 s")
                        + f" (state {message.state})")

    for failure in failures:
        print("FAIL: " + failure, flush=True)
    if not failures:
        print("PASS: path and direct LXMF delivery to Columba through RTNode's Bluetooth", flush=True)
    os._exit(1 if failures else 0)


if __name__ == "__main__":
    main()
