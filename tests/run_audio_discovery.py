#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read live DNS-SD and RTSP /info from the actual Linux application."""
import pathlib
import plistlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def name_at(packet, offset, depth=0):
    if depth > 32:
        raise ValueError("DNS compression loop")
    labels = []
    while True:
        n = packet[offset]
        offset += 1
        if n == 0:
            return ".".join(labels), offset
        if n & 0xC0 == 0xC0:
            target = ((n & 63) << 8) | packet[offset]
            suffix, _ = name_at(packet, target, depth + 1)
            return ".".join(labels + [suffix]), offset + 1
        if n > 63:
            raise ValueError("DNS label length")
        labels.append(packet[offset:offset+n].decode("ascii"))
        offset += n


def txt_features(packet, owner):
    _, flags, questions, answers, authority, additional = struct.unpack_from("!6H", packet)
    if not flags & 0x8000:
        return None
    offset = 12
    for _ in range(questions):
        _, offset = name_at(packet, offset)
        offset += 4
    for _ in range(answers + authority + additional):
        name, offset = name_at(packet, offset)
        kind, _, _, length = struct.unpack_from("!HHIH", packet, offset)
        offset += 10
        end = offset + length
        if kind == 16 and name.lower() == owner.lower():
            while offset < end:
                n = packet[offset]
                entry = packet[offset+1:offset+1+n].decode("ascii")
                offset += n + 1
                if entry.startswith("features="):
                    low, high = entry.split("=", 1)[1].split(",")
                    return int(low, 16) | (int(high, 16) << 32)
        offset = end
    return None


def check(binary, audio):
    with tempfile.TemporaryDirectory(prefix="rtcp-discovery-") as temp:
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        name = "AudioCapsOn" if audio else "AudioCapsOff"
        owner = name + "._airplay._tcp.local"
        command = [str(binary), "--name", name, "--ip", "127.0.0.1", "--port", str(port),
                   "--seed-file", temp + "/seed", "--pairing-store", temp + "/pairs", "--no-dump"]
        if not audio:
            command.append("--no-audio")
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    connection = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                    break
                except OSError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("simulator did not start")
                    time.sleep(0.02)
            with connection:
                connection.settimeout(3)
                connection.sendall(b"GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n")
                response = b""
                while b"\r\n\r\n" not in response:
                    part = connection.recv(8192)
                    assert part, "closed before RTSP headers"
                    response += part
                header, body = response.split(b"\r\n\r\n", 1)
                assert b"200 OK" in header
                length = next(int(line.split(b":", 1)[1]) for line in header.split(b"\r\n")
                              if line.lower().startswith(b"content-length:"))
                while len(body) < length:
                    part = connection.recv(8192)
                    assert part, "closed before RTSP body"
                    body += part
                info = plistlib.loads(body[:length])
            query = struct.pack("!6H", 0, 0, 1, 0, 0, 0)
            query += b"".join(bytes([len(label)]) + label.encode("ascii") for label in owner.split("."))
            query += b"\0" + struct.pack("!HH", 16, 0x8001)
            discovered = None
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                udp.settimeout(0.2)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and discovered is None:
                    # Multicast reaches every local responder; unicast to a shared
                    # SO_REUSEPORT :5353 may be delivered to desktop daemons.
                    udp.sendto(query, ("224.0.0.251", 5353))
                    try:
                        packet, _ = udp.recvfrom(8192)
                        discovered = txt_features(packet, owner)
                    except socket.timeout:
                        pass
            assert discovered is not None, "no live DNS-SD TXT response"
            assert discovered == info["features"], (hex(discovered), hex(info["features"]))
            assert bool(discovered & (1 << 9)) == audio
            assert bool(info.get("audioFormats")) == audio
            print(f"live application audio={audio}: DNS-SD == /info == {discovered:#x}")
        finally:
            failed = sys.exc_info()[0] is not None
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            output, _ = process.communicate(timeout=5)
            if failed:
                print(output.decode(errors="replace"), file=sys.stderr)
            if process.returncode != 0:
                raise RuntimeError(output.decode(errors="replace"))


if __name__ == "__main__":
    executable = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/rt-carplay-sim").resolve()
    check(executable, True)
    check(executable, False)
