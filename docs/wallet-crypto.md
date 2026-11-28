# Wallet crypto v1, on the device

Phase P3. The wallet's papers are encrypted at rest and the device holds the key
only while the wallet is unlocked. The on-disk contract is frozen and lives in the
parent repo's `docs/wallet-format.md`, section 11; this document is the device
side of it -- the code paths, the key's lifetime, the threat boundary, and what is
still unverified.

The viewer itself, its screens and its button map are `wallet-viewer.md`.

**Every wallet change runs the scenarios in
[`wallet-test-scenarios.md`](wallet-test-scenarios.md)**, all of them, not the
ones that look related. That file is the regression suite and a landing rung
adds its own scenarios to it.

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
the generator was the whole fix -- **and it is stranded on the parent repo's
`wallet-cardroot` branch**, which `wallet` does not have (checked 2026-11-27).
The same chain then ran on that board too:
BLE push, PIN screen, list, page at 960x540. The manifest had already decrypted
there before the fix, so the GCM half was verified on both boards either way.

Found by review on 2026-11-23, while bringing the wallet branch up to `develop`.
Fixed on all three implementations at once (device, phone, `tools/walletgen.py`)
behind the flag bit, and the parity fixtures were regenerated, which is what proves
the three agree.

## Ingest: the device is the second writer of this format

T-2095, 2026-11-27. Until this landed, `tools/walletgen.py` was the only thing
that ever wrote an `EWA1` asset and the device only ever read one. A board makes
its own K now (T-2094), so nothing off the device can produce a tree this board
can read: the phone has to send the **plaintext** bitmap and the **device** has
to seal it.

The format does not change. Same 32-byte cleartext header, same AES-256-CTR
payload, same IV rule as everything above. What changes is who writes it.

`src/activities/wallet/WalletAssetIngest.h` is the sealer;
`MapTransferReceiver` is where it is wired in, because the BLE transfer channel
is the path a wallet asset already arrives on.

### One pass, because the header carries the IV

The 32-byte header arrives **first** and already holds `sha256_prefix` -- the
first 8 bytes of the plaintext payload's hash, which is exactly what
`buildAssetIvFromHash()` takes. So the IV is known before the first payload byte
lands, and every chunk after it is encrypted where it arrives, at a running
payload offset. No staging file, no second write of 585 KB, no 48 KB buffer.

The header the device writes is the sender's, with `kFlagEncrypted` and
`kFlagContentIv` set in byte 20 and **nothing else touched** -- geometry,
version, presentation and the hash prefix all survive. Measured on an X3,
2026-11-27: the other 31 bytes came back byte-identical.

### The prefix is trusted early and checked late

Building a keystream out of `sha256_prefix` means trusting a number nobody has
verified yet, because the payload it hashes has not arrived. So the plaintext is
hashed as it streams and compared with that prefix **before the rename**.

This check is not optional and it is not belt-and-braces. A wrong prefix is not
an error anywhere: every structural check passes, the file lands, and the asset
decrypts to noise on the panel for ever. The hash is the only thing in the system
that can catch it. Proved firing on hardware -- a flipped payload byte with an
honest CRC, so the CRC could not catch it, answered `ERR hash mismatch` and left
the previous good file untouched (X3, 2026-11-27).

### `OK` means something weaker for a wallet asset, on purpose

A tile transfer answers `OK <bytes> <crc32>` with the CRC read back **off the
card**, which is what makes it mean "the card holds these bytes"
(`MapTransferReceiver.h`, "Write to .part, rename at the end").

That cannot work here. The card holds ciphertext, and its CRC is a number the
sender has never seen -- the phone compares the reply with the CRC of the
plaintext it sent (`../../docs/android-wallet.md`, 14.5). Get that wrong and the
phone's ledger marks every asset changed for ever.

So the CRC is accumulated from the **arriving plaintext** instead, taken before
the flag byte is rewritten, and the read-back is skipped. The guarantee weakens
from *the card holds these bytes* to **these bytes arrived whole and were
written**. Nothing on the device re-reads a sealed asset to confirm it until the
viewer opens it.

### What is refused, with nothing left on the card

Decided at `begin`, before the `.part` is opened, or on the chunk that completes
the header:

| refusal | why |
|---|---|
| `ERR wallet locked` | no session key. A locked wallet gets a refusal, not a half-written document |
| `ERR no panel` | no live renderer, so the geometry gate cannot run |
| `ERR already encrypted` | the sender used a key this board does not have; sealing again buries the payload under two keystreams |
| `ERR bad asset` | magic, header parse, bit depth, an `assetType` this firmware does not know, or geometry that cannot be true |
| `ERR wrong panel` | built for another panel in the line |
| `ERR asset length` | the header's `rawLen` and the transfer's declared length disagree |
| `ERR hash mismatch` | the plaintext does not hash to the prefix the keystream came from |

