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
  plaintext flash, and `esptool` dumps flash. The pattern slows a person pressing
  buttons; it does not slow anybody who takes the flash image away and attacks the
  wrap offline. The start cell (`pstart`) is in that same plaintext flash, which
  costs nothing -- it is on the panel before the first press anyway.

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

## K and the pattern are two different secrets

The one thing a reader of this document gets wrong first. **The pattern decrypts
nothing.** It opens the box that K sits in, on this board.

```
pattern + deviceSecret  --PBKDF2-->  KEK
KEK + wrap (from NVS)   --unwrap-->  K
K + a file on the card  --AES----->  a document on the panel
```

| | K | pattern |
|---|---|---|
| what it is | 32 random bytes | a walk of 8 to 12 cells on a 4x4 grid |
| made where | the phone, or `walletgen.py` on the laptop | chosen by the rider |
| scope | one wallet, every device that reads it | one device |
| what it unlocks | every asset and the manifest | the wrap, and only on this board |
| lives where | RAM while unlocked; wrapped in NVS between unlocks | nowhere -- wiped after every attempt |

Four consequences, and each one is a design property rather than a side effect:

- **Changing the pattern costs nothing.** The same K is re-wrapped under a new
  KEK and a new salt. No asset is re-encrypted and the card is not touched.
- **The same K reads the same card on any device, with a different pattern on
  each.** The pattern is tied to `deviceSecret`, which never leaves the board, so
  a pattern carries no meaning off the board that made it.
- **Whoever holds K needs no pattern at all.** K reads the card on a laptop. That
  is why a leaked `provision.json` is a leaked wallet while a shoulder-surfed
  pattern is not, and why the phone -- which holds K -- never learns the pattern.
- **The pattern is the only route to K through the product**, and not the only
  route to K. An attacker who dumps the flash has the wrap and attacks it offline,
  with no rate limiter and no 1 s PBKDF2 in the way. See the threat boundary
  above; `scripts/pattern_space.py` counts the space it would search.

The mechanism, the NVS layout and the wiping rules are "The key's lifetime"
below.

## Assets: CTR, because a window has to be seekable

`IV = sha256(payload)[0:8] || u32 version little endian || u32 zero`
(`buildAssetIvFromHash()`, `src/activities/wallet/WalletCrypto.h`), chosen by
`flags` bit 1 (`kFlagContentIv`). An asset written before 2026-11-23 has that bit
clear and keeps the old `assetId || version` form (`buildAssetIv()`);
`buildAssetIvForAsset()` is the one place that decides, so no call site can pick
the wrong form and decrypt to silent noise.

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

### The IV follows the bytes

One keystream per (payload, version), used once.

**It used to be per (assetId, version), and that was not unique.** An assetId is a
hash of the recipe -- panel, item, page, type, index, version -- with no content in
it, which is deliberate: it is what makes the phone's delta sync fall out of the
ledger arithmetic instead of being computed (android `WalletStore.addItem`).
Nothing ever incremented the version either. So re-importing a document under the
same title and the same source filenames wrote **different bytes under the same
keystream**, and an attacker holding both cards gets `P1 xor P2`. The belief that
"a version bump changes the IV" was true and useless: no bump existed.

The payload's sha256 prefix is already in the asset's cleartext header, so the
device has it before it has decrypted anything, and it changes exactly when the
bytes change. Identical payloads still produce identical ciphertext -- no new leak,
the plaintext hash is in the clear either way.

**Proved end to end on an X4 Pro, 2026-11-23**, on the directional PIN this
screen no longer has -- the crypto below is unchanged by the gate swap, the
screen in it is not. `tools/walletgen.py --demo
--new-key --pin UDLRUD` wrote an encrypted tree whose three assets all carry
`flags=0x03`; the files went to the card over BLE (`tools/blepush.py`, paths
relative to `/explorink`), `CMD:WALLETPROVISION` stored the wrap, 18,000 PBKDF2
iterations in 1,047 ms, `CMD:WALLETUNLOCK` unwrapped it in 1,027 ms, and the
browse list and the page both drew -- one 48,000-byte FIT asset, 165 ms off the
card, decrypted with the payload-hash IV before it reached the panel. A page that
rendered at all is the proof: a wrong IV is noise, not a document. **Both timings
are one run per board**, ESP32-S3: 1,047 ms to provision on the X4 Pro, 1,047 ms
to provision and 1,027 ms to unwrap on the T5 S3 Pro. **The C3 figure was
measured on 2026-11-26**, on an X3 running `env:default`: 903,157 us to
provision and 902,558 us to unwrap, same 18,000 iterations. It had stood here
as "unmeasured -- an S3 does not answer for it" since the wrap was written; the
19,900 iters/s the generator's default was sized against predicted ~904 ms, so
the default was right all along
(`docs/device-shots/2026-11-23-x4pro-wallet-page-contentiv-480x800.png` in the
parent repo).

