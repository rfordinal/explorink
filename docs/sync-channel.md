# The sync channel: who may push a document to the device

T-2098. **Step 1, the decision: accepted 2026-11-28** (QR in place of a compared
number, with the three conditions below). **Step 2, the device half: built
2026-11-28, host-verified, not yet run on a board** -- "Step 2: the device half,
as built" at the end of this file. The plan is `../../../docs/tasks/T-2098.md`
in the parent repo.

## The problem in one paragraph

Since T-2095 the phone sends **plaintext** wallet documents and the device
seals them. They cross BLE with no pairing and no bonding
(`lib/BlePositionServer/src/BlePositionServer.cpp`, the
`setSecurityAuth(false, false, false)` call in `begin()`) or cleartext HTTP
(`docs/android-wallet.md` 14.13 in the parent repo). Anyone in radio range can
read a passport off the air, and anyone in range can push one in. Three other
open tasks need that same stranger: T-2113 item 1 (push while the map is open),
T-2114 (keystream residue from a chosen IV), T-2103 (`POST /upload` and WebDAV
reach the card with no wallet gate).

## Decision in short

1. **Our own layer, not BLE pairing.** BLE LE Secure Connections is in the stack
   and works, but it covers one of the two transports and binds to the phone,
   not the app. Measure 1 of `wallet-provisioning.md` (parent repo, "The
   transport is the thing to protect now", accepted 2026-11-26) already asks for
   one mechanism over both transports that does not depend on the phone's
   Bluetooth stack.
2. **The handshake is Noise, one pattern: `Noise_IK_25519_AESGCM_SHA256`**, plus
   its `psk2` variant for the first meeting. Named, specified, with published test
   vectors, and the same shape WireGuard uses. Every primitive it needs is
   already compiled into both chip families.
3. **A phone is paired once, by scanning a QR code off the panel.** The QR holds
   the device's public key and a one-time 256-bit secret. **This replaces "a
   number compared on both ends"**, which the plan asked for. Why a bare number is
   not safe without extra machinery is below; it is the one point here the
   maintainer has to accept or reject.
4. **Each sync is a fresh Noise session** between two keys that already know each
   other. The session key is not K, never touches the card, and dies with the
   sync.
5. **The channel carries wallet transfers and their replies, nothing else.**
   Tiles, position and the `CMD:` grammar stay as they are. T-222 owns the
   grammar question.
6. **It lives on the wallet screen, never the map.** Its state is created and
   wiped there, and the map refuses a wallet path outright (T-2097). Pairing, the
   gate and accepting a transfer stay three separate acts.

## Question 1: BLE pairing, or our own layer?

### What the pinned stack actually offers

Read off the disk 2026-11-28, not searched:

- **NimBLE has LE Secure Connections with numeric comparison.** The IO-capability
  table in `NimBLE-Arduino/src/nimble/nimble/host/src/ble_sm_sc.c`
  (`ble_sm_sc_init_ioa`) picks `IOACT_NUMCMP` for a DisplayYesNo device against a
  phone, and the pinned NimBLE-Arduino 2.5.1 (our fork, commit `63d8732`) exposes
  it as `NimBLEServerCallbacks::onConfirmPassKey()` plus
  `NimBLEDevice::injectConfirmPasskey()` (`src/NimBLEServer.h`,
  `src/NimBLEDevice.h`). The device panel shows six digits, a button confirms.
- **SC is compiled in, legacy is compiled in too, SC-only is off.**
  `CONFIG_BT_NIMBLE_SM_SC=1`, `CONFIG_BT_NIMBLE_SM_LEGACY=1`
  (`NimBLE-Arduino/src/nimconfig.h`), and `CONFIG_BT_NIMBLE_SM_SC_ONLY=0` in the
  prebuilt `framework-arduinoespressif32-libs/esp32{c3,s3}/sdkconfig` (read
  2026-11-28; that directory is rewritten by whichever build ran last, see
  `build-environments.md`). Using LESC would mean turning SC-only on, or a
  legacy-pairing downgrade stays open.
- **The bond table is sized for zero use.** `CONFIG_BT_NIMBLE_MAX_BONDS=1` in
  `platformio.ini`, with a comment saying the table is dead because nothing bonds.
  Several phones per device means raising it.
- **LESC numeric comparison carries a commitment.** The responder sends a confirm
  value before its nonce (`ble_sm_sc_confirm_exec()` in `ble_sm_sc.c`). That
  commitment is what makes six digits enough. Remember it for question 3 of the
  pairing below.

### Why not use it

1. **It covers BLE only.** The Wi-Fi path (HTTP to the device's softAP,
   `android-wallet.md` 14.13) needs a second mechanism. Two mechanisms means two
   things to get right and two things to audit, and step 4 of the plan would have
   to invent the second one anyway.
2. **It binds to the phone, not to our app.** A bond is a property of the phone's
   Bluetooth stack. Any app on that phone with `BLUETOOTH_CONNECT` reaches the
   encrypted characteristics without a prompt. Our own layer keeps the key in our
   app's storage.
3. **The phone app never sees the number.** On Android the comparison dialog is
   system UI; reading or confirming the number from an app needs
   `BLUETOOTH_PRIVILEGED`, which a store app cannot get. So the app cannot word the
   prompt, cannot tie it to "pair for the wallet", and cannot tell a pairing for
   the map from one for documents. iOS behaves the same way: CoreBluetooth starts
   pairing on its own when an encrypted characteristic is touched. **[open --
   read off platform documentation from memory, not re-checked this session.]**
4. **It changes the position channel's world.** The map's writes must keep working
   on a phone that never paired (plan invariant 6). Per-characteristic
   `READ_ENC`/`WRITE_ENC` flags would allow that, but every phone that pairs for
   the wallet then also carries a bond for the map link, and a bond gone stale on
   one side (re-flash, forget on the phone) is a known source of reconnect
   failures we do not have today.
5. **Pairing-method confusion is a published attack class** against Bluetooth
   numeric comparison and passkey entry ("Method Confusion Attack on Bluetooth
   Pairing", IEEE S&P 2021). Named here as a lead, not re-read this session.

What it would have bought: no code of ours in the crypto path, and no new
library on either side. That is real and it is why it was read first. It does not
outweigh 1 and 2.

### The standard systems made the same call

- **Matter** commissions over BLE or IP with the same session layer on both: a
  PAKE on a setup code for the first meeting (PASE), certificate-based sessions
  after (CASE). Its BLE link stays unencrypted at the link layer.
- **HomeKit Accessory Protocol**: pair-setup once with a code, pair-verify with
  long-term keys per session, same over BLE and IP.
- **Wi-Fi Easy Connect (DPP)**: a QR code carries the device's public key, which
  is what option 3 below does.
- **WireGuard**: `Noise_IKpsk2` between keys that already know each other.

None of them uses link-layer pairing as the security of the payload. All four are
from specification knowledge, not a document re-read this session.

## The handshake: Noise IK, with a psk for the first meeting

**Do not compose ECDH, a KDF and GCM by hand.** The plan's rule, and the reason
for a named pattern. Noise is a framework spec with fixed patterns, a fixed
transcript hash, fixed nonce rules and published test vectors (the cacophony and
noise-c vector files). Implementing it from the spec and passing those vectors is
implementing a standard, not inventing one.

The pattern, `Noise_IK_25519_AESGCM_SHA256`:

```
<- s                      (device's static key, known in advance)
...
-> e, es, s, ss           (phone: ephemeral, then its own static, encrypted)
<- e, ee, se              (device: ephemeral)
```

Why IK and not KK: with KK the device has to know which phone is calling before
the first message. With IK the phone's static key arrives inside message 1,
encrypted, and the device looks it up in its pairing table. Several paired phones
cost nothing extra. WireGuard picks IK for the same reason.

Why AESGCM and not ChaChaPoly: `CONFIG_MBEDTLS_CHACHA20_C` and `POLY1305_C` are
**off** in both prebuilt sdkconfigs, `CONFIG_MBEDTLS_GCM_C=y` is on, and the wallet
already runs AES-GCM. Curve25519 is on
(`CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED=y`), so is `CONFIG_MBEDTLS_HKDF_C`.
mbedtls on disk is 3.6.5
(`framework-espidf/components/mbedtls/mbedtls/include/mbedtls/build_info.h`).
Nothing new is linked on the device.

Each sync gets two fresh ephemerals, so:

- **The session key is new per sync** and dies with it (plan invariant 1).
- **A recorded sync cannot be replayed**: the device's ephemeral differs every
  time, so a replayed message 1 produces a different session key and the
  recorded transport messages fail their tag (invariant 4). Inside a session, the
  Noise nonce counter rejects a reordered or repeated frame.
- **Forward secrecy**: a later dump of either static key does not open a recorded
  sync.

## Question 2: what is paired -- a phone, or a session?

**A phone, once. A session per sync.** The rider confirms once.

### Pairing: the QR code

1. On the wallet screen the rider chooses "pair a phone". The device makes a
   random 32-byte secret, keeps it in RAM only, and draws a QR code holding:
   format version, the device's static public key (32 bytes), the secret.
   About 70 bytes, well inside one QR. The device already draws QR codes
   (`src/util/QrUtils.h`, `drawQrCode()`).
2. The phone scans it. The app already decodes QR with zxing
   (`android`, `wallet/CodeReader.kt`); a still from the system camera intent is
   enough, no camera library added.
3. The phone runs `Noise_IKpsk2_25519_AESGCM_SHA256`, psk = the secret. Only a
   phone that saw the panel knows it. The device accepts **one** attempt per
   code, only while the screen is open, then forgets the secret either way.
4. On success the device stores the phone's static public key in its pairing
   table in NVS and the screen says "paired". The phone stores the device's public
   key and its own private key.

### Why not a six-digit number to compare

A Noise handshake followed by "compare these six digits" is broken without more
work. A man in the middle runs two handshakes and picks his own ephemeral key
**after** seeing the others. He can grind it, about a million tries, until both
sides show the same six digits. LESC closes this with the commitment above;
ZRTP closes it with a hash commitment. Adding a commitment to Noise is composing a
protocol by hand, which is the thing the plan forbids.

The other named answer is a PAKE on a code typed into the phone (Matter's
SPAKE2+, Thread's EC J-PAKE). `CONFIG_MBEDTLS_ECJPAKE_C=y` is on in both
sdkconfigs, so the device half exists. **Against it**: mbedtls's EC J-PAKE speaks
the TLS extension encoding, the phone side would need a library that matches that
encoding byte for byte, and that interop is untested **[open]**. Kept as the
fallback if QR scanning fails in practice (glare, a phone with a broken camera).

The QR needs no secret to be short, so there is nothing to grind and nothing to
brute-force offline. Plan invariant 3 changes from "the number is compared on both
ends" to "**the code is only on the panel, and only a phone that read it can
pair**". The rider's yes on the device is opening the pairing screen.

### Three conditions on the QR, added by the supervisor 2026-11-28

Accepted, with these, because without them the pairing rests on a value that is
easier to take than it looks:

1. **The QR is a secret while it is on the panel, and `CMD:SCREENSHOT` is
   ungated in every build** (T-2081). A cable, or a radio in range, reads the
   framebuffer and gets the code with no camera and no line of sight. So: one
   attempt, one code, dead when the pairing screen closes, and dead after a
   short timeout even if it stays open. Never redrawn with the same secret.
   Write that in the screen's code as the reason, not just in this doc.
2. **The phone cannot scan today.** `wallet/CodeReader.kt` on `feat/wallet`
   decodes an image with zxing, and the app holds **no `CAMERA` permission** at
   all (checked 2026-11-28 against `app/src/main/AndroidManifest.xml`). A camera
   intent returns a still and needs no permission, which is the cheap path, but
   it is still new phone work and a new iOS question. Price it in T-174 rather
   than discovering it there.
3. **Say what happens when scanning fails.** Glare on e-ink, a cracked lens, a
   phone the rider will not grant the camera to. Either the J-PAKE fallback below
   is built, or the device says plainly that a camera is required to pair. One of
   the two, decided, not left open.

### Phase 1 is the QR, not a typed PIN

The maintainer asked whether a PIN could come first and the QR second
(2026-11-28). **It is the other way round, and the reason is arithmetic rather
than taste.**

- **A typed short code is not a psk.** In `Noise_IKpsk2` the psk is mixed into
  the transcript, so somebody who records one handshake can test psk guesses
  **offline**. Eight digits is 10^8 tries on a laptop. The one-attempt rule above
  limits *online* guessing and does nothing about that.
- **A short typed secret is exactly what a PAKE is for** -- SPAKE2+ or EC
  J-PAKE, named above. That is the bigger build, not the smaller one: the device
  half exists (`CONFIG_MBEDTLS_ECJPAKE_C=y`) and the phone half is an untested
  interop against mbedtls's TLS-extension encoding.
- **A 256-bit secret needs no PAKE**, which is what makes the QR the *cheap*
  first phase.
- **And the device half of the QR already ships.** `QrUtils::drawQrCode()` draws
  the Wi-Fi join code on the AP screen today
  (`src/activities/network/CrossPointWebServerActivity.cpp`, the `WIFI:T:nopass`
  payload). The panel work is a layout, not a feature.

So: **QR first.** A typed code is a later phase and arrives with a PAKE, if the
camera turns out to be a problem in the field.

### Where the keys live

- **Device static key and pairing table: NVS.** NVS is plaintext flash and
  `esptool` reads it -- the same boundary the wallet wrap lives on
  (`wallet-crypto.md`, "The threat boundary, first"). With the flash dumped, a
  thief can pose as this device to a paired phone, or as a paired phone to this
  device. Same out-of-scope line, same rule: no product text claims otherwise.
- **Phone static key: our app's private storage, Keystore-wrapped**, the same way
  `android-wallet.md` 16.2 stores data today. **[open]**: whether Android Keystore
  can hold an X25519 key and run the agreement itself at `minSdk 31`; if it can
  only from a later API level, wrap it like 16.2 does.
- **Forget**: the device can delete one pairing or all from the wallet screen
  without the phone. Forgetting the wallet (T-2099) wipes the pairing table and the
  device static key with it, so a device given away pairs with nobody (invariant
  5).

### Three acts, and they stay three

T-2097's rules (`docs/tasks/T-2098.md` in the parent repo, "What T-2097's rules
mean for this channel"):

1. **Pairing a phone**: once per phone, this task.
2. **Opening the gate**: the pattern, every time the wallet is used.
3. **Accepting a transfer**: the rider says "receiving now" on the wallet
   screen, T-2097.

None implies another. A paired phone is not an unlocked wallet; an unlocked
wallet is not an accepted transfer. The channel only answers "is this a phone the
rider paired". A sync runs when all three hold, and is refused otherwise.

## Where the channel lives: the wallet screen, not the map

**Today BLE is up only on the map and the tile-sync screen**
(`MapActivity.cpp`, `TileSyncActivity.cpp`, the two places that call
`BlePositionServer::begin()` and attach the transfer receiver), and every bench
recipe starts with `CMD:GOTO_MAP`. **The design does not depend on that.** T-2097
moves wallet ingest to the wallet screen; this channel is built for that state:

1. **The channel is bytes in, bytes out.** It does not know which screen started
   the BLE server or whether the carrier is GATT or HTTP. It attaches to the
   transfer `onFrame` hook (`BlePositionServer.cpp`) of whoever owns the server --
   after T-2097, the wallet screen.
2. **Its state is owned by the wallet screen and dies with it.** Device static
   key, pairing table and any handshake or session state are loaded in the wallet
   screen's `onEnter`, wiped in its `onExit`. Nothing of it exists on the map.
   Reason: the map draws from a heap whose largest free block is about 43 KB with
   BLE up (`map-memory.md`), and taking the wallet off that budget is half of why
   it moves.
3. **The QR for pairing is drawn on the wallet screen.** Not the map, not a sync
   screen.
4. **The map and the tile-sync screen refuse a wallet path outright** -- no
   session, no handshake, no "paired phone may". That refusal is T-2097's removal;
   this channel adds nothing to those screens.
5. **The position channel stays unpaired.** Showing the rider where they are
   never needs pairing.

**Consequence for step 2 of the plan.** The device half is host-testable on its
own (pure header in front of the receiver). On a board it needs a screen that owns
the BLE server and the channel at once -- that is T-2097's wallet-screen move. So
the on-board half of step 2 waits for that move, or for T-2097 to land its server
lifecycle first. **Do not bench it by parking the channel on the map** to get
around the wait: the instrument would then test a place the product refuses.

## Question 3: what the channel carries

**Wallet transfers, and only those.** In detail:

1. **Transfer frames to a wallet path** (`begin`/`chunk`/`abort` whose path is
   under the wallet tree) and **their status lines** (`RDY`/`OK`/`ERR`) travel as
   Noise transport messages, on the wallet screen only. There, a wallet-path
   `begin` outside a session is refused; everywhere else it is refused anyway
   (section above, item 4). The frames arrive on their own characteristic
   (`BlePositionServer.cpp`, the `onFrame` hook) and the receiver's decisions are
   a pure header since T-2106, so the channel layer sits in front of it and is
   host-testable.
2. **Tile transfers stay open, on the map and tile-sync screens.** Tiles are
   public data, and tile sync must keep working for a phone that never paired.
3. **The position channel and the `CMD:` grammar do not move.** What a stranger
   may still read or do through `CMD:` (position in `info`, `have`, `missing`) is
   T-222's audit. This task does not answer it by accident.
4. **The cable.** Transfer frames arrive through the BLE `onFrame` hook
   (`BlePositionServer.cpp`); a serial path for them was not found in this read,
   **[open]** until checked against `main.cpp`'s dispatcher in step 2. The wallet
   test commands (`CMD:WALLETPROVISION` and friends) build only with
   `-DENABLE_WALLET_TEST_CMDS=1` (`platformio.ini`, the comment above that flag);
   T-175 retires the provisioning path from the product.
5. **Wi-Fi.** The same Noise messages ride an HTTP body to one new endpoint. The
   bytes are identical, only the carrier differs. `POST /upload` and WebDAV `PUT`
   **refuse any wallet path outright** -- that closes T-2103 without waiting for
   the HTTP carrier to exist.

## The wire is a format, so it versions

The QR and the first handshake byte carry a format version. A device and a phone
of different versions refuse with a readable message; they never fall back to an
older or plainer mode. There is no unencrypted fallback (invariant 2).

## What this does to T-2114 and T-2113

- **T-2114**: a stranger can no longer push. The format fix still lands, because a
  paired phone with a bug must not be able to do it either.
- **T-2113 item 1**: a push while the map is open is refused outright once
  T-2097 lands; until then this channel does not touch the map. The idle-lock gap
  itself stays a T-2113 fix.
- **T-2103**: closed by the wallet-path refusal in item 5 above.

## Costs and risks to measure before building

1. **Heap on C3, on the wallet screen.** A C3 running the frame-gate build was
   seen near 27.5 KB free. The Noise state itself is a few hundred bytes (keys,
   chaining key, hash, nonce -- from the spec, not measured); mbedtls X25519
   scratch during the handshake is the unknown. Measure on an X3 with the wallet
   screen owning BLE, not on the map and not by arithmetic (`CLAUDE.md`, "A flash
   or RAM cost is two builds").
2. **Handshake time on C3.** Two X25519 operations per side per sync. Measure.
3. **Flash cost.** Small, since every primitive is linked already. Two builds.
4. **The phone's Noise.** `noise-java` (MIT) exists but is old; a Kotlin port
   checked against the same vector files is the alternative. Either way the
   vectors run in both test suites, which is also the iOS contract.
5. **A sniff proves "nothing readable".** The instrument is a BLE sniffer (nRF52
   dongle with nRF Sniffer, or an Ubertooth). **None is on this desk** as far as
   `docs/` knows. Until one is, the claim is "verified on the host and by the
   tag failing on a tampered frame", not "verified on the air".

## Verification status

Steps 1's sections are **read off the code or the sdkconfig**, 2026-11-28, or
stated from protocol knowledge where marked. Step 2's section below is **built
and host-tested** (vectors, a second implementation, mutants); nothing in it has
run on a board yet. Platform claims marked `[open]` stay open.

## Step 2: the device half, as built

Branch `t2098-device-half`, off `wallet-viewer`, 2026-11-28.

### The files

| file | what |
|---|---|
| `src/activities/wallet/SyncNoise.h` | Noise IK and IKpsk2: the spec's CipherState, SymmetricState, HandshakeState, two token sequences. Primitives injected. |
| `src/activities/wallet/SyncChannel.h` | The channel: frame dispatch, pairing code lifecycle, phone table, sealed in and out. Pure. The wire format is its header comment. |
| `src/activities/wallet/SyncCryptoDevice.cpp` | The primitives on mbedtls. `CMD:SYNCSELFTEST`. |
| `src/activities/wallet/SyncPairingStore.cpp` | NVS namespace `cpsync`: `dev_sk`, `phones`. |
| `src/activities/wallet/WalletPairActivity.cpp` | The QR screen, opened from the last row of the wallet list. |
| `src/activities/map/MapTransferDecide.h` | `decideWalletGates`: a wallet target outside a sealed session is refused `not paired`. |
| `test/sync_channel/` | Host suite (OpenSSL), and `sync_channel_host` for the script below. |
| `scripts/sync_channel_phone.py` | A phone on Python `noiseprotocol`, driving `sync_channel_host`. |

### How the handshake was checked, since it is not a library

The plan allowed "a named implementation or a transcript-checked one". noise-c is
a framework (every pattern, three curves, its own crypto backends); we need two
token sequences over primitives already linked. So `SyncNoise.h` is the
transcript-checked option, and three independent things agree with it:

1. **The published vectors.** cacophony's `vectors/cacophony.txt` (Haskell,
   public domain) has exactly one vector each for `Noise_IK_25519_AESGCM_SHA256`
   and `Noise_IKpsk2_25519_AESGCM_SHA256`. Both match byte for byte, both roles,
   handshake hash and four transport messages
   (`test/sync_channel/SyncNoiseTest.cpp`). snow's and noise-c's vector files have
   no IKpsk2 over AESGCM/SHA256 (checked 2026-11-28).
2. **A second implementation, live.** `scripts/sync_channel_phone.py` pairs and
   syncs against the device's own `Channel` using Python `noiseprotocol` 0.3.1
   (MIT). Five scenarios, all pass.
3. **A mutant.** Dropping the psk-mode `MixKey(e)` -- the one line that makes
   IKpsk2 differ from IK at message 1 -- fails the IKpsk2 vector and one
   property test.

### Decisions step 2 had to make

1. **The pairing is stored on the first sealed frame, not on message 1.** In
   IKpsk2 the psk enters at message 2, so a valid message 1 proves nothing about
   the QR. The first transport message from the phone is the first thing keyed by
   the psk. The phone sends one inner byte, `0x30 PAIRED`; the device stores the
   key, answers `OK paired` sealed, and **closes the session** -- a sync is a
   separate `OPEN`, so pairing never leaves a transfer-capable session behind
   (three acts stay three).
2. **The code is spent by the first PAIR frame, malformed or not.** Otherwise a
   stranger could probe for free. It lives `kCodeLifetimeMs` = 90 s (a judgment;
   revisit after the first pairing on a real phone), dies when the screen closes,
   and `issueCode()` always draws a new secret.
3. **A frame that does not open ends the session.** A forgery, a replay, a
   desynchronised counter: on one ordered link with write-with-response there is
   no benign cause.
4. **`not paired` comes before `wallet locked`** in the receiver's ladder, so a
   stranger cannot learn whether the wallet is open.
5. **Four phones per device.** Full means no code is issued at all -- a code
   that can only end in "table full" is a secret on the panel for nothing.
6. **Forgetting.** On the pairing screen, LEFT twice forgets every phone, on the
   device alone. `KeyStore::forgetWrap()` forgets the table and the static key
   (invariant 5). Nothing calls `forgetWrap()` yet; T-2099 is the forget-the-wallet
   flow that will.
7. **When the phone cannot scan: refuse, plainly.** Phase 1 needs a camera. No
   J-PAKE fallback is built, for the arithmetic in "Phase 1 is the QR, not a typed
   PIN": a short typed code needs a PAKE, and the PAKE's phone half is an
   untested interop. If the field shows glare or a refused camera permission to be
   a real problem, the fallback is a later phase with its own plan. The app must
   say "pairing needs the camera" rather than offer a typed code that would be
   offline-guessable.
8. **Status lines grew to 128 bytes** (`BlePositionServer`, `kPendingStatusBytes`):
   a sealed line is `NXS ` plus base64 of up to 79 bytes, 112 characters.
   +128 B DRAM, two parking slots.

### What is wired and what is not

- **Wired:** the receiver refuses every wallet transfer outside a session
  (`MapTransferReceiver.cpp`, the `false` passed as `sealedSession`). Nothing
  delivers sealed frames yet, so **every wallet push is refused `not paired`
  today, on the map and on tile sync.** That is T-2097 rule 1 for those screens,
  arriving early. Tiles are untouched. **The current phone app's wallet push
  stops working on this branch** until T-2097 (the wallet screen owns BLE and
  feeds `Channel::onFrame`) and T-174 (the phone speaks the channel).
- **Not wired:** no BLE frame reaches a `Channel`. `WalletPairActivity` draws a
  real code that times out unused. T-2097 attaches the transfer characteristic to
  the wallet screen's `Channel`, and the receiver's `sealedSession` becomes "this
  frame came out of `Channel::onFrame` as `DeliverInner`". Sealed status lines go
  back through `Channel::sealLine()`, never plain.
- **Parking the channel on the map to test it is forbidden** (above), so the
  host is the whole evidence for the protocol until T-2097.

### What the host cannot reach

- **mbedtls.** The host binds OpenSSL. `CMD:SYNCSELFTEST` (bench builds only,
  `ENABLE_WALLET_TEST_CMDS`) runs the IK vector's responder side through mbedtls
  on the chip and prints `SYNCSELFTEST_OK handshake_us=... heap_free=...
  heap_min=...`. That answers costs 1 and 2 above (heap and handshake time on a
  C3) for the crypto alone. **Not yet run on a board.**
- **The QR on glass.** Size, scan distance, glare. Needs a board and a phone.
- **BLE.** MTU of at least 101 for the 98-byte PAIR/OPEN frames; indication of a
  112-character line. Needs T-2097.

### Running it

```
cmake -S test -B test/build && cmake --build test/build --target SyncChannelTest sync_channel_host
test/build/sync_channel/SyncChannelTest
pip install noiseprotocol
python3 scripts/sync_channel_phone.py test/build/sync_channel/sync_channel_host
```