**The path is the signal, not a flag on the wire.** A `.dat` under
`/explorink/wallet` is sealed; everything else takes the old path untouched. A
sender cannot ask for a document to be stored in the clear by clearing a bit, and
cannot turn a map tile into a wallet asset by setting one.

**No manifest is involved in an asset's gates**, because the manifest is its own
transfer. That is why ingest does not call `checkAssetForPanel()`: a page image's
stride normally comes from the manifest, so here it is derived from the file's own
`rawLen / height` instead. The gate refuses nothing it cannot actually prove wrong.

The manifest's own refusals (T-2096), decided the same way -- at `begin` wherever
the begin frame carries enough to decide, and nothing is written to the card until
every one of them has passed:

| refusal | why |
|---|---|
| `ERR wallet locked` | no session key. The list is sealed or it is not stored |
| `ERR manifest too big` | over `kMaxManifestPlaintextBytes`, i.e. it would seal above the cap the read path enforces. **At `begin`**, off the declared length, so it costs no bytes on the wire -- and this string means only that. The board running out of heap is `ERR oom`, wherever it happens |
| `ERR oom` | no heap for the manifest -- at `begin` for the arriving copy, or at the end for the buffer the verification reads it back into. On a C3 with BLE up the second one is the common case long before the format's cap is reached |
| `ERR not a manifest` | the first non-blank byte is not `{`, or `ManifestParser` could not read it as a manifest of a `formatVersion` this firmware knows |
| `ERR crc mismatch` | the bytes received are not the bytes the sender declared |
| `ERR manifest unreadable` | the sealed file did not open off the card. The previous list stays live and the `.part` is deleted |
| `ERR wallet locked` (at the end) | the wallet locked between the seal and the read-back -- `Session::clear()` runs on the main task against a window of a 32 KB write plus a 32 KB read. The file was perfect; saying "unreadable" would have the phone re-send it for ever |
| `ERR write failed` | the card refused the sealed bytes, or took fewer than were offered. The `.part` is deleted and the previous list stays live |
| `ERR swap failed` | the card refused a rename. Either the previous list or the new one is live, never a half |
| `ERR not a wallet file` | a path under the wallet directory that this device does not write. See the section below |

`wallet/manifest.json` is matched case-insensitively, exactly as the asset gate
is and for the same reason: SdFat compares names folded, so an exact-case gate
would let `MANIFEST.JSON` through as an ordinary file and leave a cleartext index
of every document title on the card.

### Under the wallet directory: sealed, or refused. Never stored as it arrived

**Two name gates are not a defence on their own, because FAT gives one file more
than one name.** SdFat builds an 8.3 alias for every name that is not already
8.3 -- `lfnParse()` sets `FNAME_FLAG_LOST_CHARS` and writes `~1` at `seqPos`
(`FatLib/FatFileLFN.cpp`) -- and `open()` matches a query spelled with that alias
against the same directory entry, through the plain-SFN `memcmp` branch. So
`wallet/MANIFE~1.JSO` **is** `wallet/manifest.json` on the card, and is not
`manifest.json` to any gate that compares names.

Found in review, 2026-11-27, and it was not theoretical: that path passes
`relPathIsSafe()` (a `~` is printable ASCII), misses both wallet gates, takes the
ordinary tile path, and at completion `Storage.exists()` resolves to the real
list, `remove()` deletes it and `rename()` puts the sender's bytes under a name
nothing will look up again. No key, no pairing, BLE range.

`manifest.enc` and `manifest.bak` needed no alias at all. And since the backup
became the half-swap survivor, junk written over it destroys the only list a
half-installed card has -- while on a **cleartext** card a pushed `manifest.bak`
flips `treeIsEncrypted()` and sends the rider to an unlock screen for a wallet
that has no wrap.

So the receiver refuses, at `begin`, **anything under `/explorink/wallet` that is
neither a `.dat` asset nor `manifest.json`** (`pathIsUnderWalletDir()`,
`WalletAssetIngest.h`). A name nobody can seal is a name nobody can leave in the
clear either. Nothing legitimate is lost: an encrypted tree has no `.rle`
sidecars by definition, and this firmware never reads one anyway.

**A leading space was the third spelling, and it beat all of this.** SdFat's
`FatFile::parsePathName()` opens with `while (*path == ' ') path++;` for every
component, so ` wallet/ab/<id>.dat` is the same directory entry as
`wallet/ab/<id>.dat` -- and a different string to every gate here, all of which
compare names. It passed the path check (a space is printable), missed the
whitelist, took the ordinary file path, and its completion deleted the sealed
document and renamed the sender's bytes over it, with `kFlagEncrypted` clear.
Found in a third review round, 2026-11-27.

The fix is in the path check, not in the gates: **one file has to have one
spelling before a name comparison means anything.** `relPathIsSafe()` now
rejects a component that begins with a space as well as one that ends in a
space or a dot, and it moved to `src/activities/map/MapTransferPath.h` so it
can be host-tested -- it had no test at all, which is why two of the three
spellings got past it.