**Not a new idea.** An IV derived from the plaintext's own hash is convergent
encryption -- Tahoe-LAFS, ZFS `dedup=on` -- and the nearest standard is AES-SIV,
RFC 5297. **The difference is named rather than glossed over:** SIV computes its
synthetic IV with a MAC under a key, so nothing about the plaintext leaks out of
it. Here it is an unkeyed `sha256(payload)[0:8]`, and that value already sits in
the asset header in the clear. So somebody who guesses a document can confirm the
guess from the header -- which was true before this change too, because the
header is cleartext on purpose (a recovery scan rebuilds a lost manifest from
it). A collision on 64 bits arrives around 2^32 assets, orders of magnitude above
what a wallet holds. `read`, not measured.

**And once more with no test command anywhere in the binary.** Both boards were
re-flashed from the branch itself -- `CMD:WALLETSTATUS` answers nothing on it,
which is what a build without `ENABLE_WALLET_TEST_CMDS` has to do. A cold boot
then put the X4 Pro's Home > Wallet row on the PIN screen, because the tree is
encrypted and a reboot takes the session key with it; six button presses
(`up down left right up down`, which is `UDLRUD`) and `confirm` unlocked it and
the browse list came back. That is the rider's own path end to end: no serial
command took part in it except the button injector standing in for a thumb
(`docs/device-shots/2026-11-23-x4pro-wallet-pin-screen-480x800.png`).

**The T5 S3 Pro needed a generator row, not a firmware change.** The first tree
pushed to it was refused -- `manifest panel x4 800x480/48000, device
960x540/64800` -- because `tools/walletgen.py` knew only the `x3` and `x4`
panels. That is the manifest's panel gate doing its job: one asset set per
wallet, so a set cut for another screen is refused whole
(`WalletAsset.h`, `panelMatches()`). A `PanelProfile("t5s3", 960, 540)` row in
the generator was the whole fix, and the same chain then ran on that board too:
BLE push, PIN screen, list, page at 960x540. The manifest had already decrypted
there before the fix, so the GCM half was verified on both boards either way.

Found by review on 2026-11-23, while bringing the wallet branch up to `develop`.
Fixed on all three implementations at once (device, phone, `tools/walletgen.py`)
behind the flag bit, and the parity fixtures were regenerated, which is what proves
the three agree.

## Driving the whole chain from a host

**This recipe was rewritten for the pattern gate on 2026-11-26 and has not been
run since.** The chain is the same; the two commands that took a PIN take a
pattern now. A dated run of the PIN version is recorded above and stays there as
evidence of what the panel did on 2026-11-23, not as instructions.

The bench path, end to end. **`ENABLE_WALLET_TEST_CMDS`
lives only in `env:default`** (`platformio.ini`), so every other environment needs
it added for the run and taken away afterwards -- a release build must never carry
these commands.

```bash
# 1. a tree, its key and its pattern. --panel picks the board: x4, x3 or t5s3.
#    567ba98c is right, right, down, left, left, left, down from cell 5 --
#    eight cells, the wallet's floor. walletgen refuses a path the grid cannot
#    walk, so a typo is caught before the card is written.
python3 tools/walletgen.py --demo --title "Bench" --panel x4 \
  --out /tmp/wt/tree --new-key /tmp/wt/key.hex --pattern 567ba98c \
  --provision-file /tmp/wt/provision.json

# 2. a build that answers the wallet commands, on a board that is not env:default
PLATFORMIO_BUILD_FLAGS="-DENABLE_WALLET_TEST_CMDS=1" pio run -e t5s3pro

# 3. the card. BLE only advertises with the map open (see ble-advertising.md),
#    and --chunk is not optional here (../../docs/synchro.md, blepush and the MTU).
printf 'CMD:GOTO_MAP\n' > /dev/serial/by-id/<board>
python3 tools/blepush.py /tmp/wt/tree/manifest.enc wallet/manifest.enc --chunk 180
for f in /tmp/wt/tree/*/*.dat; do
  python3 tools/blepush.py "$f" "wallet/$(basename $(dirname $f))/$(basename $f)" --chunk 180
done

# 4. the wrap, then the key in the session. Both take ~1 s at 18,000 iterations.
#    key, pattern, salt and iteration count all come out of provision.json.
CMD:WALLETPROVISION <keyhex64> 567ba98c <salthex32> 18000
CMD:WALLETUNLOCK 567ba98c
# CMD:WALLETSTATUS now reports pattern_start=5, which is what the gate opens on.
```

