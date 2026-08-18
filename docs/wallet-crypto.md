# Wallet crypto v1, on the device

Phase P3. The wallet's papers are encrypted at rest and the device holds the key
only while the wallet is unlocked. The on-disk contract is frozen and lives in the
parent repo's `docs/wallet-format.md`, section 11; this document is the device
side of it -- the code paths, the key's lifetime, the threat boundary, and what is
still unverified.

The viewer itself, its screens and its button map are `wallet-viewer.md`.

## The threat boundary, first

Two cases, and they are not close to each other:

- **Card pulled out and read somewhere else: nothing readable.** Every document
  pixel, every title, every code payload and the whole item/page structure are
  either inside an encrypted asset or inside the encrypted manifest. What the card
  still tells somebody: how many assets exist, their exact sizes, and from the
  cleartext headers their type, bit depth, tile position, presentation, version and
  the panel geometry. So: that this is an ExplorInk wallet, which device it was
  built for, roughly how many pages, and which assets are codes.
- **Device in hand with a USB cable: readable.** The wrapped key is in NVS, NVS is
  plaintext flash, and `esptool` dumps flash. The PIN slows a person pressing
  buttons; it does not slow anybody who takes the flash image away and attacks the
  wrap offline.

Closing the second case needs flash encryption plus an irreversible eFuse burn,
which breaks the reflash workflow this project runs on (`CLAUDE.md`, "Never flash
the device without asking"). **It is out of scope, and no product text may claim
otherwise** -- not the site, not the user guide, not a release note. "Encrypted on
the card" is true. "Encrypted on the device" is not.

## What is encrypted, and with what

```
assets     AES-256-CTR, key K, IV per (asset, version)
manifest   AES-256-GCM, key K, fresh 12-byte nonce per write
K          32 bytes, made on the laptop, wrapped in NVS, never on the card
```

mbedtls does the primitives (`WalletCryptoDevice.cpp`): the firmware already links
it for the OTA image (`src/network/FirmwareFlasher.cpp`) and on the ESP32-C3 its
AES and SHA-256 run on the hardware accelerators.

## Assets: CTR, because a window has to be seekable

`IV = assetId raw bytes (8) || u32 version little endian || u32 zero`
(`buildAssetIv()`, `src/activities/wallet/WalletCrypto.h`).

To reach payload byte `o`: start the counter at block `o / 16` and throw away
`o % 16` bytes of keystream (`ctrStartForOffset()`, `ctrXorInPlace()`).

**That is the property design B is built on.** A windowed read seeks to 480 rows at
arbitrary payload offsets per frame -- a 1:1 page image has a 322-byte stride, so
almost none of them are 16-aligned -- and each row is decrypted on its own
(`PageReader::readWindow()`, `WalletStore.cpp`). Anything chained would have forced
a decrypt from byte zero per row: 480 passes over up to 585 KB for one pan.

Two offsets live in that loop and they are **not the same number**: the file offset
includes the 32-byte cleartext header, the CTR offset does not. The keystream is
indexed from the first byte of the payload.

**Decrypted in place, always.** The read already lands in the framebuffer and a
second 48 KB buffer does not exist to land in -- with BLE up the largest contiguous
block is about 43 KB (`map-memory.md:57`). CTR makes that free: it is an XOR over
the bytes where they lie, needing 16 bytes of keystream and nothing else.

One keystream per (asset, version), used once. A version bump changes the IV, which
is what stops one keystream covering two plaintexts.

## The plaintext hash is not a MAC

The header's 8-byte `sha256_prefix` and the manifest's full `sha256` both cover the
**plaintext**. On an encrypted asset the prefix is therefore checked **after**
decryption (`readWholeScreen()`, `WalletStore.cpp`), and it earns its keep twice:

- it catches a corrupt payload, as it did before encryption;
- it catches **a wrong key**, which would otherwise put 48 KB of noise on the panel
  looking exactly like a hardware fault.

It is **not a MAC**. It does not authenticate the ciphertext, and anybody who can
rewrite the card rewrites the prefix along with it. Only the manifest is
authenticated. Saying "integrity-checked" about an asset here means "we would
notice a bad sector", not "we would notice an attacker".

No hash check on a **page-image window**: a window is a fraction of the payload, so
there is nothing to check it against. A wrong key cannot get that far anyway -- the
manifest that named the asset would not have authenticated.

## Manifest: the tag verifies before anything is parsed

```
manifest.enc = "EWM1" (4) | u8 version=1 | u8 flags | u8 nonce[12]
               | u32 plaintextLen | ciphertext | u8 tag[16]
```

`parseManifestEnvelope()` reads the envelope, `feedEncryptedManifest()`
(`WalletStore.cpp`) verifies the tag over the whole ciphertext, and only then feeds
the plaintext to the same `StreamingJsonParser` the cleartext path uses, in the same
256-byte bites. **There is no streaming path for an encrypted manifest and there
must not be one**: GCM only authenticates once it has seen everything, and parsing
JSON that has not authenticated yet is the mistake this ordering exists to prevent.

**The envelope itself is not authenticated.** The generator seals with no associated
data, so magic, version, flags and `plaintextLen` are all outside the tag. A spliced
`plaintextLen` is caught by the structural check that it must equal the ciphertext
length, not by GCM. Worth knowing, because the natural assumption is the opposite.

A tag failure is a hard error with one message for two causes -- wrong key or
altered file -- because GCM cannot distinguish them, and pretending otherwise would
be a lie about what was checked.

### The manifest has to fit in RAM

The cap is **32 KB** (`kMaxEncryptedManifestBytes`). Above it the wallet says
"Wallet list is too big for this device" instead of failing an allocation.

In documents rather than bytes: the demo tree's manifest is **8.8 KB for one
document** with all three tile levels, 21 assets and one code. So the cap is about
**three or four documents of that shape**, and considerably more for a page-image
tree or a document whose 1:1 grid is small -- the tile arrays are most of the bulk.
That is a real limit and it is stated rather than discovered: a rider with ten
passports' worth of tiles hits it.

Below the cap the allocation can still fail when BLE holds the big blocks; that
lands on the same message.

The plaintext is wiped (`secureWipe()`) before the buffer is freed. It held every
title in the wallet.

**`manifest.bak` is not read.** The format keeps a backup of the previous good
`manifest.enc` and this firmware ignores it: a fallback that silently loads an older
manifest would show a document set that does not match the assets on the card. What
a recovery path should do is **open** -- it needs a decision about whether the rider
is told, and there is no evidence yet about which failure actually happens in the
field.

## The key's lifetime

```
NVS:  dsecret 32 B | salt 16 B | iters u32 | wrap 60 B | fails u8
RAM:  K, while unlocked, in wallet::Session
```

`KEK = PBKDF2-HMAC-SHA256(password = PIN bytes || deviceSecret, salt, iters)`,
and the wrap is AES-256-GCM over K under that KEK (`WalletKeyStore.cpp`).

- **`deviceSecret` is 32 random bytes made on this device** the first time anything
  provisions it, and it never leaves. That is why the laptop cannot compute the KEK
  and never sees the wrap: a leaked `provision.json` is a leaked key, but it is not
  a wrap, and it is useless against a different device.
- **PIN bytes are the ASCII letters** `U`/`D`/`L`/`R`, exactly as `provision.json`
  spells them. A phone app has to send the same spelling.
- K is set on a successful unwrap and **cleared explicitly** on every one of:
  sleep (`enterDeepSleep()`, first thing it does), `CMD:WALLETLOCK`, a fresh
  provisioning, and **120 seconds of idle** with a wallet screen up
  (`Session::expireIfIdle()`, touched by any button so the timeout measures
  idleness, not how long the screen has been open). Reboot needs no code: deep sleep
  wake is a chip reset.
- The wipe is `secureWipe()`, a volatile write loop, not `memset` and not a
  destructor. A `memset` over a buffer that is about to die is a dead store and a
  release build may drop it, which is exactly how a key survives a "zeroize"; a
  destructor is a promise about a code path, and the sleep path is not one to make
  promises about.

The typed PIN is wiped after every attempt and again in `onExit()`
(`WalletUnlockActivity.cpp`), and the KDF password buffer -- which holds the PIN
next to the device secret -- is wiped before `deriveKek()` returns.

## What the PIN is for

Six to ten of four symbols is 4^6 to 4^10, i.e. **2^12 to 2^20**. That is small, and
it is not the strength:

- against **guessing on the device**, the rate limiter is the defence;
- against **an attacker with the flash image**, the device secret is what makes the
  wrap worth anything, and PBKDF2 iterations only add a constant factor. That case
  is out of scope anyway (see the threat boundary).

A direction sequence rather than digits because the device has four direction
buttons and no keyboard, and because a rider enters it in gloves at a roadside. The
screen shows **dots only** -- one per symbol, never which symbol -- so a shoulder
learns the length and no more.

### Rate limiting is a UX defence, not a cryptographic one

| consecutive failures | next attempt waits |
|---|---|
| 1-3 | nothing. Three fat-fingered presses cost nothing |
| 4 | 1 s |
| 5 | 2 s |
| 6 | 4 s |
| 7 | 8 s |
| 8 | 16 s |
| 9 | 30 s (capped, so the screen never looks hung) |
| 10 | **locked out**: the wrap is refused until the device is provisioned again |

`pinFailureDelayMs()` / `pinIsLockedOut()`, `WalletCrypto.h`. The counter lives in
NVS and is written **before** the next attempt is allowed, so pulling the power
mid-guess does not buy a free retry -- and the delay earned by a past session is
applied again when the unlock screen opens.

It slows a person at the device. It does nothing about the flash-image case, and
the hard stop at ten exists precisely because a 2^12 PIN loses to unlimited tries.

## The test commands, and the flag that keeps them out of a release

Five serial commands exist purely so a host can drive the crypto path with nobody
at the device. **All five are gated behind `-DENABLE_WALLET_TEST_CMDS=1`, which is
set in `[env:default]` and in no release environment** -- `gh_release`,
`gh_release_rc`, `slim` and `sticky` all leave it unset, so a shipping binary cannot
carry them (`platformio.ini`, `src/main.cpp`).

```
CMD:WALLETPROVISION <keyhex64> <pin> <salthex32> <iters> [force]
                      -> WALLETPROVISION_OK iters=50000 provision_us=<n>
CMD:WALLETUNLOCK <pinsymbols>
                      -> WALLETUNLOCK_OK unwrap_us=<n>
                      -> WALLETUNLOCK_ERR <reason> attempts=<n> of 10 wait_ms=<n>
CMD:WALLETSTATUS      -> WALLETSTATUS provisioned=1 unlocked=0 attempts=0 of 10
                         wait_ms=0 manifest=enc items=-1 idle_left_ms=0 iters=50000
CMD:WALLETLOCK        -> WALLETLOCK_OK was_unlocked=1
CMD:WALLETPBKDF2 <n>  -> WALLETPBKDF2_OK iters=n us=<n> ms=<n> iters_per_s=<n>
```

Measured, by building the same environment both ways: the five commands cost
**4,928 bytes of flash** (`firmware.bin` 3,982,048 with them, 3,977,120 without;
`.flash.text` -3,362, `.rodata` -1,568) and **no RAM**. With the flag at 0 the build
is clean -- no unused-variable or dead-code warnings -- so the gate is a real
compile-out, not a runtime `if`.

**The trade in that flag, stated because it is a real one.** The commands are ON in
the environment everybody builds and flashes day to day, not opt-in through
`platformio.local.ini` like the grayscale bench. Reason: PIN entry is four physical
buttons, so without them the entire list under "What the host tests cannot reach"
stays unreached unless a person is standing at the device -- and a verification path
that needs an untracked file edited first is one that stops being run. What it costs
is that a **dev** device accepts "unlock this wallet" over USB, which is already
inside the threat boundary above: a device in hand with a cable is readable. A
release build has neither the commands nor that exposure.

### CMD:WALLETUNLOCK drives the real path

It calls `KeyStore::tryUnlock()` -- **the same function the PIN screen calls**, with
the same unwrap, the same rate limiter and the same session. That is deliberate: a
test command that installed K directly would verify nothing about the thing it is
meant to verify, and the paths could then drift without anybody noticing.

So a wrong PIN over serial **consumes an attempt**, gets the same refusal a person
gets, and moves the same persisted counter. `attempts=<n> of 10` and `wait_ms=<n>`
come out of the reply, so the limiter can be walked from a script: three free, then
1 s, 2, 4, 8, 16, 30 (capped), then `locked_out` at ten. The reasons are stable
one-word tokens -- `wrong_pin`, `rate_limited`, `locked_out`, `not_provisioned`,
`malformed_pin` -- and they are part of the command's contract, pinned by
`WalletUnlockOutcomes.EveryResultHasItsOwnStableToken`. `malformed_pin` costs no
attempt: a typo is not a guess.

`not_provisioned` prints a second line saying to run `CMD:WALLETPROVISION` first.

**The rate-limit gate is the session's, not the screen's** (`Session::armRetryDelay`
/ `retryWaitMs`). Both paths arm and read one gate, so neither can be used to step
around a delay the other is enforcing, and `CMD:WALLETLOCK` deliberately does **not**
reset it -- locking must not be a way to clear a penalty.

### CMD:WALLETSTATUS, so a check reads state instead of guessing it

One line: provisioned, unlocked, the attempt count and the cap, the delay being
enforced, which manifest is on the card (`enc` / `json` / `none`), the item count,
the idle time left on the key, and the provisioned iteration count.

`items=-1` means **not knowable right now**, not zero -- an encrypted tree with no
key held cannot be counted, because the titles are inside the manifest. Two
different answers, two different values.

### CMD:WALLETLOCK says whether it did anything

`was_unlocked=1` if it dropped a key, `0` if the wallet was already locked. A script
proving the unlock path twice in one session needs to tell those apart.

## Provisioning: a test path, and it says so

The wallet key and the PIN are supposed to arrive from the phone over BLE (P4/P6).
That app does not exist, so the laptop writes `provision.json` and
`CMD:WALLETPROVISION` (above) pushes it in.

**The key crosses a USB cable in the clear and lands in any serial log of that
session.** The command logs two loud lines saying so every time it runs, and it is
what makes the whole crypto path verifiable without a phone. It goes when BLE
provisioning lands.

It **refuses an already provisioned device** unless the fifth argument is `force`:
overwriting a wrap makes every asset already on the card unreadable for ever, and
that has to be asked for, not defaulted into. A malformed key, salt, PIN or
iteration count is refused with a usage line -- never zero-padded into a key that
half works. A successful provisioning drops any session key, because it may be a
different wallet.

`--force`-style overwrite, a bad hex length and a short PIN are all covered by the
host tests; the NVS write itself is not (see below).

## The PBKDF2 measurement: the command exists, the number does not

The format doc's **50,000 iterations is a placeholder off a laptop rate** and says
so. `CMD:WALLETPBKDF2 <n>` times the real thing on the chip at full clock (power
saving is dropped for every `CMD:` first -- at 10 MHz the number would be a lie,
`power-management.md`), and `CMD:WALLETPROVISION` reports `provision_us` for the count
it just provisioned, so a real provisioning also produces the figure.

**Nothing has run it.** This phase had no device access, and an estimate is what the
number is supposed to replace, so none is offered here. What to run, and what to do
with it:

```
CMD:WALLETPBKDF2 1000          # a fast probe
CMD:WALLETPBKDF2 50000         # the current placeholder
```

Both commands cap the count at **200,000** (`kMaxProvisionIterations`). Not
arbitrary: PBKDF2 is one blocking mbedtls call, the serial commands run on the main
loop, and the task watchdog panics after 5 s (`sdkconfig.defaults`,
`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`). The cap is far above any count that could be
chosen for a sub-second unlock, so it cannot get in the way -- and if a run does trip
the watchdog, **the reboot is itself the answer** that the count is too large.
`WALLETPROVISION` reports `provision_us`, which is PBKDF2 plus the wrap plus the NVS
writes: an upper bound on an unlock, not the clean figure.

`iters_per_s` comes straight out of the reply. The count to put in the format doc is
the **largest one whose `ms` stays under 1,000** -- that is the unlock's whole
budget, because everything else in an unlock is a screen refresh. Round it down to
something round, and change `provision.json`'s count rather than the format: the
count is provisioning data, not part of the on-disk contract.

## Heap

**Derived from the type and allocation sizes, not measured** -- this phase could not
run on hardware. Two log lines exist so the measured numbers come out of the next
flash for free, attributable to a screen rather than to whatever the 10-second `MEM`
line happened to catch:

```
WALLETPIN  heap on the unlock screen: <free>, <largest block>
WALLET     heap with the browse list up (encrypted manifest): <free>, <largest block>
```

What the derivation says to expect:

| screen | heap while up | where it comes from |
|---|---|---|
| unlock | **~120 bytes**, transient only during an unwrap | the activity object plus a 42-byte KDF password, a 32-byte KEK, a 60-byte wrap and a 32-byte K, all on the stack and all wiped |
| browse, encrypted manifest | **1.2 KB steady** + a **transient spike of the manifest's file size** (8.8 KB for the demo tree, capped at 32 KB) | 24 `ItemEntry` rows as before; the spike is the one buffer the ciphertext goes into and the plaintext comes out of, freed before `listItems()` returns |
| viewer / code screen | **as P2** -- no crypto buffer at all | CTR is an XOR in place; the framebuffer is the only destination |

The spike is the number that matters, and it is why the cap is stated in documents
above. Nothing on the wallet path allocates per asset, per row or per frame.

`.bss` grew by exactly **48 bytes**: `wallet::Session`, which is the 32-byte key, a
flag and a timestamp. That is the only static cost of encryption.

## What the host tests cover, and what they cannot reach

`test/wallet/WalletTest.cpp`, against **verbatim generator output** in
`test/wallet/fixtures/enc/` (an encrypted `manifest.enc` and the encrypted QR
asset, built with the committed non-secret test key):

- the CTR IV, byte for byte, including the little-endian version word and the carry
  out of the counter word;
- the offset split, and **decryption at twelve offsets** -- 0, 1, 15, 16, 17, 99,
  100, 322, 644, 4800, 47900, 47999 -- each proved equal to the same slice of a
  whole-payload decrypt. That is the design-B property, tested against real
  ciphertext;
- the cross-tree check: the decrypted asset hashes to the value the **cleartext**
  tree's manifest states for the same asset id. Two trees, one plaintext, and no
  expected-plaintext fixture;
- a one-bit-wrong key and a wrong version both **fail the plaintext hash**;
- the manifest envelope, the tag verifying, and the tag **failing** on a flipped
  ciphertext bit, a flipped tag bit and the wrong key -- each proved, not assumed;
- the envelope refusing wrong magic, an unknown version, a truncated file and a
  spliced `plaintextLen`;
- the KEK password being PIN-then-secret, the wrap round-tripping, and the unwrap
  **failing** for a wrong PIN and for the same PIN on a different device secret;
- the PIN normaliser, the rate-limit table, the hex decoder's exact-length rule and
  `secureWipe()`.

The host binds that shared arithmetic to **OpenSSL**; the device binds it to
**mbedtls**. Two independent primitive implementations agreeing on ciphertext a
third one (Python `cryptography`) wrote is the strongest evidence available without
hardware.

**What none of it reaches**, and what a hardware run has to confirm:

- mbedtls's own calls: `mbedtls_aes_crypt_ecb`, `mbedtls_gcm_auth_decrypt`,
  `mbedtls_pkcs5_pbkdf2_hmac_ext`. The in-place GCM decrypt in particular reads
  `output == input`, which mbedtls allows (`gcm.c` refuses only a *partial* overlap)
  -- **read off the source, never executed here**;
- NVS: every `Preferences` call, the device secret's first generation, and whether
  the failure counter survives a power cycle in practice;
- the unlock screen's *drawing*: dots, the delay message, the lockout message, and
  that the PIN is never spelled on the panel. Its *logic* is now reachable from a
  host through `CMD:WALLETUNLOCK`, which runs the same `KeyStore::tryUnlock()`;
- `esp_fill_random`'s quality;
- the PBKDF2 timing, which is the whole point of the command above;
- the real heap figures.

## Status

- The on-disk contract: **frozen**, parent repo `docs/wallet-format.md` section 11.
  Nothing here diverges from it; everything in it that this firmware reads was
  checked against real generator bytes.
- CTR arithmetic, the IV, the offset property, the envelope, the KEK password, the
  PIN codec, the rate-limit table: **host-tested** against verbatim generator output
  with an independent crypto library, 14 cases, and every failure path proved to
  fire.
- The encrypted tree **rendered host-side** through the device's own reader code
  (`wallet_preview --key`): the decrypted QR is **byte-identical** to the cleartext
  tree's render and still decodes to the exact payload through `zxing-cpp`.
- Flash and static RAM: **measured**. Against the P2 tip: `+17,216` bytes of binary
  with the test commands compiled in, `+12,288` without them, and `+48` bytes of
  `.bss` (the session key holder) either way.
- mbedtls calls, NVS, the unlock screen, the idle timeout, the sleep wipe:
  **written and compiled, never run** on this side. `CMD:WALLETUNLOCK` now makes all
  of them reachable from a host with nobody at the device, which is what they were
  waiting for.
- The serial reply tokens and the rate-limit sequence a host script keys on:
  **host-tested** (`WalletUnlockOutcomes.*`), so a rename or an unmapped result fails
  a test instead of a script.
- PBKDF2 iterations per second on the C3: **not measured.** The command is there;
  the number is the next thing anybody with the device should collect.
- Heap: **derived**, with two log lines waiting to replace the derivation.
- `manifest.bak`: **not read.** Open, and it needs a decision about what the rider
  is told before it is written.