**The shape is checked whole, not as a prefix and a suffix.** `wallet/<x>/x.dat`
with any `<x>` used to be a wallet asset, so `wallet/manifest.bak/x.dat` passed
the whitelist and the directory creation made `manifest.bak` a directory again
-- the gate authorised the leaf, the mkdir acted on the parents. The accepted
shape is `<walletdir>/<2 hex>/<16 hex>.dat` with the shard matching the id,
which is the only shape `buildAssetPathIn()` writes, plus `manifest.json`, plus
nothing. The wallet directory node itself is refused too: a push of the bare
path `wallet` would make a regular file where the directory has to be, and the
wallet could then never exist on that card.

### The sender's spelling is a lookup key, not an address

**This is the answer to the four rounds above, and it is a different shape from
all of them.** Each of those was an attempt to make a gate's idea of what a
path means equal SdFat's idea of it. That is a fixed point you cannot reach by
inspection: the rules live in a pinned dependency (`FatLib/FatFileLFN.cpp`),
there is a second implementation for exFAT, and the string a gate answers about
is not necessarily the string that later gets opened.

So the wallet stopped comparing and started parsing. A begin frame's relative
path is read as a **request**, not as a destination:

- `wallet/<2 hex>/<16 hex>.dat` parses to an asset id, and the card path is
  **rebuilt** by `buildAssetPath()` from those sixteen validated, folded hex
  characters. The shard directory is derived from the id, so a wallet asset has
  nowhere in it to name a directory.
- `wallet/manifest.json` parses to "the manifest", which is never written at
  the name it arrived under anyway -- it is sealed in RAM and installed at
  three absolute constants.
- Anything else under `wallet/` is refused, before the directory creation that
  cannot be undone.

What that buys: whatever a sender spells, a wallet file lands at a path built
out of firmware constants and hex digits. **An alias of a path nothing hands to
the filesystem is not reachable.** A folded case, a `~1` short name, a leading
space, a directory component -- none of them can change which file is opened,
because none of them survives being parsed into an id and rebuilt.

**Confirmed on both boards, 2026-11-27.** `wallet/D1/D1CCCB5B0744F2A2.DAT`
answered `OK` and the log named the file it actually opened:
`/explorink/wallet/d1/d1cccb5b0744f2a2.dat`. The shard directory afterwards
held exactly one entry, lower case, with `flags = 3` (encrypted, content IV)
and a ciphertext payload -- one file, built by the device, whatever the request
spelled.

**The half that is still a comparison.** A path that does not parse as a wallet
request is still written where the sender spelled it, so a spelling that
aliases *into* the wallet directory without naming it is held off by
`relPathIsSafe()` alone -- which is exactly the kind of completeness argument
this section exists to stop relying on. Ending it means parsing and rebuilding
every transfer kind, which is T-2105. Until then the wallet's guarantee rests
on two things rather than one, and that is stated here rather than implied.

**The gate runs before the directory is created, and that ordering is the
fix, not a detail.** `handleBegin` calls `ensureDirectoryExists()` on the
parent of every accepted path, and `SDCardManager` creates every component. A
gate placed after it left a hole as bad as the one it closed: a push to
`wallet/manifest.bak/x` made a **directory** called `manifest.bak` and only
then refused the file. `Storage.exists()` is true for a directory and
`Storage.remove()` cannot delete one, so every later sync would answer
`ERR swap failed` for ever with nothing in the firmware able to undo it -- and
on a cleartext card that directory also flips `treeIsEncrypted()`. Found in a
second review round, 2026-11-27, and confirmed closed on both boards:
`wallet/manifest.bak/x` and `wallet/manifest.enc/deep/x` are refused and the
wallet directory afterwards holds only its asset shards.

**Confirmed on both boards, 2026-11-27.** ` wallet/<shard>/<id>.dat` answers
`ERR bad path`; `wallet`, `wallet/manifest.bak/<id>.dat` and `wallet/<shard>/x.dat`
all answer `ERR not a wallet file`; the wallet directory afterwards holds only
its asset shards, and a normal asset and manifest still install and open.

**What the attack actually costs, seen on a card.** A run against a build
*without* these gates (the published v0.2.3, flashed by accident -- see
`../../docs/PROGRESS.md`) left `d1cccb5b0744f2a2.dat` with its `flags` byte at
0 and its payload reading `ATTACKER PLAINTEXT`. That is the end state the
whitelist exists to prevent: a document the viewer will draw as genuine because
nothing says it is encrypted. It is not evidence about the leading space
specifically -- that build has no wallet gate at all to walk around. The
leading space is proven by SdFat's own source, by the host test, and by the
fixed build refusing it.

