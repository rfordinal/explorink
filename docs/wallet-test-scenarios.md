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
- **Only the C3 can be unlocked by a wallet command. An S3 is unlocked by
  walking its pattern.** `ENABLE_WALLET_TEST_CMDS` is set in `env:default` and
  in **no** other environment, and the whole block goes with it --
  `WALLETPROVISION`, `WALLETUNLOCK`, `WALLETLOCK`, `WALLETSTATUS`,
  `WALLETBENCH` and `GOTO_WALLET` (`src/main.cpp`, the `#if` above
  `WALLETBENCH`). Measured 2026-11-28 on an X4 Pro: `CMD:WALLETSTATUS` and
  `CMD:GOTO_WALLET` both silent.
  **That does not mean a thumb is required.** `ENABLE_BUTTON_CMD` is set in
  `env:x4pro` and `env:t5s3pro`, so `tools/press.py` walks the gate from the
  host exactly as a thumb does -- which is how T-2094's enrollment ran on an
  X4 Pro (`wallet-crypto.md`). What a host needs on an S3 is the **board's
  pattern**, not a finger.
  **So pick the board by what its wallet is.** A board whose wallet predates
  the pattern gate cannot be opened by anything, and a board nobody recorded a
  pattern for is the same problem. **Do not guess one** -- ten wrong walks
  refuse the wrap and the board then needs a re-provision (W12).
- **A provisioned board shows the gate whatever its card holds** (T-2109,
  `walletEntryDecide()` in `src/activities/wallet/WalletEntryDecide.h`). Before
  that fix the card decided: a provisioned board with an empty card drew "No
  wallet on the card" and no gate -- measured on `t5s3pro-b`, 2026-11-28 -- and
  the first unlock there was `CMD:WALLETUNLOCK` or nothing. A board on firmware
  older than that fix still behaves the old way; ask the board which build it
  runs before reading a missing gate as a regression.
