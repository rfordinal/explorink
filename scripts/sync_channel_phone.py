#!/usr/bin/env python3
"""A phone for the sync channel, on an independent Noise implementation (T-2098).

Drives test/sync_channel's `sync_channel_host` -- the device's own
SyncChannel.h on stdin/stdout -- with Python `noiseprotocol` (MIT, pip install
noiseprotocol) playing the phone. That library shares no code with SyncNoise.h,
OpenSSL or mbedtls, so a pairing and a sync that work end to end here are the
cross-implementation check the plan asks for, on top of the cacophony vectors.

It is also the reference the Android half (T-174) can be read against: every
byte the phone sends is built here in a few lines, from the wire format in
SyncChannel.h's header comment.

    cmake --build test/build --target sync_channel_host
    python3 scripts/sync_channel_phone.py test/build/sync_channel/sync_channel_host

Exit 0 when every scenario behaves; each one prints PASS or FAIL. What it does
NOT prove: anything about BLE, the board, or mbedtls -- see
docs/sync-channel.md, "What step 2 proves and what it does not".
"""

import base64
import os
import subprocess
import sys

try:
    from noise.connection import Keypair, NoiseConnection
except ImportError:
    sys.exit("needs `pip install noiseprotocol`")

PROLOGUE = b"explorink-sync/1"
QR_PREFIX = "EXPLORINK-PAIR:1:"
OP_PAIR, OP_OPEN, OP_SEALED, INNER_PAIRED = 0x10, 0x11, 0x12, 0x30
VERSION = 1
# A begin frame for a tile: opcode, u32 total, u32 crc, u8 pathLen, path.
BEGIN = bytes([0x01, 5, 0, 0, 0, 1, 2, 3, 4, 3]) + b"a/b"


class Device:
    def __init__(self, binary):
        self.p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.now = 1000

    def ask(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def code(self):
        r = self.ask(f"code {self.now}")
        assert r.startswith("QR "), r
        return r[3:]

    def frame(self, data):
        return self.ask(f"frame {self.now} {data.hex()}")

    def close(self):
        self.p.stdin.close()
        self.p.wait()


class Phone:
    def __init__(self):
        self.priv = os.urandom(32)

    def scan(self, qr):
        assert qr.startswith(QR_PREFIX) and len(qr) == len(QR_PREFIX) + 128, qr
        body = bytes.fromhex(qr[len(QR_PREFIX):])
        self.device_pub, self.psk = body[:32], body[32:]

    def hello(self, pairing):
        name = b"Noise_IKpsk2_25519_AESGCM_SHA256" if pairing else b"Noise_IK_25519_AESGCM_SHA256"
        self.n = NoiseConnection.from_name(name)
        self.n.set_as_initiator()
        self.n.set_prologue(PROLOGUE)
        if pairing:
            self.n.set_psks(self.psk)
        self.n.set_keypair_from_private_bytes(Keypair.STATIC, self.priv)
        self.n.set_keypair_from_public_bytes(Keypair.REMOTE_STATIC, self.device_pub)
        self.n.start_handshake()
        return bytes([OP_PAIR if pairing else OP_OPEN, VERSION]) + self.n.write_message()

    def finish(self, line):
        if not line.startswith("LINE NX2 "):
            return False
        try:
            self.n.read_message(base64.b64decode(line[len("LINE NX2 "):]))
        except Exception:
            return False
        return self.n.handshake_finished

    def seal(self, inner):
        return bytes([OP_SEALED]) + self.n.encrypt(inner)

    def open(self, line):
        assert line.startswith("LINE NXS "), line
        return self.n.decrypt(base64.b64decode(line[len("LINE NXS "):])).decode()


def pair(dev, phone):
    phone.scan(dev.code())
    assert phone.finish(dev.frame(phone.hello(True))), "pairing handshake"
    return phone.open(dev.frame(phone.seal(bytes([INNER_PAIRED]))))


def scenario(name, fn, binary):
    dev = Device(binary)
    try:
        fn(dev)
        print(f"PASS  {name}")
        return True
    except AssertionError as e:
        print(f"FAIL  {name}: {e}")
        return False
    finally:
        dev.close()


def s_pair_and_sync(dev):
    phone = Phone()
    assert pair(dev, phone) == "OK paired"
    assert dev.ask("table").startswith("TABLE 1 stored=1")
    dev.ask("disconnect")
    assert phone.finish(dev.frame(phone.hello(False))), "sync handshake"
    r = dev.frame(phone.seal(BEGIN))
    assert r == "DELIVER " + BEGIN.hex(), r
    assert phone.open(dev.ask("seal RDY 0 a/b")) == "RDY 0 a/b"


def s_unknown_phone(dev):
    paired = Phone()
    pair(dev, paired)
    stranger = Phone()
    stranger.device_pub, stranger.psk = paired.device_pub, os.urandom(32)
    r = dev.frame(stranger.hello(False))
    assert r == "LINE ERR not paired", r


def s_wrong_code(dev):
    honest = Phone()
    honest.scan(dev.code())
    stranger = Phone()
    stranger.device_pub, stranger.psk = honest.device_pub, os.urandom(32)
    assert not stranger.finish(dev.frame(stranger.hello(True))), "a wrong psk finished the handshake"
    assert dev.ask("table").startswith("TABLE 0"), "stored without the code"
    # The honest phone, with the right code, now finds it spent.
    r = dev.frame(honest.hello(True))
    assert r == "LINE ERR no pairing code", r


def s_replay(dev):
    phone = Phone()
    pair(dev, phone)
    dev.ask("disconnect")
    hello = phone.hello(False)
    assert phone.finish(dev.frame(hello))
    sealed = phone.seal(BEGIN)
    assert dev.frame(sealed).startswith("DELIVER ")
    dev.ask("disconnect")
    assert dev.frame(hello).startswith("LINE NX2 ")  # message 1 replays fine...
    r = dev.frame(sealed)  # ...the recorded transfer does not
    assert r == "LINE ERR bad sealed frame", r


def s_code_expires(dev):
    phone = Phone()
    phone.scan(dev.code())
    dev.now += 90_000
    r = dev.frame(phone.hello(True))
    assert r == "LINE ERR no pairing code", r


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    binary = sys.argv[1]
    results = [
        scenario("pair by QR, then sync a transfer frame and read a sealed status", s_pair_and_sync, binary),
        scenario("an unpaired phone gets no session", s_unknown_phone, binary),
        scenario("a wrong code never pairs, and spends the code", s_wrong_code, binary),
        scenario("a recorded sync does not replay", s_replay, binary),
        scenario("a code dies after 90 s", s_code_expires, binary),
    ]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