**Two things this does not close.** An 8.3 alias of an *asset* name still ends
in `.dat`, so it is still sealed -- over the entry of the real asset it
aliases: one document overwritten by somebody in radio range, which is the
unauthenticated channel itself and is T-2098's job, not a gate's. And the gate
is on the **BLE receiver only**: `POST /upload` and WebDAV `PUT` still write any
path on the card, which is T-2103.

### The Wi-Fi upload endpoint bypasses all of this

`POST /upload` (`CrossPointWebServer`, `docs/webserver-endpoints.md`) writes any
path on the card directly and never passes through `MapTransferReceiver`. So a
wallet asset uploaded that way lands **in the clear** today, under a path the
viewer will later refuse to draw because `kFlagEncrypted` is not set. Found while
testing T-2095 on 2026-11-27 -- the same endpoint is what pulled the sealed files
back off the card for the proof below. Open, tracked as T-2103.

### Measured on an X3, 2026-11-27

`env:default`, commit `fedfd7fb`, wallet provisioned with a known K over
`CMD:WALLETPROVISION` so the laptop could decrypt what the board wrote.

| what | result |
|---|---|
| 52,304 B asset pushed plaintext over BLE | `OK 52304 dbc8ea61` -- the **plaintext** CRC, which the old read-back path could not have produced |
| same, 585,750 B 1:1 page image | `OK 585750 8c915f52` |
| both files pulled back off the card and decrypted with K and the content IV | **byte-identical to the plaintext**, 52,272 B and 585,718 B of payload |
| `sha256(decrypted)[0:8]` vs the header prefix | matches on both |
| header flags | `00` in, `03` out; other 31 bytes unchanged |
| flipped payload byte, honest CRC | `ERR hash mismatch`, previous good file intact |
| map tile push, same session | `OK 54394 4aa50b89`, read-back path unchanged |

That decrypt is a stronger check than the panel: it is byte-exact rather than
"it looked right", and it is the only thing that proves the IV, because a wrong
IV produces noise and no error.

### Re-run after the review fixes, 2026-11-27

The two serious findings below changed the sealing path itself, so the whole
pass above was run again on `d324b1be`. Everything it claims still holds, and
the bypass is now demonstrably shut on hardware rather than only in a test.

**X3, `env:default`**, wallet unlocked with the known K:

| what | result |
|---|---|
| `wallet/d0/<id>.dat` | `OK 52304 dbc8ea61`, the plaintext CRC |
| `wallet/d0/CASETEST0000BEEF.DAT` -- **the bypass** | `OK 52304 dbc8ea61` |
| both pulled off the card and decrypted with K and the content IV | `flags=03`, ciphertext on the card, **both decrypt byte-identical** |
| flipped payload byte, honest CRC | `ERR hash mismatch` |
| map tile | `OK 78392 51e90ff5`, unchanged |

Before the fix that second row returned `OK` as well -- and stored the bitmap
**in the clear**. The reply is the same; what is on the card is not. That is
why the check that matters is the decrypt, not the status line.

**X4 Pro, `env:x4pro`, release-shaped, wallet locked:**

| what | result |
|---|---|
| `wallet/21/<id>.dat` | `ERR wallet locked` |
| `wallet/21/CASETEST0000BEEF.DAT` | `ERR wallet locked` |
| `wallet/21/<id>.dat.` (trailing dot) | `ERR bad path` |
| map tile | `OK 54394 4aa50b89`, unchanged |

The second row is the whole case fix in one line: a refusal **means the gate
classified the uppercase path as a wallet asset**. Before the fix it was not one,
and the push succeeded with no key at all.

**T5 S3 Pro, `env:t5s3pro`, release-shaped** -- no wallet commands, so the gate
was walked with `CMD:BUTTON` (3 x RIGHT, DOWN, 3 x LEFT from cell 0, CONFIRM) and
the 585,750 B page image pushed at an **uppercase** path answered
`OK 585750 8c915f52`, the plaintext CRC. A sealed asset on a release-shaped S3
binary, sealed because the path gate now folds case.

**One thing the re-run did not repeat**: pulling that file back off the T5 and
decrypting it. The board slept before the web server could be reached, and this
environment has no `CMD:GOTO_WEBSERVER` to wake it with. The byte-exact decrypt
of an uppercase path was done on the C3 above, and the arithmetic is chip
independent; the S3 evidence after the fix is the plaintext-CRC reply and the
refusals, not a decrypt.

**And one thing nothing on the bench can reach**: the wallet locking *during* a
transfer, which is what `walletCipher()` re-reading the key per chunk protects
against. Locking needs the serial console, opening the serial port resets the
board, and a reset ends the transfer -- so there is no way to lock mid-push from
here. It is covered by the host test (`ACipherThatFailsMidTransferEndsIt`) and
by reading `Session::clear()`, and that is all it is covered by. A second control
channel would settle it.

