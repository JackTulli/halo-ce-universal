#!/usr/bin/env python3
"""Keeps the Xbox build's log.

The console sends every line of its log by UDP to port 9999 of the machine
beside it (port/xbox/src/nx_kernel.c: 10.0.2.2, the host of xemu's user
network). This listens and appends the lines, time-stamped, to a file:

    python tools/xbox_log.py [--output build/xbox/guest.log] [--print]
"""

import argparse
import datetime
import socket
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=9999)
    parser.add_argument("--output", default="build/xbox/guest.log")
    parser.add_argument("--print", action="store_true", help="also print the lines")
    args = parser.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    with open(args.output, "a", buffering=1, encoding="utf-8") as log:
        log.write(f"--- listening on port {args.port} from {datetime.datetime.now():%H:%M:%S}\n")
        while True:
            data, address = sock.recvfrom(4096)
            for line in data.decode("latin-1").rstrip("\n").split("\n"):
                text = f"{datetime.datetime.now():%H:%M:%S} {address[0]} {line}"
                log.write(text + "\n")
                if args.print:
                    print(text, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