**Since T-2094 there is a path with no laptop in it at all, and it is the
product's path. Proved on both chip families, 2026-11-26** -- a T5 S3 Pro and an
X3, each with its NVS wiped first, set up with buttons only. The S3 stored an
18,000-iteration wrap on start cell 6 and unwrapped it after a reboot in
1,026 ms; the C3 did the same on start cell 9 in 909 ms, which is the figure
this doc already carried for a laptop-made wrap. **The save itself is the number
to watch**: 2,543 ms of main loop on the S3 and **3,221 ms on the C3**, against
a 5 s task watchdog, and the difference is the panel rather than the KDF. The
tables are in `pattern-lock.md`, "Enrollment on the panel". On a board with no wrap and no encrypted wallet on the card,
Home > Wallet offers setup: pick a start cell, walk 8 to 12 points, walk them
again, and the device makes K, the salt and the wrap itself. Nothing above is
needed for it and nothing above can produce a key the device will accept
afterwards. The recipe below stays because a tree built by `walletgen.py` is
encrypted under a key a self-provisioned board does not have, so it is the only
way to get documents onto a board until T-2095 and T-2096 land.

**The last step has a version with no command in it at all**, and it is the one
worth running: reboot the board, open Home > Wallet, and walk the pattern on the
buttons. For `567ba98c` the cursor starts on cell 5 and the presses are
`right right down left left left down`, then confirm. That path uses nothing a
release build lacks, which is why it is the one that proves the feature rather
than the plumbing.

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
NVS:  dsecret 32 B | salt 16 B | iters u32 | pstart u8 | wrap 60 B | fails u8
RAM:  K, while unlocked, in wallet::Session
```

`KEK = PBKDF2-HMAC-SHA256(password = pattern bytes || deviceSecret, salt, iters)`,
and the wrap is AES-256-GCM over K under that KEK (`WalletKeyStore.cpp`).

- **K itself is made on the device** since T-2094, by `wallet::randomBytes()`
  during enrollment, and it never leaves either -- there is no command and no
  characteristic that can read it out, by construction. The phone will send
  plaintext documents and the device will encrypt them on the way to the card
  (T-2095, T-2096). `tools/walletgen.py` and `CMD:WALLETPROVISION` still make a
  key on a laptop; that is the bench path, and it exists only until those two
  land (../../docs/wallet-provisioning.md, "Decided 2026-11-26").
- **`deviceSecret` is 32 random bytes made on this device** the first time anything
  provisions it, and it never leaves. That is why the laptop cannot compute the KEK
  and never sees the wrap: a leaked `provision.json` is a leaked key, but it is not
  a wrap, and it is useless against a different device.
- **Pattern bytes are the gate's own encoding**: lower-case `0`-`9` then `a`-`f`,
  one character per grid cell visited, start cell first, exactly as
  `provision.json` spells it. `patternlock::Entry::encode()` produces that string
  and nothing translates it, so a phone app has to send the same spelling.
- **`pstart` is the start cell**, stored beside the salt because the wrap says
  nothing about the path that made it. It is **not a secret** -- the gate draws
  its cursor there before the first press -- but it has to be stored, because a
  gate opened on any other cell refuses every pattern a rider can walk, for ever
  (`src/components/lock/PatternLock.h`, `isWellFormed`). Provisioning reads it off
  the pattern it is wrapping, so the two cannot disagree.
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

The walked pattern is wiped by the gate after every attempt
(`PatternLockActivity::submit()`), the normalised copy is wiped by
`KeyStore::tryUnlock()`, and the KDF password buffer -- which holds the pattern
next to the device secret -- is wiped before `deriveKek()` returns.

**During enrollment there are two of them**, because the pattern is asked for
twice. The first walk lives in the gate screen between the two phases and is
wiped on the match, on the mismatch, on BACK and in `onExit()`; the second is
wiped as soon as the comparison is done, whichever way it went
(`PatternLockActivity::submitEnroll()`). K and the salt are locals in
`WalletUnlockActivity::provisionWith()` and are wiped after the wrap is stored,
with K handed to the session first so the rider is not asked to unlock the
wallet they have just set up.

## What the pattern is for

A walk of 8 to 12 cells on a 4x4 grid is **806,270 paths from an interior start,
about 2^19.6** -- counted by `scripts/pattern_space.py` and pinned by
`test/pattern_lock/`. That is small, and it is not the strength:

- against **guessing on the device**, the rate limiter is the defence;
- against **an attacker with the flash image**, the device secret is what makes the
  wrap worth anything, and PBKDF2 iterations only add a constant factor. That case
  is out of scope anyway (see the threat boundary).

A grid walk rather than digits because the device has four direction buttons and
no keyboard, and because a rider enters it in gloves at a roadside.

### What replaced what, and what was given up

Until 2026-11-26 this was a **directional PIN**: 6 to 10 presses of U/D/L/R, so
4^6 to 4^10, 12.0 to 20.0 bits. It was replaced by `PatternLockActivity`, the
shared gate (`docs/pattern-lock.md`), which the whole device can reuse and which
a second caller -- a screen lock -- already wants.

The length range was chosen to keep the numbers above rather than to match the
gate's own default. 8 to 12 points is 7 to 11 presses, one more than the PIN at
each end, for **12.3 to 19.6 bits**; the gate's default floor of 4 points is 46
walks, 5.5 bits, which would have been a silent loss of six and a half bits
against a doc that still promised twelve.

**One property was given up and is not coming back.** The dot PIN drew one dot
per symbol and never which symbol, so a shoulder learned the length and no more.
A grid cannot do that: the cursor has to be visible or the grid is pointless, and
a watcher who sees every press has the pattern. Nothing in the product text may
claim otherwise. `docs/pattern-lock.md`, "What the hidden path buys", has the
rest, including that `CMD:SCREENSHOT` is ungated and puts the cursor in a
framebuffer a host can poll (T-2081).

### A wallet provisioned before the gate cannot be opened, and there is no migration

NVS holds the wrap, the salt, the iteration count -- and, since the pattern
gate, `pstart`. A device provisioned under the directional PIN has every one of
those except the last, which means the gate has no cell to open on and nothing a
rider walks can match.

**Nothing can migrate it.** The KEK is derived from the secret, and no amount of
reading NVS re-derives a PIN from a wrap. The wrap is intact and the key inside
it is intact; the only thing that could open it has stopped existing. Such a
wallet has to be provisioned again, and the documents already on the card are
then readable only if K is held somewhere else -- the phone's own keystore, in
practice (`../../docs/android-wallet.md`).

The screen says exactly that and no more: **"This wallet predates the pattern
lock / Set it up again"**. It is a separate state from "no wallet key on this
device" on purpose. The first cut lumped the two together, and an X4 Pro
carrying a 2026-11-23 wallet showed the panel claiming it had no key while
holding one (2026-11-26). A screen that says something untrue about a rider's
documents is worse than one that says nothing.

Scope, so this is not read as bigger than it is: the only devices in that state
are the two test boards. `ENABLE_WALLET` is in `base` and so reaches release
images, but no release has ever been provisioned with a real wallet
(`wallet-plan.md`).

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

`unlockFailureDelayMs()` / `isLockedOut()`, `WalletCrypto.h`. The counter lives in
NVS and is written **before** the next attempt is allowed, so pulling the power
mid-guess does not buy a free retry -- and the delay earned by a past session is
applied again when the gate opens.

It slows a person at the device. It does nothing about the flash-image case, and
the hard stop at ten exists precisely because a 2^19.6 pattern loses to unlimited
tries.

**Auto-submit is off for this gate**, which is a limiter question and not a
comfort one: with it on, the gate probes the verifier after every point and a
single walk yields several candidates for one counted attempt
(`docs/pattern-lock.md`, "Counting attempts"). It would also run a 905 ms PBKDF2
per probe, four seconds against a five-second task watchdog.

## The test commands, and the flag that keeps them out of a release

Five serial commands exist purely so a host can drive the crypto path with nobody
at the device. **All five are gated behind `-DENABLE_WALLET_TEST_CMDS=1`, which is
set in `[env:default]` and in no release environment** -- `gh_release`,
`gh_release_rc`, `slim` and `sticky` all leave it unset, so a shipping binary cannot
carry them (`platformio.ini`, `src/main.cpp`).

```
CMD:WALLETPROVISION <keyhex64> <pattern> <salthex32> <iters> [force]
                      -> WALLETPROVISION_OK iters=50000 provision_us=<n>