**An incidental confirmation.** The T5's gate accepted the walk and the wallet
screen then said "Wrong key, or the wallet list was altered" -- its card still
held a `manifest.enc` from a different wallet. Unlocking and reading the
manifest are separate, and the session key is set either way, which is why the
push above worked on a board whose list cannot be drawn.

### And on an S3, 2026-11-27

Two boards, because the two S3 environments answer different questions.

**LilyGo T5 S3 Pro, `env:t5s3pro` plus `ENABLE_WALLET_TEST_CMDS`** -- the bench
flag, added in `platformio.local.ini` for this run only (see
`build-environments.md`, "Adding one flag to one env"), so the laptop could
provision a known K here too and decrypt what the board wrote. Panel 960x540,
120 B/row.

| what | result |
|---|---|
| 800x480 asset pushed to a 960x540 panel | `ERR wrong panel` -- the geometry gate, on hardware |
| 585,750 B 1:1 page image (stride 322, fits the window) | `OK 585750 8c915f52`, 74.5 s |
| pulled back off the card and decrypted with K and the content IV | **byte-identical**, 585,718 B of payload |
| header flags, other 31 bytes, hash prefix | `00` -> `03`, unchanged, matches |
| flipped payload byte, honest CRC | `ERR hash mismatch` |

**Xteink X4 Pro, `env:x4pro`, no bench flag** -- the release-shaped binary, which
is the one that matters for "is this code actually live in what ships". It
carries no wallet commands, so there is no key to unlock with and no decrypt to
do; what it proves is that the path exists and shuts:

| what | result |
|---|---|
| wallet `.dat` pushed with the wallet locked | `ERR wallet locked`, nothing on the card |
| map tile on the same board | `OK 49854 5285c152`, read-back path unchanged |

**Two traps on the way, both worth the next person's time.** The T5 S3 Pro's map
screen does not start BLE at all when the board has its own GNSS fix
(`bleInUse_ = SETTINGS.mapGnssPosition == 0 || forcePhonePosition_`,
`MapActivity.cpp`), and `transfer_.attach()` sits inside that branch -- so the
transfer receiver is not attached either and `blepush` finds nothing
advertising. `CMD:SETTING mapGnssPosition 0` for the run, and set it back.

And `--chunk` really is not optional on that board: BlueZ reported an MTU of 517
and the link refused a 509-byte write with
`BleakGATTProtocolError: Invalid Attribute Value Length`. `--chunk 240` ran
clean. Same note as `../../docs/synchro.md`, "blepush and the MTU".

**Cost against the transport: none visible.** The same 585,750 bytes took
**72.1 s** sealed and **72.5 s** down the plain path, both at 7.9 kB/s, one
board, one session. Sealing is not slower than the path it replaces: the
whole-file CRC read-back it removes costs about what the AES and the SHA-256 it
adds cost. The Wi-Fi figure the plan asked for could not be taken -- the Wi-Fi
upload path does not reach this code at all (see above).

### The whole manifest suite, re-run at the branch tip, 2026-11-27

`32f57693`, after five review rounds. The measurement below was taken at
`8b20eceb` -- the second commit of fifteen -- and everything after it changed
the path the manifest takes, including how `wallet/manifest.json` is routed at
all. A measurement is only valid against the state it was taken against, so the
scenarios were run again on the tip rather than carried forward.

**W27-W43 on an X3 (`env:default`, C3), W27, W30, W31, W37, W39, W40, W43 on an
X4 Pro (`env:x4pro`, S3).** All pass. The two that only one chip can answer:
the format's 32,726-byte manifest installs on the S3 and lands at exactly
32,768 bytes; the C3 refuses it long before, which is why W31 names the S3.

What the hostile pushes answered, at the tip, on both boards: `wallet`,
`wallet/manifest.enc`, `wallet/manifest.bak`, `wallet/manifest.bak/<id>.dat`,
`wallet/MANIFE~1.JSO` and `wallet/<shard>/x.dat` all answer
`ERR not a wallet file`; ` wallet/<shard>/<id>.dat` answers `ERR bad path`;
and the wallet directory afterwards holds only its asset shards. A locked board
answers `ERR wallet locked` and writes nothing.

**One number worth watching rather than filing.** The S3's NimBLE host task
reported **424 bytes free** of its 4 KB stack after an asset push at the tip
(`488` after a manifest). That is positive and it is thin. It is **not**
comparable with the `1044` the C3 reported in the same run -- different chip,
different build -- and no pre-change S3 figure was taken, so this is a baseline
rather than a delta. T-2104's pass should take the before and after properly.

### The manifest sealer, first measured 2026-11-27 at commit two of fifteen

Kept because it is the only byte-level proof of the envelope and it has not
been invalidated -- the format did not change after it. T-2096, commit
`8b20eceb`. Both boards provisioned with a known K over
`CMD:WALLETPROVISION`, a **plaintext** tree pushed over BLE, and the sealed
manifest pulled back to the laptop and decrypted there -- the proof is a byte
comparison, not a look at the panel.

