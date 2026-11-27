# Wallet test scenarios

**Run every scenario here after any change that touches the wallet.** Not the
ones that look related -- all of them. The wallet is one chain (enroll, unlock,
ingest, read) and the defects found so far were all in the seam between two
rungs, where a change to one end looked harmless from the other.

**A rung that lands adds its scenarios here, in the same pass.** The list is the
regression suite; a scenario that exists only in a session's head is gone when
the session ends. `../../../docs/TODO.md`, section "Wallet: the key is born on
the device", is the ladder this follows.

Scenario ids are stable (`W1`, `W2`, ...). **Never renumber** -- a session
report says "W7 failed" and that has to mean the same thing next month.

## Before you start

- **Two chip families, not one board.** `ENABLE_WALLET` is in `base`, so every
  wallet change reaches every environment. One C3 (`env:default`, X3) and one S3
  (`env:t5s3pro` or `env:x4pro`).
- **Take the device lock for the whole block of device work**, not per command
  (`../../../docs/synchro.md`). **Ask before every flash.** Archive the build
  after the upload.
- **Some scenarios need a dev build** (`-DENABLE_WALLET_TEST_CMDS=1`, set only in
  `[env:default]`). They are marked *devel*. A release build must never carry
  them, so the product-path scenarios below are the ones that matter most: they
  use buttons and nothing else.
- **A board that has a wallet cannot run the setup scenarios again** until
  something clears the wrap. Until T-2099 lands that is
  `esptool erase-region` over NVS. Plan which board takes which group.

## Host tests: run these first, they cost seconds

```
cd firmware/explorink/test
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target WalletTest PatternLockTest -j8
./build/wallet/WalletTest && ./build/pattern_lock/PatternLockTest
```

Both must be green before a board is touched. They cover the arithmetic, the
ingest state machine and the pattern rules; they cover **no** panel, no card and
no radio.

## A. Setup, on the device (T-2094)

| # | Scenario | Expected | Why it is here |
|---|---|---|---|
| W1 | Cold board, no wrap in NVS: Home > Wallet | Offers setup. Reached with buttons only | The product path. A board that needs a laptop is the thing this rung removed |
| W2 | Pick a start cell, walk 8-12 points, walk the same again, save | Wallet exists. *devel:* `CMD:WALLETSTATUS` reports the wrap and that start cell | The whole rung |
| W3 | Second walk differs from the first | Nothing stored, back to the first walk, retry works | A mismatch that stored anything would lock a rider out of their own documents |
| W4 | Cancel mid-enrollment (BACK), then reopen Home > Wallet | Still offers setup. Nothing in NVS | Both walks must be wiped on every exit path, cancelled included |
| W5 | Reboot, open the wallet, walk the saved pattern | Unlocks | The wrap survives a power cycle |
| W6 | Reboot, walk a different legal pattern | Refused, failure counted | A wrong walk must not open the wrap |
| W7 | Walk shorter than 8 points | Refused before any PBKDF2 runs | The wallet's floor is 8, not the gate's default of 4 |
| W8 | Board with a wrap made before the pattern gate (no start cell) | Says "This wallet predates the pattern lock / Set it up again" -- **not** "no wallet key" | Two different states; the panel said the wrong one once already (2026-11-26) |
| W9 | Board with no wallet key, card holds an encrypted wallet | Says "No wallet key on this device / This card was set up somewhere else" | The safety branch: never offer to mint a new key over documents it cannot see |

## B. The session key (T-2094)

| # | Scenario | Expected | Why it is here |
|---|---|---|---|
| W10 | Unlock, then leave the wallet screen open and untouched for 120 s | Key gone, gate again on the next action | `kIdleTimeoutMs`. A device left on a table must lock itself |
| W11 | Unlock, sleep the device, wake it | Key gone | Sleep wipes the key first thing |
| W12 | Ten consecutive wrong walks | Delays grow (1, 2, 4, 8, 16, 30 s), then the wrap is refused | Rate limiting. **Destructive:** the board then needs a re-provision |
| W13 | Wrong walks, then power cycle before the delay expires | The delay is still owed | The counter is in NVS and is written before the next attempt |

## C. Ingest: the device seals what arrives (T-2095)

Build a cleartext tree with `tools/walletgen.py` (no `--key`) and push it with
`tools/blepush.py`; the board must be unlocked and on a screen that advertises.

| # | Scenario | Expected | Why it is here |
|---|---|---|---|
| W14 | Push a plaintext asset to an unlocked board | `OK <bytes> <crc32>` carrying the CRC of the **plaintext sent**; the file on the card is ciphertext | The rung. The reply means "arrived whole and was written", not "the card holds these bytes" |
| W15 | Pull that file off the card and decrypt it with K and the content IV | Byte-identical to the plaintext that was sent | The only proof the IV was right. A wrong IV is noise, not an error |
| W16 | Open the document on the panel after a reboot and an unlock | Draws | Same proof, through the product path |
| W17 | Flip one payload byte, keep the CRC honest | `ERR`, hash mismatch, nothing renamed | The device must check the prefix it derived the IV from |
| W18 | Push to `wallet/ab/<id>.DAT` (uppercase) | Recognised as a wallet asset and sealed | **Regression, 2026-11-27.** FAT folds case, so the lowercase gate let an attacker's bitmap replace a sealed document |
| W19 | Push a path with a `.` component, or a trailing dot or space | Refused | Same FAT equivalence, different spelling |
| W20 | Push a wallet asset while the wallet is locked | Refused at `begin`, nothing on the card | Storing the plaintext "for now" is what this rung exists to prevent |
| W21 | Push an asset that already carries the encrypted flag | Refused | It was sealed under a key this board does not have |
| W22 | Push a map tile | Unchanged behaviour, read-back CRC included | **Regression.** The tile path must not notice the wallet exists |