CMD:WALLETUNLOCK <pattern>
                      -> WALLETUNLOCK_OK unwrap_us=<n>
                      -> WALLETUNLOCK_ERR <reason> attempts=<n> of 10 wait_ms=<n>
CMD:WALLETSTATUS      -> WALLETSTATUS provisioned=1 unlocked=0 attempts=0 of 10
                         wait_ms=0 manifest=enc items=-1 idle_left_ms=0 iters=50000
                         pattern_start=5
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
`platformio.local.ini` like the grayscale bench. Reason: pattern entry is a walk
driven by four physical buttons, so without them the entire list under "What the
host tests cannot reach"
stays unreached unless a person is standing at the device -- and a verification path
that needs an untracked file edited first is one that stops being run. What it costs
is that a **dev** device accepts "unlock this wallet" over USB, which is already
inside the threat boundary above: a device in hand with a cable is readable. A
release build has neither the commands nor that exposure.

### CMD:WALLETUNLOCK drives the real path

It calls `KeyStore::tryUnlock()` -- **the same function the gate's verifier
calls**, with the same unwrap, the same rate limiter and the same session. That is
deliberate: a test command that installed K directly would verify nothing about
the thing it is meant to verify, and the paths could then drift without anybody
noticing.

So a wrong pattern over serial **consumes an attempt**, gets the same refusal a
person gets, and moves the same persisted counter. `attempts=<n> of 10` and
`wait_ms=<n>` come out of the reply, so the limiter can be walked from a script:
three free, then 1 s, 2, 4, 8, 16, 30 (capped), then `locked_out` at ten. The
reasons are stable one-word tokens -- `wrong_pattern`, `rate_limited`,
`locked_out`, `not_provisioned`, `malformed_pattern` -- and they are part of the
command's contract, pinned by
`WalletUnlockOutcomes.EveryResultHasItsOwnStableToken`. `malformed_pattern` costs
no attempt: a typo is not a guess.

