#!/usr/bin/env python3
"""One-shot TCP server for the project's ESP8266 OTA protocol."""

import argparse
import re
import socket
import sys
import time
from pathlib import Path


DEFAULT_CHUNK_SIZE = 512
DEFAULT_CHUNK_DELAY = 0.05
DEFAULT_START_DELAY = 2.0


def load_package(path: Path) -> bytes:
    data = path.read_bytes()
    newline = data.find(b"\n")
    if newline < 0:
        raise ValueError("missing LEN header")

    header = data[: newline + 1]
    match = re.fullmatch(rb"LEN:(\d+)\r\n", header)
    if match is None:
        raise ValueError("invalid LEN header")

    expected = int(match.group(1))
    actual = len(data) - len(header)
    if expected != actual:
        raise ValueError(
            "LEN header mismatch: expected %d bytes, package contains %d bytes"
            % (expected, actual)
        )
    return data


def parse_args() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parent.parent
    default_package = project_root / "bin" / "Industrial_temperature_humidity_ota.bin"
    parser = argparse.ArgumentParser(description="Serve one ESP8266 OTA update")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--file", type=Path, default=default_package)
    parser.add_argument("--start-delay", type=float, default=DEFAULT_START_DELAY)
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE)
    parser.add_argument("--chunk-delay", type=float, default=DEFAULT_CHUNK_DELAY)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        package = load_package(args.file)
    except (OSError, ValueError) as exc:
        print("OTA package error: %s" % exc, file=sys.stderr)
        return 1

    payload_size = len(package) - package.find(b"\n") - 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            server.bind((args.host, args.port))
            server.listen(1)
        except OSError as exc:
            print("Cannot listen on %s:%d: %s" % (args.host, args.port, exc), file=sys.stderr)
            return 2

        print("OTA package: %s" % args.file)
        print("Firmware payload: %d bytes" % payload_size)
        print("Listening on %s:%d; click OTA on the device now." % (args.host, args.port))
        client, address = server.accept()
        with client:
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print("Device connected from %s:%d" % address)
            time.sleep(args.start_delay)
            sent = 0
            next_progress = 10
            while sent < len(package):
                end = min(sent + args.chunk_size, len(package))
                client.sendall(package[sent:end])
                sent = end
                progress = sent * 100 // len(package)
                if progress >= next_progress or sent == len(package):
                    print("Sent %d/%d bytes (%d%%)" % (sent, len(package), progress))
                    next_progress += 10
                if sent < len(package):
                    time.sleep(args.chunk_delay)

        print("OTA package sent successfully.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nOTA server stopped.")
        raise SystemExit(130)