**X3 (`env:default`, C3) and X4 Pro (`env:x4pro`, S3), end to end:** every asset
and the list sealed on arrival, and after a reboot and an unlock the board
browsed the document and drew its page. The locked board answered
`walletVersion=1` off the envelope's clear field before any key existed in RAM.

**The byte comparison, X3.** `manifest.enc` off the card decrypts under the known
K to the pushed JSON **byte for byte**: container v2, flags 0, `plaintextLen`
2,649 equal to the ciphertext length, `walletVersion` 1 in the clear and 1 in the
JSON, tag verified. 2,649 plaintext became 2,691 on the card, which is the
envelope plus the tag and nothing else. Two installs of identical content
produced **different files**, so the fresh nonce is real on hardware and not
just in the host test.

**The cap, from both ends, on the S3.** 32,727 bytes was refused at the begin
frame -- `ERR manifest too big`, three seconds, no bytes on the wire -- and
32,726 sealed, verified and installed, landing as exactly 32,768 bytes, which is
`kMaxEncryptedManifestBytes` to the byte. The derived bound is the read path's
own.

**And the C3 cannot reach it.** With BLE up, an X3 installs **20,480** bytes of
manifest and refuses **22,528**: either the ingest buffer or the read-back
buffer fails to allocate, or the AES DMA driver does. Every one of those is a
refusal with a message and the previous list stays live -- but the number to
quote for a C3 is about 20 KB, not 32. The free heap on that board with the map
screen and a transfer up is ~27 KB with a largest block of ~23 KB, so the design
cap was never going to fit there.

**The half-swap survivor, proved by making the state.** `manifest.enc` was
deleted over the web server, leaving the card exactly as a power cut between the
two renames would: the board read `manifest.bak`, listed the document, and the
panel said **"Previous list, last sync unfinished"**. One sync put it back and
the line went away.

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
# 1. a tree, its key and its pattern. **Check which walletgen you are running**
#    (parent repo, 2026-11-27): `--pattern` is on the `wallet` branch and not
#    on master, and the `t5s3` panel row is on `wallet-cardroot` and not on
#    `wallet`. The two have diverged -- 14 commits one way, 54 the other -- so
#    no single checkout has both. A run that needs a pattern AND a 960x540
#    tree has to merge them first; an X4 Pro needs neither.
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
manifest.enc = "EWM1" (4) | u8 version=2 | u8 flags | u8 nonce[12]
               | u32 plaintextLen | u32 walletVersion | ciphertext | u8 tag[16]