**`wrong_pin` and `malformed_pin` were the old spellings**, renamed with the gate
on 2026-11-26. Free, because this command is `env:default` only and has never
been in a release image -- which is not the same scope as `ENABLE_WALLET` itself,
which is in `base` and does reach one.

**The argument is the encoded path, start cell first.** A host cannot guess the
start cell, because it is the one character no button press produces, so
`CMD:WALLETSTATUS` prints it as `pattern_start` (`-1` when this device has none).
`CMD:WALLETPROVISION` refuses a path the grid cannot walk, and says so differently
from a bad alphabet, because a diagonal and a typo are different mistakes to fix.

`not_provisioned` prints a second line saying to run `CMD:WALLETPROVISION` first.

**The rate-limit gate is the session's, not the screen's** (`Session::armRetryDelay`
/ `retryWaitMs`). Both paths arm and read one gate, so neither can be used to step
around a delay the other is enforcing, and `CMD:WALLETLOCK` deliberately does **not**
reset it -- locking must not be a way to clear a penalty.

### CMD:WALLETSTATUS, so a check reads state instead of guessing it

One line: provisioned, unlocked, the attempt count and the cap, the delay being
enforced, which manifest is on the card (`enc` / `json` / `none`), the item count,
the idle time left on the key, the provisioned iteration count, and the stored
pattern start cell.

`items=-1` means **not knowable right now**, not zero -- an encrypted tree with no
key held cannot be counted, because the titles are inside the manifest. Two
different answers, two different values.

### CMD:WALLETLOCK says whether it did anything

`was_unlocked=1` if it dropped a key, `0` if the wallet was already locked. A script
proving the unlock path twice in one session needs to tell those apart.

## Provisioning: a test path, and it says so

