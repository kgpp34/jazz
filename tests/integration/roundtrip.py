#!/usr/bin/env python3
"""Real RDMA integration, opt in with JAZZ_TEST_IP; never emulate verbs."""
import argparse
import os
import selectors
import socket
import struct
import subprocess
import sys


def launch_server(binary, mode, ip, size, extra=()):
    args = [binary, "--mode", mode, "--role", "server", "--local", ip,
            "--port", "0", "--bytes", str(size), "--timeout-ms", "2000", *extra]
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as ready:
            ready.register(process.stdout, selectors.EVENT_READ)
            if not ready.select(5):
                raise AssertionError("server startup timed out")
            line = process.stdout.readline()
        if not line.startswith("LISTENING "):
            _, error = process.communicate(timeout=5)
            raise AssertionError(f"server startup failed: {line} {error}")
        return process, int(line.split()[1])
    except BaseException:
        process.kill()
        process.communicate()
        raise


def roundtrip(binary, mode, ip, size, timeout=False):
    server, port = launch_server(binary, mode, ip, size,
                                 ("--pause-ms", "700") if timeout else ())
    try:
        result = subprocess.run(
            [binary, "--mode", mode, "--role", "client", "--local", ip, "--peer", ip,
             "--port", str(port), "--bytes", str(size), "--timeout-ms", "200" if timeout else "2000"],
            capture_output=True, text=True, timeout=8)
        out, err = server.communicate(timeout=8)
        if timeout:
            assert result.returncode != 0 and server.returncode != 0, (result, out, err)
            assert "deadline" in result.stderr, result.stderr
        else:
            assert result.returncode == 0 and server.returncode == 0, (result, out, err)
            assert "PASS" in result.stdout and "PASS" in out, (result, out, err)
        print(f"PASS {mode} bytes={size} {'timeout-cleanup' if timeout else 'write-read-verify'}", flush=True)
    finally:
        if server.poll() is None:
            server.kill()
            server.communicate()


def malformed(binary, ip):
    for name, payload in (
        ("bad-version", struct.pack("!IIII", 0x4A415A5A, 99, 1, 40) + bytes(80)),
        ("truncated", b"JAZZ"),
        ("silent-peer", None),
    ):
        server, port = launch_server(binary, "oob", ip, 1)
        try:
            with socket.create_connection((ip, port), timeout=4) as peer:
                if payload is None:
                    out, err = server.communicate(timeout=5)
                else:
                    peer.sendall(payload)
                    peer.shutdown(socket.SHUT_WR)
                    out, err = server.communicate(timeout=5)
            assert server.returncode != 0 and "oob-hello" in err, (name, out, err)
            print(f"PASS oob {name}", flush=True)
        finally:
            if server.poll() is None:
                server.kill()
                server.communicate()


def no_listener(binary, mode, ip):
    with socket.socket() as reservation:
        reservation.bind((ip, 0))
        port = reservation.getsockname()[1]
        result = subprocess.run(
            [binary, "--mode", mode, "--role", "client", "--local", ip,
             "--peer", ip, "--port", str(port), "--timeout-ms", "300"],
            capture_output=True, text=True, timeout=5)
    assert result.returncode != 0 and "stage=" in result.stderr, result
    print(f"PASS {mode} no-listener", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    ip = os.environ.get("JAZZ_TEST_IP")
    if not ip:
        print("SKIP: set JAZZ_TEST_IP to a local IPv4 backed by an active RoCEv2 device")
        return 77
    for mode in ("cm", "oob"):
        for _ in range(2):
            for size in (1, 4096, 65537, 1048576):
                roundtrip(args.binary, mode, ip, size)
        roundtrip(args.binary, mode, ip, 4096, timeout=True)
        no_listener(args.binary, mode, ip)
    malformed(args.binary, ip)
    return 0


if __name__ == "__main__":
    sys.exit(main())