```

v1 envelopes carry no `walletVersion` and are 22 bytes instead of 26. Both are
read; a card written before the field existed must not become unreadable.
`walletVersion` sits **outside the tag** on purpose, so a locked device can say
which version the card holds without a key.

`parseManifestEnvelope()` reads the envelope, `feedEncryptedManifestAt()`
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

### `manifest.bak` is the half-swap survivor, and nothing else

Open since the viewer was built, decided by T-2096: the backup is read, **only**
when `manifest.enc` is missing, and the rider is told.

The reason it earns a reader now is that the device writes this file itself. A
FAT rename cannot replace an existing name, so installing a new list is two
renames -- `manifest.enc` to `manifest.bak`, then `manifest.enc.part` into place
-- and a power cut between them leaves a card with no `manifest.enc` at all.
Without a fallback that card reads as a **cleartext** tree (the encrypted one is
named by the presence of `manifest.enc`), finds no `manifest.json`, and says the
wallet is empty. With it, the board opens the list it had.

So:

- `treeIsEncrypted()` is true for either file.
- `encryptedManifestPath()` prefers `manifest.enc` and falls back to the backup,
  logging loudly.
- The browse screen replaces its document count with **"Previous list, last sync
  unfinished"** (`STR_WALLET_OLD_LIST`), because a rider shown a list that is one
  sync old has to be told rather than left to discover a missing document at a
  border.
- The next completed install clears it by itself.

The old objection -- that an older manifest may not match the assets on the card
-- still holds, and the message is the answer to it: every asset is verified by
its own hash anyway, so the worst case is a document the list names and the card
does not have, which the viewer already reports.

### Writing it: one shot, and the size trap that decides that

The device is the third writer of this envelope, after `tools/walletgen.py` and
the phone (T-2096). The phone sends plaintext JSON to `wallet/manifest.json`,
the device seals it and installs it as `manifest.enc`; the cleartext file is
never kept.

**The seal is not streamed, and mbedtls is not the reason.** `mbedtls_gcm_update()`
streams perfectly well -- the pinned ESP-IDF (5.5.2.260206) documents the
whole-block output buffering and states `output_size >= input_length + 15` is
always sufficient. The reason is the paragraph above this one: **the read path
cannot stream**, so it caps the file at 32 KB. A streamed seal would accept a
manifest above that cap, write it successfully, confirm it to the phone, and
leave a wallet that this same board can never open again. That is the worst
failure the device-key ladder can produce.

So the plaintext is buffered, bounded by `kMaxManifestPlaintextBytes` --
`kMaxEncryptedManifestBytes` minus the envelope and the tag, **derived from the
read path's own constant rather than written next to it** -- and a manifest over
it is refused at the `begin` frame, which already carries the length. The
refusal costs nothing on the wire and the phone gets `ERR manifest too big`.

The order of everything else is in `WalletManifestIngest.h` and
`WalletManifestInstall.h`. The two decisions worth repeating here:

- **The device never authors the JSON.** It seals a blob it was handed. The one
  number it reads out, `walletVersion`, it reads with `ManifestParser` -- the
  same class the browse screen uses -- so a body that seals is a body that
  opens. Authoring would hand the device the manifest format and a second
  buffer.
- **The sealed file is opened before the swap.** It is at most 32 KB and the key
  is in hand, so the cheap proof that the card holds a readable list is
  available, and nothing else gives it. An unreadable one is deleted and the
  previous list stays live.

An empty manifest (`"items": []`) is accepted: it is what a rider who deleted
their last document syncs, and refusing it would leave the device showing a list
the phone no longer has.

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

### How a caller borrows K

**By copy, for one unit of work, and never by pointer.** K lives in
`wallet::KeySlot` (`src/activities/wallet/WalletKeySlot.h`), held by `Session`.
There are two ways in and no third:

- `Session::hasKey()` -- a hint, read off the lock. For a gate that decides
  whether to start (the receiver's wallet gate, the read path's "locked" check
  before it opens a file). It can be stale by the time the caller acts.
- `wallet::KeyCopy key(Session::instance())` -- 32 bytes copied under the same
  critical section `clear()` takes, so the copy is the whole key or a refusal,
  never part of one. Wiped by the destructor when it leaves scope.

**One copy, one crypto call.** The copy is taken right before the call and dies
right after it: one sealer block of a transfer, at most 512 B (`walletCipher()`;
the largest BLE chunk at any MTU is 509 B, `WalletAssetIngest.h`, so one borrow
per chunk), one manifest seal (`walletSealManifest()`), one manifest decrypt
(`feedEncryptedManifestAt()`, after the card read), one row of a page window or
grey band (`readPlaneWindow()`, after that row's card read), one whole-screen
decrypt (`readWholeScreen()`, before the hash check). The next call borrows
again.

Two reasons for that tightness:

- **Never across a transfer.** A `KeyCopy` held across a whole transfer would
  bring back the bug it replaced, one level up: a key that survives a lock.
- **Never across a card read or write.** On a C3 a panic copies 1 KB of the
  faulting task's stack into RTC memory, and the next boot writes it as hex to
  `/crash_report.txt` on the card (`lib/hal/HalSystem.cpp`,
  `__wrap_panic_print_backtrace` and `checkPanic()`). A destructor does not run on
  a panic. A copy alive while a card fault panics is a key written to the card in
  the clear, which is the one thing the wallet promises the card never holds.
  Security review, 2026-11-28. The window cannot be zero: mbedtls's own AES
  context holds the key on the stack for the length of the call, as it did before
  T-2104. The S3 writes no stack (`#if !__riscv`).

Gates that only decide whether to start use `hasKey()`, which reads no bytes.
The sealer remembers which key it started under with `keyGeneration()`, which
reads a number and no bytes.

**Why not a pointer.** Until T-2104, `Session::key()` returned a pointer into
the session's own array, and `clear()` wiped that array in place. On a dual-core
board (`env:x4pro`, `env:t5s3pro`: NimBLE on core 0, the Arduino task on core 1,
`CONFIG_FREERTOS_UNICORE` unset) a sleep -- `clear("sleep")`, the only lock those
builds have, because `CMD:WALLETLOCK` sits behind `ENABLE_WALLET_TEST_CMDS`, which
only the single-core `env:default` sets -- could land between
the null check and `mbedtls_aes_setkey_enc()`, or inside it. The asset sealer then
enciphered the rest of the file under a half-zeroed key, the hash and the CRC
passed because both are over the plaintext, and the file was renamed over the
previous good one. Read off the code; never reproduced on a board.

**Why no lock across the AES.** The lock is a `portMUX` critical section, and the
AES call cannot run inside one: on both chips the ESP-IDF AES driver is the DMA
one, and it takes a blocking lock for the peripheral (framework-espidf 5.5.2,
`components/mbedtls/port/aes/dma/esp_aes.c`, `AES_LOCK()` ->
`esp_crypto_sha_aes_lock_acquire()` -> `_lock_acquire()`). Blocking with
interrupts masked is not allowed. The lock covers a 32-byte memcpy; the AES runs
on the copy.