**The product path is now enrollment on the device** (T-2094): the rider walks a
pattern on the panel and the device makes K itself. The command below is the
bench path and stays only until the phone can send plaintext documents
(T-2095, T-2096), because a tree built by `walletgen.py` is encrypted under a key
a self-provisioned board does not have.

The wallet key and the pattern were supposed to arrive from the phone over BLE (P4/P6).
That app does not exist, and under the 2026-11-26 decision it never will carry a
key at all, so the laptop writes `provision.json` and
`CMD:WALLETPROVISION` (above) pushes it in.

**The key crosses a USB cable in the clear and lands in any serial log of that
session.** The command logs two loud lines saying so every time it runs, and it is
what makes the whole crypto path verifiable without a phone. It goes when the
device can receive plaintext documents and seal them itself.

It **refuses an already provisioned device** unless the fifth argument is `force`:
overwriting a wrap makes every asset already on the card unreadable for ever, and
that has to be asked for, not defaulted into. A malformed key, salt, pattern or
iteration count is refused with a usage line -- never zero-padded into a key that
half works. A successful provisioning drops any session key, because it may be a
different wallet.

**It also refuses a pattern the gate could not walk**, and that refusal is not a
tidiness check: a wrap derived from a path nobody can enter is a wallet that
opens for no one, with no way back. It is the only moment that can be caught,
because it is the moment the device decides what it will later demand. The walk
check and the alphabet check give different messages -- a diagonal and a typo are
different mistakes to fix.

`--force`-style overwrite, a bad hex length, a short pattern and an unwalkable
one are all covered by the host tests; the NVS write itself is not (see below).

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
the **largest one whose `ms` stays under 1,000**.

**That is not the unlock's whole budget, and this sentence used to say it was.**
"Everything else in an unlock is a screen refresh" reads as if the refresh were
free. Measured on an X3 on 2026-11-26: one CONFIRM put the main loop at
**4,109 ms** against the same 5 s watchdog -- 903 ms of KDF and about 1.8 s of
panel waits for the gate leaving and the list arriving. The KDF is the half this
cap bounds; the other half is unbounded and unmeasured on a full card. T-2089. Round it down to
something round, and change `provision.json`'s count rather than the format: the
count is provisioning data, not part of the on-disk contract.

## Heap

**Derived from the type and allocation sizes, not measured** -- this phase could not
run on hardware. Two log lines exist so the measured numbers come out of the next
flash for free, attributable to a screen rather than to whatever the 10-second `MEM`
line happened to catch:

```
WALLETGATE heap on the gate: <free>, <largest block>
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
- the KEK password being pattern-then-secret, the wrap round-tripping, and the
  unwrap **failing** for a wrong pattern and for the same pattern on a different
  device secret;
- the pattern normaliser, the walk check that stops provisioning making an
  unopenable wallet, the rate-limit table, the hex decoder's exact-length rule and
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
- the gate on an **S3** -- the whole chain above ran on an X3 only, and the gate
  compiles into every environment, so the protocol wants one board of each chip
  family (`../../docs/device-test-protocol.md`, step 1);
- the no-wrap dead end. Its twin, the lockout screen, is confirmed; this one
  needs a device with a provisioned-elsewhere card;
- `forgetWrap()` taking the start cell with the wrap;
- `esp_fill_random`'s quality;
- the PBKDF2 timing, which is the whole point of the command above;
- the real heap figures.

## Status

- The on-disk contract: **frozen**, parent repo `docs/wallet-format.md` section 11.
  Nothing here diverges from it; everything in it that this firmware reads was
  checked against real generator bytes.
- CTR arithmetic, the IV, the offset property, the envelope, the KEK password, the
  pattern codec, the rate-limit table: **host-tested** against verbatim generator
  output with an independent crypto library, and every failure path proved to
  fire.
- The pattern gate replacing the directional PIN, 2026-11-26: **host-tested**
  (863 firmware tests, 206 in `tools/tests`), built for `env:default`,
  `env:x4pro`, `env:t5s3pro` and `env:simulator` with zero warnings, and
  **run end to end on an X3** -- provision, both refusals, unlock, the panel
  gate, the browse list off a decrypted manifest, a wrong pattern, the limiter's
  full ladder and the lockout screen (`docs/pattern-lock.md`, "On the panel,
  2026-11-26"). Not yet on an S3.
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