## CM. The manifest: the device seals the list (T-2096)

The list is pushed as **plaintext** `wallet/manifest.json`; the device seals it
and installs it as `manifest.enc`. Nothing is ever written at the name it
arrived under.

| # | Scenario | Expected | Why it is here |
|---|---|---|---|
| W27 | Push a plaintext manifest to an unlocked board | `OK`, and the card holds `manifest.enc` and no `manifest.json` | The rung. An encrypted tree carries no cleartext list |
| W28 | Pull `manifest.enc` off the card and decrypt it with K | Byte-identical to the JSON that was sent; container v2, `walletVersion` in the clear matching the JSON | The only proof the envelope is the format's and not this device's own idea of it |
| W29 | Push the same manifest twice, compare the two files | Different bytes | A fresh nonce per write. One key and one nonce over two plaintexts breaks GCM outright |
| W30 | Push a manifest of 32,727 bytes | `ERR manifest too big`, refused at `begin`, no bytes sent | One byte over the cap the read path enforces. A sealer that accepted it would write a list this same board can never open |
| W31 | Push a manifest of exactly 32,726 bytes **on an S3** | Installs, and the file is exactly 32,768 bytes | The other end of the same cap. **C3 cannot reach it** -- an X3 with BLE up installs 20,480 and answers `ERR oom` at 22,528, which is a refusal and not a loss |
| W32 | Delete `manifest.enc` over the web server, leaving only `manifest.bak`, then browse | The documents are listed, and the panel says "Previous list, last sync unfinished" | The half-swap survivor. A FAT rename is not atomic, so this is the state a power cut between the two renames leaves |
| W33 | Sync once more after W32 | The line goes away, `manifest.enc` is back | The card repairs itself on the next completed install |
| W34 | Lock the wallet, push a manifest | `ERR wallet locked`, nothing on the card | Same rule as an asset: sealed, or not stored |
| W35 | Push a body that is not JSON | `ERR not a manifest` on the **first chunk** | The cheapest refusal there is, and it must not wait for the whole body |
| W36 | Push a manifest whose `formatVersion` this firmware does not know | `ERR not a manifest`, previous list still live | The inspection uses the read path's own parser, so a body that seals is a body that opens |
| W37 | Push `wallet/manifest.enc` or `wallet/manifest.bak` directly | `ERR not a wallet file` | **Regression, 2026-11-27.** The device's own outputs are not writable from the wire; the backup is the only list a half-installed card has |
| W38 | Push `wallet/MANIFE~1.JSO` | `ERR not a wallet file` | **Regression.** FAT's 8.3 alias is the same directory entry as `manifest.json` and a different string to a name gate |
| W39 | Push ` wallet/<shard>/<id>.dat` (leading space) | `ERR bad path`, and the sealed asset of that name is untouched | **Regression.** SdFat strips leading spaces, so this is one more spelling of one file. It replaced a sealed document with plaintext on a build without the check |
| W40 | Push `wallet/manifest.bak/<id>.dat`, then list the wallet directory | `ERR not a wallet file` and **no new directory** | **Regression.** The gate must run before the directory creation, and must check the whole path shape -- a directory named `manifest.bak` can never be deleted by this firmware |
| W41 | Push the bare path `wallet` | `ERR not a wallet file` | **Regression.** A regular file there and the wallet can never exist on that card |
| W42 | Push `wallet/<shard>/x.dat` (not 16 hex), or a shard that disagrees with the id | `ERR not a wallet file` | The accepted shape is the only one the read path can open |

## D. Reading (T-2094 and earlier)

| # | Scenario | Expected | Why it is here |
|---|---|---|---|
| W23 | Browse the list after a reboot and an unlock | The documents the manifest names | The manifest decrypts and parses |
| W24 | Open a document: fit, detail, 1:1, pan the window | Draws at every level | The windowed read's CTR offsets |
| W25 | Open a machine code full screen and scan it with a phone | Scans off the glass | The code path verifies its hash before drawing |
| W26 | A card whose manifest was made under a different key | One message: "Wrong key, or the wallet list was altered" | GCM cannot tell the two apart and the panel must not pretend it can |

## E. Known and accepted, not defects

- **`CMD:SCREENSHOT` is ungated in every build**, so a cable or a radio in range
  gets the panel -- which during enrollment is the finished pattern in one grab.
  Behaviour kept deliberately (a rider who cannot see the walk cannot repeat it).
  Tracked as T-2081. **Do not file it again**; check only that the doc and the
  comment still say what it costs.
- **`POST /upload` bypasses the sealer** (T-2103), so a wallet asset over Wi-Fi
  lands in the clear and the viewer refuses it. Expected today. When T-2103
  closes, it becomes a scenario here.

## What no scenario here reaches

Say so in a report rather than implying coverage:

- **A wallet locking during a transfer.** Locking needs the serial console,
  opening the port resets the board, and the reset ends the transfer. Covered by
  a host test and by reading `Session::clear()`, nothing else.
- **The gate's own touch boxes** in `TOUCH_ANYWHERE` (T-2102). No board here
  draws them.
- **An unreadable card at runtime**, and a failed NVS write. Both are error
  branches no bench has reached.