**The lock is a `portMUX` critical section.** On an S3 it is a spinlock between
the cores. On the C3 the pinned ESP-IDF ignores the mux and masks interrupts
(framework-espidf 5.5.2, riscv `portmacro.h`, `portENTER_CRITICAL(mux)` under
`CONFIG_FREERTOS_UNICORE`), which is as exclusive on one core.

**What a lock mid-transfer does now.** The next block's borrow fails,
`AssetIngest` ends with `CipherFailed`, and the receiver calls `abandon()`, which
deletes the `.part` file and never renames. The previous good asset stays. A
manifest ends with `SealFailed` the same way.

**A different key mid-transfer ends it too.** "There is a key" is not "it is the
same key". The slot counts generations: every `set()` and every `clear()` moves
it, and `KeyCopy::generation()` says which one a borrow saw. **The same key set
again counts as new too**: the slot cannot tell a re-unlock from a re-provision.
So `CMD:WALLETUNLOCK` sent to an already unlocked `env:default` board while an
asset arrives now ends that transfer with `ERR encrypt failed`, where before
T-2104 it finished. Fails safe, devel console only. The receiver takes
the generation at begin and hands it to `walletCipher()` as the sealer's
context; a borrow under any other generation refuses like a locked wallet. Without
it, a wallet locked, re-provisioned (`CMD:WALLETPROVISION force`) and unlocked
between two chunks sealed the rest of the asset under the new key, and the file
was renamed over the good one. Reachable only from the devel console, because
the unlock screen needs the map screen gone first. The manifest needs no such
check: it is sealed once, at the end, under whichever key is there then.
Host-tested: `WalletKeySlot.TheGenerationSaysWhichKeyItWas`,
`WalletIngest.ADifferentKeyMidTransferEndsWithCipherFailed` (the sealer driven
with the same check, not `walletCipher()` itself).

**Verified on the host, not on a board** (`test/wallet/WalletKeySlotTest.cpp`,
and the slot-bound tests in `WalletIngestTest.cpp` and
`WalletManifestSealTest.cpp`). The slot-bound tests drive the sealers with the
same borrow the device binds and show the sealer ends with `CipherFailed` /
`SealFailed`; they do not run `walletCipher()` itself or the receiver's
`abandon()`, which are read off the code. The race test runs two threads against a
`std::mutex`-bound slot for at least one second and 200,000 borrows and sees zero
torn copies. Two broken slots it catches, measured 2026-11-28 on a 14-core laptop,
`-O2`:

- **a no-op lock**: fails every run on a multi-core host, 5,700 to 182,000 torn
  copies across four review passes at varying load;
- **the copy moved out of the lock, the `has_` check still inside**: 0 of 20 runs
  passed with every core busy-looped. A count-only loop (200,000 borrows, no time
  floor) passed it 4 of 20 under the same load, which is why the floor exists.

On one CPU a torn copy needs preemption mid-copy and the no-op lock passed 4 of 5
runs, so the test skips with fewer than two usable CPUs. It shows `KeySlot` takes
its lock around the copy and the wipe. It does not prove `portENTER_CRITICAL` is
exclusive across an S3's cores; that is ESP-IDF's claim.

**Cost per block: a 32-byte copy and a 32-byte wipe, against AES over up to
512 B.** Not measured on a board.

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

**The wrap is the last thing `provision()` writes**, and that order is load
bearing rather than tidy: `isProvisioned()` reads the wrap alone, so a failed
write or a power cut anywhere earlier leaves a device that still reports
unprovisioned and can be set up again. The failure counter used to sit after the
wrap, which meant a failure there returned false with a usable wrap already
stored -- harmless until enrollment could retry, and then terminal: the rider is
sent back to choose a new pattern, the next `provision()` refuses without
`force`, and the wallet is locked under the one pattern the screen said had
failed. Found in review, 2026-11-26.

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
- **Ingest, T-2095, 2026-11-27**: the sealer's arithmetic is **host-tested**
  (19 tests, OpenSSL and an independent CRC-32, every refusal proved to fire,
  the ciphertext proved identical across chunk sizes from 1 to 4096 bytes), and
  the whole chain **ran end to end on an X3** -- plaintext pushed over BLE,
  sealed on the board, pulled back off the card and decrypted byte-identical
  with the wallet key and the content IV (see "Ingest" above), **and on both S3
  environments** -- the decrypt proof repeated on a T5 S3 Pro with the bench
  flag, and the locked-wallet refusal plus an unchanged tile push on an X4 Pro
  running the release-shaped binary. **Re-run end to end on all three boards
  after the review fixes** (see "Re-run after the review fixes"), where the
  closed case-folding bypass was confirmed by decrypting an uppercase-path asset
  off the card. Still open: the Wi-Fi upload endpoint bypasses the sealer
  (T-2103), and a lock *during* a transfer cannot be reached from the bench.
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