- **Traps the S3 run of 2026-11-28 paid for**, in the order they bite:
  1. **A T5 S3 Pro with `mapGnssPosition=1` never starts BLE on the map**, so
     every push finds no board (`ble-advertising.md`, "When it advertises").
     `CMD:SETTING mapGnssPosition 0` for the run, back to `1` after.
  2. **On the map, a `CMD:` line can be eaten by the map console.**
     `CMD:SETTING` and `CMD:GOTO_WEBSERVER` both vanished there, with
     `[MAPCON] rx:` logging a line of log text glued to the command.
     `CMD:GOTO_WALLET` first, then the command, then `CMD:GOTO_MAP` again.
  3. **The session key's idle clock runs off the wallet screens too.** Only a
     wallet screen *checks* it (`Session::expireIfIdle()`), so the key lives on
     the map for as long as the pushes take -- and dies the moment a wallet
     screen opens more than 120 s after the unlock. Unlock, go to the map, push;
     do not detour through the wallet in between.
  4. **Name the board: `blepush.py --address`.** Several boards advertise the
     same service. Read the address off a scan (`--address 00:00:00:00:00:00`
     prints who is advertising); `t5s3pro-b` is `30:ED:A0:A8:37:75`.
     And `--chunk 180`, or the first write fails with
     `Invalid Attribute Value Length`.
  5. **`walletgen.py --panel t5s3` exists only on the parent's
     `wallet-cardroot` branch** (2026-11-28). An `x4` asset on a T5 S3 Pro is
     refused `ERR wrong panel`, correctly; a manifest is not, because it names
     a panel and is sealed without being drawn.
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
cmake --build build --target WalletTest PatternLockTest MapTransferTest -j8
./build/wallet/WalletTest && ./build/pattern_lock/PatternLockTest && ./build/map_transfer/MapTransferTest
```

All three must be green before a board is touched. They cover the arithmetic,
the ingest state machine, the pattern rules and -- since T-2106 -- the transfer
receiver's own decisions; they cover **no** panel, no card and no radio.

### What `MapTransferTest` now reaches, and what it does not

T-2106 lifted the receiver's frame parse, its begin ladder and its chunk state
machine into `../src/activities/map/MapTransferDecide.h`, so the **refusal
decisions** behind these scenarios are now driven by 45 host cases instead of
only by a board:

| Scenario | What the host now decides | What still needs a board |
|---|---|---|
| W20 | `wallet locked` is the answer for an asset with no key, and it is the *first* answer -- before the no-panel gate | That nothing reached the card |
| W22 | A plain tile push passes every wallet gate untouched | The read-back CRC, the rename, the `OK` |
| W30 | `manifest too big` fires one byte over the cap and not at the cap | That no bytes were sent, and the `ERR` reaching the phone |
| W34 | `wallet locked` for the manifest, ahead of the cap check | Same |
| W37, W40, W41, W42 | `not a wallet file` is answered **after** `busy` and before anything builds a path | That no directory was created |
| W39 | `bad path` is the first refusal of all, ahead of the subscription and busy gates | That the sealed file of that name is untouched |
| W43 | Nothing: the parse-and-rebuild is `WalletTest`'s | The card holding exactly one lower-case entry |

**A decision test retires no scenario.** It answers "did the gate fire, in the
right order, with the right words" and says nothing about the card, the panel,
the sealing bytes or the radio -- which is the whole of what these scenarios are
for. Run them all, as before.

Two more things the host now covers that no scenario here ever did: the chunk
state machine (a repeat, a gap, a reorder, an overrun, a whole file walked
chunk by chunk) and the stale-transfer reclaim at its exact 30 s boundary.

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
| W9a | Provisioned board, **empty** wallet directory, reboot, Home > Wallet | The gate, reached with buttons only. After the unlock: "No wallet on the card", and a push is then accepted | T-2109. The card used to decide, and a rider who rebooted between setup and the first sync could never unlock -- a locked board refuses every push. Run on both chip families |
| W9b | Provisioned board, **cleartext** tree on the card, key not in the session | The gate, then the cleartext list | The wrap decides, not the card. A locked board would refuse a sync onto this card too |

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
| W43 | Push `wallet/<SHARD>/<ID>.DAT` in upper case, then list the shard directory | Exactly one file, named in lower case, sealed | **The request is parsed and the path is rebuilt.** The sender's spelling is a lookup key; the card path comes from sixteen validated hex characters and firmware constants, so no spelling can produce a second entry or a different file |

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

## Runs on hardware

One line per scenario per chip family, newest run only. A scenario with no S3
row and no C3 row has not run on that family.

### S3: `t5s3pro-b`, 2026-11-28 (T-2108)

`wallet-viewer` `46725470`, `env:t5s3pro` plus `-DENABLE_WALLET_TEST_CMDS=1`,
archived `docs/firmware-builds/2026-11-28-t5s3pro-b-t2108-s3-wallet-testcmds-4-good`
(parent repo). Wallet born on the device in T-2094, start cell 5. Tree from
`walletgen.py --demo --panel t5s3`. The card was read back over the web server
(`GET /api/files`, `GET /download`).

The first unlock was `CMD:WALLETUNLOCK`, because the card was empty (see
"Before you start"). After W27 put a `manifest.enc` on it, the board was
rebooted, `CMD:GOTO_WALLET` drew the gate, and **`press.py` walked the
board's pattern, seven presses plus confirm**: `unlocked=1`, `attempts=0`.
The pattern itself is in the parent repo's `docs/TODO.md`, T-2108 -- **never
in this repo**, which is public: a bench pattern is still a working unlock for
the board that carries it. W14, W17 and W43 ran under that key.

| # | Result |
|---|---|
| W27 | `OK 2653 0aad1c1b`, `WALLETSTATUS manifest=enc`. Card: `manifest.enc` (`EWM1`, container v2, no title in the clear) and `manifest.bak`, **no** `manifest.json`. Pushed a second time with the `t5s3` list; that is the `.bak` |
| W14 | `OK 64832 65814f29` -- the CRC of the plaintext sent. File on the card is the same length, header identical except `flags` `0b00` -> `0b11` (encrypted, IV from the content), 64,520 of 64,800 payload bytes differ (the rest is the 1-in-256 chance match) |
| W17 | One payload byte flipped (offset 1032, `ff` -> `fe`), CRC computed over the flipped file: `ERR hash mismatch`. Nothing at that name afterwards, no `.part` |
| W43 | Pushed as `wallet/EE/EE02ECE2653441F4.DAT`: `OK 259232 be0f85ae`. The wallet directory holds one `ee`, which holds exactly one file, `ee02ece2653441f4.dat`, 259,232 B, `flags` `0b11` |
| W16 | After the reboot and the button unlock, Select drew the device-sealed page: FIT, 960x540, 1,582 ms |
| W23 | The browse list off the device-sealed manifest: "T2108 bench, 1 page" |

**Seen on the way, not a scenario yet:** a push refused by the asset header
(`ERR wrong panel`, an `x4` asset) left an **empty shard directory** `6a/` on
the card. The begin ladder runs every refusal before
`ensureDirectoryExists()` (`src/activities/map/MapTransferReceiver.cpp`,
"Below every refusal"), but the header is checked on the first data chunk
(`src/activities/wallet/WalletAssetIngest.h`, `checkAssetForIngest`), after the
directory exists. Needs a key in the session to reach. T-2110.

Shots, 1:1, parent `docs/device-shots/2026-11-28-t5s3prob-wallet-*`.

### W9a on both chip families, 2026-11-28 (T-2109)

Branch `t2109-empty-card-gate` off `wallet-viewer`, firmware `8e519d10`.
`t5s3pro-b` on `env:t5s3pro` (no test commands), the X3 (`7c:e8:b1:6f:c2:9c`)
on `env:default`. Archived in the parent repo as
`docs/firmware-builds/2026-11-28-{t5s3pro-b,x3}-t2109-empty-card-gate-5-good-gate-on-empty-card`.

**How the card was emptied without deleting anything:** every file under
`/explorink/wallet` moved into a sibling `/explorink/wallet.t2109` over the web
server (`POST /mkdir`, `POST /move`; `/rename` takes files only), shard
directories left behind empty. SHA-256 of every file taken first
(`GET /api/hash`) and compared after the move back: all equal on both boards,
the sibling directory then deleted. `t5s3pro-b` reached the web server through
Home > File Transfer with `press.py`, because `CMD:GOTO_WEBSERVER` is a test
command.

| # | Board | Result |
|---|---|---|
| W9a | `t5s3pro-b` (S3) | Reboot, Home > Wallet by `press.py` (`down down down confirm`): **the gate**, cursor on the stored start cell 5. The board's pattern, seven presses plus confirm: unlocked, and the list says "No wallet on the card" |
| W9a | X3 (C3) | `WALLETSTATUS provisioned=1 manifest=none` after the reboot. Home > Wallet by `press.py` (`down down confirm`): **the gate**, cursor on cell 5. Not walked: nobody recorded this board's pattern, and a guess counts against the limiter (W12) |
| -- | `t5s3pro-b` | Files moved back, reboot, Home > Wallet: the gate again, over the restored `manifest.enc` |

**Not run:** the push half of W9a (a push accepted after the button unlock),
and W9b. The push path itself is the one W27 already ran under a key from
`CMD:WALLETUNLOCK`; the session key is the same object either way
(`KeyStore::tryUnlock()`). W9b needs a cleartext `manifest.json` written to a
provisioned board's card.

## What no scenario here reaches

Say so in a report rather than implying coverage:

- **A wallet locking during a transfer.** Locking needs the serial console,
  opening the port resets the board, and the reset ends the transfer. Covered by
  a host test and by reading `Session::clear()`, nothing else.
- **The gate's own touch boxes** in `TOUCH_ANYWHERE` (T-2102). No board here
  draws them.
- **An unreadable card at runtime**, and a failed NVS write. Both are error
  branches no bench has reached.
